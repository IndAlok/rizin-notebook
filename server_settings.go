/// \file server_settings.go
/// \brief Settings handlers (environment variables).

package main

import (
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"
)

func serverAddSettings(root *gin.RouterGroup) {

	// View current settings.
	root.GET("/settings", func(c *gin.Context) {
		c.HTML(200, "settings.tmpl", gin.H{
			"root":        webroot,
			"environment": config.Environment,
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

		c.HTML(200, "settings-edit.tmpl", gin.H{
			"root":    webroot,
			"action":  "environment",
			"editkey": editkey,
			"data":    config.Environment,
		})
	})

	// Handle settings form submissions.
	root.POST("/settings", func(c *gin.Context) {
		action := strings.TrimSpace(c.PostForm("action"))

		if action == "environment" {
			handleEnvironmentSettings(c)
			return
		}

		c.Redirect(http.StatusFound, webroot+"settings")
	})
}

/// \brief Handles environment variable new/edit/delete form submissions.
func handleEnvironmentSettings(c *gin.Context) {
	subaction := strings.TrimSpace(c.PostForm("subaction"))
	editkey := strings.TrimSpace(c.PostForm("editkey"))
	key := strings.TrimSpace(c.PostForm("key"))
	value := strings.TrimSpace(c.PostForm("value"))

	switch subaction {
	case "new":
		if len(key) > 0 {
			config.SetEnvironment(key, value)
			config.Save()
		}
	case "edit":
		if len(editkey) > 0 && len(key) > 0 {
			// If the key name changed, remove the old one.
			if editkey != key {
				config.DelEnvironment(editkey)
			}
			config.SetEnvironment(key, value)
			config.Save()
		}
	case "delete":
		if len(editkey) > 0 {
			config.DelEnvironment(editkey)
			config.Save()
		}
	}

	c.Redirect(http.StatusFound, webroot+"settings")
}
