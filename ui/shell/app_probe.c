/*
 * The probe: an app that exists only to be asked things, so that the shell's
 * hook for methods served by an app (app.h ipc_*, app_ipc.h) can be tested
 * without a real app serving any. Simulator only (POCKETOS_SHELL_TEST_HOOKS),
 * and even there only when POCKETOS_TEST_PROBE is set: it is in no launcher,
 * no app list and no build that goes on a card.
 *
 * It is also the only app that declares an opening method (ipc_opens). No
 * app of the product does: what may open an app from outside the unit is a
 * decision of its own (ADR-016 amendment 1, proposed).
 *
 * POCKETOS_TEST_PROBE=1 gives the probe. Any other value gives the probe and
 * one more app whose declaration is unsound in the way the value names
 * ("shell", "overlap", "norequest", "list", "both", "shape"), which the shell
 * must refuse to start with.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "app_probe.h"

#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS

#include "pocketipc/pocketipc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct probe {
    unsigned created;
    unsigned asked;
} probe;

static void *probe_create(lv_obj_t *root)
{
    lv_obj_t *label = lv_label_create(root);

    lv_label_set_text(label, "probe");
    probe.created++;
    probe.asked = 0;
    return &probe;
}

static void probe_destroy(void *priv)
{
    (void)priv;
}

/* A number given as a number, or as the text `pos call` passes. */
static long param_long(const cJSON *params, const char *key, long fallback)
{
    const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;

    if (cJSON_IsNumber(v)) {
        return (long)v->valuedouble;
    }
    if (cJSON_IsString(v) && v->valuestring[0]) {
        return strtol(v->valuestring, NULL, 10);
    }
    return fallback;
}

