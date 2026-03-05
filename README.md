# Rizin Notebook

A notebook for writing notes while using rizin — think of it as the rizin equivalent of [jupyter notebook](https://jupyter.org/).

Requires rizin 0.2.0 or later.

![rizin-notebook](https://raw.githubusercontent.com/rizinorg/rizin-notebook/master/.rizin-notebook.png)

## Components

The project has three parts:

- **Go server** — the main notebook application. Stores everything in SQLite, serves a web UI, exposes a protobuf REST API.
- **rz-notebook** — a rizin core plugin (C). Adds `NB` commands to rizin so you can interact with the notebook from the rizin shell.
- **cutter-notebook-plugin** — a Cutter plugin (C++/Qt6). Adds a Plugins → Notebook menu to Cutter that calls the `NB` commands.

The plugins are optional. The Go server works standalone with just a browser.

## Building the server

```bash
go build -ldflags "-X main.NBVERSION=$(git rev-parse --short HEAD)"
```

## Running

```bash
./rizin-notebook
```

Opens at http://127.0.0.1:8000/ by default.

Flags:

```
-bind 127.0.0.1:8000    address to listen on
-root /                  URL path prefix
-notebook /path/to/data  data directory (default: ~/.rizin-notebook)
-debug-assets /path      load templates from disk instead of embedded
-debug                   enable HTTP debug logs
```

Set `RIZIN_PATH` environment variable to use a specific rizin binary.

## Building the rizin plugin (rz-notebook)

The plugin needs protobuf-c and rizin headers. On Windows, vcpkg is the easiest way to get protobuf-c.

### Generate protobuf-c files

```bash
protoc-c --c_out=rz-notebook/src proto/notebook.proto
```

This creates `notebook.pb-c.c` and `notebook.pb-c.h` in `rz-notebook/src/`.

### Windows (with Cutter SDK for rizin headers)

```powershell
cd rz-notebook

cmake -S . -B build ^
  -DCUTTER_SDK_DIR="C:\path\to\Cutter-v2.4.1-Windows-x86_64" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

### Windows (with rizin source tree)

```powershell
cd rz-notebook

cmake -S . -B build ^
  -DRIZIN_SOURCE_DIR="C:\path\to\rizin" ^
  -DRIZIN_BUILD_DIR="C:\path\to\rizin\build" ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

### Linux

```bash
# install deps (debian/ubuntu)
sudo apt install protobuf-c-compiler libprotobuf-c-dev librizin-dev libcurl4-openssl-dev

cd rz-notebook
cmake -S . -B build
cmake --build build --config Release
```

### Install

```bash
cmake --install build --config Release
```

Or copy `rz_notebook.dll` / `rz_notebook.so` to the rizin plugin directory:

- Windows: `%APPDATA%\rizin\plugins`
- Linux: `~/.local/lib/rizin/plugins`

Run `rizin -H RZ_USER_PLUGINS` to confirm the path.

## Building the Cutter plugin

See [cutter-notebook-plugin/README.md](cutter-notebook-plugin/README.md).

## Releases

GitHub Actions builds everything automatically. See `.github/workflows/build.yml`.

Two ways to make a release:

1. Push a tag like `v1.0.0` — the workflow builds all artifacts and publishes a GitHub release.
2. Go to Actions → Build Artifacts → Run workflow, check "Publish a GitHub release", and fill in the tag.

The release includes:

- server binaries for all platforms (windows/linux/darwin, amd64/arm64)
- `rz_notebook` plugin binaries (Windows + Linux)
- `CutterNotebookPlugin.dll` (Windows)
- plugin source archives
- `SHA256SUMS.txt` for integrity verification
