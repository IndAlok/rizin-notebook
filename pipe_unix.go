//go:build !windows

package main

import "os/exec"

// configureSubprocess is a no-op on non-Windows platforms.
func configureSubprocess(cmd *exec.Cmd) {}
