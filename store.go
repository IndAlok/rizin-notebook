
package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// DatabaseFile is the SQLite database filename within the data directory.
const DatabaseFile = "notebook.db"

// PageFile is the per-page JSON metadata filename (used by MigrateFromJSON).
const PageFile = "page.json"

// Store is the SQLite-backed persistence layer.
type Store struct {
	db    *sql.DB
	mutex sync.RWMutex
}

// ─── Schema ─────────────────────────────────────────────────

const schemaSQL = `
CREATE TABLE IF NOT EXISTS pages (
	id       TEXT PRIMARY KEY,
	title    TEXT NOT NULL,
	filename TEXT NOT NULL DEFAULT '',
	binary   TEXT NOT NULL DEFAULT '',
	created  INTEGER NOT NULL DEFAULT 0,
	modified INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS cells (
	id       TEXT NOT NULL,
	page_id  TEXT NOT NULL,
	type     TEXT NOT NULL CHECK(type IN ('command', 'script', 'markdown')),
	content  TEXT NOT NULL DEFAULT '',
	output   BLOB,
	position INTEGER NOT NULL DEFAULT 0,
	created  INTEGER NOT NULL DEFAULT 0,
	executed INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (page_id, id),
	FOREIGN KEY (page_id) REFERENCES pages(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS binaries (
	page_id TEXT NOT NULL,
	name    TEXT NOT NULL,
	data    BLOB NOT NULL,
	PRIMARY KEY (page_id, name),
	FOREIGN KEY (page_id) REFERENCES pages(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS settings (
	key   TEXT PRIMARY KEY,
	value TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_cells_page ON cells(page_id, position);
`

// ─── Constructor ────────────────────────────────────────────

// NewStore opens (or creates) the SQLite database and applies the schema.
func NewStore(dataDir string) (*Store, error) {
	dbPath := path.Join(dataDir, DatabaseFile)
	db, err := sql.Open("sqlite", dbPath+"?_pragma=journal_mode(wal)&_pragma=foreign_keys(on)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, fmt.Errorf("failed to open database: %w", err)
	}

	// Apply schema.
	if _, err := db.Exec(schemaSQL); err != nil {
		db.Close()
		return nil, fmt.Errorf("failed to apply schema: %w", err)
	}

	store := &Store{db: db}

	return store, nil
}

// Close closes the database connection.
func (s *Store) Close() error {
	return s.db.Close()
}

// ─── Page CRUD ──────────────────────────────────────────────

// PageRow represents a page record from the database.
type PageRow struct {
	ID       string
	Title    string
	Filename string
	Binary   string
	Created  int64
	Modified int64
}

// ListPages returns summary info for all pages, ordered by creation time.
func (s *Store) ListPages() ([]PageRow, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	rows, err := s.db.Query("SELECT id, title, filename, binary, created, modified FROM pages ORDER BY created ASC")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var pages []PageRow
	for rows.Next() {
		var p PageRow
		if err := rows.Scan(&p.ID, &p.Title, &p.Filename, &p.Binary, &p.Created, &p.Modified); err != nil {
			return nil, err
		}
		pages = append(pages, p)
	}
	return pages, rows.Err()
}

// GetPage returns a single page by ID, or nil if not found.
func (s *Store) GetPage(id string) (*PageRow, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	var p PageRow
	err := s.db.QueryRow("SELECT id, title, filename, binary, created, modified FROM pages WHERE id = ?", id).
		Scan(&p.ID, &p.Title, &p.Filename, &p.Binary, &p.Created, &p.Modified)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &p, nil
}

// CreatePage inserts a new page and returns its ID.
func (s *Store) CreatePage(id, title, filename, binaryKey string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	now := time.Now().Unix()
	_, err := s.db.Exec(
		"INSERT INTO pages (id, title, filename, binary, created, modified) VALUES (?, ?, ?, ?, ?, ?)",
		id, title, filename, binaryKey, now, now,
	)
	return err
}

// RenamePage updates the title of an existing page.
func (s *Store) RenamePage(id, title string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	now := time.Now().Unix()
	result, err := s.db.Exec("UPDATE pages SET title = ?, modified = ? WHERE id = ?", title, now, id)
	if err != nil {
		return err
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("page not found: %s", id)
	}
	return nil
}

// DeletePage removes a page and all its cells/binaries (via CASCADE).
func (s *Store) DeletePage(id string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	_, err := s.db.Exec("DELETE FROM pages WHERE id = ?", id)
	return err
}

// TouchPage updates the modified timestamp.
func (s *Store) TouchPage(id string) error {
	now := time.Now().Unix()
	_, err := s.db.Exec("UPDATE pages SET modified = ? WHERE id = ?", now, id)
	return err
}

