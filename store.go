package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// ─── Constants ──────────────────────────────────────────────

// RZNBExtension is the file extension for per-page notebook databases.
const RZNBExtension = ".rznb"

// SettingsDBFile is the server-wide settings database filename.
const SettingsDBFile = "settings.db"

// LegacyDatabaseFile is the old monolithic database filename for migration.
const LegacyDatabaseFile = "notebook.db"

// PageFile is the per-page JSON metadata filename (used by MigrateFromJSON).
const PageFile = "page.json"

// ─── PageRow ────────────────────────────────────────────────

// PageRow represents a page record from a .rznb database.
type PageRow struct {
	ID         string
	Title      string
	Filename   string
	Binary     string
	BinaryHash string
	Created    int64
	Modified   int64
}

// CellRow represents a cell record from a .rznb database.
type CellRow struct {
	ID       string
	PageID   string
	Type     string
	Content  string
	Output   []byte
	Position int
	Created  int64
	Executed int64
}

// ─── PageDB ────────────────────────────────────────────────

// pageSchemaSQL is the schema applied inside each .rznb file.
const pageSchemaSQL = `
CREATE TABLE IF NOT EXISTS meta (
	id          TEXT PRIMARY KEY,
	title       TEXT NOT NULL,
	filename    TEXT NOT NULL DEFAULT '',
	binary_key  TEXT NOT NULL DEFAULT '',
	binary_hash TEXT NOT NULL DEFAULT '',
	created     INTEGER NOT NULL DEFAULT 0,
	modified    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS cells (
	id       TEXT PRIMARY KEY,
	type     TEXT NOT NULL CHECK(type IN ('command', 'script', 'markdown')),
	content  TEXT NOT NULL DEFAULT '',
	output   BLOB,
	position INTEGER NOT NULL DEFAULT 0,
	created  INTEGER NOT NULL DEFAULT 0,
	executed INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS binary_data (
	name TEXT PRIMARY KEY,
	data BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS config (
	key   TEXT PRIMARY KEY,
	value TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_cells_pos ON cells(position);
`

// PageDB wraps a single .rznb SQLite database for one page.
type PageDB struct {
	db    *sql.DB
	path  string
	mutex sync.RWMutex
}

// OpenPageDB opens or creates a .rznb database file.
func OpenPageDB(dbPath string) (*PageDB, error) {
	db, err := sql.Open("sqlite", dbPath+"?_pragma=journal_mode(wal)&_pragma=foreign_keys(on)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, fmt.Errorf("failed to open page db: %w", err)
	}
	if _, err := db.Exec(pageSchemaSQL); err != nil {
		db.Close()
		return nil, fmt.Errorf("failed to apply page schema: %w", err)
	}
	return &PageDB{db: db, path: dbPath}, nil
}

// Close closes the database connection.
func (p *PageDB) Close() error {
	return p.db.Close()
}

// Path returns the filesystem path to this .rznb file.
func (p *PageDB) Path() string {
	return p.path
}

// ─── Meta ───────────────────────────────────────────────────

// GetMeta returns the page metadata row.
func (p *PageDB) GetMeta() (*PageRow, error) {
	p.mutex.RLock()
	defer p.mutex.RUnlock()

	var r PageRow
	err := p.db.QueryRow("SELECT id, title, filename, binary_key, binary_hash, created, modified FROM meta LIMIT 1").
		Scan(&r.ID, &r.Title, &r.Filename, &r.Binary, &r.BinaryHash, &r.Created, &r.Modified)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &r, nil
}

// InitMeta creates the single meta row for a new page.
func (p *PageDB) InitMeta(id, title, filename, binaryKey, binaryHash string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	now := time.Now().Unix()
	_, err := p.db.Exec(
		"INSERT OR REPLACE INTO meta (id, title, filename, binary_key, binary_hash, created, modified) VALUES (?, ?, ?, ?, ?, ?, ?)",
		id, title, filename, binaryKey, binaryHash, now, now,
	)
	return err
}

