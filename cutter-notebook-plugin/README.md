# cutter-notebook-plugin

Adds a Plugins -> Notebook dock and menu to Cutter for interacting with the
[rizin-notebook](../README.md) server.

This plugin talks to the notebook server directly over HTTP using Qt's network stack.
It does not depend on the `NB` commands from the rizin core plugin for its UI functionality,
but the core plugin must be installed for any `NB` command functionality in Cutter's
rizin shell.

## What you get

A dock panel with two tabs:

**Pages tab:**
- List of all notebook pages
- Per-page cell viewer showing cell type, content, and output
- Buttons: Refresh Pages, New Page, Delete Page, Attach Binary, Open Pipe, Close Pipe,
  Reload Page, Export, Import

**Compose tab:**
- Active page and pipe status
- Editor for writing and submitting cells:
  - Markdown note
  - Command cell (recorded without execution)
  - Script cell (recorded without execution)
  - Run command now (executes via server pipe, records result)
  - Run script now (executes JavaScript on server, records result)

**Menu entries** (Plugins -> Notebook):
- Connect... - connect to a running server
- Spawn Server - auto-start `rizin-notebook.exe`
- Server Status - show server version, rizin path, storage, page count
- List Pages - refresh the pages tab
- New Page... - create a page (auto-attaches the currently open binary)
- Delete Page - delete the selected page
- Attach Binary... - attach a binary file to the selected page
- Export Page... - save the selected page as a `.rznb` file
- Import Page... - import a `.rznb` file as a new page
- (separator)
- Open in Browser - open the server web UI in your browser
- Set Server URL... - change the server address

**Status bar** (bottom of dock): shows connection state, page count, open pipe count,
and server details on hover.

## Requirements

- Cutter 2.4.1 (or compatible)
- `rz_notebook` core plugin installed and loaded in Cutter (provides `NB` commands for
  the rizin shell; the Cutter plugin itself uses the HTTP JSON API directly)
- `rizin-notebook` Go server running (the Cutter plugin can auto-start it)
- Qt 6.7.x SDK with MSVC 2019 64-bit (Windows) or the matching platform toolkit
- CMake 3.16+ and a C++20 compiler

## Build (Windows)

Install Qt 6.7.2 if you don't have it:

```powershell
pip install aqtinstall
python -m aqt install-qt windows desktop 6.7.2 win64_msvc2019_64 --outputdir C:\Qt
```

Configure and build:

```powershell
cmake -S cutter-notebook-plugin -B cutter-notebook-plugin/build ^
  -DCutter_DIR="C:\path\to\Cutter-v2.4.1-Windows-x86_64" ^
  -DQt6_DIR="C:\Qt\6.7.2\msvc2019_64\lib\cmake\Qt6"

cmake --build cutter-notebook-plugin/build --config Release
```

## Build (Linux / macOS)

```bash
cmake -S cutter-notebook-plugin -B cutter-notebook-plugin/build \
  -DCutter_DIR=/opt/Cutter \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64

cmake --build cutter-notebook-plugin/build --config Release
```

## Install

Copy `CutterNotebookPlugin.dll` (Windows) or `CutterNotebookPlugin.so` (Linux) to:

- Windows: `%APPDATA%\rizin\cutter\plugins\native`
- Linux: `~/.local/share/rizin/cutter/plugins/native`
- macOS: `~/Library/Application Support/rizin/cutter/plugins/native`

The `rz_notebook` core plugin (`rz_notebook.dll` / `.so`) goes into the Rizin plugin
directory that Cutter uses. For portable Cutter builds, this is often inside the Cutter
installation directory, not just the user plugin directory.

Run `rizin -H RZ_USER_PLUGINS` to find the user path. For portable Cutter, also check
`<cutter-root>/lib/rizin/plugins/`.

## Troubleshooting

**Dock does not appear** - make sure `CutterNotebookPlugin.dll` is in the native plugins
directory and was built against the same Cutter and Qt version you are running.

**Server not reachable** - start `rizin-notebook` first, or use Spawn Server from the menu.
Use Set Server URL if the server runs on a non-default address.

**NB commands not available in Cutter's rizin shell** - `rz_notebook.dll` (the core plugin)
must be built against the same Rizin that Cutter bundles and placed in the Rizin plugin
path that Cutter loads from. The Cutter plugin itself does not require this for its dock UI,
but you need it to use `NB` commands in Cutter's command panel.

**Binary hash mismatch warning** - when you open a page whose attached binary differs from
the file currently open in Cutter, the plugin shows a warning. You can re-attach the
correct binary via Attach Binary.
