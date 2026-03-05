#include "nb_server.h"
#include "nb_http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#endif

// ── Health Check ────────────────────────────────────────────────────────

bool nb_server_is_alive(void) {
	NbHttpResponse resp;
	bool ok = nb_http_get("/api/v1/status", &resp);
	bool alive = ok && resp.ok && resp.status == 200;
	nb_http_response_free(&resp);
	return alive;
}

// ── Executable Discovery ────────────────────────────────────────────────

#ifdef _WIN32
static const char *NB_EXE_NAME = "rizin-notebook.exe";
#else
static const char *NB_EXE_NAME = "rizin-notebook";
#endif

static bool file_exists(const char *path) {
#ifdef _WIN32
	DWORD attr = GetFileAttributesA(path);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

char *nb_server_find_executable(void) {
	char buf[MAX_PATH + 1];

#ifdef _WIN32
	// 1. Next to the plugin DLL (same directory as rz_notebook.dll).
	{
		HMODULE hMod = NULL;
		// Try to get our own module handle.
		if (GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)nb_server_find_executable, &hMod)) {
			if (GetModuleFileNameA(hMod, buf, MAX_PATH)) {
				char *sep = strrchr(buf, '\\');
				if (sep) {
					sep[1] = '\0';
					strncat(buf, NB_EXE_NAME, MAX_PATH - strlen(buf));
					if (file_exists(buf)) return _strdup(buf);
				}
			}
		}
	}

	// 2. In PATH.
	{
		DWORD r = SearchPathA(NULL, NB_EXE_NAME, NULL, MAX_PATH, buf, NULL);
		if (r > 0 && r < MAX_PATH && file_exists(buf)) return _strdup(buf);
	}

	// 3. Common locations.
	{
		const char *appdata = getenv("LOCALAPPDATA");
		if (appdata) {
			snprintf(buf, sizeof(buf), "%s\\rizin-notebook\\%s", appdata, NB_EXE_NAME);
			if (file_exists(buf)) return _strdup(buf);
		}
	}
	{
		const char *progfiles = getenv("PROGRAMFILES");
		if (progfiles) {
			snprintf(buf, sizeof(buf), "%s\\rizin-notebook\\%s", progfiles, NB_EXE_NAME);
			if (file_exists(buf)) return _strdup(buf);
		}
	}

#else // POSIX

	// 1. In PATH (use which-like logic).
	{
		const char *path_env = getenv("PATH");
		if (path_env) {
			char *path_copy = strdup(path_env);
			char *saveptr = NULL;
			char *dir = strtok_r(path_copy, ":", &saveptr);
			while (dir) {
				snprintf(buf, sizeof(buf), "%s/%s", dir, NB_EXE_NAME);
				if (file_exists(buf)) {
					char *result = strdup(buf);
					free(path_copy);
					return result;
				}
				dir = strtok_r(NULL, ":", &saveptr);
			}
			free(path_copy);
		}
	}

	// 2. Common locations.
	{
		const char *home = getenv("HOME");
		if (home) {
			snprintf(buf, sizeof(buf), "%s/.local/bin/%s", home, NB_EXE_NAME);
			if (file_exists(buf)) return strdup(buf);
			snprintf(buf, sizeof(buf), "%s/go/bin/%s", home, NB_EXE_NAME);
			if (file_exists(buf)) return strdup(buf);
		}
	}
	{
		snprintf(buf, sizeof(buf), "/usr/local/bin/%s", NB_EXE_NAME);
		if (file_exists(buf)) return strdup(buf);
	}

#endif

	return NULL;
}

// ── Server Start ────────────────────────────────────────────────────────

#ifdef _WIN32

static HANDLE g_server_process = NULL;

static bool start_server_process(const char *exe_path) {
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	memset(&pi, 0, sizeof(pi));

	char cmdline[MAX_PATH + 32];
	snprintf(cmdline, sizeof(cmdline), "\"%s\"", exe_path);

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
	                    CREATE_NO_WINDOW | DETACHED_PROCESS,
	                    NULL, NULL, &si, &pi)) {
		return false;
	}

	g_server_process = pi.hProcess;
	CloseHandle(pi.hThread);
	return true;
}

#else

static pid_t g_server_pid = 0;

static bool start_server_process(const char *exe_path) {
	pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		// Child process: become daemon.
		setsid();
		// Redirect stdout/stderr to /dev/null.
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execl(exe_path, exe_path, (char *)NULL);
		_exit(127);
	}
	g_server_pid = pid;
	return true;
}

#endif

bool nb_server_wait_alive(int timeout_ms) {
	int elapsed = 0;
	int interval = 200; // ms

	while (elapsed < timeout_ms) {
		if (nb_server_is_alive()) return true;
#ifdef _WIN32
		Sleep(interval);
#else
		usleep(interval * 1000);
#endif
		elapsed += interval;
	}
	return false;
}

bool nb_server_ensure(const char *exe_path) {
	// Already running?
	if (nb_server_is_alive()) return true;

	// Find executable.
	char *found = NULL;
	if (exe_path && file_exists(exe_path)) {
		found = NULL; // Use the provided path directly.
	} else {
		found = nb_server_find_executable();
		if (!found) return false;
		exe_path = found;
	}

	// Start it.
	bool ok = start_server_process(exe_path);
	free(found);
	if (!ok) return false;

	// Wait up to 5 seconds for it to start.
	return nb_server_wait_alive(5000);
}

void nb_server_stop(void) {
	// The notebook server runs independently; we don't kill it.
	// It can be stopped by the user or will exit on its own.
#ifdef _WIN32
	if (g_server_process) {
		CloseHandle(g_server_process);
		g_server_process = NULL;
	}
#else
	g_server_pid = 0;
#endif
}
