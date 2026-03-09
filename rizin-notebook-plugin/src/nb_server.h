// Server auto-start and lifecycle management.

#ifndef NB_SERVER_H
#define NB_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool nb_server_is_alive(void);

bool nb_server_ensure(const char *exe_path);

char *nb_server_find_executable(void);

bool nb_server_wait_alive(int timeout_ms);

void nb_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // NB_SERVER_H
