# rz-notebook (Rizin Core Plugin)

Rizin core plugin that adds `NB` commands for interacting with the [rizin-notebook](../README.md) server.

When loaded, it registers a command group under `NB` and can auto-start the notebook server if it's not already running.

This plugin must be built against the same Rizin version as the host that loads it.

That means a standalone rizin install and a Cutter bundle may need different `rz_notebook` builds if they ship different Rizin versions. Both builds can still use the same notebook server.

## Commands

```
NB     list pages (same as NBl)
NBs    server status
NBl    list pages
NBn    create a new page: NBn <title> [binary-file]
NBp    show a page: NBp <page-id>
NBd    delete a page: NBd <page-id>
NBac   add command cell: NBac <page-id> <command>
NBam   add markdown cell: NBam <page-id> <text>
NBas   add script cell: NBas <page-id> <script>
NBx    execute command: NBx <page-id> <command>
NBxs   execute script: NBxs <page-id> <script>
NBo    open pipe: NBo <page-id>
NBc    close pipe: NBc <page-id>
NBu    show/set server URL: NBu [url]
```

## Dependencies

- Rizin headers (from Cutter SDK, rizin source tree, or installed rizin)
- protobuf-c (for API message serialization)
- WinHTTP (Windows, built-in) or libcurl (Linux/macOS)

## Build

### 1. Generate protobuf-c files

Run this from the repo root:

```bash
protoc-c --c_out=rz-notebook/src proto/notebook.proto
```

### 2. Configure and build

**Windows (Cutter SDK + vcpkg):**

```powershell
cd rz-notebook

cmake -S . -B build ^
  -DCUTTER_SDK_DIR="C:\path\to\Cutter-v2.4.1-Windows-x86_64" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

**Windows (rizin source tree + vcpkg):**

```powershell
cd rz-notebook

cmake -S . -B build ^
  -DRIZIN_SOURCE_DIR="C:\path\to\rizin" ^
  -DRIZIN_BUILD_DIR="C:\path\to\rizin\build" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

**Linux:**

```bash
sudo apt install protobuf-c-compiler libprotobuf-c-dev librizin-dev libcurl4-openssl-dev

cd rz-notebook
cmake -S . -B build
cmake --build build --config Release
```

## Install

```bash
cmake --install build --config Release
```

Or copy `rz_notebook.dll` / `rz_notebook.so` to:

- Windows: `%APPDATA%\rizin\plugins`
- Linux: `~/.local/lib/rizin/plugins`
- macOS: `~/Library/Application Support/rizin/plugins`

If Cutter uses its own bundled Rizin, copy the Cutter-compatible build into the Rizin plugin directory inside the Cutter installation, or wherever that Cutter build loads Rizin plugins from.

Check with `rizin -H RZ_USER_PLUGINS`.