// UpdateTitle changes the page title.
func (p *PageDB) UpdateTitle(title string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	now := time.Now().Unix()
	_, err := p.db.Exec("UPDATE meta SET title = ?, modified = ?", title, now)
	return err
}

// TouchModified updates the modified timestamp.
func (p *PageDB) TouchModified() error {
	now := time.Now().Unix()
	_, err := p.db.Exec("UPDATE meta SET modified = ?", now)
	return err
}

// ─── Cells ──────────────────────────────────────────────────

// ListCells returns all cells ordered by position.
func (p *PageDB) ListCells() ([]CellRow, error) {
	p.mutex.RLock()
	defer p.mutex.RUnlock()

	meta, err := p.getMetaUnlocked()
	if err != nil {
		return nil, err
	}
	pageID := ""
	if meta != nil {
		pageID = meta.ID
	}

	rows, err := p.db.Query(
		"SELECT id, type, content, output, position, created, executed FROM cells ORDER BY position ASC",
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cells []CellRow
	for rows.Next() {
		var c CellRow
		c.PageID = pageID
		if err := rows.Scan(&c.ID, &c.Type, &c.Content, &c.Output, &c.Position, &c.Created, &c.Executed); err != nil {
			return nil, err
		}
		cells = append(cells, c)
	}
	return cells, rows.Err()
}

// GetCell returns a single cell by ID.
func (p *PageDB) GetCell(cellID string) (*CellRow, error) {
	p.mutex.RLock()
	defer p.mutex.RUnlock()

	meta, _ := p.getMetaUnlocked()
	pageID := ""
	if meta != nil {
		pageID = meta.ID
	}

	var c CellRow
	c.PageID = pageID
	err := p.db.QueryRow(
		"SELECT id, type, content, output, position, created, executed FROM cells WHERE id = ?",
		cellID,
	).Scan(&c.ID, &c.Type, &c.Content, &c.Output, &c.Position, &c.Created, &c.Executed)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &c, nil
}

// NextCellPosition returns the next available position index.
func (p *PageDB) NextCellPosition() (int, error) {
	var pos sql.NullInt64
	err := p.db.QueryRow("SELECT MAX(position) FROM cells").Scan(&pos)
	if err != nil {
		return 0, err
	}
	if !pos.Valid {
		return 0, nil
	}
	return int(pos.Int64) + 1, nil
}

// AddCell inserts a new cell at the end.
func (p *PageDB) AddCell(cellID, cellType, content string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	pos, err := p.NextCellPosition()
	if err != nil {
		return err
	}

	now := time.Now().Unix()
	_, err = p.db.Exec(
		"INSERT INTO cells (id, type, content, position, created, executed) VALUES (?, ?, ?, ?, ?, 0)",
		cellID, cellType, content, pos, now,
	)
	if err != nil {
		return err
	}

	return p.touchUnlocked()
}

// UpdateCellContent updates the content of an existing cell.
func (p *PageDB) UpdateCellContent(cellID, content string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	result, err := p.db.Exec("UPDATE cells SET content = ? WHERE id = ?", content, cellID)
	if err != nil {
		return err
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("cell not found: %s", cellID)
	}
	return p.touchUnlocked()
}

// UpdateCellOutput sets the output bytes and execution timestamp.
func (p *PageDB) UpdateCellOutput(cellID string, output []byte) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	now := time.Now().Unix()
	result, err := p.db.Exec("UPDATE cells SET output = ?, executed = ? WHERE id = ?",
		output, now, cellID)
	if err != nil {
		return err
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("cell not found: %s", cellID)
	}
	return p.touchUnlocked()
}

// DeleteCell removes a cell.
func (p *PageDB) DeleteCell(cellID string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	_, err := p.db.Exec("DELETE FROM cells WHERE id = ?", cellID)
	if err != nil {
		return err
	}
	return p.touchUnlocked()
}

// touchUnlocked updates modified timestamp (caller holds lock).
func (p *PageDB) touchUnlocked() error {
	now := time.Now().Unix()
	_, err := p.db.Exec("UPDATE meta SET modified = ?", now)
	return err
}

