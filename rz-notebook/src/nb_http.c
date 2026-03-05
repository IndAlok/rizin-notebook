#include "nb_http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ── Configuration ───────────────────────────────────────────────────────

static char g_base_url[512] = "http://127.0.0.1:8000";

void nb_http_set_base_url(const char *base_url) {
	if (!base_url) return;
	snprintf(g_base_url, sizeof(g_base_url), "%s", base_url);
	// Strip trailing slash.
	size_t len = strlen(g_base_url);
	while (len > 0 && g_base_url[len - 1] == '/') {
		g_base_url[--len] = '\0';
	}
}

const char *nb_http_get_base_url(void) {
	return g_base_url;
}

void nb_http_response_free(NbHttpResponse *resp) {
	if (!resp) return;
	free(resp->body);
	free(resp->error);
	resp->body = NULL;
	resp->error = NULL;
}

// ═══════════════════════════════════════════════════════════════════════
// Windows implementation using WinHTTP
// ═══════════════════════════════════════════════════════════════════════

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

static HINTERNET g_session = NULL;

bool nb_http_init(void) {
	if (g_session) return true;
	g_session = WinHttpOpen(L"rz-notebook/1.0",
	                        WINHTTP_ACCESS_TYPE_NO_PROXY,
	                        WINHTTP_NO_PROXY_NAME,
	                        WINHTTP_NO_PROXY_BYPASS, 0);
	if (!g_session) return false;

	// Set timeouts: 5s connect, 10s send/recv.
	WinHttpSetTimeouts(g_session, 5000, 5000, 10000, 10000);
	return true;
}

void nb_http_fini(void) {
	if (g_session) {
		WinHttpCloseHandle(g_session);
		g_session = NULL;
	}
}

static wchar_t *build_url_wide(const char *path) {
	char full[1024];
	snprintf(full, sizeof(full), "%s%s", g_base_url, path);

	int wlen = MultiByteToWideChar(CP_UTF8, 0, full, -1, NULL, 0);
	if (wlen <= 0) return NULL;
	wchar_t *wurl = malloc(wlen * sizeof(wchar_t));
	if (!wurl) return NULL;
	MultiByteToWideChar(CP_UTF8, 0, full, -1, wurl, wlen);
	return wurl;
}

static bool winhttp_request(const char *method, const char *path,
                            const uint8_t *body, size_t body_len,
                            NbHttpResponse *resp) {
	memset(resp, 0, sizeof(*resp));

	if (!g_session) {
		resp->error = _strdup("HTTP subsystem not initialized");
		return false;
	}

	wchar_t *url_w = build_url_wide(path);
	if (!url_w) {
		resp->error = _strdup("Failed to build URL");
		return false;
	}

	// Crack URL.
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.dwSchemeLength = (DWORD)-1;
	uc.dwHostNameLength = (DWORD)-1;
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwExtraInfoLength = (DWORD)-1;

	if (!WinHttpCrackUrl(url_w, 0, 0, &uc)) {
		resp->error = _strdup("Failed to parse URL");
		free(url_w);
		return false;
	}

	// Extract host.
	wchar_t host[256] = {0};
	if (uc.dwHostNameLength > 0 && uc.dwHostNameLength < 255) {
		wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength);
		host[uc.dwHostNameLength] = L'\0';
	}

	// Extract path (includes query string).
	wchar_t req_path[512] = {0};
	if (uc.dwUrlPathLength > 0) {
		size_t plen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
		if (plen < 511) {
			wcsncpy(req_path, uc.lpszUrlPath, plen);
			req_path[plen] = L'\0';
		}
	}

	// Convert method to wide.
	wchar_t method_w[16] = {0};
	MultiByteToWideChar(CP_UTF8, 0, method, -1, method_w, 16);

	HINTERNET conn = WinHttpConnect(g_session, host, uc.nPort, 0);
	if (!conn) {
		resp->error = _strdup("WinHttpConnect failed");
		free(url_w);
		return false;
	}

	HINTERNET req = WinHttpOpenRequest(conn, method_w, req_path,
	                                   NULL, WINHTTP_NO_REFERER,
	                                   WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!req) {
		resp->error = _strdup("WinHttpOpenRequest failed");
		WinHttpCloseHandle(conn);
		free(url_w);
		return false;
	}

	// Set content type header for protobuf.
	if (body && body_len > 0) {
		WinHttpAddRequestHeaders(req,
			L"Content-Type: application/x-protobuf\r\n",
			(DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	}
	WinHttpAddRequestHeaders(req,
		L"Accept: application/x-protobuf\r\n",
		(DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

	// Send.
	BOOL ok = WinHttpSendRequest(req,
	                              WINHTTP_NO_ADDITIONAL_HEADERS, 0,
	                              (LPVOID)body, (DWORD)body_len,
	                              (DWORD)body_len, 0);
	if (!ok) {
		DWORD err = GetLastError();
		char msg[128];
		snprintf(msg, sizeof(msg), "WinHttpSendRequest failed: %lu", err);
		resp->error = _strdup(msg);
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		free(url_w);
		return false;
	}

	ok = WinHttpReceiveResponse(req, NULL);
	if (!ok) {
		DWORD err = GetLastError();
		char msg[128];
		snprintf(msg, sizeof(msg), "WinHttpReceiveResponse failed: %lu", err);
		resp->error = _strdup(msg);
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		free(url_w);
		return false;
	}

	// Get status code.
	DWORD status_code = 0;
	DWORD sz = sizeof(status_code);
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                    WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz,
	                    WINHTTP_NO_HEADER_INDEX);
	resp->status = (int)status_code;

	// Read body.
	size_t total = 0;
	size_t cap = 4096;
	uint8_t *buf = malloc(cap);
	if (!buf) {
		resp->error = _strdup("Out of memory");
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		free(url_w);
		return false;
	}

	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;

		if (total + available > cap) {
			while (cap < total + available) cap *= 2;
			uint8_t *nb = realloc(buf, cap);
			if (!nb) break;
			buf = nb;
		}

		DWORD read = 0;
		if (!WinHttpReadData(req, buf + total, available, &read)) break;
		total += read;
	}

	resp->body = buf;
	resp->body_len = total;
	resp->ok = true;

	WinHttpCloseHandle(req);
	WinHttpCloseHandle(conn);
	free(url_w);
	return true;
}

bool nb_http_get(const char *path, NbHttpResponse *resp) {
	return winhttp_request("GET", path, NULL, 0, resp);
}

bool nb_http_post(const char *path, const uint8_t *body, size_t body_len,
                  NbHttpResponse *resp) {
	return winhttp_request("POST", path, body, body_len, resp);
}

bool nb_http_put(const char *path, const uint8_t *body, size_t body_len,
                 NbHttpResponse *resp) {
	return winhttp_request("PUT", path, body, body_len, resp);
}

bool nb_http_delete(const char *path, NbHttpResponse *resp) {
	return winhttp_request("DELETE", path, NULL, 0, resp);
}

#else
// ═══════════════════════════════════════════════════════════════════════
// Non-Windows implementation using libcurl
// ═══════════════════════════════════════════════════════════════════════

#include <curl/curl.h>

static CURL *g_curl = NULL;

bool nb_http_init(void) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	g_curl = curl_easy_init();
	return g_curl != NULL;
}

