/*
 * Methods an app serves on shell.sock (app.h: ipc_prefix, ipc_request,
 * ipc_status, ipc_opens; docs/api/shell.md "Methods served by an app").
 *
 * This file is the part with no screen in it: whose method a name is, what a
 * closed app's method gets, whether the apps' declarations are sound, and
 * the counters. The shell (shell.c) does what these say.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_APP_IPC_H
#define POCKETOS_APP_IPC_H

#include "app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a handler may take before it is counted as slow. It runs on the LVGL
 * thread: this is a budget that is measured and reported, not a deadline the
 * shell could enforce - a synchronous call cannot be interrupted safely. */
#define APP_IPC_BUDGET_US 2000u
/* The largest result an app method may give, serialised. A larger one is
 * replaced by an error. One limit for every app. */
#define APP_IPC_RESULT_MAX 8192u
/* Entries read from an app's ipc_status or ipc_opens list. */
#define APP_IPC_LIST_MAX 16u

/* What a method gets while its app is not in front. */
enum app_ipc_closed {
    APP_IPC_CLOSED_REFUSE = 0, /* an error: the app is not open */
    APP_IPC_CLOSED_STATUS,     /* {"available": false}: a declared status method */
    APP_IPC_CLOSED_OPEN,       /* open the app, then ask it: a declared opening method */
};

struct app_ipc_stats {
    uint32_t calls;          /* handlers called */
    uint32_t slow;           /* of those, over APP_IPC_BUDGET_US */
    uint32_t worst_us;       /* the slowest */
    char worst_method[48];   /* and which it was */
    uint32_t too_large;      /* results over APP_IPC_RESULT_MAX, replaced by an error */
    uint32_t closed_status;  /* status methods answered for a closed app */
    uint32_t closed_refused; /* other methods refused because the app was not open */
    uint32_t opened;         /* apps opened by an opening method */
    uint32_t open_refused;   /* opening methods refused: locked, or an alarm showing */
    uint32_t events;         /* events an app raised */
    uint32_t events_dropped; /* events refused: not the raising app's prefix */
};

/* The app whose prefix `method` begins with, or NULL. */
const struct pocketos_app *app_ipc_owner(const struct pocketos_app *const *apps, size_t count,
                                         const char *method);
/* What `method` of `app` gets while the app is not in front. */
enum app_ipc_closed app_ipc_closed_rule(const struct pocketos_app *app, const char *method);
/* Whether `app` may raise `event`: only a name under its own prefix. */
bool app_ipc_event_allowed(const struct pocketos_app *app, const char *event);
/* Whether the declarations of all apps are sound: 0, or -1 with the reason
 * in `why`. Checked once at start; a shell with an unsound declaration does
 * not start. */
int app_ipc_check(const struct pocketos_app *const *apps, size_t count, char *why, size_t whylen);
/* One handler's time. */
void app_ipc_note_time(struct app_ipc_stats *st, const char *method, uint32_t us);

#endif
