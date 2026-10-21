/*
 * RIFT's meshcored client: the explicit route (rift_ipc.h, rift_route.h).
 * Kept apart from rift_ipc.c, which owns the connection and the one writer
 * of mesh.send (rift_ipc_write_send), so neither file is everything.
 *
 * Two requests come from here:
 *
 *   - mesh.path_preview for the route builder's draft. A question: it
 *     transmits nothing, creates no message and changes no learned route
 *     (docs/api/mesh.md). Its answer is applied only to the draft it was
 *     asked about (rift_route_check_answered), so a late answer cannot mark
 *     a different route valid.
 *
 *   - a SEND on the route staged on a conversation, from COMMS' submit and
 *     nowhere else (tests/rift_lint.sh). It is a submission from the first
 *     moment - the composer waits, a second press is refused - but before
 *     mesh.send is written the staged route is previewed again, against the
 *     node list the service holds now. Only when that answer, and only that
 *     one, says the route is valid is mesh.send written, with exactly the
 *     keys that were checked, in their order. A refusal, a service that went,
 *     or an answer that is not the service's "valid" ends the submission with
 *     nothing sent and the reader's text still in the composer. Nothing here
 *     ever writes a mesh.send without `via` for an explicit route.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_ipc.h"

#include "rift_format.h"

#include <stdio.h>
#include <string.h>

/* The relays as mesh.path_preview and mesh.send take them: an array of whole
 * public keys, in forwarding order. */
static cJSON *via_json(const char (*via)[RIFT_ROUTE_KEY], int count)
{
    cJSON *a = cJSON_CreateArray();
    int i;

    for (i = 0; a && i < count; i++) {
        cJSON *s = cJSON_CreateString(via[i]);

        if (!s) {
            cJSON_Delete(a);
            return NULL;
        }
        cJSON_AddItemToArray(a, s);
    }
    return a;
}

/* Write one mesh.path_preview. Returns the request's id, or -1. */
static int write_preview(struct rift_ipc *c, const char *to, const char (*via)[RIFT_ROUTE_KEY],
                         int count, int64_t now)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *arr = via_json(via, count);
    int id;

    if (!params || !arr) {
        cJSON_Delete(params);
        cJSON_Delete(arr);
        return -1;
    }
    cJSON_AddStringToObject(params, "to", to);
    cJSON_AddItemToObject(params, "via", arr);
    /* request() numbers it with next_id, and keeps the number in the pending
     * table; the answer is matched on it. */
    id = c->next_id;
    if (rift_ipc_write(c, RIFT_REQ_PATH_PREVIEW, params, now) != 0) {
        return -1;
    }
    return id;
}

int rift_ipc_path_preview(struct rift_ipc *c)
{
    struct rift_route *r;
    int id;

    if (!c || !c->model || c->fd < 0) {
        return -1;
    }
    r = &c->model->route;
    if (!rift_route_wants_check(r)) {
        return -1;
    }
    id = write_preview(c, r->draft.to, (const char(*)[RIFT_ROUTE_KEY])r->draft.via,
                       r->draft.count, rift_mono_ms());
    if (id < 0) {
        return -1;
    }
    rift_route_check_sent(r, id);
    c->revision++;
    return 0;
}

/* A send that ends before mesh.send was written: nothing went on the air. */
static void check_failed(struct rift_model *m, const char *why)
{
    m->outbox.checking = 0;
    rift_model_send_failed(m, why);
}

static int refuse(struct rift_ipc *c, const char *why)
{
    rift_model_send_refused(c->model, why);
    c->revision++;
    return -1;
}

