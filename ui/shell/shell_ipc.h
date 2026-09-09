/*
 * Tiny pocketipc convenience for the shell and in-process apps: one lazily
 * connected socket per service with automatic reconnect.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_IPC_H
#define POCKETOS_SHELL_IPC_H

#include "pocketipc/pocketipc.h"

/* The one deadline for everything that runs on the LVGL thread on a timer:
 * the shell's status poll and every app tick. Less than the tick that drives
 * them, so a service that has stopped answering costs at most one frame per
 * tick and never accumulates. Verified on unit A (M5, 2026-09-08): the poll
 * alone being bounded was not enough, because the Radio app's tick made the
 * same calls without a deadline and froze the panel until radiod answered. */
#ifndef SHELL_IPC_UI_TIMEOUT_MS
#define SHELL_IPC_UI_TIMEOUT_MS 200
#endif

/* Synchronous call; params consumed. Returns result (caller frees) or NULL
 * and fills err (may be empty when the service is unreachable). Waits for as
 * long as the service takes. Only for a request whose completion is the
 * point, made because the user asked for it: radio.send is the example, and
 * a deadline there would report failure for a packet that is on the air. */
cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params,
                      char *err, size_t errlen);
/* Same, giving up after timeout_ms (0 waits forever): the connection is
 * dropped on timeout and the next call reconnects, so a late answer is never
 * read as the reply to a later request. The deadline is one budget for the
 * whole operation, connecting included (a full listen backlog used to block
 * the reconnect with no deadline at all, unit A 2026-09-08). Anything on the
 * LVGL thread that runs on a timer, whether the shell's own poll or an app's
 * tick, uses this with SHELL_IPC_UI_TIMEOUT_MS. */
cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params,
                              int timeout_ms, char *err, size_t errlen);
/* Close all cached connections. */
void shell_ipc_shutdown(void);

#endif
