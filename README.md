# Rizin Notebook

A notebook for binary analysis with rizin. Keep notes, commands, and scripts organized per binary.

Requires rizin 0.7.0 or later.

## What it does

Each notebook **page** is tied to one binary. It stores the binary itself, its SHA-256 hash,
and a list of **cells**:

- **command cells** - a rizin command and its output
- **script cells** - a JavaScript snippet run on the server against a rizin pipe
- **markdown cells** - freeform notes

Pages are stored as individual `.rznb` files (SQLite databases) in `~/.rizin-notebook/`.
They can be exported and imported as single self-contained files.

## Components

- **Go server** (`rizin-notebook`) - the main application. Serves a web UI, exposes a
  protobuf + JSON REST API, stores everything in per-page `.rznb` SQLite files.
- **rizin-notebook-plugin** - a rizin core plugin (C). Adds `NB` commands to rizin so you
  can interact with the notebook from the rizin shell.
- **cutter-notebook-plugin** - a Cutter plugin (C++/Qt6). Adds a Plugins -> Notebook dock
  and menu to Cutter.

The plugins are optional. The Go server works standalone with just a browser.

## Compatibility note

The Go server is platform-independent. The plugins depend on the Rizin version they are
built against.

If standalone rizin and Cutter ship different Rizin versions on your machine, build
`rz_notebook` once for each. Both builds can talk to the same server and share the same
`.rznb` data.

Simple rule:

- one server
- one `rz_notebook` build per Rizin version in use
- one Cutter plugin if you want the Cutter dock

## Building the server

```bash
go build -o rizin-notebook .
```

Embed the version string during release builds:

```bash
go build -ldflags "-X main.NBVERSION=$(git rev-parse --short HEAD)" -o rizin-notebook .
```

## Running

```bash
./rizin-notebook
```

Opens at `http://127.0.0.1:8000/` by default.

Flags:

```
-bind 127.0.0.1:8000    address and port to listen on
-root /                  URL path prefix
-notebook /path/to/data  data directory (default: ~/.rizin-notebook)
-debug-assets /path      load templates from disk instead of embedded
-debug                   enable HTTP request logs
```

Set `RIZIN_PATH` to override the rizin binary path used for pipe execution.

## Building the rizin plugin

See [rizin-notebook-plugin/README.md](rizin-notebook-plugin/README.md).

## Building the Cutter plugin

See [cutter-notebook-plugin/README.md](cutter-notebook-plugin/README.md).

If Cutter ships its own embedded Rizin, the `rz_notebook` plugin loaded by Cutter must be
built against that same bundled Rizin version.

## Data storage

Each page is a `.rznb` file in the data directory. It is a SQLite database containing:

- page metadata (title, original filename, binary storage key, SHA-256 hash)
- cells (type, content, output, timestamps)
- the binary blob itself
- per-page config (key/value)

The server maintains a `catalog.db` index for fast listing across all pages.

On first run the server automatically migrates any pages from an older monolithic
`notebook.db` format into the per-page `.rznb` layout.

## API

All `NB` plugin commands use the protobuf API (`Content-Type: application/x-protobuf`).
The schema is in `proto/notebook.proto`.

A parallel JSON API at `/api/v1/json/...` is used by the Cutter plugin and the browser UI.