// getMetaUnlocked reads meta without acquiring a read lock (caller must hold one).
func (p *PageDB) getMetaUnlocked() (*PageRow, error) {
	var r PageRow
	err := p.db.QueryRow("SELECT id, title, filename, binary_key, binary_hash, created, modified FROM meta LIMIT 1").
		Scan(&r.ID, &r.Title, &r.Filename, &r.Binary, &r.BinaryHash, &r.Created, &r.Modified)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &r, nil
}

// ─── Binary Storage ─────────────────────────────────────────

// SaveBinary stores a binary blob and updates the meta.
func (p *PageDB) SaveBinary(name string, data []byte) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	_, err := p.db.Exec("INSERT OR REPLACE INTO binary_data (name, data) VALUES (?, ?)", name, data)
	return err
}

// GetBinary retrieves a binary blob by name.
func (p *PageDB) GetBinary(name string) ([]byte, error) {
	p.mutex.RLock()
	defer p.mutex.RUnlock()

	var data []byte
	err := p.db.QueryRow("SELECT data FROM binary_data WHERE name = ?", name).Scan(&data)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return data, err
}

// AttachBinary stores or replaces the binary for this page and updates hash.
func (p *PageDB) AttachBinary(filename string, data []byte) error {
	if len(filename) < 1 {
		return fmt.Errorf("filename is required")
	}
	if len(data) == 0 {
		return fmt.Errorf("binary data is required")
	}

	p.mutex.Lock()
	defer p.mutex.Unlock()

	// Read old binary key to clean up.
	var oldKey string
	p.db.QueryRow("SELECT binary_key FROM meta LIMIT 1").Scan(&oldKey)

	binaryKey := Nonce(ElementNonceSize)
	hash := sha256.Sum256(data)
	hashHex := hex.EncodeToString(hash[:])

	if _, err := p.db.Exec("INSERT OR REPLACE INTO binary_data (name, data) VALUES (?, ?)", binaryKey, data); err != nil {
		return err
	}

	now := time.Now().Unix()
	if _, err := p.db.Exec(
		"UPDATE meta SET filename = ?, binary_key = ?, binary_hash = ?, modified = ?",
		filename, binaryKey, hashHex, now,
	); err != nil {
		return err
	}

	if oldKey != "" && oldKey != binaryKey {
		p.db.Exec("DELETE FROM binary_data WHERE name = ?", oldKey)
	}

	return nil
}

// GetBinaryForPipe extracts the binary to a temp file and returns the path.
func (p *PageDB) GetBinaryForPipe(tempDir string) (string, error) {
	meta, err := p.GetMeta()
	if err != nil || meta == nil {
		return "", fmt.Errorf("page metadata not found")
	}
	if meta.Binary == "" {
		return "", fmt.Errorf("page has no binary attached")
	}

	data, err := p.GetBinary(meta.Binary)
	if err != nil || data == nil {
		return "", fmt.Errorf("binary data not found")
	}

	binPath := filepath.Join(tempDir, meta.ID, meta.Binary)
	if err := os.MkdirAll(filepath.Dir(binPath), 0755); err != nil {
		return "", err
	}
	if err := os.WriteFile(binPath, data, 0644); err != nil {
		return "", err
	}
	return binPath, nil
}

// ─── Config ─────────────────────────────────────────────────

// GetConfig returns a page-level config value.
func (p *PageDB) GetConfig(key string) (string, error) {
	p.mutex.RLock()
	defer p.mutex.RUnlock()

	var v string
	err := p.db.QueryRow("SELECT value FROM config WHERE key = ?", key).Scan(&v)
	if err == sql.ErrNoRows {
		return "", nil
	}
	return v, err
}

// SetConfig upserts a page-level config value.
func (p *PageDB) SetConfig(key, value string) error {
	p.mutex.Lock()
	defer p.mutex.Unlock()

	_, err := p.db.Exec("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)", key, value)
	return err
}

