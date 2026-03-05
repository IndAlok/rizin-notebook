package main

import (
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
}

// ─── Status ─────────────────────────────────────────────────

func apiStatus(c *gin.Context) {
	var rzversion string
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
	if req.Filename == "" {
		respondError(c, http.StatusBadRequest, "filename is required")
		return
	}
	if len(req.Binary) == 0 {
		respondError(c, http.StatusBadRequest, "binary data is required")
		return
	}

	pageID := Nonce(PageNonceSize)
	binaryKey := Nonce(ElementNonceSize)

	if err := store.CreatePage(pageID, req.Title, req.Filename, binaryKey); err != nil {
		respondError(c, http.StatusInternalServerError, "failed to create page: "+err.Error())
		return
	}

	if err := store.SaveBinary(pageID, binaryKey, req.Binary); err != nil {
		store.DeletePage(pageID)
		respondError(c, http.StatusInternalServerError, "failed to save binary: "+err.Error())
		return
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


