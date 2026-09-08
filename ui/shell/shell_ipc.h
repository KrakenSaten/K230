/*
 * Tiny pocketipc convenience for the shell and in-process apps: one lazily
 * connected socket per service with automatic reconnect.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_IPC_H
#define POCKETOS_SHELL_IPC_H

#include "pocketipc/pocketipc.h"

/* Synchronous call; params consumed. Returns result (caller frees) or NULL
 * and fills err (may be empty when the service is unreachable). Waits for as
 * long as the service takes; this is what apps use. */
cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params,
                      char *err, size_t errlen);
/* Same, giving up after timeout_ms (0 waits forever). For calls the shell
 * makes on the LVGL thread on a timer, where a service that has stopped
 * answering must cost one frame rather than the session: the connection is
 * dropped on timeout and the next tick reconnects.
 *
 * Not for anything an app initiates. A call whose completion is the point --
 * radio.send is the example -- must keep waiting, or the user is told the
 * packet failed while it is being transmitted. */
cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params,
                              int timeout_ms, char *err, size_t errlen);
/* Close all cached connections. */
void shell_ipc_shutdown(void);

#endif
