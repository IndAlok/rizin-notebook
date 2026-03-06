package main

import (
	"encoding/base64"
	"io"
	"net/http"
	"os"
	"os/exec"

	"github.com/gin-gonic/gin"
	pb "github.com/rizinorg/rizin-notebook/pb"
	"google.golang.org/protobuf/proto"
)

// protobufContentType is the MIME type for protobuf-encoded responses.
const protobufContentType = "application/x-protobuf"

// ─── Helpers ────────────────────────────────────────────────

// respondProto writes a protobuf message as the response body.
func respondProto(c *gin.Context, code int, msg proto.Message) {
	data, err := proto.Marshal(msg)
	if err != nil {
		c.Data(http.StatusInternalServerError, protobufContentType,
			mustMarshalError(http.StatusInternalServerError, "failed to serialize response"))
		return
	}
	c.Data(code, protobufContentType, data)
}

// respondError writes a protobuf ErrorResponse.
func respondError(c *gin.Context, code int, message string) {
	c.Data(code, protobufContentType, mustMarshalError(code, message))
}

// mustMarshalError marshals an ErrorResponse (panics on marshal failure, which should never happen).
func mustMarshalError(code int, message string) []byte {
	data, _ := proto.Marshal(&pb.ErrorResponse{
		Code:    int32(code),
		Message: message,
	})
	return data
}

// readProto reads and unmarshals a protobuf request body.
func readProto(c *gin.Context, msg proto.Message) error {
	body, err := io.ReadAll(c.Request.Body)
	if err != nil {
		return err
	}
	return proto.Unmarshal(body, msg)
}

// cellRowToProto converts a database CellRow to a protobuf Cell.
func cellRowToProto(c *CellRow) *pb.Cell {
	var cellType pb.CellType
	switch c.Type {
	case "command":
		cellType = pb.CellType_CELL_TYPE_COMMAND
	case "script":
		cellType = pb.CellType_CELL_TYPE_SCRIPT
	case "markdown":
		cellType = pb.CellType_CELL_TYPE_MARKDOWN
	}
	return &pb.Cell{
		Id:       c.ID,
		Type:     cellType,
		Content:  c.Content,
		Output:   c.Output,
		Created:  c.Created,
		Executed: c.Executed,
	}
}

// pageRowToProto converts a database PageRow to a protobuf Page (without cells).
func pageRowToProto(p *PageRow, pipeOpen bool) *pb.Page {
	return &pb.Page{
		Id:       p.ID,
		Title:    p.Title,
		Filename: p.Filename,
		Binary:   p.Binary,
		Pipe:     pipeOpen,
		Created:  p.Created,
		Modified: p.Modified,
	}
}

// ─── Route Registration ────────────────────────────────────

// serverAddAPI registers all /api/v1/ routes on the given router group.
func serverAddAPI(root *gin.RouterGroup) {
	api := root.Group("/api/v1")

	// ─── Health Check ───────────────────────────────────
	api.GET("/ping", apiPing)

	// ─── Status ─────────────────────────────────────────
	api.GET("/status", apiStatus)

	// ─── Pages ──────────────────────────────────────────
	api.GET("/pages", apiListPages)
	api.POST("/pages", apiCreatePage)
	api.GET("/pages/:id", apiGetPage)
	api.PUT("/pages/:id", apiUpdatePage)
	api.DELETE("/pages/:id", apiDeletePage)

	// ─── Cells ──────────────────────────────────────────
	api.POST("/pages/:id/cells", apiAddCell)
	api.GET("/pages/:id/cells/:cellId", apiGetCellOutput)
	api.PUT("/pages/:id/cells/:cellId", apiUpdateCell)
	api.DELETE("/pages/:id/cells/:cellId", apiDeleteCell)

	// ─── Execution ──────────────────────────────────────
	api.POST("/pages/:id/exec", apiExecCommand)
	api.POST("/pages/:id/script", apiExecScript)

	// ─── Pipe Management ────────────────────────────────
	api.POST("/pages/:id/pipe/open", apiPipeOpen)
	api.POST("/pages/:id/pipe/close", apiPipeClose)

	// ─── Settings ───────────────────────────────────────
	api.GET("/settings", apiGetSettings)
	api.PUT("/settings", apiSetSetting)
	api.DELETE("/settings/:key", apiDeleteSetting)

	jsonAPI := api.Group("/json")
	jsonAPI.GET("/status", apiJSONStatus)
	jsonAPI.GET("/pages", apiJSONListPages)
	jsonAPI.GET("/pages/:id", apiJSONGetPage)
	jsonAPI.POST("/pages", apiJSONCreatePage)
	jsonAPI.DELETE("/pages/:id", apiJSONDeletePage)
	jsonAPI.POST("/pages/:id/cells", apiJSONAddCell)
	jsonAPI.POST("/pages/:id/exec", apiJSONExecCommand)
	jsonAPI.POST("/pages/:id/script", apiJSONExecScript)
	jsonAPI.POST("/pages/:id/pipe/open", apiJSONPipeOpen)
	jsonAPI.POST("/pages/:id/pipe/close", apiJSONPipeClose)
}

