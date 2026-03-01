/// \file server.go
/// \brief HTTP router setup and route group registration.

package main

import (
	"github.com/gin-gonic/gin"
)

/// \brief Ensures webroot ends with a trailing slash.
func sanitizeWebRoot(path string) string {
	if len(path) < 2 {
		return "/"
	}
	if path[len(path)-1] != '/' {
		return path + "/"
	}
	return path
}

/// \brief Creates and configures the Gin router with all route groups.
func setupRouter(assets, bind string, debug bool) *gin.Engine {
	gin.DisableConsoleColor()
	if debug {
		gin.SetMode(gin.DebugMode)
	} else {
		gin.SetMode(gin.ReleaseMode)
	}

	router := gin.Default()

	static, templates := setupTemplate(assets, router)

	root := router.Group(sanitizeWebRoot(webroot))

	serverAddAssets(root, assets, static, templates)

	// Index page: list all notebook pages.
	root.GET("/", func(c *gin.Context) {
		c.HTML(200, "index.tmpl", gin.H{
			"root": webroot,
			"list": notebook.list(),
		})
	})

	serverAddAbout(root)
	serverAddSettings(root)
	serverAddPage(root)

	pipe := root.Group("/pipe")
	serverAddPipe(pipe)

	markdown := root.Group("/markdown")
	serverAddMarkdown(markdown)

	output := root.Group("/output")
	serverAddOutput(output)

	return router
}
