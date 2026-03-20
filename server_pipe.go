package main

import (
	"net/http"

	"github.com/gin-gonic/gin"
)

func serverAddPipe(pipe *gin.RouterGroup) {

	// Open a Rizin pipe for a page.
	pipe.GET("/open/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		page, err := catalog.GetPage(unique)
		if err != nil || page == nil {
			c.HTML(http.StatusNotFound, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "page not found",
			})
			return
		}

		if page.Binary == "" {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":     webroot,
				"error":    "This page has no binary attached yet. Attach one from the edit page before opening a Rizin pipe.",
				"location": webroot + "edit/" + unique,
			})
			return
		}

		if notebook.open(unique, true) == nil {
			c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
				"root":     webroot,
				"error":    "Failed to open Rizin pipe. Check that the binary file exists and rizin is installed.",
				"location": webroot + "view/" + unique,
			})
			return
		}
		c.Redirect(http.StatusFound, webroot+"view/"+unique)
	})

	// Close a Rizin pipe for a page.
	pipe.GET("/close/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		notebook.closePipe(unique)
		c.Redirect(http.StatusFound, webroot+"view/"+unique)
	})
}
