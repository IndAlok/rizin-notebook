/// \file config.go
/// \brief Persistent notebook configuration (environment variables) as JSON.

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path"
	"strings"
	"sync"
)

// ConfigFile is the filename for the notebook configuration JSON file.
const ConfigFile = "config.json"

/// \brief Persistent configuration stored as JSON on disk.
type NotebookConfig struct {
	Environment map[string]string `json:"environment"`
	filename    string
	mutex       sync.Mutex
}

/// \brief Loads config from disk, creating defaults if absent.
func NewNotebookConfig(folder string) *NotebookConfig {
	nc := &NotebookConfig{}
	nc.filename = path.Join(folder, ConfigFile)

	bytes, err := os.ReadFile(nc.filename)
	if err == nil {
		if jsonErr := json.Unmarshal(bytes, nc); jsonErr != nil {
			fmt.Printf("warning: failed to parse config file: %v\n", jsonErr)
		}
	}

	if nc.Environment == nil {
		nc.Environment = map[string]string{}
	}

	// Auto-populate RIZIN_PATH from the OS environment if not configured.
	if value, ok := nc.Environment["RIZIN_PATH"]; !ok || len(value) < 1 {
		nc.Environment["RIZIN_PATH"] = os.Getenv("RIZIN_PATH")
	}

	return nc
}

/// \brief Applies all configured env vars to os.Setenv.
func (nc *NotebookConfig) UpdateEnvironment() {
	nc.mutex.Lock()
	defer nc.mutex.Unlock()
	for key, value := range nc.Environment {
		os.Setenv(key, value)
	}
}

/// \brief Removes an env var from config and OS.
func (nc *NotebookConfig) DelEnvironment(key string) {
	nc.mutex.Lock()
	defer nc.mutex.Unlock()
	key = strings.TrimSpace(key)
	delete(nc.Environment, key)
	os.Unsetenv(key)
}

/// \brief Sets an env var in both config and OS.
func (nc *NotebookConfig) SetEnvironment(key, value string) {
	nc.mutex.Lock()
	defer nc.mutex.Unlock()
	value = strings.TrimSpace(value)
	key = strings.TrimSpace(key)
	os.Setenv(key, value)
	nc.Environment[key] = value
}

/// \brief Persists config to disk as formatted JSON.
func (nc *NotebookConfig) Save() {
	nc.mutex.Lock()
	defer nc.mutex.Unlock()
	bytes, err := json.MarshalIndent(nc, "", "\t")
	if err != nil {
		fmt.Printf("error: failed to marshal config: %v\n", err)
		return
	}
	if err := os.WriteFile(nc.filename, bytes, 0644); err != nil {
		fmt.Printf("error: failed to save config: %v\n", err)
	}
}
