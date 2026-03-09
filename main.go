package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path"
	"runtime/debug"
	"syscall"
	"time"
)

var (
	NBVERSION string
	webroot   string
	notebook  *Notebook
	config    *NotebookConfig
	catalog   *Catalog
)

func usage() {
	flag.PrintDefaults()
	fmt.Fprintf(os.Stderr, "Environment vars:\n  RIZIN_PATH\n    \toverrides where rizin executable is installed\n")
}

func main() {
	var debug bool
	var assets string
	var bind string
	var rizinbin = "rizin"
	var dataDir = ".rizin-notebook"

	if len(NBVERSION) < 1 || NBVERSION == "unknown" {
		NBVERSION = resolveNotebookVersion()
	}

	if homedir, err := os.UserHomeDir(); err == nil {
		dataDir = path.Join(homedir, ".rizin-notebook")
	}

	// Parse flags BEFORE using dataDir to create config or notebook,
	// so that the -notebook flag is respected on first load.
	flag.StringVar(&bind, "bind", "127.0.0.1:8000", "[address]:[port] address to bind to.")
	flag.StringVar(&webroot, "root", "/", "defines where the web root of the application is.")
	flag.StringVar(&dataDir, "notebook", dataDir, "defines where the notebook folder is located.")
	flag.StringVar(&assets, "debug-assets", "", "allows you to debug the assets (-debug-assets /path/to/assets).")
	flag.BoolVar(&debug, "debug", false, "enable http debug logs.")
	flag.Usage = usage
	flag.Parse()

	// Ensure the data directory exists.
	if err := os.MkdirAll(dataDir, os.ModePerm); err != nil && !os.IsExist(err) {
		fmt.Fprintf(os.Stderr, "fatal: cannot create data directory: %v\n", err)
		os.Exit(1)
	}

	// Load config AFTER flags are parsed so -notebook is respected.
	config = NewNotebookConfig(dataDir)
	config.UpdateEnvironment()

	// Initialize catalog (per-page .rznb storage).
	var err error
	catalog, err = NewCatalog(dataDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fatal: cannot open catalog: %v\n", err)
		os.Exit(1)
	}
	defer catalog.Close()

	// Migrate legacy monolithic notebook.db into per-page .rznb files.
	if migrated, merr := catalog.MigrateFromLegacyDB(dataDir); merr != nil {
		fmt.Printf("warning: legacy DB migration error: %v\n", merr)
	} else if migrated > 0 {
		fmt.Printf("Migrated %d pages from legacy notebook.db to .rznb files.\n", migrated)
	}

	// Import JSON page data into .rznb files if present.
	if migrated, merr := catalog.MigrateFromJSON(dataDir); merr != nil {
		fmt.Printf("warning: JSON migration error: %v\n", merr)
	} else if migrated > 0 {
		fmt.Printf("Migrated %d pages from JSON to .rznb files.\n", migrated)
	}
	if merr := catalog.MigrateSettings(dataDir); merr != nil {
		fmt.Printf("warning: settings migration error: %v\n", merr)
	}

	// Allow RIZIN_PATH env var to override the default rizin binary location.
	if loc := os.Getenv("RIZIN_PATH"); len(loc) > 1 {
		rizinbin = loc
	}

	// Prevent the rz_notebook plugin from auto-starting another server
	// instance when we spawn rizin sub-processes (to load commands, etc.).
	os.Setenv("RIZIN_NOTEBOOK_NO_AUTOSTART", "1")

	fmt.Printf("Server data dir '%s'\n", dataDir)

	// Create notebook WITHOUT loading commands yet; the HTTP server
	// must be listening BEFORE we spawn any rizin sub-process, so that
	// the rz_notebook plugin inside those sub-processes sees us as alive
	// and does not try to start yet another server instance.
	notebook = NewNotebook(dataDir, rizinbin)

	// Start the HTTP server FIRST (synchronous listen, async serve).
	srv := startServer(assets, bind, debug)

	// NOW load rizin commands; any rizin sub-process will find us alive.
	notebook.LoadCommands()

	// Block until SIGINT or SIGTERM is received.
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	fmt.Println("\nShutting down server...")

	// Close all open Rizin pipes to prevent orphan processes.
	notebook.closeAllPipes()

	// Gracefully shut down the HTTP server with a 5-second timeout.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "server shutdown error: %v\n", err)
	}
	fmt.Println("Server stopped.")
}

func resolveNotebookVersion() string {
	bi, ok := debug.ReadBuildInfo()
	if !ok {
		return "dev"
	}

	if bi.Main.Version != "" && bi.Main.Version != "(devel)" {
		return bi.Main.Version
	}

	var rev string
	var dirty bool
	for _, s := range bi.Settings {
		switch s.Key {
		case "vcs.revision":
			rev = s.Value
		case "vcs.modified":
			dirty = s.Value == "true"
		}
	}

	if rev == "" {
		return "dev"
	}

	if len(rev) > 12 {
		rev = rev[:12]
	}

	if dirty {
		return "dev-" + rev + "-dirty"
	}
	return "dev-" + rev
}

func startServer(assets, bind string, debug bool) *http.Server {
	router := setupRouter(assets, bind, debug)
	srv := &http.Server{
		Handler: router,
	}

	// Bind the socket synchronously so the server is reachable
	// before we return.  This guarantees that any rizin sub-process
	// spawned later will find the health-check endpoint alive.
	ln, err := net.Listen("tcp", bind)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fatal: listen error: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("Server listening at http://%s\n", bind)

	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "serve error: %v\n", err)
			os.Exit(1)
		}
	}()

	return srv
}