int rift_ipc_send_explicit(struct rift_ipc *c, const char *conv_key, const char *text)
{
    char why[RIFT_TEXT_MAX];
    struct rift_model *m;
    struct rift_route *r;
    const struct rift_route_stage *s;
    int64_t now;
    int id;
    int i;

    if (!c || !c->model) {
        return -1;
    }
    m = c->model;
    r = &m->route;
    s = rift_route_staged(r, conv_key);
    if (!s) {
        return refuse(c, "no route is set for this conversation; nothing was sent");
    }
    if (rift_send_text_check(text, why, sizeof(why)) != 0) {
        return refuse(c, why);
    }
    if (c->fd < 0) {
        return refuse(c, "meshcored is not answering; nothing was sent");
    }
    if (rift_model_sending(m)) {
        return -1; /* the one in flight keeps its caption, and its reply */
    }
    /* Never `via` to a service that has not said it takes it: an older one
     * ignores the member and sends on its automatic route. */
    if (!rift_route_supported(r)) {
        return refuse(c, r->caps_known
                             ? "this meshcored does not take a chosen route; nothing was sent"
                             : "meshcored has not said it takes a chosen route; nothing was sent");
    }
    for (i = 0; i < s->count; i++) {
        if (!rift_model_find(m, s->via[i])) {
            return refuse(c, "a relay of the route has left the node list; nothing was sent");
        }
    }
    now = rift_mono_ms();
    if (rift_model_send_begin(m, conv_key, text, now) != 0) {
        return refuse(c, "that is not a node this app can address");
    }
    m->outbox.explicit_route = 1;
    m->outbox.checking = 1;
    memcpy(r->send_via, s->via, sizeof(r->send_via));
    r->send_count = s->count;
    id = write_preview(c, conv_key, (const char(*)[RIFT_ROUTE_KEY])r->send_via, r->send_count,
                       now);
    if (id < 0) {
        /* A write that failed has dropped the connection, and that settled
         * the submission already (rift_ipc_path_lost); anything else is
         * settled here. */
        if (rift_model_sending(m)) {
            check_failed(m, "the route could not be checked; nothing was sent");
        }
        c->revision++;
        return -1;
    }
    r->send_check_request = id;
    c->revision++;
    return 0;
}

/* The service's words for a refusal, without the "manual path rejected: "
 * every one of them starts with: the screen already says it is the route. */
static void error_words(const cJSON *error, char *out, size_t len)
{
    static const char lead[] = "manual path rejected: ";
    const cJSON *message = cJSON_IsObject(error)
                               ? cJSON_GetObjectItemCaseSensitive(error, "message")
                               : NULL;
    const char *s = (cJSON_IsString(message) && message->valuestring) ? message->valuestring
                                                                       : "refused";

    if (strncmp(s, lead, sizeof(lead) - 1) == 0 && s[sizeof(lead) - 1]) {
        s += sizeof(lead) - 1;
    }
    snprintf(out, len, "%s", s);
}

int rift_ipc_path_reply(struct rift_ipc *c, enum rift_req what, const cJSON *msg)
{
    struct rift_model *m = c->model;
    struct rift_route *r = &m->route;
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    char why[RIFT_ROUTE_WHY_MAX];
    char text[RIFT_ROUTE_WHY_MAX + 40];
    int id;

    if (what != RIFT_REQ_PATH_PREVIEW) {
        return 0;
    }
    id = cJSON_IsNumber(jid) ? (int)jid->valuedouble : 0;
    error_words(error, why, sizeof(why));
    if (cJSON_IsObject(error)) {
        c->errors_in++;
        snprintf(c->last_error, sizeof(c->last_error), "mesh.path_preview: %.76s", why);
    }
    if (id > 0 && id == r->send_check_request) {
        struct rift_route_encoding enc;
        cJSON *via;

        r->send_check_request = 0;
        /* The submission it was checking may have ended since (the service
         * went and came back): then this answers nothing. */
        if (!m->outbox.active || !m->outbox.checking) {
            return 1;
        }
        if (cJSON_IsObject(error)) {
            snprintf(text, sizeof(text), "route refused: %s; nothing was sent", why);
            check_failed(m, text);
            return 1;
        }
        if (rift_route_parse_preview(result, r->send_count, &enc) != 0) {
            check_failed(m, "meshcored did not confirm the route; nothing was sent");
            return 1;
        }
        via = via_json((const char(*)[RIFT_ROUTE_KEY])r->send_via, r->send_count);
        if (!via) {
            check_failed(m, "out of memory; nothing was sent");
            return 1;
        }
        m->outbox.checking = 0;
        /* The service revalidates the same keys itself and refuses rather
         * than fall back (docs/api/mesh.md, mesh.send). */
        rift_ipc_write_send(c, m->outbox.conv_key, m->outbox.text, 0, rift_mono_ms(), via);
        return 1;
    }
    if (cJSON_IsObject(error)) {
        rift_route_check_refused(r, id, why);
    } else {
        rift_route_check_answered(r, id, result);
    }
    return 1;
}

void rift_ipc_path_lost(struct rift_ipc *c)
{
    struct rift_model *m = c ? c->model : NULL;

    if (!m) {
        return;
    }
    if (m->outbox.active && m->outbox.checking) {
        check_failed(m, "meshcored went away while the route was checked; nothing was sent");
    }
    rift_route_service_lost(&m->route);
}
