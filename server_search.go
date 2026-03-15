package main

import (
	"fmt"
	"net/http"
	"strconv"
	"strings"

	"github.com/gin-gonic/gin"
)

func apiJSONSearchPage(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}

	page, err := catalog.GetPage(pageID)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if page == nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "page not found"})
		return
	}

	query := strings.TrimSpace(c.Query("query"))
	if query == "" {
		query = strings.TrimSpace(c.Query("q"))
	}

	limit, err := parseNotebookSearchLimit(c.Query("limit"))
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	cells, err := catalog.ListCells(pageID)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	response, err := SearchNotebook(cells, NotebookSearchOptions{
		Query:          query,
		Mode:           NotebookSearchMode(strings.TrimSpace(c.DefaultQuery("mode", string(NotebookSearchModeLiteral)))),
		CaseSensitive:  parseNotebookSearchBool(c.Query("case_sensitive")),
		Limit:          limit,
		SurfaceFilter:  NotebookSearchSurfaceFilter(strings.TrimSpace(c.DefaultQuery("surface", string(NotebookSearchSurfaceFilterAll)))),
		CellTypeFilter: NotebookSearchCellTypeFilter(strings.TrimSpace(c.DefaultQuery("cell_type", string(NotebookSearchCellTypeFilterAll)))),
	})
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	c.JSON(http.StatusOK, response)
}

func parseNotebookSearchLimit(raw string) (int, error) {
	if strings.TrimSpace(raw) == "" {
		return getNotebookSearchConfig().MaxResults, nil
	}
	limit, err := strconv.Atoi(raw)
	if err != nil {
		return 0, fmt.Errorf("invalid limit: %w", err)
	}
	if limit <= 0 {
		return getNotebookSearchConfig().MaxResults, nil
	}
	return limit, nil
}

func parseNotebookSearchBool(raw string) bool {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "1", "true", "yes", "on":
		return true
	default:
		return false
	}
}
