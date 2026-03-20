package main

import (
	"encoding/json"
	"html/template"
	"strconv"
	"strings"
)

const (
	notebookSearchSettingsPrefix = "ns:"
	defaultSearchMaxResults      = 200
	minSearchMaxResults          = 10
	maxSearchMaxResults          = 500
)

type notebookSearchConfig struct {
	MaxResults  int                          `json:"max_results"`
	DefaultMode NotebookSearchMode           `json:"default_mode"`
	Surface     NotebookSearchSurfaceFilter  `json:"default_surface"`
	CellType    NotebookSearchCellTypeFilter `json:"default_cell_type"`
}

var notebookSearchSettingOrder = []string{
	"max_results",
	"default_mode",
	"default_surface",
	"default_cell_type",
}

var notebookSearchSettingLabels = map[string]string{
	"max_results":       "Max Results",
	"default_mode":      "Default Mode",
	"default_surface":   "Default Surface",
	"default_cell_type": "Default Cell Type",
}

var notebookSearchSettingDefaults = map[string]string{
	"max_results":       strconv.Itoa(defaultSearchMaxResults),
	"default_mode":      string(NotebookSearchModeLiteral),
	"default_surface":   string(NotebookSearchSurfaceFilterAll),
	"default_cell_type": string(NotebookSearchCellTypeFilterAll),
}

var notebookSearchSettingKinds = map[string]string{
	"max_results":       "number",
	"default_mode":      "select",
	"default_surface":   "select",
	"default_cell_type": "select",
}

var notebookSearchSettingChoices = map[string][]map[string]string{
	"default_mode": {
		{"value": string(NotebookSearchModeLiteral), "label": "Literal"},
		{"value": string(NotebookSearchModeRegex), "label": "Regex"},
		{"value": string(NotebookSearchModeFuzzy), "label": "Fuzzy"},
	},
	"default_surface": {
		{"value": string(NotebookSearchSurfaceFilterAll), "label": "All Surfaces"},
		{"value": string(NotebookSearchSurfaceFilterContent), "label": "Input / Source"},
		{"value": string(NotebookSearchSurfaceFilterOutput), "label": "Outputs"},
	},
	"default_cell_type": {
		{"value": string(NotebookSearchCellTypeFilterAll), "label": "All Cell Types"},
		{"value": string(NotebookSearchCellTypeFilterCommand), "label": "Command Cells"},
		{"value": string(NotebookSearchCellTypeFilterScript), "label": "Script Cells"},
		{"value": string(NotebookSearchCellTypeFilterMarkdown), "label": "Markdown Cells"},
	},
}

func getNotebookSearchConfig() notebookSearchConfig {
	cfg := notebookSearchConfig{
		MaxResults:  defaultSearchMaxResults,
		DefaultMode: NotebookSearchModeLiteral,
		Surface:     NotebookSearchSurfaceFilterAll,
		CellType:    NotebookSearchCellTypeFilterAll,
	}

	if catalog == nil {
		return cfg
	}

	settings, _ := catalog.GetAllSettings()
	if settings == nil {
		return cfg
	}

	for key, value := range settings {
		if !strings.HasPrefix(key, notebookSearchSettingsPrefix) {
			continue
		}
		settingKey := strings.TrimPrefix(key, notebookSearchSettingsPrefix)
		switch settingKey {
		case "max_results":
			val, err := strconv.Atoi(value)
			if err == nil {
				cfg.MaxResults = clampInt(val, minSearchMaxResults, maxSearchMaxResults)
			}
		case "default_mode":
			mode, err := normalizeNotebookSearchMode(NotebookSearchMode(value))
			if err == nil {
				cfg.DefaultMode = mode
			}
		case "default_surface":
			surface, err := normalizeNotebookSearchSurfaceFilter(NotebookSearchSurfaceFilter(value))
			if err == nil {
				cfg.Surface = surface
			}
		case "default_cell_type":
			cellType, err := normalizeNotebookSearchCellTypeFilter(NotebookSearchCellTypeFilter(value))
			if err == nil {
				cfg.CellType = cellType
			}
		}
	}

	return cfg
}

func getNotebookSearchConfigJSON() template.JS {
	cfg := getNotebookSearchConfig()
	bytes, err := json.Marshal(cfg)
	if err != nil {
		return template.JS("{}")
	}
	return template.JS(bytes)
}

func notebookSearchActions() []map[string]string {
	cfg := getNotebookSearchConfig()
	actions := make([]map[string]string, 0, len(notebookSearchSettingOrder))
	for _, key := range notebookSearchSettingOrder {
		value := notebookSearchSettingDefaults[key]
		switch key {
		case "max_results":
			value = strconv.Itoa(cfg.MaxResults)
		case "default_mode":
			value = string(cfg.DefaultMode)
		case "default_surface":
			value = string(cfg.Surface)
		case "default_cell_type":
			value = string(cfg.CellType)
		}
		actions = append(actions, map[string]string{
			"name":            key,
			"label":           notebookSearchSettingLabels[key],
			"value":           value,
			"value_display":   notebookSearchSettingValueLabel(key, value),
			"default_value":   notebookSearchSettingDefaults[key],
			"default_display": notebookSearchSettingValueLabel(key, notebookSearchSettingDefaults[key]),
		})
	}
	return actions
}

func notebookSearchSettingValueLabel(key, value string) string {
	choices, ok := notebookSearchSettingChoices[key]
	if !ok {
		return value
	}
	for _, choice := range choices {
		if choice["value"] == value {
			return choice["label"]
		}
	}
	return value
}