void nb_http_fini(void) {
	if (g_curl) {
		curl_easy_cleanup(g_curl);
		g_curl = NULL;
	}
	curl_global_cleanup();
}

struct curl_buf {
	uint8_t *data;
	size_t   len;
	size_t   cap;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
	struct curl_buf *cb = (struct curl_buf *)userdata;
	size_t total = size * nmemb;
	if (cb->len + total > cb->cap) {
		while (cb->cap < cb->len + total) cb->cap *= 2;
		uint8_t *nb = realloc(cb->data, cb->cap);
		if (!nb) return 0;
		cb->data = nb;
	}
	memcpy(cb->data + cb->len, ptr, total);
	cb->len += total;
	return total;
}

static bool curl_request(const char *method, const char *path,
                          const uint8_t *body, size_t body_len,
                          NbHttpResponse *resp) {
	memset(resp, 0, sizeof(*resp));
	if (!g_curl) {
		resp->error = strdup("HTTP subsystem not initialized");
		return false;
	}

	char url[1024];
	snprintf(url, sizeof(url), "%s%s", g_base_url, path);

	CURL *c = g_curl;
	curl_easy_reset(c);

	struct curl_buf cb = { .data = malloc(4096), .len = 0, .cap = 4096 };
	if (!cb.data) {
		resp->error = strdup("Out of memory");
		return false;
	}

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &cb);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/x-protobuf");

	if (body && body_len > 0) {
		headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(c);
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		resp->error = strdup(curl_easy_strerror(res));
		free(cb.data);
		return false;
	}

	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	resp->status = (int)code;
	resp->body = cb.data;
	resp->body_len = cb.len;
	resp->ok = true;
	return true;
}

bool nb_http_get(const char *path, NbHttpResponse *resp) {
	return curl_request("GET", path, NULL, 0, resp);
}

bool nb_http_post(const char *path, const uint8_t *body, size_t body_len,
                  NbHttpResponse *resp) {
	return curl_request("POST", path, body, body_len, resp);
}

bool nb_http_put(const char *path, const uint8_t *body, size_t body_len,
                 NbHttpResponse *resp) {
	return curl_request("PUT", path, body, body_len, resp);
}

bool nb_http_delete(const char *path, NbHttpResponse *resp) {
	return curl_request("DELETE", path, NULL, 0, resp);
}

#endif