// ─── Cell CRUD ──────────────────────────────────────────────

// CellRow represents a cell record from the database.
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

// ListCells returns all cells for a page, ordered by position.
func (s *Store) ListCells(pageID string) ([]CellRow, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	rows, err := s.db.Query(
		"SELECT id, page_id, type, content, output, position, created, executed FROM cells WHERE page_id = ? ORDER BY position ASC",
		pageID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cells []CellRow
	for rows.Next() {
		var c CellRow
		if err := rows.Scan(&c.ID, &c.PageID, &c.Type, &c.Content, &c.Output, &c.Position, &c.Created, &c.Executed); err != nil {
			return nil, err
		}
		cells = append(cells, c)
	}
	return cells, rows.Err()
}

// GetCell returns a single cell by page ID and cell ID.
func (s *Store) GetCell(pageID, cellID string) (*CellRow, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	var c CellRow
	err := s.db.QueryRow(
		"SELECT id, page_id, type, content, output, position, created, executed FROM cells WHERE page_id = ? AND id = ?",
		pageID, cellID,
	).Scan(&c.ID, &c.PageID, &c.Type, &c.Content, &c.Output, &c.Position, &c.Created, &c.Executed)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &c, nil
}

// NextCellPosition returns the next available position index for a page.
func (s *Store) NextCellPosition(pageID string) (int, error) {
	var pos sql.NullInt64
	err := s.db.QueryRow("SELECT MAX(position) FROM cells WHERE page_id = ?", pageID).Scan(&pos)
	if err != nil {
		return 0, err
	}
	if !pos.Valid {
		return 0, nil
	}
	return int(pos.Int64) + 1, nil
}

// AddCell inserts a new cell at the end of the page.
func (s *Store) AddCell(pageID, cellID, cellType, content string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	pos, err := s.NextCellPosition(pageID)
	if err != nil {
		return err
	}

	now := time.Now().Unix()
	_, err = s.db.Exec(
		"INSERT INTO cells (id, page_id, type, content, position, created, executed) VALUES (?, ?, ?, ?, ?, ?, 0)",
		cellID, pageID, cellType, content, pos, now,
	)
	if err != nil {
		return err
	}

	return s.touchPageUnlocked(pageID)
}

// UpdateCellContent updates the content of an existing cell.
func (s *Store) UpdateCellContent(pageID, cellID, content string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	result, err := s.db.Exec("UPDATE cells SET content = ? WHERE page_id = ? AND id = ?", content, pageID, cellID)
	if err != nil {
		return err
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("cell not found: %s/%s", pageID, cellID)
	}
	return s.touchPageUnlocked(pageID)
}

// UpdateCellOutput sets the output bytes and execution timestamp for a cell.
func (s *Store) UpdateCellOutput(pageID, cellID string, output []byte) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	now := time.Now().Unix()
	result, err := s.db.Exec("UPDATE cells SET output = ?, executed = ? WHERE page_id = ? AND id = ?",
		output, now, pageID, cellID)
	if err != nil {
		return err
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("cell not found: %s/%s", pageID, cellID)
	}
	return s.touchPageUnlocked(pageID)
}

// DeleteCell removes a cell from the database.
func (s *Store) DeleteCell(pageID, cellID string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	_, err := s.db.Exec("DELETE FROM cells WHERE page_id = ? AND id = ?", pageID, cellID)
	if err != nil {
		return err
	}
	return s.touchPageUnlocked(pageID)
}

// touchPageUnlocked updates modified timestamp without acquiring mutex.
func (s *Store) touchPageUnlocked(id string) error {
	now := time.Now().Unix()
	_, err := s.db.Exec("UPDATE pages SET modified = ? WHERE id = ?", now, id)
	return err
}

// ─── Binary Storage ─────────────────────────────────────────

// SaveBinary stores a binary blob associated with a page.
func (s *Store) SaveBinary(pageID, name string, data []byte) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	_, err := s.db.Exec(
		"INSERT OR REPLACE INTO binaries (page_id, name, data) VALUES (?, ?, ?)",
		pageID, name, data,
	)
	return err
}

// GetBinary retrieves a binary blob by page ID and name.
func (s *Store) GetBinary(pageID, name string) ([]byte, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	var data []byte
	err := s.db.QueryRow("SELECT data FROM binaries WHERE page_id = ? AND name = ?", pageID, name).Scan(&data)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return data, err
}

