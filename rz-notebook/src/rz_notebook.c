// Rizin core plugin: NB command group for notebook server interaction.
//
// Commands:
//   NB         - list pages (default group action)
//   NBc <url>  - connect to notebook server
//   NBs        - spawn (auto-start) the notebook server
//   NBi        - show server info/status
//   NBl        - list all pages
//   NBn [title]- create a new page with current binary
//   NBo <id>   - open/select a page as the active page
//   NBq        - close/detach the active page
//   NBp [id]   - show page details (active or specified)
//   NBd <id>   - delete a page
//   NBx <cmd>  - execute command locally and record to active page
//   NBxs <scr> - execute script on server for active page
//   NBe [path] - export active page as .rznb file
//   NBm <path> - import a .rznb file as a new page
//   NBu [url]  - show or set the server URL

#include <rz_core.h>
#include <rz_cmd.h>
#include <rz_util.h>
#include <rz_config.h>
#include <rz_io.h>

#include "notebook.pb-c.h"
#include "nb_http.h"
#include "nb_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ── Logging ─────────────────────────────────────────────────────────────

#define NB_LOG_ERR(fmt, ...)  RZ_LOG_ERROR("[notebook] " fmt "\n", ##__VA_ARGS__)
#define NB_LOG_INFO(fmt, ...) RZ_LOG_INFO("[notebook] " fmt "\n", ##__VA_ARGS__)

// ── Session State ───────────────────────────────────────────────────────

// The active page ID for this session.  Empty string means no page selected.
static char g_active_page[64] = "";
// Whether we are currently connected to a server.
static bool g_connected = false;

static bool is_connected(void) {
	if (g_connected && nb_server_is_alive()) {
		return true;
	}
	g_connected = false;
	return false;
}

static bool require_connected(void) {
	if (is_connected()) {
		return true;
	}
	rz_cons_println("Not connected to a notebook server. Use NBc <url> or NBs to connect.");
	return false;
}

static bool require_active_page(void) {
	if (!require_connected()) {
		return false;
	}
	if (g_active_page[0] == '\0') {
		rz_cons_println("No active page. Use NBo <page-id> to open one.");
		return false;
	}
	return true;
}

// ── Helpers ─────────────────────────────────────────────────────────────

static const char *cell_type_str(Notebook__CellType t) {
	switch (t) {
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_COMMAND:  return "cmd";
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_SCRIPT:   return "script";
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_MARKDOWN: return "markdown";
	default: return "?";
	}
}

static const char *nb_current_filename(RzCore *core) {
	if (!core || !core->io) {
		return NULL;
	}
	int cur_fd = rz_io_fd_get_current(core->io);
	if (cur_fd < 0) {
		return NULL;
	}
	RzIODesc *cur_desc = rz_io_desc_get(core->io, cur_fd);
	if (!cur_desc || !cur_desc->name || !*cur_desc->name) {
		return NULL;
	}
	return cur_desc->name;
}

// ── Command Handlers ────────────────────────────────────────────────────

