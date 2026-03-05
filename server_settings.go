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
		settings, _ := store.GetAllSettings()
		if settings == nil {
			settings = map[string]string{}
		}
		c.HTML(200, "settings.tmpl", gin.H{
			"root":        webroot,
			"environment": settings,
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

		settings, _ := store.GetAllSettings()
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

func handleEnvironmentSettings(c *gin.Context) {
	subaction := strings.TrimSpace(c.PostForm("subaction"))
	editkey := strings.TrimSpace(c.PostForm("editkey"))
	key := strings.TrimSpace(c.PostForm("key"))
	value := strings.TrimSpace(c.PostForm("value"))

	switch subaction {
	case "new":
		if len(key) > 0 {
			store.SetSetting(key, value)
			os.Setenv(key, value)
		}
	case "edit":
		if len(editkey) > 0 && len(key) > 0 {
			if editkey != key {
				store.DeleteSetting(editkey)
				os.Unsetenv(editkey)
			}
			store.SetSetting(key, value)
			os.Setenv(key, value)
		}
	case "delete":
		if len(editkey) > 0 {
			store.DeleteSetting(editkey)
			os.Unsetenv(editkey)
		}
	}

	c.Redirect(http.StatusFound, webroot+"settings")
}
