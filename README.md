# Rizin Notebook

A notebook to write notes while using `rizin`.

If you want to compare it with something similar, you can call it as the rizin equivalent of [jupyter notebook](https://jupyter.org/)

## Requirements

Requires at least rizin version `0.2.0`

## Screenshot

![rizin-notebook](https://raw.githubusercontent.com/rizinorg/rizin-notebook/master/.rizin-notebook.png)

## Building

```bash
go build -ldflags "-X main.NBVERSION=$(git rev-parse --short HEAD)"
```

## Running

```bash
./rizin-notebook
```

Default URL: `http://127.0.0.1:8000/`

Useful flags:

- `-bind 127.0.0.1:8000`
- `-root /`
- `-notebook /path/to/storage`
- `-debug-assets /path/to/assets`

Environment variable:

- `RIZIN_PATH` to force a specific rizin executable.

## Cutter Plugin

This repository includes a native Cutter plugin in [cutter-notebook-plugin](cutter-notebook-plugin).

What it does:

- embeds rizin-notebook in a `QWebEngineView` dock widget,
- starts/stops the server with `QProcess`,
- supports auto/fixed port,
- stores settings with `QSettings`,
- syncs the currently opened Cutter binary into a notebook page.

### Build the plugin

You need Cutter + Rizin development packages and Qt6 (including WebEngine).

```bash
cd cutter-notebook-plugin
cmake -S . -B build
cmake --build build --config Release
cmake --install build --config Release
```

The install step uses `Cutter_USER_PLUGINDIR` from Cutter's CMake config.

### Install without CMake install

If you want to copy manually:

1. Build `CutterNotebookPlugin`.
2. Copy the produced plugin library (`.dll` / `.so` / `.dylib`) to Cutter's native plugin directory.
3. Ensure `rizin-notebook` binary is either:
	- configured in the plugin settings dialog,
	- available in `PATH`,
	- or available via `RIZIN_NOTEBOOK_PATH`.

## GitHub Automation

Workflows are in [.github/workflows](.github/workflows):

- [ci.yaml](.github/workflows/ci.yaml): PR build check.
- [build.yml](.github/workflows/build.yml): manual multi-platform artifact build + optional GitHub Release.

### Trigger a manual build

On GitHub:

1. Open **Actions**.
2. Select **Build Artifacts**.
3. Click **Run workflow**.

This produces downloadable artifacts for each target platform plus a plugin source archive.

### Trigger a release from the same workflow

In the same **Run workflow** form:

- set `publish_release` to `true`,
- set `release_tag` (example: `v1.2.0`),
- optionally set `release_name`.

The workflow creates/updates the release and uploads all generated artifacts.

### Tag-based release

If you push a tag like `v1.2.0`, the workflow also builds and publishes a release automatically.