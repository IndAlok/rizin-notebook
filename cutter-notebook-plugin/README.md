# Cutter Notebook Plugin

Adds a Plugins → Notebook menu to Cutter for interacting with the [rizin-notebook](../README.md) server.

Each menu action calls a rizin `NB` command through Cutter's command interface. The `NB` commands come from the [rz-notebook](../rz-notebook) core plugin, which talks to the Go server over HTTP.

This plugin stores nothing — all data lives in the Go server's SQLite database.

## Menu entries

- **Server Status** — runs `NBs`, shows if the server is reachable
- **List Pages** — runs `NBl`, shows all notebook pages
- **New Page...** — runs `NBn <title>`, prompts for a page title
- **Open in Browser** — opens the server URL in your browser
- **Set Server URL...** — runs `NBu <url>`, prompts for the server address

## Requirements

- Cutter 2.4.1 (or compatible)
- rz-notebook core plugin installed (provides the `NB` commands)
- rizin-notebook Go server running (the core plugin can auto-start it)
- Qt6 SDK matching your Cutter version (for building)
- CMake 3.16+ and a C++20 compiler (MSVC 2022 on Windows)

## Build (Windows)

```powershell
# install Qt 6.7.2 if you don't have it
pip install aqtinstall
python -m aqt install-qt windows desktop 6.7.2 win64_msvc2019_64 --outputdir C:\Qt

cd cutter-notebook-plugin

cmake -S . -B build ^
  -DCutter_DIR="C:\path\to\Cutter-v2.4.1-Windows-x86_64" ^
  -DCMAKE_PREFIX_PATH="C:\Qt\6.7.2\msvc2019_64" ^
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

## Build (Linux / macOS)

```bash
cd cutter-notebook-plugin

cmake -S . -B build \
  -DCutter_DIR=/opt/Cutter \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.7.2/gcc_64

cmake --build build --config Release
```

## Install

```bash
cmake --install build --config Release
```

Or copy the built file manually:

- Windows: `%APPDATA%\rizin\cutter\plugins\native`
- Linux: `~/.local/share/rizin/cutter/plugins/native`
- macOS: `~/Library/Application Support/rizin/cutter/plugins/native`

## Troubleshooting

**Plugin doesn't load** — make sure the DLL is in the right directory and was built against the same Cutter/Qt version you're running.

**NB commands not found** — the rz-notebook core plugin (`rz_notebook.dll` / `.so`) needs to be in rizin's plugin path. Run `rizin -H RZ_USER_PLUGINS` to check.

**Server unreachable** — start the Go server (`rizin-notebook`) first. Use Set Server URL if it's not on the default `http://127.0.0.1:8000`.
