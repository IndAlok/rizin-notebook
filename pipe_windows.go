//go:build windows

package main

import (
	"os/exec"
	"syscall"
)

// configureSubprocess hides the console window for spawned rizin sub-processes
// so they don't flash a CMD window on the user's desktop.
func configureSubprocess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: 0x08000000, // CREATE_NO_WINDOW
	}
}
