package main

import (
	"net/http"

	"github.com/gin-gonic/gin"
)

func serverAddOutput(output *gin.RouterGroup) {

	// View a rendered output cell.
	output.GET("/view/*path", func(c *gin.Context) {
		unique, eunique, ok := parseElementPath(c.Param("path"))
		if !ok {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid path",
			})
			return
		}

		cell, _ := store.GetCell(unique, eunique)
		var data []byte
		if cell != nil {
			data = cell.Output
		}

		// Convert ANSI escape sequences to HTML.
		htmlOutput := toHtml(string(data))

		c.HTML(200, "output.tmpl", gin.H{
			"root":   webroot,
			"output": string(htmlOutput),
		})
	})

	// Delete an output cell.
	output.GET("/delete/*path", func(c *gin.Context) {
		unique, eunique, ok := parseElementPath(c.Param("path"))
		if !ok {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid path",
			})
			return
		}

		store.DeleteCell(unique, eunique)

		c.Redirect(http.StatusFound, webroot+"output/deleted")
	})

	// Command line input form (rendered in an iframe).
	output.GET("/input/console/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		c.HTML(200, "console.tmpl", gin.H{
			"root":   webroot,
			"unique": unique,
		})
	})

	// Script editor input form (rendered in an iframe).
	output.GET("/input/script/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		c.HTML(200, "script.tmpl", gin.H{
			"root":   webroot,
			"unique": unique,
		})
	})

	// Execute a Rizin command.
	output.POST("/exec/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		command := c.PostForm("command")
		if command == "" {
			c.Redirect(http.StatusFound, webroot+"output/deleted")
			return
		}

		// Get or open the pipe.
		rz := notebook.open(unique, true)
		if rz == nil {
			c.HTML(http.StatusInternalServerError, "console-error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to open Rizin pipe",
			})
			return
		}

		// Execute the command.
		result, err := rz.exec(command)
		if err != nil {
			c.HTML(200, "console-error.tmpl", gin.H{
				"root":  webroot,
				"error": err.Error(),
			})
			return
		}

		// Create a command cell and save the output in SQLite.
		eunique := Nonce(ElementNonceSize)
		if storeErr := store.AddCell(unique, eunique, "command", command); storeErr != nil {
			c.HTML(http.StatusInternalServerError, "console-error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to create command element",
			})
			return
		}

		store.UpdateCellOutput(unique, eunique, []byte(result))

		// Redirect to "loaded" to trigger parent page reload.
		c.Redirect(http.StatusFound, webroot+"output/loaded")
	})

	// Execute a JavaScript script.
	output.POST("/script/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		script := c.PostForm("script")
		if script == "" {
			c.Redirect(http.StatusFound, webroot+"output/deleted")
			return
		}

		// Get or open the pipe.
		rz := notebook.open(unique, true)

		// Create a script cell in SQLite.
		eunique := Nonce(ElementNonceSize)
		if storeErr := store.AddCell(unique, eunique, "script", script); storeErr != nil {
			c.HTML(http.StatusInternalServerError, "console-error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to create script element",
			})
			return
		}

		// Execute the script. rz may be nil if no pipe is open,
		// which is handled by the JavaScript engine's rizin.cmd error.
		result, err := notebook.jsvm.exec(script, rz)

		if err != nil {
			store.UpdateCellOutput(unique, eunique, []byte(err.Error()))
			c.HTML(200, "console-error.tmpl", gin.H{
				"root":  webroot,
				"error": err.Error(),
			})
			return
		}

		store.UpdateCellOutput(unique, eunique, []byte(result))

		// Redirect to "loaded" to trigger parent page reload.
		c.Redirect(http.StatusFound, webroot+"output/loaded")
	})

	// Pseudo-page for iframe deletion detection.
	// When the iframe navigates here, the parent page's output() handler
	// removes the iframe's container div from the DOM.
	output.GET("/deleted", func(c *gin.Context) {
		c.String(200, "deleted")
	})

	// Pseudo-page for parent page reload trigger.
	// When the iframe navigates here, the parent page's output() handler
	// calls location.reload() to refresh the entire page.
	output.GET("/loaded", func(c *gin.Context) {
		c.HTML(200, "reload.tmpl", gin.H{
			"root": webroot,
		})
	})
}
