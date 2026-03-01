/// \file notebook.go
/// \brief Core data model: page CRUD, element management, pipe lifecycle.

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"sync"

	"github.com/gin-gonic/gin"
)

// PageFile is the filename for page metadata JSON within each page directory.
const PageFile = "page.json"

/// \brief Reads and parses a JSON file into a gin.H map.
func readJSON(filepath string) (gin.H, error) {
	var data = gin.H{}
	bytes, err := os.ReadFile(filepath)
	if err != nil {
		return nil, fmt.Errorf("failed to read %s: %w", filepath, err)
	}
	if err := json.Unmarshal(bytes, &data); err != nil {
		return nil, fmt.Errorf("failed to parse %s: %w", filepath, err)
	}
	return data, nil
}

/// \brief Converts raw JSON-decoded map[string]interface{} entries to gin.H.
func sanitizeLines(lines []interface{}) []interface{} {
	for i, v := range lines {
		p, ok := v.(map[string]interface{})
		if !ok {
			continue
		}
		n := gin.H{}
		for k := range p {
			n[k] = p[k]
		}
		lines[i] = n
	}
	return lines
}

/// \brief Finds index of a line element matching key=value, or -1.
func findLineByKey(lines []interface{}, key, value string) int {
	for i, n := range lines {
		p, ok := n.(gin.H)
		if !ok {
			continue
		}
		if v, exists := p[key]; exists && value == v {
			return i
		}
	}
	return -1
}

/// \brief Returns index of target in slice, or -1.
func findInSlice(slice []string, target string) int {
	for i, n := range slice {
		if n == target {
			return i
		}
	}
	return -1
}

/// \brief Central data structure managing all pages, pipes, and JS engine.
type Notebook struct {
	mutex   sync.Mutex
	pages   gin.H
	uniques []string
	storage string
	pipes   map[string]*Rizin
	jsvm    *JavaScript
	rizin   string
	cmds    map[string]RizinCommand
}

/// \brief Loads existing pages from disk, initializes JS VM and command cache.
func NewNotebook(storage, rizinbin string) *Notebook {
	pages := gin.H{}
	uniques := []string{}

	cmds, err := RizinCommands(rizinbin)
	if err != nil {
		fmt.Printf("warning: failed to load rizin commands: %v\n", err)
		cmds = map[string]RizinCommand{}
	}

	files, err := filepath.Glob(path.Join(storage, "*", PageFile))
	if err != nil {
		panic(fmt.Sprintf("failed to scan storage directory: %v", err))
	}

	for _, file := range files {
		page, err := readJSON(file)
		if err != nil {
			fmt.Printf("warning: skipping corrupt page %s: %v\n", file, err)
			continue
		}

		// Derive the page ID from the parent directory name.
		unique := filepath.Base(filepath.Dir(file))
		if !IsValidNonce(unique, PageNonceSize) {
			fmt.Printf("warning: skipping page with invalid identifier %s\n", file)
			continue
		}

		if rawLines, ok := page["lines"].([]interface{}); ok {
			page["lines"] = sanitizeLines(rawLines)
		} else {
			page["lines"] = []interface{}{}
		}
		page["unique"] = unique
		pages[unique] = page
		uniques = append(uniques, unique)
	}

	jsvm := NewJavaScript()
	if jsvm == nil {
		panic("failed to create scripting engine")
	}

	sort.Strings(uniques)
	return &Notebook{
		pages:   pages,
		uniques: uniques,
		storage: storage,
		pipes:   map[string]*Rizin{},
		jsvm:    jsvm,
		rizin:   rizinbin,
		cmds:    cmds,
	}
}

/// \brief Returns summary list of all pages (title, unique, pipe status).
func (n *Notebook) list() []gin.H {
	n.mutex.Lock()
	defer n.mutex.Unlock()
	pages := make([]gin.H, len(n.uniques))
	for i, unique := range n.uniques {
		page := n.pages[unique].(gin.H)
		pipe := n.pipes[unique]

		pages[i] = gin.H{
			"title":  page["title"],
			"unique": unique,
			"pipe":   pipe != nil,
		}
	}
	return pages
}

/// \brief Returns `rizin -version` output.
func (n *Notebook) info() ([]string, error) {
	return RizinInfo(n.rizin)
}