// NBc <url> - connect to an existing notebook server
static RzCmdStatus cmd_nb_connect(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (argc < 2) {
		rz_cons_println("Usage: NBc <url>  (e.g. NBc http://127.0.0.1:8000)");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	nb_http_set_base_url(argv[1]);
	if (core && core->config && rz_config_get(core->config, "notebook.url")) {
		rz_config_set(core->config, "notebook.url", nb_http_get_base_url());
	}

	if (!nb_server_is_alive()) {
		rz_cons_printf("Cannot reach server at %s\n", nb_http_get_base_url());
		g_connected = false;
		return RZ_CMD_STATUS_ERROR;
	}

	g_connected = true;
	rz_cons_printf("Connected to %s\n", nb_http_get_base_url());
	return RZ_CMD_STATUS_OK;
}

// NBs - spawn the notebook server
static RzCmdStatus cmd_nb_spawn(RzCore *core, int argc, const char **argv) {
	(void)core; (void)argc; (void)argv;

	// Already alive?
	if (nb_server_is_alive()) {
		g_connected = true;
		rz_cons_printf("Server already running at %s\n", nb_http_get_base_url());
		return RZ_CMD_STATUS_OK;
	}

	NB_LOG_INFO("spawning notebook server...");
	if (!nb_server_ensure(NULL)) {
		NB_LOG_ERR("failed to start the notebook server");
		rz_cons_println("Failed to start the notebook server. Check that rizin-notebook is installed.");
		return RZ_CMD_STATUS_ERROR;
	}

	g_connected = true;
	rz_cons_printf("Server started at %s\n", nb_http_get_base_url());
	return RZ_CMD_STATUS_OK;
}

// NBi - show server status/info
static RzCmdStatus cmd_nb_info(RzCore *core, int argc, const char **argv) {
	(void)core; (void)argc; (void)argv;
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}

	NbHttpResponse resp;
	if (!nb_http_get("/api/v1/status", &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status != 200) {
		rz_cons_printf("Server responded with status %d\n", resp.status);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	Notebook__StatusResponse *st =
		notebook__status_response__unpack(NULL, resp.body_len, resp.body);
	if (!st) {
		rz_cons_println("Failed to decode status response");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	rz_cons_printf("Notebook Server Status\n");
	rz_cons_printf("  URL:        %s\n", nb_http_get_base_url());
	rz_cons_printf("  Version:    %s\n", st->version);
	rz_cons_printf("  Rizin:      %s\n", st->rizin_version);
	rz_cons_printf("  Rizin Path: %s\n", st->rizin_path);
	rz_cons_printf("  Storage:    %s\n", st->storage);
	rz_cons_printf("  Pages:      %d\n", st->pages);
	rz_cons_printf("  Open Pipes: %d\n", st->open_pipes);
	if (g_active_page[0]) {
		rz_cons_printf("  Active Page: %s\n", g_active_page);
	} else {
		rz_cons_printf("  Active Page: (none)\n");
	}

	notebook__status_response__free_unpacked(st, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBl - list all pages
static RzCmdStatus cmd_nb_list(RzCore *core, int argc, const char **argv) {
	(void)core; (void)argc; (void)argv;
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}

	NbHttpResponse resp;
	if (!nb_http_get("/api/v1/pages", &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status != 200) {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	Notebook__ListPagesResponse *lp =
		notebook__list_pages_response__unpack(NULL, resp.body_len, resp.body);
	if (!lp) {
		rz_cons_println("Failed to decode response");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	if (lp->n_pages == 0) {
		rz_cons_println("No pages. Use NBn to create one.");
	} else {
		rz_cons_printf("  %-34s  %-30s  %s\n", "ID", "Title", "Cells");
		rz_cons_printf("  %-34s  %-30s  %s\n",
		               "----------------------------------",
		               "------------------------------", "-----");
		for (size_t i = 0; i < lp->n_pages; i++) {
			Notebook__Page *p = lp->pages[i];
			const char *marker = "";
			if (g_active_page[0] && strcmp(p->id, g_active_page) == 0) {
				marker = "* ";
			}
			rz_cons_printf("%s%-34s  %-30s  %zu\n",
			               marker, p->id, p->title, p->n_cells);
		}
	}

	notebook__list_pages_response__free_unpacked(lp, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBn [title] - create a new page with current binary
static RzCmdStatus cmd_nb_new(RzCore *core, int argc, const char **argv) {
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}

	const char *title = (argc >= 2) ? argv[1] : "Untitled";

	// Try to get the current file being analyzed.
	const char *filename = nb_current_filename(core);

	// Read the binary data if available.
	uint8_t *bin_data = NULL;
	size_t bin_len = 0;
	if (filename) {
		size_t rlen = 0;
		bin_data = (uint8_t *)rz_file_slurp(filename, &rlen);
		if (!bin_data) {
			rz_cons_printf("Warning: could not read '%s', creating page without binary\n", filename);
			filename = NULL;
		} else {
			bin_len = rlen;
		}
	}

	// Build protobuf request.
	Notebook__CreatePageRequest req = NOTEBOOK__CREATE_PAGE_REQUEST__INIT;
	req.title = (char *)title;
	req.filename = (char *)(filename ? filename : "");
	req.binary.data = bin_data;
	req.binary.len = bin_len;

	size_t packed_len = notebook__create_page_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		free(bin_data);
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__create_page_request__pack(&req, packed);
	free(bin_data);

	NbHttpResponse resp;
	if (!nb_http_post("/api/v1/pages", packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200 || resp.status == 201) {
		Notebook__CreatePageResponse *cr =
			notebook__create_page_response__unpack(NULL, resp.body_len, resp.body);
		if (cr && cr->page) {
			rz_cons_printf("Created page: %s (ID: %s)\n", cr->page->title, cr->page->id);
			// Auto-select the new page as active.
			snprintf(g_active_page, sizeof(g_active_page), "%s", cr->page->id);
			rz_cons_printf("Active page set to: %s\n", g_active_page);
			notebook__create_page_response__free_unpacked(cr, NULL);
		} else {
			rz_cons_println("Page created (could not decode response)");
		}
	} else {
		Notebook__ErrorResponse *err =
			notebook__error_response__unpack(NULL, resp.body_len, resp.body);
		if (err) {
			rz_cons_printf("Error %d: %s\n", err->code, err->message);
			notebook__error_response__free_unpacked(err, NULL);
		} else {
			rz_cons_printf("Server error: HTTP %d\n", resp.status);
		}
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBo <page-id> - open/select a page as the active page
static RzCmdStatus cmd_nb_open(RzCore *core, int argc, const char **argv) {
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBo <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	const char *page_id = argv[1];

	// Verify the page exists on the server.
	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s", page_id);

	NbHttpResponse resp;
	if (!nb_http_get(path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status == 404) {
		rz_cons_printf("Page not found: %s\n", page_id);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	// Decode to check binary hash.
	Notebook__GetPageResponse *gp =
		notebook__get_page_response__unpack(NULL, resp.body_len, resp.body);
	nb_http_response_free(&resp);

	if (!gp || !gp->page) {
		rz_cons_println("Failed to decode page response");
		if (gp) notebook__get_page_response__free_unpacked(gp, NULL);
		return RZ_CMD_STATUS_ERROR;
	}

	// If the page has a binary_hash, warn if current binary differs.
	if (gp->page->binary_hash && gp->page->binary_hash[0]) {
		const char *cur_file = nb_current_filename(core);
		if (cur_file) {
			size_t file_len = 0;
			uint8_t *file_data = (uint8_t *)rz_file_slurp(cur_file, &file_len);
			if (file_data && file_len > 0) {
				RzHash *hash_ctx = rz_hash_new();
				if (hash_ctx) {
					RzHashCfg *cfg = rz_hash_cfg_new(hash_ctx);
					if (cfg) {
						char *cur_hash = rz_hash_cfg_calculate_small_block_string(
							cfg, "sha256", file_data, file_len, NULL, false);
						if (cur_hash) {
							if (strcmp(cur_hash, gp->page->binary_hash) != 0) {
								rz_cons_printf("Warning: binary hash mismatch.\n");
								rz_cons_printf("  Page:    %s\n", gp->page->binary_hash);
								rz_cons_printf("  Current: %s\n", cur_hash);
							}
							free(cur_hash);
						}
						rz_hash_cfg_free(cfg);
					}
					rz_hash_free(hash_ctx);
				}
			}
			free(file_data);
		}
	}

	// Set as active page.
	snprintf(g_active_page, sizeof(g_active_page), "%s", page_id);
	rz_cons_printf("Active page: %s (%s)\n", gp->page->title, page_id);

	notebook__get_page_response__free_unpacked(gp, NULL);
	return RZ_CMD_STATUS_OK;
}

// NBq - close/detach the active page
static RzCmdStatus cmd_nb_close(RzCore *core, int argc, const char **argv) {
	(void)core; (void)argc; (void)argv;
	if (g_active_page[0] == '\0') {
		rz_cons_println("No active page to close.");
		return RZ_CMD_STATUS_OK;
	}
	rz_cons_printf("Detached from page: %s\n", g_active_page);
	g_active_page[0] = '\0';
	return RZ_CMD_STATUS_OK;
}

// NBp [page-id] - show page details (active page if no id given)
static RzCmdStatus cmd_nb_page(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}

	const char *page_id = (argc >= 2) ? argv[1] : g_active_page;
	if (!page_id || page_id[0] == '\0') {
		rz_cons_println("No active page. Use NBo <page-id> or specify an ID.");
		return RZ_CMD_STATUS_ERROR;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s", page_id);

	NbHttpResponse resp;
	if (!nb_http_get(path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status == 404) {
		rz_cons_printf("Page not found: %s\n", page_id);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	Notebook__GetPageResponse *gp =
		notebook__get_page_response__unpack(NULL, resp.body_len, resp.body);
	if (!gp || !gp->page) {
		rz_cons_println("Failed to decode response");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	Notebook__Page *p = gp->page;
	rz_cons_printf("Page: %s\n", p->title);
	rz_cons_printf("  ID:          %s\n", p->id);
	rz_cons_printf("  File:        %s\n", p->filename[0] ? p->filename : "-");
	rz_cons_printf("  Binary:      %s\n", p->binary[0] ? p->binary : "-");
	rz_cons_printf("  Binary Hash: %s\n", (p->binary_hash && p->binary_hash[0]) ? p->binary_hash : "-");
	rz_cons_printf("  Pipe:        %s\n", p->pipe ? "open" : "closed");
	rz_cons_printf("  Cells:       %zu\n", p->n_cells);

	for (size_t i = 0; i < p->n_cells; i++) {
		Notebook__Cell *c = p->cells[i];
		rz_cons_printf("\n  [%zu] %s (ID: %s)\n", i + 1, cell_type_str(c->type), c->id);
		if (c->content[0]) {
			rz_cons_printf("    > %s\n", c->content);
		}
		if (c->output.len > 0) {
			rz_cons_printf("    ");
			rz_cons_memcat((const char *)c->output.data, c->output.len);
			rz_cons_newline();
		}
	}

	notebook__get_page_response__free_unpacked(gp, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBd <page-id> - delete a page
static RzCmdStatus cmd_nb_delete(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBd <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	const char *page_id = argv[1];
	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s", page_id);

	NbHttpResponse resp;
	if (!nb_http_delete(path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status == 200) {
		rz_cons_printf("Deleted page: %s\n", page_id);
		// Clear active page if it was the deleted one.
		if (strcmp(g_active_page, page_id) == 0) {
			g_active_page[0] = '\0';
		}
	} else if (resp.status == 404) {
		rz_cons_printf("Page not found: %s\n", page_id);
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBx <command> - execute command locally and record to active page
static RzCmdStatus cmd_nb_exec(RzCore *core, int argc, const char **argv) {
	if (!require_active_page()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBx <command>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	const char *command = argv[1];

	// Execute the command locally using rizin core.
	char *result = rz_core_cmd_str(core, command);
	if (!result) {
		result = strdup("");
	}

	// Print the result to the user.
	if (result[0]) {
		rz_cons_printf("%s", result);
		size_t rlen = strlen(result);
		if (rlen > 0 && result[rlen - 1] != '\n') {
			rz_cons_newline();
		}
	}

	// Record the command + output to the notebook server via /record endpoint.
	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/record", g_active_page);

	Notebook__RecordCommandRequest req = NOTEBOOK__RECORD_COMMAND_REQUEST__INIT;
	req.page_id = g_active_page;
	req.command = (char *)command;
	req.output.data = (uint8_t *)result;
	req.output.len = strlen(result);

	size_t packed_len = notebook__record_command_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		free(result);
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__record_command_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Warning: command executed but failed to record: %s\n",
		               resp.error ? resp.error : "request failed");
	} else if (resp.status != 200 && resp.status != 201) {
		rz_cons_printf("Warning: command executed but recording returned HTTP %d\n", resp.status);
	}

	free(packed);
	free(result);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBxs <script> - execute script on server for active page
static RzCmdStatus cmd_nb_exec_script(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!require_active_page()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBxs <script>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/script", g_active_page);

	Notebook__ExecScriptRequest req = NOTEBOOK__EXEC_SCRIPT_REQUEST__INIT;
	req.page_id = g_active_page;
	req.script = (char *)argv[1];

	size_t packed_len = notebook__exec_script_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__exec_script_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200) {
		Notebook__ExecScriptResponse *er =
			notebook__exec_script_response__unpack(NULL, resp.body_len, resp.body);
		if (er) {
			if (er->cell && er->cell->output.len > 0) {
				rz_cons_memcat((const char *)er->cell->output.data,
				               er->cell->output.len);
				rz_cons_newline();
			}
			if (er->error && er->error[0]) {
				rz_cons_printf("Error: %s\n", er->error);
			}
			notebook__exec_script_response__free_unpacked(er, NULL);
		}
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBe [path] - export active page as .rznb file
static RzCmdStatus cmd_nb_export(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!require_active_page()) {
		return RZ_CMD_STATUS_ERROR;
	}

	char api_path[256];
	snprintf(api_path, sizeof(api_path), "/api/v1/pages/%s/export", g_active_page);

	NbHttpResponse resp;
	if (!nb_http_get(api_path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status != 200) {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}

	Notebook__ExportPageResponse *er =
		notebook__export_page_response__unpack(NULL, resp.body_len, resp.body);
	if (!er || er->data.len == 0) {
		rz_cons_println("Failed to decode export response or empty data");
		nb_http_response_free(&resp);
		if (er) notebook__export_page_response__free_unpacked(er, NULL);
		return RZ_CMD_STATUS_ERROR;
	}

	// Determine output path.
	const char *out_path = NULL;
	char default_path[512];
	if (argc >= 2) {
		out_path = argv[1];
	} else {
		snprintf(default_path, sizeof(default_path), "%s",
		         er->filename[0] ? er->filename : "export.rznb");
		out_path = default_path;
	}

	// Write to disk.
	if (rz_file_dump(out_path, er->data.data, (int)er->data.len, false)) {
		rz_cons_printf("Exported to: %s (%zu bytes)\n", out_path, er->data.len);
	} else {
		rz_cons_printf("Failed to write file: %s\n", out_path);
	}

	notebook__export_page_response__free_unpacked(er, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBm <path> - import a .rznb file as a new page
static RzCmdStatus cmd_nb_import(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!require_connected()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBm <path-to-rznb-file>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	const char *file_path = argv[1];
	size_t file_len = 0;
	uint8_t *file_data = (uint8_t *)rz_file_slurp(file_path, &file_len);
	if (!file_data || file_len == 0) {
		rz_cons_printf("Cannot read file: %s\n", file_path);
		free(file_data);
		return RZ_CMD_STATUS_ERROR;
	}

	// POST raw .rznb bytes to /api/v1/pages/import
	NbHttpResponse resp;
	if (!nb_http_post("/api/v1/pages/import", file_data, file_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(file_data);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(file_data);

	if (resp.status == 200 || resp.status == 201) {
		Notebook__ImportPageResponse *ir =
			notebook__import_page_response__unpack(NULL, resp.body_len, resp.body);
		if (ir && ir->page) {
			rz_cons_printf("Imported page: %s (ID: %s)\n", ir->page->title, ir->page->id);
			snprintf(g_active_page, sizeof(g_active_page), "%s", ir->page->id);
			notebook__import_page_response__free_unpacked(ir, NULL);
		} else {
			rz_cons_println("Page imported (could not decode response)");
		}
	} else {
		Notebook__ErrorResponse *err =
			notebook__error_response__unpack(NULL, resp.body_len, resp.body);
		if (err) {
			rz_cons_printf("Error %d: %s\n", err->code, err->message);
			notebook__error_response__free_unpacked(err, NULL);
		} else {
			rz_cons_printf("Server error: HTTP %d\n", resp.status);
		}
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

// NBu [url] - show or set the notebook server URL
static RzCmdStatus cmd_nb_url(RzCore *core, int argc, const char **argv) {
	if (argc < 2) {
		rz_cons_printf("%s\n", nb_http_get_base_url());
		return RZ_CMD_STATUS_OK;
	}
	nb_http_set_base_url(argv[1]);
	if (core && core->config && rz_config_get(core->config, "notebook.url")) {
		rz_config_set(core->config, "notebook.url", nb_http_get_base_url());
	}
	rz_cons_printf("%s\n", nb_http_get_base_url());
	return RZ_CMD_STATUS_OK;
}

// ── Command Help & Arg Descriptions ─────────────────────────────────────

static const RzCmdDescArg nb_no_args[] = {
	{ 0 },
};

static const RzCmdDescHelp nb_group_help = {
	.summary = "Notebook commands",
	.description = "Interact with the rizin-notebook server to manage analysis notebooks.\n"
	               "Use NBc or NBs to connect first, then NBo to select a page.",
	.args = nb_no_args,
};

// NBc
static const RzCmdDescArg nb_connect_args[] = {
	{ .name = "url", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_connect_help = {
	.summary = "Connect to a notebook server",
	.args = nb_connect_args,
};

// NBs
static const RzCmdDescHelp nb_spawn_help = {
	.summary = "Spawn (auto-start) the notebook server",
	.args = nb_no_args,
};

// NBi
static const RzCmdDescHelp nb_info_help = {
	.summary = "Show notebook server status and session info",
	.args = nb_no_args,
};

// NBl
static const RzCmdDescHelp nb_list_help = {
	.summary = "List notebook pages",
	.args = nb_no_args,
};

// NBn
static const RzCmdDescArg nb_new_args[] = {
	{ .name = "title", .type = RZ_CMD_ARG_TYPE_STRING, .optional = true },
	{ 0 },
};
static const RzCmdDescHelp nb_new_help = {
	.summary = "Create a new notebook page (auto-attaches current binary)",
	.args = nb_new_args,
};

// NBo
static const RzCmdDescArg nb_open_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_open_help = {
	.summary = "Open/select a page as the active page",
	.args = nb_open_args,
};

// NBq
static const RzCmdDescHelp nb_close_help = {
	.summary = "Close/detach the active page",
	.args = nb_no_args,
};

// NBp
static const RzCmdDescArg nb_page_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING, .optional = true },
	{ 0 },
};
static const RzCmdDescHelp nb_page_help = {
	.summary = "Show page details (active page if no id given)",
	.args = nb_page_args,
};

// NBd
static const RzCmdDescArg nb_delete_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_delete_help = {
	.summary = "Delete a notebook page",
	.args = nb_delete_args,
};

// NBx
static const RzCmdDescArg nb_exec_args[] = {
	{ .name = "command", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_exec_help = {
	.summary = "Execute a command locally and record to the active page",
	.args = nb_exec_args,
};

// NBxs
static const RzCmdDescArg nb_exec_script_args[] = {
	{ .name = "script", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_exec_script_help = {
	.summary = "Execute a script on the server for the active page",
	.args = nb_exec_script_args,
};

// NBe
static const RzCmdDescArg nb_export_args[] = {
	{ .name = "output-path", .type = RZ_CMD_ARG_TYPE_FILE, .optional = true },
	{ 0 },
};
static const RzCmdDescHelp nb_export_help = {
	.summary = "Export the active page as a .rznb file",
	.args = nb_export_args,
};

// NBm
static const RzCmdDescArg nb_import_args[] = {
	{ .name = "rznb-file", .type = RZ_CMD_ARG_TYPE_FILE },
	{ 0 },
};
static const RzCmdDescHelp nb_import_help = {
	.summary = "Import a .rznb file as a new page",
	.args = nb_import_args,
};

// NBu
static const RzCmdDescArg nb_url_args[] = {
	{ .name = "url", .type = RZ_CMD_ARG_TYPE_STRING, .optional = true },
	{ 0 },
};
static const RzCmdDescHelp nb_url_help = {
	.summary = "Show or set the notebook server URL",
	.args = nb_url_args,
};

// ── Plugin Init & Fini ──────────────────────────────────────────────────

static bool rz_notebook_init(RzCore *core) {
	// Initialize HTTP subsystem.
	if (!nb_http_init()) {
		NB_LOG_ERR("failed to initialize HTTP subsystem");
		return false;
	}

	// Read base URL from rizin config if set.
	const char *cfg_url = rz_config_get(core->config, "notebook.url");
	if (cfg_url && *cfg_url) {
		nb_http_set_base_url(cfg_url);
	}
	// Ensure config has the effective URL, even when default is used.
	if (rz_config_get(core->config, "notebook.url")) {
		rz_config_set(core->config, "notebook.url", nb_http_get_base_url());
	}

	// Reset session state.
	g_active_page[0] = '\0';
	g_connected = false;

	// Register NB command group.
	RzCmd *cmd = core->rcmd;
	RzCmdDesc *root_cd = rz_cmd_get_root(cmd);
	if (!root_cd) {
		NB_LOG_ERR("failed to get root command descriptor");
		return false;
	}

	// Create the NB group.
	RzCmdDesc *nb_cd = rz_cmd_desc_group_new(cmd, root_cd, "NB",
	                                          cmd_nb_list, &nb_list_help, &nb_group_help);
	if (!nb_cd) {
		NB_LOG_ERR("failed to create NB command group");
		return false;
	}

	// Register subcommands under NB.
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBc", cmd_nb_connect, &nb_connect_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBs", cmd_nb_spawn, &nb_spawn_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBi", cmd_nb_info, &nb_info_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBl", cmd_nb_list, &nb_list_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBn", cmd_nb_new, &nb_new_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBo", cmd_nb_open, &nb_open_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBq", cmd_nb_close, &nb_close_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBp", cmd_nb_page, &nb_page_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBd", cmd_nb_delete, &nb_delete_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBx", cmd_nb_exec, &nb_exec_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBxs", cmd_nb_exec_script, &nb_exec_script_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBe", cmd_nb_export, &nb_export_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBm", cmd_nb_import, &nb_import_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBu", cmd_nb_url, &nb_url_help);

	// NO auto-start. The user must explicitly connect via NBc or NBs.
	const char *no_auto = getenv("RIZIN_NOTEBOOK_NO_AUTOSTART");
	if (no_auto && *no_auto == '1') {
		NB_LOG_INFO("notebook plugin loaded (sub-process mode, no auto-connect)");
	} else {
		NB_LOG_INFO("notebook plugin loaded (use NBc or NBs to connect)");
	}

	return true;
}

static bool rz_notebook_fini(RzCore *core) {
	(void)core;
	g_active_page[0] = '\0';
	g_connected = false;
	nb_http_fini();
	NB_LOG_INFO("notebook plugin unloaded");
	return true;
}

// ── Plugin Registration ─────────────────────────────────────────────────

static RzCorePlugin rz_core_plugin_notebook = {
	.name = "rz-notebook",
	.desc = "Rizin Notebook integration",
	.license = "LGPL-3.0-only",
	.author = "rizinorg",
	.version = "0.2.0",
	.init = rz_notebook_init,
	.fini = rz_notebook_fini,
};

#ifdef _MSC_VER
__declspec(dllexport)
#endif
RzLibStruct rizin_plugin = {
	.type = RZ_LIB_TYPE_CORE,
	.data = &rz_core_plugin_notebook,
	.version = RZ_VERSION,
};
