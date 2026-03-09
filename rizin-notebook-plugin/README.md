# rizin-notebook-plugin

Rizin core plugin that adds `NB` commands for interacting with the
[rizin-notebook](../README.md) server.

This plugin must be built against the same Rizin version as the host that loads it.

That means standalone rizin and Cutter may need different `rz_notebook` builds if they
ship different Rizin versions. Both can still use the same notebook server and data.

## Commands

```
NB               list pages (default action)
NBc <url>        connect to a running notebook server
NBs              spawn (auto-start) the notebook server
NBi              show server status and session info
NBl              list all pages
NBn [title]      create a new page; auto-attaches the currently open binary
NBo <page-id>    open/select a page as the active page;
                   if no binary is loaded, downloads the page binary from
                   the server and opens it in rizin automatically
NBq              close/detach the active page
NBp [page-id]    show page details (active page if no id given)
NBd <page-id>    delete a page
NBx <command>    execute a command locally and record it to the active page
NBxs <script>    execute a JavaScript script on the server for the active page
NBe [path]       export the active page as a .rznb file
NBm <path>       import a .rznb file as a new page
NBu [url]        show or set the server URL
```

### Typical workflow

```
[rizin /path/to/binary]
NBs              -- start the server if not running
NBn "My Analysis"   -- create a page; the binary is attached automatically
NBx aaa          -- run 'aaa', output is recorded as a command cell
NBx pdf @ main   -- same
NBe mywork.rznb  -- export for sharing or backup
```

### Binary auto-download

When you run `NBo <page-id>` and no binary is loaded in rizin, the plugin checks whether
the page has an attached binary. If it does, it downloads the binary from the server, saves
it to a temporary file, and opens it in rizin automatically.

This removes the need to locate and open the binary file manually before working on a page.

## Dependencies

- Rizin headers (from a Cutter SDK, rizin source tree, or installed rizin)
- protobuf-c (for serializing API messages)
- WinHTTP (Windows, built-in) or libcurl (Linux/macOS)
- OpenSSL (Linux/macOS, for SHA-256; Windows uses WinCrypt)

## Build

### 1. Generate protobuf-c files

Run from the repo root. Requires `protoc` and `protoc-gen-c` on your PATH.

```bash
protoc --plugin=protoc-gen-c=/path/to/protoc-gen-c \
       --c_out=rizin-notebook-plugin/src \
       proto/notebook.proto
```

This generates `notebook.pb-c.c` and `notebook.pb-c.h` in
`rizin-notebook-plugin/src/proto/`. Copy them up one directory to
`rizin-notebook-plugin/src/` before building.

### 2. Configure and build

**Windows (Cutter SDK + vcpkg):**

```powershell
cmake -S rizin-notebook-plugin -B rizin-notebook-plugin/build ^
  -DCUTTER_SDK_DIR="C:\path\to\Cutter-v2.4.1-Windows-x86_64" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake --build rizin-notebook-plugin/build --config Release
```

**Windows (rizin source tree + vcpkg):**

```powershell
cmake -S rizin-notebook-plugin -B rizin-notebook-plugin/build ^
  -DRIZIN_SOURCE_DIR="C:\path\to\rizin" ^
  -DRIZIN_BUILD_DIR="C:\path\to\rizin\build" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake --build rizin-notebook-plugin/build --config Release
```

**Linux:**

```bash
sudo apt install protobuf-c-compiler libprotobuf-c-dev librizin-dev \
                 libcurl4-openssl-dev libssl-dev

cmake -S rizin-notebook-plugin -B rizin-notebook-plugin/build
cmake --build rizin-notebook-plugin/build --config Release
```

### 3. Install

```bash
cmake --install rizin-notebook-plugin/build --config Release
```

Or copy `rz_notebook.dll` / `rz_notebook.so` manually to the rizin plugin directory:

- Windows: `%APPDATA%\rizin\plugins`
- Linux: `~/.local/lib/rizin/plugins`

Run `rizin -H RZ_USER_PLUGINS` to confirm the correct path.

If you use Cutter with a bundled Rizin, also copy the Cutter-compatible build into the
Rizin plugin directory inside the Cutter installation.

## Notes

- The plugin does **not** auto-connect on load. You must run `NBc` or `NBs` first.
- `NBx` runs the command in the current rizin session and records both command and output.
  It requires a binary to be open. If you used `NBo` and the binary was auto-downloaded,
  it is already open and `NBx` works immediately.
- `NBxs` runs JavaScript on the server side via a rizin pipe. The pipe must be open
  (opened automatically by the server when you open a page with a binary).
- `NBm` auto-selects the imported page as the active page.
- `NBe` uses the page title as the filename if no path is given.
