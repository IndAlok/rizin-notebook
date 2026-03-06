// Rizin core plugin: NB command group for notebook server interaction.
//
// Commands: NBl NBn NBp NBd NBac NBam NBas NBx NBxs NBo NBc NBs NBu

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

// ── Helpers ─────────────────────────────────────────────────────────────

static bool ensure_server(void) {
	if (nb_server_is_alive()) {
		return true;
	}
	NB_LOG_INFO("server not running, attempting to start...");
	if (!nb_server_ensure(NULL)) {
		NB_LOG_ERR("failed to start the notebook server");
		return false;
	}
	NB_LOG_INFO("server started successfully");
	return true;
}

static const char *cell_type_str(Notebook__CellType t) {
	switch (t) {
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_COMMAND:  return "cmd";
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_SCRIPT:   return "script";
	case NOTEBOOK__CELL_TYPE__CELL_TYPE_MARKDOWN: return "markdown";
	default: return "?";
	}
}

// ── Command Handlers ────────────────────────────────────────────────────

static RzCmdStatus cmd_nb_status(RzCore *core, int argc, const char **argv) {
	(void)argc; (void)argv;
	if (!ensure_server()) {
		rz_cons_println("Server is not running and could not be started.");
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
	rz_cons_printf("  Version:    %s\n", st->version);
	rz_cons_printf("  Rizin:      %s\n", st->rizin_version);
	rz_cons_printf("  Rizin Path: %s\n", st->rizin_path);
	rz_cons_printf("  Storage:    %s\n", st->storage);
	rz_cons_printf("  Pages:      %d\n", st->pages);
	rz_cons_printf("  Open Pipes: %d\n", st->open_pipes);

	notebook__status_response__free_unpacked(st, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_list(RzCore *core, int argc, const char **argv) {
	(void)argc; (void)argv;
	if (!ensure_server()) {
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
		rz_cons_printf("%-36s  %-30s  %s\n", "ID", "Title", "Cells");
		rz_cons_printf("%-36s  %-30s  %s\n",
		               "------------------------------------",
		               "------------------------------", "-----");
		for (size_t i = 0; i < lp->n_pages; i++) {
			Notebook__Page *p = lp->pages[i];
			rz_cons_printf("%-36s  %-30s  %zu\n",
			               p->id, p->title, p->n_cells);
		}
	}

	notebook__list_pages_response__free_unpacked(lp, NULL);
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_new(RzCore *core, int argc, const char **argv) {
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBn <title> [binary-file]");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	const char *title = argv[1];
	const char *filename = argc > 2 ? argv[2] : NULL;

	// If a binary file is specified, try to read it.
	uint8_t *bin_data = NULL;
	size_t bin_len = 0;
	if (filename) {
		size_t rlen = 0;
		bin_data = (uint8_t *)rz_file_slurp(filename, &rlen);
		if (!bin_data) {
			rz_cons_printf("Warning: could not read '%s', using current binary\n", filename);
			if (core->io) {
				int cur_fd = rz_io_fd_get_current(core->io);
				RzIODesc *cur_desc = rz_io_desc_get(core->io, cur_fd);
				filename = cur_desc ? cur_desc->name : NULL;
			} else {
				filename = NULL;
			}
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

static RzCmdStatus cmd_nb_page(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBp <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s", argv[1]);

	NbHttpResponse resp;
	if (!nb_http_get(path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status == 404) {
		rz_cons_printf("Page not found: %s\n", argv[1]);
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
	rz_cons_printf("  ID:       %s\n", p->id);
	rz_cons_printf("  File:     %s\n", p->filename[0] ? p->filename : "-");
	rz_cons_printf("  Binary:   %s\n", p->binary[0] ? p->binary : "-");
	rz_cons_printf("  Pipe:     %s\n", p->pipe ? "open" : "closed");
	rz_cons_printf("  Cells:    %zu\n", p->n_cells);

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

static RzCmdStatus cmd_nb_delete(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBd <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s", argv[1]);

	NbHttpResponse resp;
	if (!nb_http_delete(path, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	if (resp.status == 200) {
		rz_cons_printf("Deleted page: %s\n", argv[1]);
	} else if (resp.status == 404) {
		rz_cons_printf("Page not found: %s\n", argv[1]);
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_add_cell(const char *page_id, Notebook__CellType type,
                                    const char *content, const char *type_name) {
	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/cells", page_id);

	Notebook__AddCellRequest req = NOTEBOOK__ADD_CELL_REQUEST__INIT;
	req.page_id = (char *)page_id;
	req.type = type;
	req.content = (char *)content;

	size_t packed_len = notebook__add_cell_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__add_cell_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200 || resp.status == 201) {
		rz_cons_printf("Added %s cell to page %s\n", type_name, page_id);
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_add_cmd(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 3) {
		rz_cons_println("Usage: NBac <page-id> <command>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}
	return cmd_nb_add_cell(argv[1], NOTEBOOK__CELL_TYPE__CELL_TYPE_COMMAND,
	                       argv[2], "command");
}

static RzCmdStatus cmd_nb_add_md(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 3) {
		rz_cons_println("Usage: NBam <page-id> <markdown-text>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}
	return cmd_nb_add_cell(argv[1], NOTEBOOK__CELL_TYPE__CELL_TYPE_MARKDOWN,
	                       argv[2], "markdown");
}

static RzCmdStatus cmd_nb_add_script(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 3) {
		rz_cons_println("Usage: NBas <page-id> <script-code>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}
	return cmd_nb_add_cell(argv[1], NOTEBOOK__CELL_TYPE__CELL_TYPE_SCRIPT,
	                       argv[2], "script");
}

static RzCmdStatus cmd_nb_exec(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 3) {
		rz_cons_println("Usage: NBx <page-id> <command>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/exec", argv[1]);

	Notebook__ExecCommandRequest req = NOTEBOOK__EXEC_COMMAND_REQUEST__INIT;
	req.page_id = (char *)argv[1];
	req.command = (char *)argv[2];

	size_t packed_len = notebook__exec_command_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__exec_command_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200) {
		Notebook__ExecCommandResponse *er =
			notebook__exec_command_response__unpack(NULL, resp.body_len, resp.body);
		if (er) {
			if (er->cell && er->cell->output.len > 0) {
				rz_cons_memcat((const char *)er->cell->output.data,
				               er->cell->output.len);
				rz_cons_newline();
			}
			if (er->error && er->error[0]) {
				rz_cons_printf("Error: %s\n", er->error);
			}
			notebook__exec_command_response__free_unpacked(er, NULL);
		}
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_exec_script(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 3) {
		rz_cons_println("Usage: NBxs <page-id> <script>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/script", argv[1]);

	Notebook__ExecScriptRequest req = NOTEBOOK__EXEC_SCRIPT_REQUEST__INIT;
	req.page_id = (char *)argv[1];
	req.script = (char *)argv[2];

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

static RzCmdStatus cmd_nb_pipe_open(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBo <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/pipe/open", argv[1]);

	Notebook__PipeRequest req = NOTEBOOK__PIPE_REQUEST__INIT;
	req.page_id = (char *)argv[1];

	size_t packed_len = notebook__pipe_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__pipe_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200) {
		Notebook__PipeResponse *pr =
			notebook__pipe_response__unpack(NULL, resp.body_len, resp.body);
		if (pr) {
			if (pr->open) {
				rz_cons_printf("Pipe opened for page %s\n", argv[1]);
			} else {
				rz_cons_printf("Failed to open pipe: %s\n",
				               (pr->error && pr->error[0]) ? pr->error : "unknown");
			}
			notebook__pipe_response__free_unpacked(pr, NULL);
		}
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

static RzCmdStatus cmd_nb_pipe_close(RzCore *core, int argc, const char **argv) {
	(void)core;
	if (!ensure_server()) {
		return RZ_CMD_STATUS_ERROR;
	}
	if (argc < 2) {
		rz_cons_println("Usage: NBc <page-id>");
		return RZ_CMD_STATUS_WRONG_ARGS;
	}

	char path[256];
	snprintf(path, sizeof(path), "/api/v1/pages/%s/pipe/close", argv[1]);

	Notebook__PipeRequest req = NOTEBOOK__PIPE_REQUEST__INIT;
	req.page_id = (char *)argv[1];

	size_t packed_len = notebook__pipe_request__get_packed_size(&req);
	uint8_t *packed = malloc(packed_len);
	if (!packed) {
		return RZ_CMD_STATUS_ERROR;
	}
	notebook__pipe_request__pack(&req, packed);

	NbHttpResponse resp;
	if (!nb_http_post(path, packed, packed_len, &resp) || !resp.ok) {
		rz_cons_printf("Error: %s\n", resp.error ? resp.error : "request failed");
		free(packed);
		nb_http_response_free(&resp);
		return RZ_CMD_STATUS_ERROR;
	}
	free(packed);

	if (resp.status == 200) {
		rz_cons_printf("Pipe closed for page %s\n", argv[1]);
	} else {
		rz_cons_printf("Server error: HTTP %d\n", resp.status);
	}
	nb_http_response_free(&resp);
	return RZ_CMD_STATUS_OK;
}

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
	.description = "Interact with the rizin-notebook server to manage analysis notebooks.",
	.args = nb_no_args,
};

static const RzCmdDescHelp nb_status_help = {
	.summary = "Show notebook server status",
	.args = nb_no_args,
};

static const RzCmdDescHelp nb_list_help = {
	.summary = "List notebook pages",
	.args = nb_no_args,
};

static const RzCmdDescArg nb_new_args[] = {
	{ .name = "title", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "binary-file", .type = RZ_CMD_ARG_TYPE_FILE, .optional = true },
	{ 0 },
};
static const RzCmdDescHelp nb_new_help = {
	.summary = "Create a new notebook page",
	.args = nb_new_args,
};

static const RzCmdDescArg nb_page_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_page_help = {
	.summary = "Show a notebook page and its cells",
	.args = nb_page_args,
};

static const RzCmdDescArg nb_delete_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_delete_help = {
	.summary = "Delete a notebook page",
	.args = nb_delete_args,
};

static const RzCmdDescArg nb_add_cmd_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "command", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_add_cmd_help = {
	.summary = "Add a command cell to a page",
	.args = nb_add_cmd_args,
};

static const RzCmdDescArg nb_add_md_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "markdown-text", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_add_md_help = {
	.summary = "Add a markdown cell to a page",
	.args = nb_add_md_args,
};

static const RzCmdDescArg nb_add_script_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "script-code", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_add_script_help = {
	.summary = "Add a script cell to a page",
	.args = nb_add_script_args,
};

static const RzCmdDescArg nb_exec_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "command", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_exec_help = {
	.summary = "Execute a command and record in the notebook",
	.args = nb_exec_args,
};

static const RzCmdDescArg nb_exec_script_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ .name = "script", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_exec_script_help = {
	.summary = "Execute a script and record in the notebook",
	.args = nb_exec_script_args,
};

static const RzCmdDescArg nb_pipe_open_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_pipe_open_help = {
	.summary = "Open a rizin pipe for a page",
	.args = nb_pipe_open_args,
};

static const RzCmdDescArg nb_pipe_close_args[] = {
	{ .name = "page-id", .type = RZ_CMD_ARG_TYPE_STRING },
	{ 0 },
};
static const RzCmdDescHelp nb_pipe_close_help = {
	.summary = "Close a rizin pipe for a page",
	.args = nb_pipe_close_args,
};

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
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBs", cmd_nb_status, &nb_status_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBl", cmd_nb_list, &nb_list_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBn", cmd_nb_new, &nb_new_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBp", cmd_nb_page, &nb_page_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBd", cmd_nb_delete, &nb_delete_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBac", cmd_nb_add_cmd, &nb_add_cmd_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBam", cmd_nb_add_md, &nb_add_md_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBas", cmd_nb_add_script, &nb_add_script_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBx", cmd_nb_exec, &nb_exec_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBxs", cmd_nb_exec_script, &nb_exec_script_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBo", cmd_nb_pipe_open, &nb_pipe_open_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBc", cmd_nb_pipe_close, &nb_pipe_close_help);
	rz_cmd_desc_argv_new(cmd, nb_cd, "NBu", cmd_nb_url, &nb_url_help);

	// Proactively try to ensure the server is reachable on plugin load.
	// If this fails, individual commands will retry and show concrete errors.
	// Skip auto-start when loaded inside a sub-process spawned by the Go
	// server itself (RIZIN_NOTEBOOK_NO_AUTOSTART=1), to avoid recursive
	// process spawning and unnecessary error spam.
	const char *no_auto = getenv("RIZIN_NOTEBOOK_NO_AUTOSTART");
	if (no_auto && *no_auto == '1') {
		NB_LOG_INFO("auto-start skipped (RIZIN_NOTEBOOK_NO_AUTOSTART=1)");
	} else if (!ensure_server()) {
		NB_LOG_INFO("server was not reachable during init; commands will retry startup");
	}

	NB_LOG_INFO("notebook plugin loaded (server: %s)", nb_http_get_base_url());
	return true;
}

static bool rz_notebook_fini(RzCore *core) {
	(void)core;
	nb_server_stop();
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
	.version = "0.1.0",
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
