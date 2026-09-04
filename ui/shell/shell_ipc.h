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
 * and fills err (may be empty when the service is unreachable). */
cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params,
                      char *err, size_t errlen);
/* Close all cached connections. */
void shell_ipc_shutdown(void);

#endif