// ─── Health Check ───────────────────────────────────────────

// apiPing is a lightweight health-check endpoint that returns 200 OK
// with no body processing.  Used by the rz_notebook plugin to verify
// the server is alive without triggering heavy work like rizin version
// detection.
func apiPing(c *gin.Context) {
	c.String(http.StatusOK, "pong")
}

// ─── Status ─────────────────────────────────────────────────

func apiStatus(c *gin.Context) {
	rzversion := "unknown"
	if info, err := notebook.info(); err == nil && len(info) > 0 {
		rzversion = info[0]
	}

	rzpath := os.Getenv("RIZIN_PATH")
	if len(rzpath) < 1 {
		rzpath = notebook.rizin
	}
	if resolved, err := exec.LookPath(rzpath); err == nil {
		rzpath = resolved
	}

	// Count open pipes.
	notebook.mutex.Lock()
	openPipes := len(notebook.pipes)
	notebook.mutex.Unlock()

	respondProto(c, http.StatusOK, &pb.StatusResponse{
		Version:      NBVERSION,
		RizinVersion: rzversion,
		RizinPath:    rzpath,
		Storage:      notebook.storage,
		Pages:        int32(store.PageCount()),
		OpenPipes:    int32(openPipes),
	})
}

func cellRowToJSON(c *CellRow) gin.H {
	return gin.H{
		"id":       c.ID,
		"type":     c.Type,
		"content":  c.Content,
		"output":   string(c.Output),
		"created":  c.Created,
		"executed": c.Executed,
	}
}

func pageRowToJSON(p *PageRow, pipeOpen bool) gin.H {
	return gin.H{
		"id":       p.ID,
		"title":    p.Title,
		"filename": p.Filename,
		"binary":   p.Binary,
		"pipe":     pipeOpen,
		"created":  p.Created,
		"modified": p.Modified,
	}
}

func jsonStatusPayload() gin.H {
	rzversion := "unknown"
	if info, err := notebook.info(); err == nil && len(info) > 0 {
		rzversion = info[0]
	}

	rzpath := os.Getenv("RIZIN_PATH")
	if len(rzpath) < 1 {
		rzpath = notebook.rizin
	}
	if resolved, err := exec.LookPath(rzpath); err == nil {
		rzpath = resolved
	}

	notebook.mutex.Lock()
	openPipes := len(notebook.pipes)
	notebook.mutex.Unlock()

	return gin.H{
		"version":       NBVERSION,
		"rizin_version": rzversion,
		"rizin_path":    rzpath,
		"storage":       notebook.storage,
		"pages":         store.PageCount(),
		"open_pipes":    openPipes,
	}
}

func apiJSONStatus(c *gin.Context) {
	c.JSON(http.StatusOK, jsonStatusPayload())
}

