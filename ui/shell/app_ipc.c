/*
 * Methods an app serves on shell.sock: the rules (app_ipc.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_ipc.h"

#include <stdio.h>
#include <string.h>

static const char SHELL_PREFIX[] = "shell.";

static bool begins(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool listed(const char *const *list, const char *method)
{
    size_t k;

    for (k = 0; list && k < APP_IPC_LIST_MAX && list[k]; k++) {
        if (strcmp(list[k], method) == 0) {
            return true;
        }
    }
    return false;
}

const struct pocketos_app *app_ipc_owner(const struct pocketos_app *const *apps, size_t count,
                                         const char *method)
{
    size_t k;

    if (!method || !method[0]) {
        return NULL;
    }
    for (k = 0; k < count; k++) {
        if (apps[k] && apps[k]->ipc_prefix && apps[k]->ipc_request &&
            begins(method, apps[k]->ipc_prefix)) {
            return apps[k];
        }
    }
    return NULL;
}

enum app_ipc_closed app_ipc_closed_rule(const struct pocketos_app *app, const char *method)
{
    if (!app || !method) {
        return APP_IPC_CLOSED_REFUSE;
    }
    if (listed(app->ipc_opens, method)) {
        return APP_IPC_CLOSED_OPEN;
    }
    if (listed(app->ipc_status, method)) {
        return APP_IPC_CLOSED_STATUS;
    }
    return APP_IPC_CLOSED_REFUSE;
}

bool app_ipc_event_allowed(const struct pocketos_app *app, const char *event)
{
    return app && app->ipc_prefix && app->ipc_request && event &&
           begins(event, app->ipc_prefix) && event[strlen(app->ipc_prefix)] != '\0';
}

/* A prefix is lower-case words joined by dots and ends in one: "capture." */
static bool prefix_shape(const char *p)
{
    size_t n = strlen(p);
    size_t k;

    if (n < 2 || n > 32 || p[0] == '.' || p[n - 1] != '.') {
        return false;
    }
    for (k = 0; k < n; k++) {
        char ch = p[k];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.')) {
            return false;
        }
        if (ch == '.' && k > 0 && p[k - 1] == '.') {
            return false;
        }
    }
    return true;
}

static int list_check(const struct pocketos_app *app, const char *const *list, const char *what,
                      char *why, size_t whylen)
{
    size_t k;

    for (k = 0; list && list[k]; k++) {
        if (k >= APP_IPC_LIST_MAX) {
            snprintf(why, whylen, "app %s: more than %u %s methods", app->id, APP_IPC_LIST_MAX, what);
            return -1;
        }
        if (!begins(list[k], app->ipc_prefix) || list[k][strlen(app->ipc_prefix)] == '\0') {
            snprintf(why, whylen, "app %s: %s method \"%s\" is not under its prefix \"%s\"", app->id,
                     what, list[k], app->ipc_prefix);
            return -1;
        }
    }
    return 0;
}

int app_ipc_check(const struct pocketos_app *const *apps, size_t count, char *why, size_t whylen)
{
    size_t a;
    size_t b;
    size_t k;

    if (why && whylen) {
        why[0] = '\0';
    }
    for (a = 0; a < count; a++) {
        const struct pocketos_app *app = apps[a];

        if (!app) {
            continue;
        }
        if (!app->ipc_prefix) {
            /* An app that serves nothing declares nothing. */
            if (app->ipc_request || app->ipc_status || app->ipc_opens) {
                snprintf(why, whylen, "app %s: serves methods without an ipc_prefix", app->id);
                return -1;
            }
            continue;
        }
        if (!app->ipc_request) {
            snprintf(why, whylen, "app %s: ipc_prefix \"%s\" without ipc_request", app->id, app->ipc_prefix);
            return -1;
        }
        if (!prefix_shape(app->ipc_prefix)) {
            snprintf(why, whylen, "app %s: ipc_prefix \"%s\" is not lower-case words ending in a dot",
                     app->id, app->ipc_prefix);
            return -1;
        }
        /* The shell's own names are the shell's: an app can neither take them
         * nor a name they would hide. */
        if (begins(app->ipc_prefix, SHELL_PREFIX) || begins(SHELL_PREFIX, app->ipc_prefix)) {
            snprintf(why, whylen, "app %s: ipc_prefix \"%s\" is in the shell's own names", app->id,
                     app->ipc_prefix);
            return -1;
        }
        if (list_check(app, app->ipc_status, "status", why, whylen) != 0 ||
            list_check(app, app->ipc_opens, "opening", why, whylen) != 0) {
            return -1;
        }
        /* A method is one or the other: answered for a closed app, or opening it. */
        for (k = 0; app->ipc_opens && app->ipc_opens[k]; k++) {
            if (listed(app->ipc_status, app->ipc_opens[k])) {
                snprintf(why, whylen, "app %s: \"%s\" is both a status and an opening method", app->id,
                         app->ipc_opens[k]);
                return -1;
            }
        }
        for (b = 0; b < a; b++) {
            const struct pocketos_app *other = apps[b];

            if (other && other->ipc_prefix &&
                (begins(app->ipc_prefix, other->ipc_prefix) || begins(other->ipc_prefix, app->ipc_prefix))) {
                snprintf(why, whylen, "apps %s and %s: ipc prefixes \"%s\" and \"%s\" overlap", other->id,
                         app->id, other->ipc_prefix, app->ipc_prefix);
                return -1;
            }
        }
    }
    return 0;
}

void app_ipc_note_time(struct app_ipc_stats *st, const char *method, uint32_t us)
{
    st->calls++;
    if (us > APP_IPC_BUDGET_US) {
        st->slow++;
    }
    if (us >= st->worst_us) {
        st->worst_us = us;
        snprintf(st->worst_method, sizeof(st->worst_method), "%s", method ? method : "");
    }
}