/// \brief Returns page data by unique ID, or nil if not found.
func (n *Notebook) get(unique string) gin.H {
	if !IsValidNonce(unique, PageNonceSize) {
		return nil
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()
	if page, ok := n.pages[unique]; ok {
		return page.(gin.H)
	}
	return nil
}

/// \brief Creates a new markdown element; returns element nonce or "".
func (n *Notebook) newmd(unique string) string {
	if !IsValidNonce(unique, PageNonceSize) {
		return ""
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	data, ok := n.pages[unique]
	if !ok {
		return ""
	}
	page := data.(gin.H)

	// Generate a unique element nonce.
	eunique := Nonce(ElementNonceSize)
	for n.existsUnlocked(unique, eunique+".md") {
		eunique = Nonce(ElementNonceSize)
	}

	if !n.saveUnlocked([]byte{}, unique, eunique+".md") {
		return ""
	}

	page["lines"] = append(page["lines"].([]interface{}), gin.H{
		"type":   "markdown",
		"unique": eunique,
	})

	if !n.writePageJSON(unique, page) {
		return ""
	}
	return eunique
}

/// \brief Creates a new script element; returns element nonce or "".
func (n *Notebook) newscript(unique, script string) string {
	if !IsValidNonce(unique, PageNonceSize) {
		return ""
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	data, ok := n.pages[unique]
	if !ok {
		return ""
	}
	page := data.(gin.H)

	eunique := Nonce(ElementNonceSize)
	for n.existsUnlocked(unique, eunique+".out") {
		eunique = Nonce(ElementNonceSize)
	}

	page["lines"] = append(page["lines"].([]interface{}), gin.H{
		"type":   "script",
		"unique": eunique,
		"script": script,
	})

	if !n.writePageJSON(unique, page) {
		return ""
	}
	return eunique
}

/// \brief Creates a new command element; returns element nonce or "".
func (n *Notebook) newcmd(unique, command string) string {
	if !IsValidNonce(unique, PageNonceSize) {
		return ""
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	data, ok := n.pages[unique]
	if !ok {
		return ""
	}
	page := data.(gin.H)

	eunique := Nonce(ElementNonceSize)
	for n.existsUnlocked(unique, eunique+".out") {
		eunique = Nonce(ElementNonceSize)
	}

	page["lines"] = append(page["lines"].([]interface{}), gin.H{
		"type":    "command",
		"unique":  eunique,
		"command": command,
	})

	if !n.writePageJSON(unique, page) {
		return ""
	}
	return eunique
}

/// \brief Removes element data file and its entry from the page JSON.
func (n *Notebook) deleteElem(unique, eunique string, markdown bool) bool {
	if !IsValidNonce(unique, PageNonceSize) || !IsValidNonce(eunique, ElementNonceSize) {
		return false
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	data, ok := n.pages[unique]
	if !ok {
		return false
	}

	suffix := ".md"
	if !markdown {
		suffix = ".out"
	}
	page := data.(gin.H)

	// Remove the data file. Errors are non-fatal (file may already be gone).
	os.Remove(path.Join(n.storage, unique, eunique+suffix))

	lines := page["lines"].([]interface{})
	if idx := findLineByKey(lines, "unique", eunique); idx > -1 {
		page["lines"] = append(lines[:idx], lines[idx+1:]...)
	}

	return n.writePageJSON(unique, page)
}

/// \brief Creates a new page directory and metadata; returns page nonce or "".
func (n *Notebook) new(title, filename, binary string) string {
	if len(title) < 1 {
		return ""
	}

	n.mutex.Lock()
	defer n.mutex.Unlock()

	unique := Nonce(PageNonceSize)
	for {
		if _, ok := n.pages[unique]; !ok {
			break
		}
		unique = Nonce(PageNonceSize)
	}

	data := gin.H{
		"title":    title,
		"unique":   unique,
		"filename": filename,
		"binary":   binary,
		"lines":    []interface{}{},
	}

	if err := os.MkdirAll(path.Join(n.storage, unique), os.ModePerm); err != nil {
		fmt.Printf("error: failed to create page directory: %v\n", err)
		return ""
	}

	fp := path.Join(n.storage, unique, PageFile)
	bytes, _ := json.MarshalIndent(data, "", "\t")
	if err := os.WriteFile(fp, bytes, 0644); err != nil {
		fmt.Printf("error: failed to write page file: %v\n", err)
		return ""
	}

	n.pages[unique] = data
	n.uniques = append(n.uniques, unique)
	sort.Strings(n.uniques)
	return unique
}

/// \brief Renames a page title.
func (n *Notebook) rename(unique, title string) bool {
	if len(title) < 1 || !IsValidNonce(unique, PageNonceSize) {
		return false
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	data, ok := n.pages[unique]
	if !ok {
		return false
	}
	page := data.(gin.H)
	page["title"] = title
	return n.writePageJSON(unique, page)
}

/// \brief Closes pipe, removes page directory, and removes from in-memory index.
func (n *Notebook) delete(unique string) bool {
	if !IsValidNonce(unique, PageNonceSize) {
		return false
	}

	// Close the pipe first (outside the main lock to avoid deadlock,
	// since close() also acquires the notebook mutex via n.close()).
	n.closePipe(unique)

	n.mutex.Lock()
	defer n.mutex.Unlock()

	if _, ok := n.pages[unique]; !ok {
		return false
	}

	fp := path.Join(n.storage, unique)
	if err := os.RemoveAll(fp); err != nil {
		fmt.Printf("error: failed to remove page directory: %v\n", err)
		return false
	}

	delete(n.pages, unique)
	if idx := findInSlice(n.uniques, unique); idx >= 0 {
		n.uniques = append(n.uniques[:idx], n.uniques[idx+1:]...)
	}
	return true
}

/// \brief Returns existing pipe or creates one if open=true.
func (n *Notebook) open(unique string, open bool) *Rizin {
	if !IsValidNonce(unique, PageNonceSize) {
		return nil
	}
	n.mutex.Lock()
	defer n.mutex.Unlock()

	if p, ok := n.pipes[unique]; ok {
		return p
	}

	if p, ok := n.pages[unique]; ok && open {
		page := p.(gin.H)
		binaryPath := path.Join(n.storage, unique, page["binary"].(string))
		prjPath := path.Join(n.storage, unique, "project.rzdb")
		rizin := NewRizin(n.rizin, binaryPath, prjPath)
		if rizin == nil {
			return nil
		}
		n.pipes[unique] = rizin
		return rizin
	}
	return nil
}

/// \brief Closes the Rizin pipe for a page (saves project first).
func (n *Notebook) closePipe(unique string) {
	if !IsValidNonce(unique, PageNonceSize) {
		return
	}
	n.mutex.Lock()
	p, ok := n.pipes[unique]
	if ok {
		delete(n.pipes, unique)
	}
	n.mutex.Unlock()

	if ok && p != nil {
		p.close()
	}
}

/// \brief Closes all open pipes (called during shutdown).
func (n *Notebook) closeAllPipes() {
	n.mutex.Lock()
	pipesToClose := make(map[string]*Rizin)
	for k, v := range n.pipes {
		pipesToClose[k] = v
	}
	n.pipes = map[string]*Rizin{}
	n.mutex.Unlock()

	for unique, p := range pipesToClose {
		fmt.Printf("Closing pipe for page %s...\n", unique)
		p.close()
	}
}

/// \brief Checks if a file exists in page storage.
func (n *Notebook) exists(paths ...string) bool {
	args := append([]string{n.storage}, paths...)
	_, err := os.Stat(path.Join(args...))
	return !os.IsNotExist(err)
}

/// \brief Like exists() but without acquiring the mutex.
func (n *Notebook) existsUnlocked(paths ...string) bool {
	args := append([]string{n.storage}, paths...)
	_, err := os.Stat(path.Join(args...))
	return !os.IsNotExist(err)
}

/// \brief Reads a file from page storage.
func (n *Notebook) file(paths ...string) ([]byte, error) {
	args := append([]string{n.storage}, paths...)
	fp := path.Join(args...)
	return os.ReadFile(fp)
}

/// \brief Writes data to a file in page storage.
func (n *Notebook) save(data []byte, paths ...string) bool {
	args := append([]string{n.storage}, paths...)
	fp := path.Join(args...)
	if err := os.WriteFile(fp, data, 0644); err != nil {
		fmt.Printf("error: failed to write file %s: %v\n", fp, err)
		return false
	}
	return true
}

/// \brief Like save() but without acquiring the mutex.
func (n *Notebook) saveUnlocked(data []byte, paths ...string) bool {
	args := append([]string{n.storage}, paths...)
	fp := path.Join(args...)
	if err := os.WriteFile(fp, data, 0644); err != nil {
		fmt.Printf("error: failed to write file %s: %v\n", fp, err)
		return false
	}
	return true
}

/// \brief Writes page data to page.json and updates in-memory state. Caller must hold mutex.
func (n *Notebook) writePageJSON(unique string, page gin.H) bool {
	fp := path.Join(n.storage, unique, PageFile)
	bytes, err := json.MarshalIndent(page, "", "\t")
	if err != nil {
		fmt.Printf("error: failed to marshal page data: %v\n", err)
		return false
	}
	if err := os.WriteFile(fp, bytes, 0644); err != nil {
		fmt.Printf("error: failed to write page file: %v\n", err)
		return false
	}
	n.pages[unique] = page
	return true
}