func apiJSONListPages(c *gin.Context) {
	pages, err := store.ListPages()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	list := make([]gin.H, 0, len(pages))
	for i := range pages {
		p := &pages[i]
		notebook.mutex.Lock()
		pipeOpen := notebook.pipes[p.ID] != nil
		notebook.mutex.Unlock()

		item := pageRowToJSON(p, pipeOpen)
		cells, _ := store.ListCells(p.ID)
		item["cells_count"] = len(cells)
		list = append(list, item)
	}

	c.JSON(http.StatusOK, gin.H{"pages": list})
}

func apiJSONGetPage(c *gin.Context) {
	id := c.Param("id")
	if !IsValidNonce(id, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}

	page, err := store.GetPage(id)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if page == nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "page not found"})
		return
	}

	notebook.mutex.Lock()
	pipeOpen := notebook.pipes[id] != nil
	notebook.mutex.Unlock()

	resp := pageRowToJSON(page, pipeOpen)
	cells, err := store.ListCells(id)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	items := make([]gin.H, 0, len(cells))
	for i := range cells {
		items = append(items, cellRowToJSON(&cells[i]))
	}
	resp["cells"] = items
	c.JSON(http.StatusOK, gin.H{"page": resp})
}

func apiJSONCreatePage(c *gin.Context) {
	var req struct {
		Title        string `json:"title"`
		Filename     string `json:"filename"`
		BinaryBase64 string `json:"binary_base64"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	if req.Title == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "title is required"})
		return
	}

	var binary []byte
	if req.BinaryBase64 != "" {
		decoded, err := base64.StdEncoding.DecodeString(req.BinaryBase64)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": "invalid binary_base64: " + err.Error()})
			return
		}
		binary = decoded
	}

	filename := req.Filename
	if filename == "" && len(binary) > 0 {
		filename = req.Title
	}

	pageID := Nonce(PageNonceSize)
	binaryKey := ""
	if len(binary) > 0 {
		binaryKey = Nonce(ElementNonceSize)
	}

	if err := store.CreatePage(pageID, req.Title, filename, binaryKey); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to create page: " + err.Error()})
		return
	}
	if binaryKey != "" {
		if err := store.SaveBinary(pageID, binaryKey, binary); err != nil {
			store.DeletePage(pageID)
			c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to save binary: " + err.Error()})
			return
		}
	}

	page, _ := store.GetPage(pageID)
	c.JSON(http.StatusCreated, gin.H{"page": pageRowToJSON(page, false)})
}

func apiJSONDeletePage(c *gin.Context) {
	id := c.Param("id")
	if !IsValidNonce(id, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	notebook.closePipe(id)
	if err := store.DeletePage(id); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to delete page: " + err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"ok": true})
}

func apiJSONAddCell(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	var req struct {
		Type    string `json:"type"`
		Content string `json:"content"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	if req.Content == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "content is required"})
		return
	}
	if req.Type != "command" && req.Type != "script" && req.Type != "markdown" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid cell type"})
		return
	}
	cellID := Nonce(ElementNonceSize)
	if err := store.AddCell(pageID, cellID, req.Type, req.Content); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to create cell: " + err.Error()})
		return
	}
	cell, _ := store.GetCell(pageID, cellID)
	c.JSON(http.StatusCreated, gin.H{"cell": cellRowToJSON(cell)})
}