// ─── Catalog ────────────────────────────────────────────────

// Catalog manages the collection of .rznb page files and server-wide settings.
type Catalog struct {
	pagesDir   string
	settingsDB *sql.DB
	openPages  map[string]*PageDB // pageID → open PageDB
	mutex      sync.RWMutex
}

const settingsSchemaSQL = `
CREATE TABLE IF NOT EXISTS settings (
	key   TEXT PRIMARY KEY,
	value TEXT NOT NULL DEFAULT ''
);
`

// NewCatalog creates a new catalog that manages .rznb files in pagesDir.
func NewCatalog(dataDir string) (*Catalog, error) {
	pagesDir := filepath.Join(dataDir, "pages")
	if err := os.MkdirAll(pagesDir, 0755); err != nil {
		return nil, fmt.Errorf("cannot create pages directory: %w", err)
	}

	settingsPath := filepath.Join(dataDir, SettingsDBFile)
	sdb, err := sql.Open("sqlite", settingsPath+"?_pragma=journal_mode(wal)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, fmt.Errorf("failed to open settings db: %w", err)
	}
	if _, err := sdb.Exec(settingsSchemaSQL); err != nil {
		sdb.Close()
		return nil, fmt.Errorf("failed to apply settings schema: %w", err)
	}

	return &Catalog{
		pagesDir:   pagesDir,
		settingsDB: sdb,
		openPages:  make(map[string]*PageDB),
	}, nil
}

// Close closes all open page databases and the settings database.
func (c *Catalog) Close() error {
	c.mutex.Lock()
	defer c.mutex.Unlock()

	for id, pdb := range c.openPages {
		pdb.Close()
		delete(c.openPages, id)
	}
	return c.settingsDB.Close()
}

// PagesDir returns the directory containing .rznb files.
func (c *Catalog) PagesDir() string {
	return c.pagesDir
}

// ─── Page Lifecycle ─────────────────────────────────────────

// pageFilePath returns the .rznb file path for a page ID.
func (c *Catalog) pageFilePath(pageID string) string {
	return filepath.Join(c.pagesDir, pageID+RZNBExtension)
}

// ListPages scans the pages directory and returns metadata for all pages.
func (c *Catalog) ListPages() ([]PageRow, error) {
	c.mutex.RLock()
	defer c.mutex.RUnlock()

	entries, err := os.ReadDir(c.pagesDir)
	if err != nil {
		return nil, err
	}

	var pages []PageRow
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), RZNBExtension) {
			continue
		}

		pageID := strings.TrimSuffix(entry.Name(), RZNBExtension)
		pdb, err := c.openPageUnlocked(pageID)
		if err != nil {
			fmt.Printf("warning: cannot open %s: %v\n", entry.Name(), err)
			continue
		}

		meta, err := pdb.GetMeta()
		if err != nil || meta == nil {
			continue
		}
		pages = append(pages, *meta)
	}

	sort.Slice(pages, func(i, j int) bool {
		return pages[i].Created < pages[j].Created
	})

	return pages, nil
}

// GetPage returns the metadata for a single page, or nil if not found.
func (c *Catalog) GetPage(pageID string) (*PageRow, error) {
	c.mutex.RLock()
	defer c.mutex.RUnlock()

	pdb, err := c.openPageUnlocked(pageID)
	if err != nil {
		return nil, nil
	}
	return pdb.GetMeta()
}

// OpenPage returns the PageDB for a page, opening it if needed.
func (c *Catalog) OpenPage(pageID string) (*PageDB, error) {
	c.mutex.Lock()
	defer c.mutex.Unlock()

	return c.openPageUnlocked(pageID)
}

// openPageUnlocked opens a PageDB if it isn't already open.
func (c *Catalog) openPageUnlocked(pageID string) (*PageDB, error) {
	if pdb, ok := c.openPages[pageID]; ok {
		return pdb, nil
	}

	dbPath := c.pageFilePath(pageID)
	if _, err := os.Stat(dbPath); os.IsNotExist(err) {
		return nil, fmt.Errorf("page not found: %s", pageID)
	}

	pdb, err := OpenPageDB(dbPath)
	if err != nil {
		return nil, err
	}
	c.openPages[pageID] = pdb
	return pdb, nil
}

