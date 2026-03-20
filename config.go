package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path"
	"sync"
)

// ConfigFile is the filename for the notebook configuration JSON file.
const ConfigFile = "config.json"

type NotebookConfig struct {
	Environment map[string]string `json:"environment"`
	filename    string
	mutex       sync.Mutex
}

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

func (nc *NotebookConfig) UpdateEnvironment() {
	nc.mutex.Lock()
	defer nc.mutex.Unlock()
	for key, value := range nc.Environment {
		os.Setenv(key, value)
	}
}