func apiJSONExecCommand(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	var req struct {
		Command string `json:"command"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	if req.Command == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "command is required"})
		return
	}
	rz := notebook.open(pageID, true)
	if rz == nil {
		c.JSON(http.StatusOK, gin.H{"success": false, "error": "failed to open Rizin pipe; check binary file and rizin installation"})
		return
	}
	result, err := rz.exec(req.Command)
	cellID := Nonce(ElementNonceSize)
	if storeErr := store.AddCell(pageID, cellID, "command", req.Command); storeErr != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to create cell: " + storeErr.Error()})
		return
	}
	output := []byte(result)
	if err != nil {
		output = []byte(err.Error())
	}
	if storeErr := store.UpdateCellOutput(pageID, cellID, output); storeErr != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to save output: " + storeErr.Error()})
		return
	}
	cell, _ := store.GetCell(pageID, cellID)
	c.JSON(http.StatusOK, gin.H{"success": err == nil, "error": errorString(err), "cell": cellRowToJSON(cell)})
}

func apiJSONExecScript(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	var req struct {
		Script string `json:"script"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request body: " + err.Error()})
		return
	}
	if req.Script == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "script is required"})
		return
	}
	rz := notebook.open(pageID, true)
	cellID := Nonce(ElementNonceSize)
	if storeErr := store.AddCell(pageID, cellID, "script", req.Script); storeErr != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to create cell: " + storeErr.Error()})
		return
	}
	result, err := notebook.jsvm.exec(req.Script, rz)
	output := []byte(result)
	if err != nil {
		output = []byte(err.Error())
	}
	if storeErr := store.UpdateCellOutput(pageID, cellID, output); storeErr != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to save output: " + storeErr.Error()})
		return
	}
	cell, _ := store.GetCell(pageID, cellID)
	c.JSON(http.StatusOK, gin.H{"success": err == nil, "error": errorString(err), "cell": cellRowToJSON(cell)})
}

func apiJSONPipeOpen(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	rz := notebook.open(pageID, true)
	if rz == nil {
		c.JSON(http.StatusOK, gin.H{"open": false, "error": "failed to open Rizin pipe; check binary file and rizin installation"})
		return
	}
	c.JSON(http.StatusOK, gin.H{"open": true})
}

func apiJSONPipeClose(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid page identifier"})
		return
	}
	notebook.closePipe(pageID)
	c.JSON(http.StatusOK, gin.H{"open": false})
}

func errorString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// ─── Pages ──────────────────────────────────────────────────

func apiListPages(c *gin.Context) {
	pages, err := store.ListPages()
	if err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}

	resp := &pb.ListPagesResponse{}
	for i := range pages {
		p := &pages[i]
		notebook.mutex.Lock()
		pipeOpen := notebook.pipes[p.ID] != nil
		notebook.mutex.Unlock()

		pbPage := pageRowToProto(p, pipeOpen)

		// Include cell count info but not full cell data for list.
		cells, _ := store.ListCells(p.ID)
		if cells != nil {
			pbPage.Cells = make([]*pb.Cell, len(cells))
			for j := range cells {
				pbPage.Cells[j] = cellRowToProto(&cells[j])
			}
		}

		resp.Pages = append(resp.Pages, pbPage)
	}

	respondProto(c, http.StatusOK, resp)
}

func apiGetPage(c *gin.Context) {
	id := c.Param("id")
	if !IsValidNonce(id, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	page, err := store.GetPage(id)
	if err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}
	if page == nil {
		respondError(c, http.StatusNotFound, "page not found")
		return
	}

	notebook.mutex.Lock()
	pipeOpen := notebook.pipes[id] != nil
	notebook.mutex.Unlock()

	pbPage := pageRowToProto(page, pipeOpen)

	cells, err := store.ListCells(id)
	if err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}
	for i := range cells {
		pbPage.Cells = append(pbPage.Cells, cellRowToProto(&cells[i]))
	}

	respondProto(c, http.StatusOK, &pb.GetPageResponse{Page: pbPage})
}

func apiCreatePage(c *gin.Context) {
	var req pb.CreatePageRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if req.Title == "" {
		respondError(c, http.StatusBadRequest, "title is required")
		return
	}
	filename := req.Filename
	if filename == "" && len(req.Binary) > 0 {
		filename = req.Title
	}

	pageID := Nonce(PageNonceSize)
	binaryKey := ""
	if len(req.Binary) > 0 {
		binaryKey = Nonce(ElementNonceSize)
	}

	if err := store.CreatePage(pageID, req.Title, filename, binaryKey); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to create page: "+err.Error())
		return
	}

	if binaryKey != "" {
		if err := store.SaveBinary(pageID, binaryKey, req.Binary); err != nil {
			store.DeletePage(pageID)
			respondError(c, http.StatusInternalServerError, "failed to save binary: "+err.Error())
			return
		}
	}

	page, _ := store.GetPage(pageID)
	respondProto(c, http.StatusCreated, &pb.CreatePageResponse{
		Page: pageRowToProto(page, false),
	})
}