// CreatePage creates a new .rznb file with the given metadata and optional binary.
func (c *Catalog) CreatePage(title, filename string, binaryData []byte) (string, error) {
	pageID := Nonce(PageNonceSize)

	c.mutex.Lock()
	defer c.mutex.Unlock()

	dbPath := c.pageFilePath(pageID)
	pdb, err := OpenPageDB(dbPath)
	if err != nil {
		return "", err
	}

	binaryKey := ""
	binaryHash := ""
	if len(binaryData) > 0 {
		binaryKey = Nonce(ElementNonceSize)
		hash := sha256.Sum256(binaryData)
		binaryHash = hex.EncodeToString(hash[:])
		if err := pdb.SaveBinary(binaryKey, binaryData); err != nil {
			pdb.Close()
			os.Remove(dbPath)
			return "", err
		}
	}

	if err := pdb.InitMeta(pageID, title, filename, binaryKey, binaryHash); err != nil {
		pdb.Close()
		os.Remove(dbPath)
		return "", err
	}

	c.openPages[pageID] = pdb
	return pageID, nil
}

// DeletePage closes and removes a .rznb file.
func (c *Catalog) DeletePage(pageID string) error {
	c.mutex.Lock()
	defer c.mutex.Unlock()

	if pdb, ok := c.openPages[pageID]; ok {
		pdb.Close()
		delete(c.openPages, pageID)
	}

	dbPath := c.pageFilePath(pageID)
	return os.Remove(dbPath)
}

// RenamePage updates the title of a page.
func (c *Catalog) RenamePage(pageID, title string) error {
	c.mutex.Lock()
	defer c.mutex.Unlock()

	pdb, err := c.openPageUnlocked(pageID)
	if err != nil {
		return err
	}
	return pdb.UpdateTitle(title)
}

// PageCount returns the number of .rznb files in the pages directory.
func (c *Catalog) PageCount() int {
	entries, err := os.ReadDir(c.pagesDir)
	if err != nil {
		return 0
	}
	count := 0
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), RZNBExtension) {
			count++
		}
	}
	return count
}

// ─── Cell Pass-through ──────────────────────────────────────

// ListCells returns all cells for a page.
func (c *Catalog) ListCells(pageID string) ([]CellRow, error) {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return nil, err
	}
	return pdb.ListCells()
}

// GetCell returns a single cell.
func (c *Catalog) GetCell(pageID, cellID string) (*CellRow, error) {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return nil, err
	}
	return pdb.GetCell(cellID)
}

// AddCell adds a cell to a page.
func (c *Catalog) AddCell(pageID, cellID, cellType, content string) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.AddCell(cellID, cellType, content)
}

// UpdateCellContent updates a cell's content.
func (c *Catalog) UpdateCellContent(pageID, cellID, content string) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.UpdateCellContent(cellID, content)
}

// UpdateCellOutput sets cell output.
func (c *Catalog) UpdateCellOutput(pageID, cellID string, output []byte) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.UpdateCellOutput(cellID, output)
}

// DeleteCell removes a cell.
func (c *Catalog) DeleteCell(pageID, cellID string) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.DeleteCell(cellID)
}

// ─── Binary Pass-through ────────────────────────────────────

// AttachBinary stores or replaces the binary for a page.
func (c *Catalog) AttachBinary(pageID, filename string, data []byte) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.AttachBinary(filename, data)
}

// SaveBinary stores a raw binary blob in a page.
func (c *Catalog) SaveBinary(pageID, name string, data []byte) error {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return err
	}
	return pdb.SaveBinary(name, data)
}

// GetBinary retrieves a binary blob from a page.
func (c *Catalog) GetBinary(pageID, name string) ([]byte, error) {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return nil, err
	}
	return pdb.GetBinary(name)
}

