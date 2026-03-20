package main

import (
	"encoding/json"
	"html/template"
	"strconv"
	"strings"
)

// Autocomplete setting defaults and bounds.
const (
	autocompleteSettingsPrefix = "ac:"

	defaultACMaxResults = 12
	defaultACMinChars   = 2

	minACMaxResults = 1
	maxACMaxResults = 100
	minACMinChars   = 1
	maxACMinChars   = 10
)

// AutocompleteSettingKeys lists the configurable autocomplete settings.
var AutocompleteSettingKeys = []string{"max_results", "min_chars"}

// AutocompleteLabels maps setting keys to human-readable labels.
var AutocompleteLabels = map[string]string{
	"max_results": "Max Results",
	"min_chars":   "Minimum Characters",
}

// AutocompleteDefaults maps setting keys to their default string values.
var AutocompleteDefaults = map[string]string{
	"max_results": strconv.Itoa(defaultACMaxResults),
	"min_chars":   strconv.Itoa(defaultACMinChars),
}

// GetAutocompleteConfig returns the current autocomplete configuration,
// merging database overrides with defaults and clamping values to safe bounds.
func GetAutocompleteConfig() map[string]int {
	result := map[string]int{
		"max_results": defaultACMaxResults,
		"min_chars":   defaultACMinChars,
	}

	if catalog == nil {
		return result
	}

	settings, _ := catalog.GetAllSettings()
	if settings == nil {
		return result
	}

	for k, v := range settings {
		if !strings.HasPrefix(k, autocompleteSettingsPrefix) {
			continue
		}
		key := strings.TrimPrefix(k, autocompleteSettingsPrefix)
		val, err := strconv.Atoi(v)
		if err != nil {
			continue
		}
		switch key {
		case "max_results":
			result["max_results"] = clampInt(val, minACMaxResults, maxACMaxResults)
		case "min_chars":
			result["min_chars"] = clampInt(val, minACMinChars, maxACMinChars)
		}
	}

	return result
}

// getAutocompleteConfigJSON returns the autocomplete config as a JSON
// object suitable for embedding in a <script> tag.
func getAutocompleteConfigJSON() template.JS {
	cfg := GetAutocompleteConfig()
	bytes, err := json.Marshal(cfg)
	if err != nil {
		return template.JS("{}")
	}
	return template.JS(bytes)
}

// autocompleteActions returns ordered setting info for the settings page.
func autocompleteActions() []map[string]string {
	cfg := GetAutocompleteConfig()
	actions := make([]map[string]string, 0, len(AutocompleteSettingKeys))
	for _, key := range AutocompleteSettingKeys {
		actions = append(actions, map[string]string{
			"name":          key,
			"label":         AutocompleteLabels[key],
			"value":         strconv.Itoa(cfg[key]),
			"default_value": AutocompleteDefaults[key],
		})
	}
	return actions
}

func clampInt(val, lo, hi int) int {
	if val < lo {
		return lo
	}
	if val > hi {
		return hi
	}
	return val
}