func apiUpdatePage(c *gin.Context) {
	id := c.Param("id")
	if !IsValidNonce(id, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	var req pb.UpdatePageRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if req.Title == "" {
		respondError(c, http.StatusBadRequest, "title is required")
		return
	}

	if err := store.RenamePage(id, req.Title); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to update page: "+err.Error())
		return
	}

	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "page updated"})
}

func apiDeletePage(c *gin.Context) {
	id := c.Param("id")
	if !IsValidNonce(id, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	// Close the pipe first.
	notebook.closePipe(id)

	if err := store.DeletePage(id); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to delete page: "+err.Error())
		return
	}

	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "page deleted"})
}

// ─── Cells ──────────────────────────────────────────────────

func apiAddCell(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	var req pb.AddCellRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	page, err := store.GetPage(pageID)
	if err != nil || page == nil {
		respondError(c, http.StatusNotFound, "page not found")
		return
	}

	var cellType string
	switch req.Type {
	case pb.CellType_CELL_TYPE_COMMAND:
		cellType = "command"
	case pb.CellType_CELL_TYPE_SCRIPT:
		cellType = "script"
	case pb.CellType_CELL_TYPE_MARKDOWN:
		cellType = "markdown"
	default:
		respondError(c, http.StatusBadRequest, "invalid cell type")
		return
	}

	cellID := Nonce(ElementNonceSize)
	if err := store.AddCell(pageID, cellID, cellType, req.Content); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to add cell: "+err.Error())
		return
	}

	cell, _ := store.GetCell(pageID, cellID)
	respondProto(c, http.StatusCreated, &pb.AddCellResponse{
		Cell: cellRowToProto(cell),
	})
}

func apiGetCellOutput(c *gin.Context) {
	pageID := c.Param("id")
	cellID := c.Param("cellId")
	if !IsValidNonce(pageID, PageNonceSize) || !IsValidNonce(cellID, ElementNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid identifier")
		return
	}

	cell, err := store.GetCell(pageID, cellID)
	if err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}
	if cell == nil {
		respondError(c, http.StatusNotFound, "cell not found")
		return
	}

	htmlOutput := toHtml(string(cell.Output))

	respondProto(c, http.StatusOK, &pb.GetCellOutputResponse{
		Output:     cell.Output,
		Html:       string(htmlOutput),
		ExecutedAt: cell.Executed,
	})
}

func apiUpdateCell(c *gin.Context) {
	pageID := c.Param("id")
	cellID := c.Param("cellId")
	if !IsValidNonce(pageID, PageNonceSize) || !IsValidNonce(cellID, ElementNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid identifier")
		return
	}

	var req pb.UpdateCellRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if err := store.UpdateCellContent(pageID, cellID, req.Content); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to update cell: "+err.Error())
		return
	}

	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "cell updated"})
}

func apiDeleteCell(c *gin.Context) {
	pageID := c.Param("id")
	cellID := c.Param("cellId")
	if !IsValidNonce(pageID, PageNonceSize) || !IsValidNonce(cellID, ElementNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid identifier")
		return
	}

	if err := store.DeleteCell(pageID, cellID); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to delete cell: "+err.Error())
		return
	}

	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "cell deleted"})
}

// ─── Execution ──────────────────────────────────────────────

func apiExecCommand(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	var req pb.ExecCommandRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if req.Command == "" {
		respondError(c, http.StatusBadRequest, "command is required")
		return
	}

	// Ensure pipe is open.
	rz := notebook.open(pageID, true)
	if rz == nil {
		respondError(c, http.StatusServiceUnavailable, "failed to open Rizin pipe")
		return
	}

	// Execute the command.
	result, err := rz.exec(req.Command)
	if err != nil {
		respondProto(c, http.StatusOK, &pb.ExecCommandResponse{
			Success: false,
			Error:   err.Error(),
		})
		return
	}

	// Create cell and save output.
	cellID := Nonce(ElementNonceSize)
	if storeErr := store.AddCell(pageID, cellID, "command", req.Command); storeErr != nil {
		respondError(c, http.StatusInternalServerError, "failed to create cell: "+storeErr.Error())
		return
	}

	if storeErr := store.UpdateCellOutput(pageID, cellID, []byte(result)); storeErr != nil {
		respondError(c, http.StatusInternalServerError, "failed to save output: "+storeErr.Error())
		return
	}

	cell, _ := store.GetCell(pageID, cellID)
	respondProto(c, http.StatusOK, &pb.ExecCommandResponse{
		Cell:    cellRowToProto(cell),
		Success: true,
	})
}

