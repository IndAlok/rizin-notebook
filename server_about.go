/// \file server_about.go
/// \brief About page handler — displays app and Rizin version info.

package main

import (
	"os"
	"os/exec"

	"github.com/gin-gonic/gin"
)

/// \brief Registers GET /about with version info from `rizin -version`.
func serverAddAbout(root *gin.RouterGroup) {
	root.GET("/about", func(c *gin.Context) {
		var rzversion, rzbuild string
		if info, err := notebook.info(); err == nil && len(info) > 0 {
			rzversion = info[0]
			if len(info) > 1 {
				rzbuild = info[1]
			}
		}

		rzpath := os.Getenv("RIZIN_PATH")
		if len(rzpath) < 1 {
			rzpath = notebook.rizin
		}
		if resolved, err := exec.LookPath(rzpath); err == nil {
			rzpath = resolved
		}

		c.HTML(200, "about.tmpl", gin.H{
			"root":      webroot,
			"nbversion": NBVERSION,
			"rzversion": rzversion,
			"rzbuild":   rzbuild,
			"rzpath":    rzpath,
			"storage":   notebook.storage,
		})
	})
}
