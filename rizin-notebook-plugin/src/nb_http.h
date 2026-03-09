// HTTP client for communicating with the notebook server.
// Uses WinHTTP on Windows, libcurl on other platforms.

#ifndef NB_HTTP_H
#define NB_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int      status;
	uint8_t *body;
	size_t   body_len;
	bool     ok;
	char    *error;
} NbHttpResponse;

void nb_http_response_free(NbHttpResponse *resp);

bool nb_http_init(void);

void nb_http_fini(void);

void nb_http_set_base_url(const char *base_url);

const char *nb_http_get_base_url(void);

bool nb_http_get(const char *path, NbHttpResponse *resp);

bool nb_http_post(const char *path, const uint8_t *body, size_t body_len,
                  NbHttpResponse *resp);

bool nb_http_put(const char *path, const uint8_t *body, size_t body_len,
                 NbHttpResponse *resp);

bool nb_http_delete(const char *path, NbHttpResponse *resp);

#ifdef __cplusplus
}
#endif

#endif // NB_HTTP_H