static int probe_request(void *priv, const char *method, const cJSON *params, cJSON **result,
                         const char **message)
{
    struct probe *p = priv;

    p->asked++;
    if (strcmp(method, "probe.status") == 0) {
        *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(*result, "available", true);
        cJSON_AddNumberToObject(*result, "created", p->created);
        cJSON_AddNumberToObject(*result, "asked", p->asked);
        return 0;
    }
    if (strcmp(method, "probe.echo") == 0 || strcmp(method, "probe.open") == 0) {
        const cJSON *text = params ? cJSON_GetObjectItemCaseSensitive(params, "text") : NULL;

        *result = cJSON_CreateObject();
        cJSON_AddStringToObject(*result, "text", cJSON_IsString(text) ? text->valuestring : "");
        cJSON_AddNumberToObject(*result, "created", p->created);
        return 0;
    }
    if (strcmp(method, "probe.big") == 0) {
        /* A result of about `bytes` bytes, for the limit. */
        long n = param_long(params, "bytes", 0);
        char *fill;

        if (n < 0 || n > 1000000) {
            *message = "bytes out of range";
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        fill = malloc((size_t)n + 1);
        if (!fill) {
            *message = "no memory";
            return POCKETIPC_ERR_BACKEND;
        }
        memset(fill, 'x', (size_t)n);
        fill[n] = '\0';
        *result = cJSON_CreateObject();
        cJSON_AddStringToObject(*result, "fill", fill);
        free(fill);
        return 0;
    }
    if (strcmp(method, "probe.slow") == 0) {
        /* What no real handler may do: wait. Here so that the shell's
         * measurement of a slow handler can be seen to work. */
        long ms = param_long(params, "ms", 0);
        struct timespec ts;

        if (ms < 0 || ms > 200) {
            *message = "ms out of range";
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        ts.tv_sec = 0;
        ts.tv_nsec = ms * 1000000L;
        nanosleep(&ts, NULL);
        *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(*result, "slept_ms", (double)ms);
        return 0;
    }
    if (strcmp(method, "probe.fail") == 0) {
        *message = "the probe says no";
        return POCKETIPC_ERR_POLICY;
    }
    if (strcmp(method, "probe.nothing") == 0) {
        /* Success with no result: the shell answers an empty object. */
        return 0;
    }
    if (strcmp(method, "probe.wrong") == 0) {
        /* A code no pocketipc error has, and a result left behind with it. */
        *result = cJSON_CreateObject();
        return 99;
    }
    if (strcmp(method, "probe.event") == 0) {
        cJSON *mine = cJSON_CreateObject();
        cJSON *other = cJSON_CreateObject();

        cJSON_AddNumberToObject(mine, "asked", p->asked);
        pocketos_shell_app_event("probe.ping", mine);
        /* Not the probe's to raise: dropped and counted. */
        pocketos_shell_app_event("shell.app", other);
        pocketos_shell_app_event("probe.", NULL);
        *result = cJSON_CreateObject();
        return 0;
    }
    *message = "unknown method";
    return POCKETIPC_ERR_UNKNOWN_METHOD;
}

static const char *const probe_status[] = { "probe.status", NULL };
static const char *const probe_opens[] = { "probe.open", NULL };

static const struct pocketos_app app_probe = {
    .id = "probe",
    .name = "Probe",
    .icon = "?",
    .create = probe_create,
    .destroy = probe_destroy,
    .ipc_prefix = "probe.",
    .ipc_request = probe_request,
    .ipc_status = probe_status,
    .ipc_opens = probe_opens,
};

/* The unsound ones. None is ever opened: the shell must not start. */
static const char *const bad_list_opens[] = { "other.open", NULL };
static const char *const bad_both[] = { "bad.x", NULL };

static const struct pocketos_app bad_shell = { .id = "bad", .name = "Bad", .icon = "?", .create = probe_create,
                                               .destroy = probe_destroy, .ipc_prefix = "shell.bad.",
                                               .ipc_request = probe_request };
static const struct pocketos_app bad_overlap = { .id = "bad", .name = "Bad", .icon = "?", .create = probe_create,
                                                 .destroy = probe_destroy, .ipc_prefix = "probe.sub.",
                                                 .ipc_request = probe_request };
static const struct pocketos_app bad_norequest = { .id = "bad", .name = "Bad", .icon = "?",
                                                   .create = probe_create, .destroy = probe_destroy,
                                                   .ipc_prefix = "bad." };
static const struct pocketos_app bad_list = { .id = "bad", .name = "Bad", .icon = "?", .create = probe_create,
                                              .destroy = probe_destroy, .ipc_prefix = "bad.",
                                              .ipc_request = probe_request, .ipc_opens = bad_list_opens };
static const struct pocketos_app bad_both_app = { .id = "bad", .name = "Bad", .icon = "?", .create = probe_create,
                                                  .destroy = probe_destroy, .ipc_prefix = "bad.",
                                                  .ipc_request = probe_request, .ipc_status = bad_both,
                                                  .ipc_opens = bad_both };
static const struct pocketos_app bad_shape = { .id = "bad", .name = "Bad", .icon = "?", .create = probe_create,
                                               .destroy = probe_destroy, .ipc_prefix = "Bad",
                                               .ipc_request = probe_request };

size_t app_probe_apps(const struct pocketos_app **out, size_t max)
{
    const char *want = getenv("POCKETOS_TEST_PROBE");
    size_t n = 0;

    if (!want || !want[0] || max < 2) {
        return 0;
    }
    out[n++] = &app_probe;
    if (strcmp(want, "shell") == 0) {
        out[n++] = &bad_shell;
    } else if (strcmp(want, "overlap") == 0) {
        out[n++] = &bad_overlap;
    } else if (strcmp(want, "norequest") == 0) {
        out[n++] = &bad_norequest;
    } else if (strcmp(want, "list") == 0) {
        out[n++] = &bad_list;
    } else if (strcmp(want, "both") == 0) {
        out[n++] = &bad_both_app;
    } else if (strcmp(want, "shape") == 0) {
        out[n++] = &bad_shape;
    }
    return n;
}

#else

size_t app_probe_apps(const struct pocketos_app **out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

#endif
