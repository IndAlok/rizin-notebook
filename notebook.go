package main

import (
	"fmt"
	"path/filepath"
	"sync"
)

type Notebook struct {
	mutex   sync.Mutex
	storage string
	pipes   map[string]*Rizin
	jsvm    *JavaScript
	rizin   string
	cmds    map[string]RizinCommand
}

func NewNotebook(storage, rizinbin string) *Notebook {
	jsvm := NewJavaScript()
	if jsvm == nil {
		panic("failed to create scripting engine")
	}

	return &Notebook{
		storage: storage,
		pipes:   map[string]*Rizin{},
		jsvm:    jsvm,
		rizin:   rizinbin,
		cmds:    map[string]RizinCommand{},
	}
}

// LoadCommands fetches the list of rizin commands by spawning rizin.
// This MUST be called after the HTTP server is listening, because the
// spawned rizin process loads rz_notebook which health-checks us.
func (n *Notebook) LoadCommands() {
	cmds, err := RizinCommands(n.rizin)
	if err != nil {
		fmt.Printf("warning: failed to load rizin commands: %v\n", err)
		return
	}
	n.mutex.Lock()
	n.cmds = cmds
	n.mutex.Unlock()
}

func (n *Notebook) info() ([]string, error) {
	return RizinInfo(n.rizin)
}

// Returns existing pipe or opens a new one, extracting the binary to a temp file.
func (n *Notebook) open(unique string, open bool) *Rizin {
	if !IsValidNonce(unique, PageNonceSize) {
		return nil
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	if p, ok := n.pipes[unique]; ok {
		return p
	}

	if !open {
		return nil
	}

	page, err := store.GetPage(unique)
	if err != nil || page == nil {
		return nil
	}

	binaryPath, err := store.GetBinaryForPipe(unique, n.storage)
	if err != nil {
		fmt.Printf("warning: failed to extract binary for pipe: %v\n", err)
		return nil
	}
	prjPath := filepath.Join(n.storage, unique, "project.rzdb")

	rizin := NewRizin(n.rizin, binaryPath, prjPath)
	if rizin == nil {
		return nil
	}
	n.pipes[unique] = rizin
	return rizin
}

func (n *Notebook) closePipe(unique string) {
	if !IsValidNonce(unique, PageNonceSize) {
		return
	}
	n.mutex.Lock()
	p, ok := n.pipes[unique]
	if ok {
		delete(n.pipes, unique)
	}
	n.mutex.Unlock()

	if ok && p != nil {
		p.close()
	}
}

func (n *Notebook) closeAllPipes() {
	n.mutex.Lock()
	pipesToClose := make(map[string]*Rizin)
	for k, v := range n.pipes {
		pipesToClose[k] = v
	}
	n.pipes = map[string]*Rizin{}
	n.mutex.Unlock()

	for unique, p := range pipesToClose {
		fmt.Printf("Closing pipe for page %s...\n", unique)
		p.close()
	}
}
