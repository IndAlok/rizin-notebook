/// \file main.go
/// \brief Entry point: CLI flags, config, HTTP server, graceful shutdown.

package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"path"
	"runtime/debug"
	"syscall"
	"time"
)

var (
	NBVERSION string ///< Set at build time via -ldflags.
	webroot   string ///< URL path prefix for all routes.
	notebook  *Notebook
	config    *NotebookConfig
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

	// Allow RIZIN_PATH env var to override the default rizin binary location.
	if loc := os.Getenv("RIZIN_PATH"); len(loc) > 1 {
		rizinbin = loc
	}

	fmt.Printf("Server data dir '%s'\n", dataDir)
	notebook = NewNotebook(dataDir, rizinbin)

	// Start the HTTP server in a goroutine.
	srv := startServer(assets, bind, debug)

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

/// \brief Resolves notebook version from build metadata when -ldflags version is not set.
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

/// \brief Starts the HTTP server in a goroutine; returns *http.Server for shutdown.
func startServer(assets, bind string, debug bool) *http.Server {
	router := setupRouter(assets, bind, debug)
	srv := &http.Server{
		Addr:    bind,
		Handler: router,
	}

	go func() {
		fmt.Printf("Server listening at http://%s\n", bind)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "listen error: %v\n", err)
			os.Exit(1)
		}
	}()

	return srv
}
