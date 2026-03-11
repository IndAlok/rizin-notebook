package main

import (
	"encoding/json"
	"html/template"
	"strings"
)

// DefaultKeybindings defines the built-in keyboard shortcut defaults.
// Keys are action names; values are combo strings.
var DefaultKeybindings = map[string]string{
	"about":         "alt+a",
	"new_page":      "ctrl+n",
	"settings":      "alt+s",
	"toggle_pipe":   "alt+o",
	"new_command":   "alt+c",
	"new_markdown":  "alt+m",
	"new_script":    "alt+j",
	"save":          "mod+s",
	"cancel":        "escape",
	"execute":       "ctrl+enter",
	"edit_markdown": "alt+e",
}

// KeybindingOrder defines the display order for the settings page.
var KeybindingOrder = []string{
	"about", "new_page", "settings", "toggle_pipe",
	"new_command", "new_markdown", "new_script",
	"save", "execute", "cancel", "edit_markdown",
}

var KeybindingLabels = map[string]string{
	"about":         "About",
	"new_page":      "New Page",
	"settings":      "Settings",
	"toggle_pipe":   "Open / Close Pipe",
	"new_command":   "Command Line",
	"new_markdown":  "New Markdown",
	"new_script":    "New Script",
	"save":          "Save",
	"cancel":        "Cancel / Go Back",
	"execute":       "Execute / Run",
	"edit_markdown": "Edit Markdown",
}

const keybindingSettingsPrefix = "kb:"

func GetAllKeybindings() map[string]string {
	result := make(map[string]string, len(DefaultKeybindings))
	for k, v := range DefaultKeybindings {
		result[k] = v
	}
	if catalog == nil {
		return result
	}
	settings, _ := catalog.GetAllSettings()
	if settings == nil {
		return result
	}
	for k, v := range settings {
		if strings.HasPrefix(k, keybindingSettingsPrefix) {
			action := strings.TrimPrefix(k, keybindingSettingsPrefix)
			if _, ok := DefaultKeybindings[action]; ok && len(v) > 0 {
				result[action] = v
			}
		}
	}
	return result
}

func getKeybindingsJSON() template.JS {
	kb := GetAllKeybindings()
	bytes, err := json.Marshal(kb)
	if err != nil {
		return template.JS("{}")
	}
	return template.JS(bytes)
}

func keybindingActions() []map[string]string {
	kb := GetAllKeybindings()
	actions := make([]map[string]string, 0, len(KeybindingOrder))
	for _, name := range KeybindingOrder {
		actions = append(actions, map[string]string{
			"name":         name,
			"label":        KeybindingLabels[name],
			"combo":        kb[name],
			"default_combo": DefaultKeybindings[name],
		})
	}
	return actions
}
