package main

import (
	"net/http"
	"os"
	"strings"

	"github.com/gin-gonic/gin"
)

func serverAddSettings(root *gin.RouterGroup) {

	// View current settings.
	root.GET("/settings", func(c *gin.Context) {
		settings, _ := catalog.GetAllSettings()
		if settings == nil {
			settings = map[string]string{}
		}
		// Filter out keybinding entries from the environment display.
		env := map[string]string{}
		for k, v := range settings {
			if !strings.HasPrefix(k, keybindingSettingsPrefix) {
				env[k] = v
			}
		}
		c.HTML(200, "settings.tmpl", gin.H{
			"root":        webroot,
			"environment": env,
			"kb_actions":  keybindingActions(),
		})
	})

	// Edit or create an environment variable form.
	root.GET("/settings/environment/edit/:key", func(c *gin.Context) {
		key := c.Param("key")

		// "new" is a special sentinel value meaning create a new variable.
		editkey := ""
		if key != "new" {
			editkey = key
		}

		settings, _ := catalog.GetAllSettings()
		if settings == nil {
			settings = map[string]string{}
		}

		c.HTML(200, "settings-edit.tmpl", gin.H{
			"root":    webroot,
			"action":  "environment",
			"editkey": editkey,
			"data":    settings,
		})
	})

	// Edit a keybinding.
	root.GET("/settings/keybinding/edit/:action", func(c *gin.Context) {
		action := c.Param("action")
		if _, ok := DefaultKeybindings[action]; !ok {
			c.Redirect(http.StatusFound, webroot+"settings")
			return
		}
		kb := GetAllKeybindings()
		c.HTML(200, "settings-edit.tmpl", gin.H{
			"root":          webroot,
			"action":        "keybinding",
			"editkey":       action,
			"label":         KeybindingLabels[action],
			"combo":         kb[action],
			"default_combo": DefaultKeybindings[action],
		})
	})

	// Reset all keybindings to defaults.
	root.GET("/settings/keybinding/reset", func(c *gin.Context) {
		for action := range DefaultKeybindings {
			catalog.DeleteSetting(keybindingSettingsPrefix + action)
		}
		c.Redirect(http.StatusFound, webroot+"settings")
	})

	// Handle settings form submissions.
	root.POST("/settings", func(c *gin.Context) {
		action := strings.TrimSpace(c.PostForm("action"))

		if action == "environment" {
			handleEnvironmentSettings(c)
			return
		}

		if action == "keybinding" {
			handleKeybindingSettings(c)
			return
		}

		c.Redirect(http.StatusFound, webroot+"settings")
	})
}

func handleEnvironmentSettings(c *gin.Context) {
	subaction := strings.TrimSpace(c.PostForm("subaction"))
	editkey := strings.TrimSpace(c.PostForm("editkey"))
	key := strings.TrimSpace(c.PostForm("key"))
	value := strings.TrimSpace(c.PostForm("value"))

	switch subaction {
	case "new":
		if len(key) > 0 {
			catalog.SetSetting(key, value)
			os.Setenv(key, value)
		}
	case "edit":
		if len(editkey) > 0 && len(key) > 0 {
			if editkey != key {
				catalog.DeleteSetting(editkey)
				os.Unsetenv(editkey)
			}
			catalog.SetSetting(key, value)
			os.Setenv(key, value)
		}
	case "delete":
		if len(editkey) > 0 {
			catalog.DeleteSetting(editkey)
			os.Unsetenv(editkey)
		}
	}

	c.Redirect(http.StatusFound, webroot+"settings")
}

func handleKeybindingSettings(c *gin.Context) {
	subaction := strings.TrimSpace(c.PostForm("subaction"))
	editkey := strings.TrimSpace(c.PostForm("editkey"))
	value := strings.TrimSpace(c.PostForm("value"))

	switch subaction {
	case "save":
		if len(editkey) > 0 && len(value) > 0 {
			catalog.SetSetting(keybindingSettingsPrefix+editkey, value)
		}
	case "reset":
		if len(editkey) > 0 {
			catalog.DeleteSetting(keybindingSettingsPrefix + editkey)
		}
	}

	c.Redirect(http.StatusFound, webroot+"settings")
}
