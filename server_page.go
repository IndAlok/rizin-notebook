/// \file server_page.go
/// \brief Page CRUD handlers (create, edit, view, delete with file upload).

package main

import (
	"io"
	"net/http"

	"github.com/gin-gonic/gin"
)

func serverAddPage(root *gin.RouterGroup) {

	// Show new page form (empty unique).
	root.GET("/new", func(c *gin.Context) {
		c.HTML(200, "page-new.tmpl", gin.H{
			"root":   webroot,
			"page":   gin.H{},
			"unique": "",
			"title":  "",
		})
	})

	// Show edit page form (existing page).
	root.GET("/edit/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		page := notebook.get(unique)
		if page == nil {
			c.HTML(http.StatusNotFound, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "page not found",
			})
			return
		}

		title, _ := page["title"].(string)

		c.HTML(200, "page-new.tmpl", gin.H{
			"root":   webroot,
			"page":   page,
			"unique": unique,
			"title":  title,
		})
	})

	// Create or update a page (handles file upload for new pages).
	root.POST("/edit", func(c *gin.Context) {
		unique := c.PostForm("unique")
		title := c.PostForm("title")

		if len(title) < 1 {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Title is required",
			})
			return
		}

		if len(unique) > 0 {
			// Editing an existing page: just rename.
			if !IsValidNonce(unique, PageNonceSize) {
				c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
					"root":  webroot,
					"error": "invalid page identifier",
				})
				return
			}
			notebook.rename(unique, title)
			c.Redirect(http.StatusFound, webroot+"view/"+unique)
			return
		}

		// Creating a new page: handle file upload.
		file, header, err := c.Request.FormFile("binary")
		if err != nil {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "A binary file is required for a new page",
			})
			return
		}
		defer file.Close()

		// Read the uploaded binary into memory.
		data, err := io.ReadAll(file)
		if err != nil {
			c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to read uploaded file",
			})
			return
		}

		// Generate a nonce-based filename for the binary.
		binaryNonce := Nonce(ElementNonceSize)

		// Create the page in the notebook.
		unique = notebook.new(title, header.Filename, binaryNonce)
		if unique == "" {
			c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to create page",
			})
			return
		}

		// Save the binary file to the page directory.
		if !notebook.save(data, unique, binaryNonce) {
			c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to save binary file",
			})
			return
		}

		c.Redirect(http.StatusFound, webroot+"view/"+unique)
	})

	// View a page with all its cells.
	root.GET("/view/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		page := notebook.get(unique)
		if page == nil {
			c.HTML(http.StatusNotFound, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "page not found",
			})
			return
		}

		// Check if a Rizin pipe is open for this page.
		pipe := notebook.open(unique, false) != nil

		c.HTML(200, "page-view.tmpl", gin.H{
			"root": webroot,
			"page": page,
			"pipe": pipe,
			"cmds": notebook.cmds,
		})
	})

	// Delete a page.
	root.GET("/delete/:unique", func(c *gin.Context) {
		unique := c.Param("unique")
		if !IsValidNonce(unique, PageNonceSize) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "invalid page identifier",
			})
			return
		}

		notebook.delete(unique)
		c.Redirect(http.StatusFound, webroot)
	})
}