// GetBinaryForPipe extracts a binary to disk for pipe opening.
func (c *Catalog) GetBinaryForPipe(pageID, tempDir string) (string, error) {
	pdb, err := c.OpenPage(pageID)
	if err != nil {
		return "", err
	}
	return pdb.GetBinaryForPipe(tempDir)
}

// ─── Import / Export ────────────────────────────────────────

// ExportPage returns the raw bytes of a .rznb file for download.
func (c *Catalog) ExportPage(pageID string) ([]byte, string, error) {
	// Ensure the page DB is flushed.
	c.mutex.Lock()
	if pdb, ok := c.openPages[pageID]; ok {
		// Checkpoint WAL so the main file is self-contained.
		pdb.db.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	}
	c.mutex.Unlock()

	dbPath := c.pageFilePath(pageID)
	data, err := os.ReadFile(dbPath)
	if err != nil {
		return nil, "", fmt.Errorf("page not found or unreadable: %w", err)
	}

	// Determine a nice filename.
	meta, _ := c.GetPage(pageID)
	filename := pageID + RZNBExtension
	if meta != nil && meta.Title != "" {
		safe := strings.Map(func(r rune) rune {
			if r == '/' || r == '\\' || r == ':' || r == '*' || r == '?' || r == '"' || r == '<' || r == '>' || r == '|' {
				return '-'
			}
			return r
		}, meta.Title)
		filename = safe + RZNBExtension
	}

	return data, filename, nil
}

// ImportPage imports a .rznb file, assigning it a new page ID.
func (c *Catalog) ImportPage(data []byte) (string, error) {
	newPageID := Nonce(PageNonceSize)

	c.mutex.Lock()
	defer c.mutex.Unlock()

	dbPath := c.pageFilePath(newPageID)

	// Write the uploaded data as a .rznb file.
	if err := os.WriteFile(dbPath, data, 0644); err != nil {
		return "", fmt.Errorf("failed to write imported page: %w", err)
	}

	// Open it and update the internal page ID.
	pdb, err := OpenPageDB(dbPath)
	if err != nil {
		os.Remove(dbPath)
		return "", fmt.Errorf("imported file is not a valid .rznb database: %w", err)
	}

	// Update the meta row to use the new page ID.
	_, err = pdb.db.Exec("UPDATE meta SET id = ?", newPageID)
	if err != nil {
		pdb.Close()
		os.Remove(dbPath)
		return "", fmt.Errorf("failed to update imported page ID: %w", err)
	}

	c.openPages[newPageID] = pdb
	return newPageID, nil
}

// ─── Settings ───────────────────────────────────────────────

// GetAllSettings returns all server-wide settings.
func (c *Catalog) GetAllSettings() (map[string]string, error) {
	rows, err := c.settingsDB.Query("SELECT key, value FROM settings")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	settings := make(map[string]string)
	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			return nil, err
		}
		settings[k] = v
	}
	return settings, rows.Err()
}

// GetSetting returns a single server-wide setting.
func (c *Catalog) GetSetting(key string) (string, error) {
	var v string
	err := c.settingsDB.QueryRow("SELECT value FROM settings WHERE key = ?", key).Scan(&v)
	if err == sql.ErrNoRows {
		return "", nil
	}
	return v, err
}

// SetSetting upserts a server-wide setting.
func (c *Catalog) SetSetting(key, value string) error {
	_, err := c.settingsDB.Exec("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)", key, value)
	return err
}

// DeleteSetting removes a server-wide setting.
func (c *Catalog) DeleteSetting(key string) error {
	_, err := c.settingsDB.Exec("DELETE FROM settings WHERE key = ?", key)
	return err
}

// ─── Legacy Migration ───────────────────────────────────────

