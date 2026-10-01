/*
 * RIFT's actions: the requests a reader makes that are not a message - an
 * advert, forgetting a node, forgetting a route. The fourth translation unit
 * over struct rift_model; it obeys the rules at the top of rift_model.h.
 *
 * Each is the outbox's shape again, for the outbox's reason: a request is
 * written and then answered or refused, and nothing here decides that it
 * happened. An advert the service answered was ACCEPTED - queued for its
 * dispatcher - which is not the same as transmitted; how the transmit went
 * arrives in the activity feed in the service's own words.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_model.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

static int is_node_action(enum rift_action kind)
{
    return kind == RIFT_ACTION_FORGET || kind == RIFT_ACTION_RESET_PATH;
}

struct rift_action_state *rift_model_action_slot(struct rift_model *m, enum rift_action kind)
{
    if (!m) {
        return NULL;
    }
    if (kind == RIFT_ACTION_ADVERT_NEAR || kind == RIFT_ACTION_ADVERT_MESH) {
        return &m->advert;
    }
    if (is_node_action(kind)) {
        return &m->node_op;
    }
    return NULL;
}

const struct rift_action_state *rift_model_action_of(const struct rift_model *m,
                                                     enum rift_action kind)
{
    return rift_model_action_slot((struct rift_model *)m, kind);
}

int rift_model_action_busy(const struct rift_model *m, enum rift_action kind)
{
    const struct rift_action_state *s = rift_model_action_of(m, kind);

    return s && s->active;
}

int rift_model_action_begin(struct rift_model *m, enum rift_action kind, const char *key,
                            const char *label, int64_t now_ms)
{
    struct rift_action_state *s = rift_model_action_slot(m, kind);

    if (!s || s->active) {
        return -1;
    }
    /* A node action names a node by its whole key, because the service will
     * take nothing less for a change to what it holds (docs/api/mesh.md). */
    if (is_node_action(kind) && !hex_only(key, 64)) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->active = 1;
    if (is_node_action(kind)) {
        snprintf(s->key, sizeof(s->key), "%s", key);
    }
    rift_utf8_copy(s->label, sizeof(s->label), label ? label : "");
    s->have_mono = 1;
    s->mono_ms = now_ms;
    return 0;
}

void rift_model_action_done(struct rift_model *m, enum rift_action kind, int64_t now_ms)
{
    struct rift_action_state *s = rift_model_action_slot(m, kind);

    /* An answer with nothing in flight is not one this app asked for, and
     * is not allowed to say that something was done. */
    if (!s || !s->active) {
        return;
    }
    s->active = 0;
    s->done = 1;
    s->failed = 0;
    s->unknown = 0;
    s->have_mono = 1;
    s->mono_ms = now_ms;
    /* A node forgotten is room made in the service's table: the adverts it
     * had turned away before say nothing about whether it is full now. The
     * mesh.node "removed" event says the same thing, for a client that is
     * subscribed; the answer is enough on its own. */
    if (kind == RIFT_ACTION_FORGET) {
        m->unretained_baseline = m->nodes_unretained;
    }
}

void rift_model_action_failed(struct rift_model *m, enum rift_action kind, const char *why,
                              int64_t now_ms)
{
    struct rift_action_state *s = rift_model_action_slot(m, kind);

    if (!s) {
        return;
    }
    s->kind = kind;
    s->active = 0;
    s->done = 0;
    s->failed = 1;
    s->unknown = 0;
    s->have_mono = 1;
    s->mono_ms = now_ms;
    rift_utf8_copy(s->error, sizeof(s->error), why ? why : "refused");
}

void rift_model_action_clear(struct rift_model *m, enum rift_action kind)
{
    struct rift_action_state *s = rift_model_action_slot(m, kind);

    if (s && !s->active) {
        memset(s, 0, sizeof(*s));
    }
}
