/*
 * RIFT's meshcored client: repeater control. See rift_ipc.h and
 * rift_repeater.h.
 *
 * Four of these transmit, each from exactly one function and each reached
 * only from a button a reader pressed (tests/rift_lint.sh):
 *
 *   rift_ipc_scan_repeaters   mesh.discover: one zero-hop request; refused
 *                             here, unwritten, while a round is open
 *   rift_ipc_repeater_login   mesh.remote_login
 *   rift_ipc_repeater_ask     mesh.remote_request (status, neighbours, owner)
 *   rift_ipc_repeater_cli     mesh.remote_cli, only for a command the rule in
 *                             rift_repeater.h does not refuse
 *
 * Two do not: mesh.remote_logout (MeshCore has no logout on the air) and the
 * questions mesh.discovered and mesh.remote_session. The method names are the
 * connection's (rift_ipc.c, method_of); this file names none.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_ipc.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

/* Write one repeater request, recorded in the model first. params is
 * consumed. Returns 0 when it went out. */
static int rep_request(struct rift_ipc *c, enum rift_req what, enum rift_rep_kind kind,
                       cJSON *params)
{
    struct rift_repeater *r = &c->model->repeater;
    int64_t now = rift_mono_ms();

    if (rift_rep_begin(r, kind, now) != 0) {
        /* One is already waiting: meshcored would refuse it too, and a
         * second press must not become a second packet. */
        cJSON_Delete(params);
        return -1;
    }
    if (c->fd < 0) {
        cJSON_Delete(params);
        rift_rep_refused(r, kind, "meshcored is not answering");
        c->revision++;
        return -1;
    }
    if (!params) {
        rift_rep_refused(r, kind, "out of memory");
        c->revision++;
        return -1;
    }
    if (rift_ipc_write(c, what, params, now) != 0) {
        rift_rep_refused(r, kind, "the request could not be written");
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

static cJSON *node_params(const char *key)
{
    cJSON *params;

    if (!key || strlen(key) != 64) {
        return NULL;
    }
    params = cJSON_CreateObject();
    if (params) {
        cJSON_AddStringToObject(params, "node", key);
    }
    return params;
}

int rift_ipc_scan_repeaters(struct rift_ipc *c)
{
    struct rift_repeater *r;
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    r = &c->model->repeater;
    now = rift_mono_ms();
    /* The open round is collecting answers; pressing again changes nothing
     * and puts nothing on the air. */
    if (rift_rep_scanning(r, now) || r->scan.unsupported) {
        return -1;
    }
    if (c->fd < 0) {
        snprintf(r->scan.error, sizeof(r->scan.error), "meshcored is not answering");
        c->revision++;
        return -1;
    }
    r->scan.asking = 1;
    r->scan.error[0] = '\0';
    /* The one place this app asks repeaters to answer. */
    if (rift_ipc_write(c, RIFT_REQ_DISCOVER, NULL, now) != 0) {
        r->scan.asking = 0;
        snprintf(r->scan.error, sizeof(r->scan.error), "the request could not be written");
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

int rift_ipc_request_discovered(struct rift_ipc *c)
{
    if (!c || c->fd < 0) {
        return -1;
    }
    return rift_ipc_write(c, RIFT_REQ_DISCOVERED, NULL, rift_mono_ms());
}

int rift_ipc_repeater_login(struct rift_ipc *c, const char *key, char *password, size_t cap)
{
    cJSON *params;
    int rc;

    if (!c || !c->model || !password) {
        return -1;
    }
    params = node_params(key);
    if (params) {
        /* A reference, not a copy: cJSON does not free it, and the one
         * buffer that holds the password is the caller's, wiped below
         * whatever happened. */
        cJSON_AddItemToObject(params, "password", cJSON_CreateStringReference(password));
    }
    if (strlen(password) > RIFT_REP_PASSWORD_MAX) {
        cJSON_Delete(params);
        params = NULL;
        rc = -1;
        rift_rep_refused(&c->model->repeater, RIFT_REP_LOGIN,
                         "a repeater password is at most 15 bytes");
        c->revision++;
    } else {
        rift_rep_set_target(&c->model->repeater, key);
        rc = rep_request(c, RIFT_REQ_REMOTE_LOGIN, RIFT_REP_LOGIN, params);
    }
    {
        volatile char *p = password;
        size_t i;

        for (i = 0; i < cap; i++) {
            p[i] = '\0';
        }
    }
    return rc;
}

int rift_ipc_repeater_ask(struct rift_ipc *c, const char *key, enum rift_rep_kind kind)
{
    cJSON *params;
    const char *word = kind == RIFT_REP_STATUS       ? "status"
                       : kind == RIFT_REP_NEIGHBOURS ? "neighbours"
                       : kind == RIFT_REP_OWNER      ? "owner"
                                                     : NULL;

    if (!c || !c->model || !word) {
        return -1;
    }
    params = node_params(key);
    if (params) {
        cJSON_AddStringToObject(params, "kind", word);
    }
    return rep_request(c, RIFT_REQ_REMOTE_REQUEST, kind, params);
}

int rift_ipc_repeater_cli(struct rift_ipc *c, const char *key, const char *command)
{
    const char *why = NULL;
    cJSON *params;

    if (!c || !c->model || !command) {
        return -1;
    }
    while (*command == ' ') {
        command++;
    }
    /* The rule, applied here too and not only in the screen: whatever
     * reaches this call, a refused command never becomes a request. */
    if (rift_rep_cli_class(command, &why) == RIFT_CLI_REFUSED || !command[0]) {
        rift_rep_refused(&c->model->repeater, RIFT_REP_CLI, why ? why : "empty command");
        c->revision++;
        return -1;
    }
    params = node_params(key);
    if (params) {
        cJSON_AddStringToObject(params, "command", command);
    }
    if (rep_request(c, RIFT_REQ_REMOTE_CLI, RIFT_REP_CLI, params) != 0) {
        return -1;
    }
    rift_rep_note_command(&c->model->repeater, command);
    return 0;
}

int rift_ipc_repeater_logout(struct rift_ipc *c)
{
    struct rift_repeater *r;
    int was;

    if (!c || !c->model) {
        return -1;
    }
    r = &c->model->repeater;
    was = r->active || r->asking == RIFT_REP_LOGIN;
    /* Forgotten here at once, whatever the service says: the page must not
     * go on showing a session the reader ended. Nothing goes on the air -
     * MeshCore has no logout packet. */
    rift_rep_session_clear(r);
    if (was) {
        snprintf(r->note, sizeof(r->note), "LOGGED OUT" RIFT_SEP "nothing is sent for a logout");
        r->note_is_error = 0;
    }
    c->revision++;
    if (c->fd < 0) {
        return -1;
    }
    return rift_ipc_write(c, RIFT_REQ_REMOTE_LOGOUT, NULL, rift_mono_ms());
}

static enum rift_rep_kind kind_of_req(enum rift_req what, const struct rift_repeater *r)
{
    switch (what) {
    case RIFT_REQ_REMOTE_LOGIN:
        return RIFT_REP_LOGIN;
    case RIFT_REQ_REMOTE_REQUEST:
    case RIFT_REQ_REMOTE_CLI:
        return r->asking;
    default:
        return RIFT_REP_NONE;
    }
}

int rift_ipc_repeater_reply(struct rift_ipc *c, enum rift_req what, const cJSON *msg)
{
    struct rift_repeater *r = &c->model->repeater;
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    int64_t now = rift_mono_ms();

    switch (what) {
    case RIFT_REQ_DISCOVER:
    case RIFT_REQ_DISCOVERED:
    case RIFT_REQ_REMOTE_LOGIN:
    case RIFT_REQ_REMOTE_REQUEST:
    case RIFT_REQ_REMOTE_CLI:
    case RIFT_REQ_REMOTE_LOGOUT:
    case RIFT_REQ_REMOTE_SESSION:
        break;
    default:
        return 0;
    }
    if (cJSON_IsObject(error)) {
        const char *why = str_of(error, "message");
        double code = 0;

        c->errors_in++;
        (void)num_of(error, "code", &code);
        /* "unknown method": a meshcored from before repeater control. The
         * screen says so instead of offering buttons that cannot work. */
        if (code == POCKETIPC_ERR_UNKNOWN_METHOD) {
            rift_rep_mark_unsupported(r, what == RIFT_REQ_DISCOVER || what == RIFT_REQ_DISCOVERED);
            return 1;
        }
        if (what == RIFT_REQ_DISCOVER) {
            r->scan.asking = 0;
            snprintf(r->scan.error, sizeof(r->scan.error), "%s", why ? why : "refused");
        } else if (kind_of_req(what, r) != RIFT_REP_NONE) {
            rift_rep_refused(r, kind_of_req(what, r), why);
        }
        return 1;
    }
    switch (what) {
    case RIFT_REQ_DISCOVER:
        rift_rep_apply_discover(r, result);
        break;
    case RIFT_REQ_DISCOVERED:
        rift_rep_apply_discovered(r, result);
        break;
    case RIFT_REQ_REMOTE_LOGIN:
    case RIFT_REQ_REMOTE_REQUEST:
    case RIFT_REQ_REMOTE_CLI:
        rift_rep_accepted(r, result, now);
        break;
    case RIFT_REQ_REMOTE_LOGOUT:
        rift_rep_apply_session(r, cJSON_GetObjectItemCaseSensitive(result, "session"));
        break;
    case RIFT_REQ_REMOTE_SESSION:
        rift_rep_apply_session(r, result);
        break;
    default:
        break;
    }
    return 1;
}

void rift_ipc_repeater_poll(struct rift_ipc *c, int64_t now_ms)
{
    struct rift_repeater *r;

    if (!c || !c->model) {
        return;
    }
    r = &c->model->repeater;
    if (rift_rep_expire(r, now_ms)) {
        c->revision++;
    }
    /* The repeater has gone from the node list the service holds (it was
     * forgotten, or the table was replaced): the session cannot outlive the
     * contact it was with. meshcored ends it too when the contact is
     * forgotten; this covers a list read after the fact. */
    if (r->active && c->model->snapshot_valid && !rift_model_find(c->model, r->key)) {
        (void)rift_ipc_repeater_logout(c);
        snprintf(r->note, sizeof(r->note), "THE REPEATER LEFT THE NODE LIST" RIFT_SEP
                                           "logged out");
        r->note_is_error = 1;
    }
}

void rift_ipc_repeater_leave(struct rift_ipc *c)
{
    const struct rift_repeater *r = (c && c->model) ? &c->model->repeater : NULL;

    /* Leaving RIFT ends a repeater session: nobody is looking at it, and a
     * login left standing is one nobody remembers making. */
    if (r && (r->active || r->asking != RIFT_REP_NONE)) {
        (void)rift_ipc_repeater_logout(c);
    }
}