// MigrateFromLegacyDB migrates the old monolithic notebook.db into per-page .rznb files.
func (c *Catalog) MigrateFromLegacyDB(dataDir string) (int, error) {
	legacyPath := filepath.Join(dataDir, LegacyDatabaseFile)
	if _, err := os.Stat(legacyPath); os.IsNotExist(err) {
		return 0, nil // No legacy DB
	}

	// Skip if we already have pages.
	if c.PageCount() > 0 {
		return 0, nil
	}

	legacyDB, err := sql.Open("sqlite", legacyPath+"?_pragma=journal_mode(wal)&_pragma=foreign_keys(on)&_pragma=busy_timeout(5000)")
	if err != nil {
		return 0, fmt.Errorf("failed to open legacy db: %w", err)
	}
	defer legacyDB.Close()

	// Read all pages from legacy DB.
	rows, err := legacyDB.Query("SELECT id, title, filename, binary, created, modified FROM pages ORDER BY created ASC")
	if err != nil {
		return 0, err
	}
	defer rows.Close()

	migrated := 0
	for rows.Next() {
		var p PageRow
		if err := rows.Scan(&p.ID, &p.Title, &p.Filename, &p.Binary, &p.Created, &p.Modified); err != nil {
			fmt.Printf("warning: migration scan error: %v\n", err)
			continue
		}

		if err := c.migrateOneLegacyPage(legacyDB, &p); err != nil {
			fmt.Printf("warning: failed to migrate page %s: %v\n", p.ID, err)
			continue
		}
		migrated++
	}

	// Migrate settings.
	c.migrateLegacySettings(legacyDB)

	// Rename legacy DB so we don't re-migrate.
	os.Rename(legacyPath, legacyPath+".migrated")

	return migrated, nil
}

func (c *Catalog) migrateOneLegacyPage(legacyDB *sql.DB, p *PageRow) error {
	dbPath := c.pageFilePath(p.ID)
	pdb, err := OpenPageDB(dbPath)
	if err != nil {
		return err
	}

	// Read binary data from legacy DB.
	var binaryData []byte
	if p.Binary != "" {
		legacyDB.QueryRow("SELECT data FROM binaries WHERE page_id = ? AND name = ?", p.ID, p.Binary).Scan(&binaryData)
	}

	binaryHash := ""
	if len(binaryData) > 0 {
		hash := sha256.Sum256(binaryData)
		binaryHash = hex.EncodeToString(hash[:])
		pdb.SaveBinary(p.Binary, binaryData)
	}

	pdb.InitMeta(p.ID, p.Title, p.Filename, p.Binary, binaryHash)

	// Update timestamps to match original.
	pdb.db.Exec("UPDATE meta SET created = ?, modified = ?", p.Created, p.Modified)

	// Migrate cells.
	cellRows, err := legacyDB.Query(
		"SELECT id, type, content, output, position, created, executed FROM cells WHERE page_id = ? ORDER BY position ASC",
		p.ID,
	)
	if err == nil {
		defer cellRows.Close()
		for cellRows.Next() {
			var c CellRow
			if err := cellRows.Scan(&c.ID, &c.Type, &c.Content, &c.Output, &c.Position, &c.Created, &c.Executed); err != nil {
				continue
			}
			pdb.db.Exec(
				"INSERT OR IGNORE INTO cells (id, type, content, output, position, created, executed) VALUES (?, ?, ?, ?, ?, ?, ?)",
				c.ID, c.Type, c.Content, c.Output, c.Position, c.Created, c.Executed,
			)
		}
	}

	c.openPages[p.ID] = pdb
	return nil
}

func (c *Catalog) migrateLegacySettings(legacyDB *sql.DB) {
	rows, err := legacyDB.Query("SELECT key, value FROM settings")
	if err != nil {
		return
	}
	defer rows.Close()

	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			continue
		}
		c.SetSetting(k, v)
	}
}

// MigrateFromJSON imports old JSON-file-based pages into .rznb files.
func (c *Catalog) MigrateFromJSON(dataDir string) (int, error) {
	files, err := filepath.Glob(path.Join(dataDir, "*", PageFile))
	if err != nil {
		return 0, err
	}
	if len(files) == 0 {
		return 0, nil
	}

	// Skip if we already have pages.
	if c.PageCount() > 0 {
		return 0, nil
	}

	migrated := 0
	for _, file := range files {
		if err := c.migrateOneJSONPage(dataDir, file); err != nil {
			fmt.Printf("warning: migration skipped %s: %v\n", file, err)
			continue
		}
		migrated++
	}

	return migrated, nil
}

