package main

import (
	"github.com/gin-gonic/gin"
)

func sanitizeWebRoot(path string) string {
	if len(path) < 2 {
		return "/"
	}
	if path[len(path)-1] != '/' {
		return path + "/"
	}
	return path
}

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

	// Index page: list all notebook pages
	root.GET("/", func(c *gin.Context) {
		pages, err := catalog.ListPages()
		if err != nil {
			c.HTML(500, "error.tmpl", gin.H{"root": webroot, "error": err.Error()})
			return
		}

		list := make([]gin.H, len(pages))
		for i, p := range pages {
			notebook.mutex.Lock()
			pipeOpen := notebook.pipes[p.ID] != nil
			notebook.mutex.Unlock()
			list[i] = gin.H{
				"title":  p.Title,
				"unique": p.ID,
				"pipe":   pipeOpen,
			}
		}

		c.HTML(200, "index.tmpl", gin.H{
			"root": webroot,
			"list": list,
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

	// Protobuf REST API.
	serverAddAPI(root)

	return router
}
