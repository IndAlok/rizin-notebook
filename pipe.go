package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strings"
	"sync"
	"time"
)

// pipeReadTimeout is the maximum time to wait for a response from Rizin.
const pipeReadTimeout = 5 * time.Minute

type RizinCommandArg struct {
	Type       string   `json:"type"`
	Name       string   `json:"name"`
	DefaultArg string   `json:"default,omitempty"`
	Required   bool     `json:"required,omitempty"`
	IsOption   bool     `json:"is_option,omitempty"`
	IsArray    bool     `json:"is_array,omitempty"`
	Choices    []string `json:"choices,omitempty"`
}

type RizinCommandDetailEntry struct {
	Text    string `json:"text,omitempty"`
	Comment string `json:"comment,omitempty"`
	Arg     string `json:"arg_str,omitempty"`
}

type RizinCommandDetail struct {
	Name    string                    `json:"name"`
	Entries []RizinCommandDetailEntry `json:"entries,omitempty"`
}

type RizinCommand struct {
	Command     string               `json:"cmd"`
	ArgsStr     string               `json:"args_str"`
	Args        []RizinCommandArg    `json:"args,omitempty"`
	Description string               `json:"description,omitempty"`
	Summary     string               `json:"summary,omitempty"`
	Details     []RizinCommandDetail `json:"details,omitempty"`
}

type Rizin struct {
	pipe    *exec.Cmd
	stdin   io.WriteCloser
	reader  *bufio.Reader
	mutex   sync.Mutex
	project string
	closed  bool
}

func RizinInfo(rizinbin string) ([]string, error) {
	flags := [][]string{{"-version"}, {"--version"}, {"-v"}}
	var lastErr error

	for _, args := range flags {
		out, err := exec.Command(rizinbin, args...).CombinedOutput()
		if err != nil {
			lastErr = err
			continue
		}

		text := strings.TrimSpace(string(out))
		if text == "" {
			continue
		}

		lines := []string{}
		for _, line := range strings.Split(text, "\n") {
			line = strings.TrimSpace(line)
			if line != "" {
				lines = append(lines, line)
			}
		}
		if len(lines) > 0 {
			return lines, nil
		}
	}

	if lastErr == nil {
		lastErr = errors.New("no version output")
	}
	return nil, lastErr
}

func RizinCommands(rizinbin string) (map[string]RizinCommand, error) {
	out, err := exec.Command(rizinbin, "-qc", "?*j").Output()
	if err != nil {
		return nil, err
	}

	var commands map[string]RizinCommand
	err = json.Unmarshal(out, &commands)
	return commands, err
}

// Launches Rizin with -2 -0 -e scr.color=3 -p <project> and waits for initial null-byte.
func NewRizin(rizinbin, file, project string) *Rizin {
	args := []string{
		"-2",
		"-0",
		"-e", "scr.color=3",
		"-p", project,
		file,
	}
	pipe := exec.Command(rizinbin, args...)

	stdin, err := pipe.StdinPipe()
	if err != nil {
		fmt.Println("pipe error:", err)
		return nil
	}

	stdout, err := pipe.StdoutPipe()
	if err != nil {
		fmt.Println("pipe error:", err)
		return nil
	}

	if err := pipe.Start(); err != nil {
		fmt.Println("pipe error:", err)
		return nil
	}

	rizin := &Rizin{
		pipe:    pipe,
		stdin:   stdin,
		reader:  bufio.NewReader(stdout),
		project: project,
		closed:  false,
	}

	// Wait for the initial null-byte prompt indicating Rizin is ready.
	// This is done in a goroutine with the mutex held to prevent commands
	// from being sent before Rizin has finished initialization.
	go func(r *Rizin) {
		r.mutex.Lock()
		defer r.mutex.Unlock()
		if _, err := r.reader.ReadString('\x00'); err != nil {
			fmt.Println("pipe error: initial prompt:", err)
		}
	}(rizin)

	return rizin
}

func (r *Rizin) exec(cmd string) (string, error) {
	r.mutex.Lock()
	defer r.mutex.Unlock()

	if r.closed {
		return "", fmt.Errorf("pipe is closed")
	}

	if _, err := fmt.Fprintln(r.stdin, cmd); err != nil {
		fmt.Println("pipe error:", err)
		return "", err
	}

	// Use a context with timeout to prevent indefinite blocking.
	ctx, cancel := context.WithTimeout(context.Background(), pipeReadTimeout)
	defer cancel()

	type readResult struct {
		data string
		err  error
	}
	ch := make(chan readResult, 1)

	go func() {
		buf, err := r.reader.ReadString('\x00')
		if err != nil && err != io.EOF {
			ch <- readResult{"", err}
			return
		}
		buf = string(bytes.Trim([]byte(buf), "\x00"))
		ch <- readResult{buf, nil}
	}()

	select {
	case result := <-ch:
		if result.err != nil {
			fmt.Println("pipe error:", result.err)
		}
		return result.data, result.err
	case <-ctx.Done():
		return "", fmt.Errorf("pipe read timed out after %v", pipeReadTimeout)
	}
}

func (r *Rizin) close() {
	if r.closed {
		return
	}
	// Save the project before quitting. Errors are logged but not fatal.
	if _, err := r.exec("Ps " + r.project); err != nil {
		fmt.Println("pipe warning: failed to save project:", err)
	}

	// Send quit command. Use direct stdin write since exec() checks closed flag.
	r.mutex.Lock()
	r.closed = true
	fmt.Fprintln(r.stdin, "q!")
	r.mutex.Unlock()

	// Wait for the process to exit (with a timeout).
	done := make(chan error, 1)
	go func() {
		done <- r.pipe.Wait()
	}()

	select {
	case <-done:
		// Process exited cleanly.
	case <-time.After(10 * time.Second):
		// Force kill if it doesn't exit in time.
		fmt.Println("pipe warning: force killing unresponsive rizin process")
		r.pipe.Process.Kill()
	}
}