// GetBinaryForPipe extracts a binary to a temp file and returns the path.
// This is needed because Rizin needs a file path, not a blob.
func (s *Store) GetBinaryForPipe(pageID, tempDir string) (string, error) {
	page, err := s.GetPage(pageID)
	if err != nil || page == nil {
		return "", fmt.Errorf("page not found: %s", pageID)
	}

	data, err := s.GetBinary(pageID, page.Binary)
	if err != nil || data == nil {
		return "", fmt.Errorf("binary not found for page: %s", pageID)
	}

	// Write binary to temp directory using the original filename.
	binPath := filepath.Join(tempDir, pageID, page.Binary)
	if err := os.MkdirAll(filepath.Dir(binPath), 0755); err != nil {
		return "", err
	}
	if err := os.WriteFile(binPath, data, 0644); err != nil {
		return "", err
	}
	return binPath, nil
}

// ─── Settings ───────────────────────────────────────────────

// GetAllSettings returns all key-value settings.
func (s *Store) GetAllSettings() (map[string]string, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	rows, err := s.db.Query("SELECT key, value FROM settings")
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

// GetSetting returns a single setting value, or empty string if not found.
func (s *Store) GetSetting(key string) (string, error) {
	s.mutex.RLock()
	defer s.mutex.RUnlock()

	var v string
	err := s.db.QueryRow("SELECT value FROM settings WHERE key = ?", key).Scan(&v)
	if err == sql.ErrNoRows {
		return "", nil
	}
	return v, err
}

// SetSetting upserts a setting key-value pair.
func (s *Store) SetSetting(key, value string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	_, err := s.db.Exec("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)", key, value)
	return err
}

// DeleteSetting removes a setting by key.
func (s *Store) DeleteSetting(key string) error {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	_, err := s.db.Exec("DELETE FROM settings WHERE key = ?", key)
	return err
}

// ─── JSON Data Import ─────────────────────────────────────────

// MigrateFromJSON imports JSON-file-based pages into SQLite on first startup.
func (s *Store) MigrateFromJSON(dataDir string) (int, error) {
	files, err := filepath.Glob(path.Join(dataDir, "*", PageFile))
	if err != nil {
		return 0, err
	}

	if len(files) == 0 {
		return 0, nil
	}

	// Check if we already have pages (skip migration if DB is populated).
	var count int
	s.db.QueryRow("SELECT COUNT(*) FROM pages").Scan(&count)
	if count > 0 {
		return 0, nil
	}

	migrated := 0
	for _, file := range files {
		if err := s.migrateOnePage(dataDir, file); err != nil {
			fmt.Printf("warning: migration skipped %s: %v\n", file, err)
			continue
		}
		migrated++
	}

	return migrated, nil
}

func (s *Store) migrateOnePage(dataDir, file string) error {
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

	now := time.Now().Unix()
	_, err = s.db.Exec(
		"INSERT OR IGNORE INTO pages (id, title, filename, binary, created, modified) VALUES (?, ?, ?, ?, ?, ?)",
		unique, title, filename, binaryKey, now, now,
	)
	if err != nil {
		return err
	}

	// Migrate binary file if it exists.
	binaryPath := path.Join(dataDir, unique, binaryKey)
	if binData, err := os.ReadFile(binaryPath); err == nil {
		s.SaveBinary(unique, binaryKey, binData)
	}

	// Migrate cells (lines).
	lines, _ := page["lines"].([]interface{})
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

		_, err = s.db.Exec(
			"INSERT OR IGNORE INTO cells (id, page_id, type, content, output, position, created, executed) VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
			cellUnique, unique, cellType, content, output, i, now,
		)
		if err != nil {
			fmt.Printf("warning: failed to migrate cell %s/%s: %v\n", unique, cellUnique, err)
		}
	}

	// Migrate project file if it exists.
	prjPath := path.Join(dataDir, unique, "project.rzdb")
	if prjData, err := os.ReadFile(prjPath); err == nil {
		s.SaveBinary(unique, "project.rzdb", prjData)
	}

	return nil
}

// MigrateSettings imports config.json environment variables into SQLite settings.
func (s *Store) MigrateSettings(dataDir string) error {
	configPath := path.Join(dataDir, ConfigFile)
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

	// Only migrate if settings table is empty.
	var count int
	s.db.QueryRow("SELECT COUNT(*) FROM settings").Scan(&count)
	if count > 0 {
		return nil
	}

	keys := make([]string, 0, len(cfg.Environment))
	for k := range cfg.Environment {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	for _, k := range keys {
		if err := s.SetSetting(k, cfg.Environment[k]); err != nil {
			fmt.Printf("warning: failed to migrate setting %s: %v\n", k, err)
		}
	}
	return nil
}

// ─── Page Count ─────────────────────────────────────────────

// PageCount returns total number of pages.
func (s *Store) PageCount() int {
	var count int
	s.db.QueryRow("SELECT COUNT(*) FROM pages").Scan(&count)
	return count
}