func (c *Catalog) migrateOneJSONPage(dataDir, file string) error {
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}

	var page map[string]interface{}
	if err := json.Unmarshal(data, &page); err != nil {
		return err
	}

	unique := filepath.Base(filepath.Dir(file))
	if !IsValidNonce(unique, PageNonceSize) {
		return fmt.Errorf("invalid page nonce: %s", unique)
	}

	title, _ := page["title"].(string)
	filename, _ := page["filename"].(string)
	binaryKey, _ := page["binary"].(string)

	dbPath := c.pageFilePath(unique)
	pdb, err := OpenPageDB(dbPath)
	if err != nil {
		return err
	}

	// Read binary file if it exists.
	var binaryData []byte
	if binaryKey != "" {
		binaryPath := path.Join(dataDir, unique, binaryKey)
		binaryData, _ = os.ReadFile(binaryPath)
	}

	binaryHash := ""
	if len(binaryData) > 0 {
		hash := sha256.Sum256(binaryData)
		binaryHash = hex.EncodeToString(hash[:])
		pdb.SaveBinary(binaryKey, binaryData)
	}

	pdb.InitMeta(unique, title, filename, binaryKey, binaryHash)

	// Migrate cells.
	lines, _ := page["lines"].([]interface{})
	now := time.Now().Unix()
	for i, rawLine := range lines {
		line, ok := rawLine.(map[string]interface{})
		if !ok {
			continue
		}

		cellType, _ := line["type"].(string)
		cellUnique, _ := line["unique"].(string)
		if cellType == "" || cellUnique == "" {
			continue
		}

		var content string
		var output []byte

		switch cellType {
		case "command":
			content, _ = line["command"].(string)
			outPath := path.Join(dataDir, unique, cellUnique+".out")
			output, _ = os.ReadFile(outPath)
		case "script":
			content, _ = line["script"].(string)
			outPath := path.Join(dataDir, unique, cellUnique+".out")
			output, _ = os.ReadFile(outPath)
		case "markdown":
			mdPath := path.Join(dataDir, unique, cellUnique+".md")
			mdData, _ := os.ReadFile(mdPath)
			content = string(mdData)
		}

		pdb.db.Exec(
			"INSERT OR IGNORE INTO cells (id, type, content, output, position, created, executed) VALUES (?, ?, ?, ?, ?, ?, 0)",
			cellUnique, cellType, content, output, i, now,
		)
	}

	c.mutex.Lock()
	c.openPages[unique] = pdb
	c.mutex.Unlock()

	return nil
}

// MigrateSettings imports config.json environment variables into settings DB.
func (c *Catalog) MigrateSettings(dataDir string) error {
	configPath := path.Join(dataDir, "config.json")
	data, err := os.ReadFile(configPath)
	if err != nil {
		return nil // No config file is not an error.
	}

	var cfg struct {
		Environment map[string]string `json:"environment"`
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return err
	}

	// Only migrate if settings are empty.
	settings, _ := c.GetAllSettings()
	if len(settings) > 0 {
		return nil
	}

	keys := make([]string, 0, len(cfg.Environment))
	for k := range cfg.Environment {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	for _, k := range keys {
		if err := c.SetSetting(k, cfg.Environment[k]); err != nil {
			fmt.Printf("warning: failed to migrate setting %s: %v\n", k, err)
		}
	}
	return nil
}

// ─── Utility ────────────────────────────────────────────────

// ComputeSHA256 computes the SHA-256 hex digest of data.
func ComputeSHA256(data []byte) string {
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:])
}

// ComputeFileSHA256 computes the SHA-256 hex digest of a file.
func ComputeFileSHA256(filePath string) (string, error) {
	f, err := os.Open(filePath)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
