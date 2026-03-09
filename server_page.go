package main

import (
	"errors"
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

		page, err := catalog.GetPage(unique)
		if err != nil || page == nil {
			c.HTML(http.StatusNotFound, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "page not found",
			})
			return
		}

		c.HTML(200, "page-new.tmpl", gin.H{
			"root":   webroot,
			"page":   gin.H{"title": page.Title, "filename": page.Filename},
			"unique": unique,
			"title":  page.Title,
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

		var uploadName string
		var uploadData []byte
		file, header, err := c.Request.FormFile("binary")
		if err == nil {
			defer file.Close()
			data, readErr := io.ReadAll(file)
			if readErr != nil {
				c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
					"root":  webroot,
					"error": "Failed to read uploaded file",
				})
				return
			}
			uploadName = header.Filename
			uploadData = data
		} else if !errors.Is(err, http.ErrMissingFile) {
			c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to read uploaded file",
			})
			return
		}

		if len(unique) > 0 {
			// Editing an existing page: rename and optionally replace the binary.
			if !IsValidNonce(unique, PageNonceSize) {
				c.HTML(http.StatusBadRequest, "error.tmpl", gin.H{
					"root":  webroot,
					"error": "invalid page identifier",
				})
				return
			}
			if err := catalog.RenamePage(unique, title); err != nil {
				c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
					"root":  webroot,
					"error": "Failed to update page",
				})
				return
			}
			if len(uploadData) > 0 {
				if err := catalog.AttachBinary(unique, uploadName, uploadData); err != nil {
					c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
						"root":  webroot,
						"error": "Failed to save binary file",
					})
					return
				}
			}
			c.Redirect(http.StatusFound, webroot+"view/"+unique)
			return
		}

		// Creating a new page: uploaded binary is optional.
		pageID, createErr := catalog.CreatePage(title, uploadName, uploadData)
		if createErr != nil {
			c.HTML(http.StatusInternalServerError, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "Failed to create page",
			})
			return
		}

		c.Redirect(http.StatusFound, webroot+"view/"+pageID)
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

		page, err := catalog.GetPage(unique)
		if err != nil || page == nil {
			c.HTML(http.StatusNotFound, "error.tmpl", gin.H{
				"root":  webroot,
				"error": "page not found",
			})
			return
		}

		// Check if a Rizin pipe is open for this page.
		pipe := notebook.open(unique, false) != nil

		// Load cells from SQLite.
		cells, _ := catalog.ListCells(unique)
		lines := make([]interface{}, len(cells))
		for i, cell := range cells {
			lines[i] = gin.H{
				"type":    cell.Type,
				"unique":  cell.ID,
				"command": cell.Content,
				"script":  cell.Content,
			}
		}

		pageData := gin.H{
			"title":      page.Title,
			"unique":     page.ID,
			"filename":   page.Filename,
			"binary":     page.Binary,
			"binaryHash": page.BinaryHash,
			"hasBinary":  page.Binary != "",
			"lines":      lines,
		}

		c.HTML(200, "page-view.tmpl", gin.H{
			"root": webroot,
			"page": pageData,
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

		notebook.closePipe(unique)
		catalog.DeletePage(unique)
		c.Redirect(http.StatusFound, webroot)
	})
}