func apiExecScript(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	var req pb.ExecScriptRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if req.Script == "" {
		respondError(c, http.StatusBadRequest, "script is required")
		return
	}

	// Get or open the pipe (rz may be nil; the JS engine handles that).
	rz := notebook.open(pageID, true)

	// Create the cell first.
	cellID := Nonce(ElementNonceSize)
	if storeErr := store.AddCell(pageID, cellID, "script", req.Script); storeErr != nil {
		respondError(c, http.StatusInternalServerError, "failed to create cell: "+storeErr.Error())
		return
	}

	result, err := notebook.jsvm.exec(req.Script, rz)

	var outputBytes []byte
	if err != nil {
		outputBytes = []byte(err.Error())
		store.UpdateCellOutput(pageID, cellID, outputBytes)
		cell, _ := store.GetCell(pageID, cellID)
		respondProto(c, http.StatusOK, &pb.ExecScriptResponse{
			Cell:    cellRowToProto(cell),
			Success: false,
			Error:   err.Error(),
		})
		return
	}

	outputBytes = []byte(result)
	store.UpdateCellOutput(pageID, cellID, outputBytes)
	cell, _ := store.GetCell(pageID, cellID)

	respondProto(c, http.StatusOK, &pb.ExecScriptResponse{
		Cell:    cellRowToProto(cell),
		Success: true,
	})
}

// ─── Pipe Management ────────────────────────────────────────

func apiPipeOpen(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	page, err := store.GetPage(pageID)
	if err != nil || page == nil {
		respondError(c, http.StatusNotFound, "page not found")
		return
	}

	rz := notebook.open(pageID, true)
	if rz == nil {
		respondProto(c, http.StatusOK, &pb.PipeResponse{
			Open:  false,
			Error: "failed to open Rizin pipe; check binary file and rizin installation",
		})
		return
	}

	respondProto(c, http.StatusOK, &pb.PipeResponse{Open: true})
}

func apiPipeClose(c *gin.Context) {
	pageID := c.Param("id")
	if !IsValidNonce(pageID, PageNonceSize) {
		respondError(c, http.StatusBadRequest, "invalid page identifier")
		return
	}

	notebook.closePipe(pageID)
	respondProto(c, http.StatusOK, &pb.PipeResponse{Open: false})
}

// ─── Settings ───────────────────────────────────────────────

func apiGetSettings(c *gin.Context) {
	settings, err := store.GetAllSettings()
	if err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}
	respondProto(c, http.StatusOK, &pb.SettingsResponse{Environment: settings})
}

func apiSetSetting(c *gin.Context) {
	var req pb.SetSettingRequest
	if err := readProto(c, &req); err != nil {
		respondError(c, http.StatusBadRequest, "invalid request body: "+err.Error())
		return
	}

	if req.Key == "" {
		respondError(c, http.StatusBadRequest, "key is required")
		return
	}

	if err := store.SetSetting(req.Key, req.Value); err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}

	// Also update the OS environment.
	os.Setenv(req.Key, req.Value)

	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "setting saved"})
}

func apiDeleteSetting(c *gin.Context) {
	key := c.Param("key")
	if key == "" {
		respondError(c, http.StatusBadRequest, "key is required")
		return
	}

	if err := store.DeleteSetting(key); err != nil {
		respondError(c, http.StatusInternalServerError, err.Error())
		return
	}

	os.Unsetenv(key)
	respondProto(c, http.StatusOK, &pb.SuccessResponse{Ok: true, Message: "setting deleted"})
}
