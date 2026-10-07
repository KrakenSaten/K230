/*
 * PocketFleet multiplayer session. See fleet_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_session.h"

#include <stdio.h>
#include <string.h>

void fleet_session_init(struct fleet_session *s, struct fleet_link *link,
                        int (*save)(void *, const uint8_t *, size_t),
                        int (*entropy)(void *, void *, size_t), void *user)
{
    memset(s, 0, sizeof(*s));
    s->link = link;
    s->save = save;
    s->entropy = entropy;
    s->user = user;
}

void fleet_session_set_saved(struct fleet_session *s, const uint8_t *blob, size_t n, int fault)
{
    s->store_fault = fault;
    s->have_saved = 0;
    if (blob && n == FLEET_MATCH_SAVE_SIZE) {
        memcpy(s->saved, blob, n);
        s->have_saved = 1;
    } else if (blob) {
        s->store_fault = 1;
    }
}

static uint32_t random32(struct fleet_session *s)
{
    uint32_t v = 0;

    if (!s->entropy || s->entropy(s->user, &v, sizeof(v)) != 0) {
        /* No entropy: a session id still has to differ from the last one. */
        v = (uint32_t)(s->now * 2654435761u) ^ 0xA5A5A5u ^ s->sent;
    }
    return v;
}

/* The match exists once our own key is known; a saved one is read then. */
static void make_ready(struct fleet_session *s)
{
    uint8_t key[FLEET_KEY_BYTES];

    if (s->ready || !s->link || s->link->ops->self_key(s->link->ctx, key) != 0) {
        return;
    }
    fleet_match_init(&s->m, key, random32(s));
    if (s->have_saved) {
        int rc = fleet_match_save_decode(&s->m, s->saved, sizeof(s->saved));

        if (rc == -2) {
            s->identity_changed = 1;
            s->store_fault = 1;
        } else if (rc != 0 || fleet_match_restore(&s->m, s->now) != 0) {
            s->store_fault = 1;
            fleet_match_init(&s->m, key, random32(s));
        }
    }
    fleet_match_set_store_fault(&s->m, s->store_fault);
    s->ready = 1;
}

/* Save, then send: the order the whole protocol rests on. */
static void flush(struct fleet_session *s)
{
    struct fleet_match_out o;

    if (!s->ready) {
        return;
    }
    if (fleet_match_dirty(&s->m)) {
        uint8_t blob[FLEET_MATCH_SAVE_SIZE];
        int n = fleet_match_save_encode(&s->m, blob, sizeof(blob));

        if (n > 0 && s->save && s->save(s->user, blob, (size_t)n) == 0) {
            fleet_match_saved(&s->m);
        } else {
            s->save_failed = 1;
            fleet_match_save_failed(&s->m);
        }
    }
    if (!s->engaged) {
        return;
    }
    while (fleet_match_pop(&s->m, &o)) {
        enum fleet_link_send rc = s->link->ops->send(s->link->ctx, o.to, o.bytes, o.len);

        if (rc == FLEET_LINK_SENT) {
            s->sent++;
            if (s->link->ops->retry_base) {
                fleet_match_set_retry_base(&s->m, s->link->ops->retry_base(s->link->ctx));
            }
        } else {
            s->busy++;
            fleet_match_tx_refused(&s->m, &o, s->now);
        }
    }
}

void fleet_session_engage(struct fleet_session *s, int64_t now)
{
    s->now = now;
    s->engaged = 1;
    make_ready(s);
}

void fleet_session_poll(struct fleet_session *s, int64_t now)
{
    uint8_t from[FLEET_KEY_BYTES];
    uint8_t buf[FLEET_PROTO_MAX + 8];
    size_t n;
    int budget = 32;

    s->now = now;
    if (!s->link) {
        return;
    }
    make_ready(s);
    if (!s->engaged) {
        /* Not even the inbox is read: nothing may be answered yet. */
        return;
    }
    s->link->ops->poll(s->link->ctx, now);
    while (budget-- > 0 && s->link->ops->recv(s->link->ctx, from, buf, &n)) {
        s->received++;
        if (s->ready) {
            fleet_match_receive(&s->m, from, buf, n, now);
            flush(s);
        }
    }
    if (s->ready) {
        fleet_match_tick(&s->m, now);
        flush(s);
    }
}

unsigned fleet_session_revision(const struct fleet_session *s)
{
    return s->ready ? s->m.revision + 1 : 0;
}

#define ACT(expr)                          \
    do {                                   \
        int rc_;                           \
        s->now = now;                      \
        if (!s->ready) {                   \
            return -1;                     \
        }                                  \
        rc_ = (expr);                      \
        flush(s);                          \
        return rc_;                        \
    } while (0)

int fleet_session_invite(struct fleet_session *s, const uint8_t peer[FLEET_KEY_BYTES],
                         const char *name, int64_t now)
{
    ACT(fleet_match_invite(&s->m, peer, name, random32(s), now));
}

int fleet_session_accept(struct fleet_session *s, int64_t now)
{
    ACT(fleet_match_accept(&s->m, now));
}

int fleet_session_decline(struct fleet_session *s, int64_t now)
{
    ACT(fleet_match_decline(&s->m, now));
}

int fleet_session_cancel(struct fleet_session *s, int64_t now)
{
    ACT(fleet_match_cancel(&s->m, now));
}

int fleet_session_deploy(struct fleet_session *s, const struct fleet_board *board, int64_t now)
{
    uint8_t salt[FLEET_SALT_BYTES];

    if (!s->entropy || s->entropy(s->user, salt, sizeof(salt)) != 0) {
        /* A predictable salt would let the fleet be guessed from its
         * commitment. Refuse rather than commit weakly. */
        return -1;
    }
    ACT(fleet_match_deploy(&s->m, board, salt, now));
}

int fleet_session_fire(struct fleet_session *s, int row, int col, int64_t now)
{
    ACT(fleet_match_fire(&s->m, row, col, now));
}

int fleet_session_forfeit(struct fleet_session *s, int64_t now)
{
    ACT(fleet_match_forfeit(&s->m, now));
}

int fleet_session_resume(struct fleet_session *s, int64_t now)
{
    fleet_session_engage(s, now);
    ACT(fleet_match_resume(&s->m, now));
}

int fleet_session_chat_send(struct fleet_session *s, const char *text, int64_t now)
{
    ACT(fleet_match_chat_send(&s->m, text, now));
}

void fleet_session_dismiss(struct fleet_session *s, int64_t now)
{
    s->now = now;
    if (s->ready) {
        fleet_match_dismiss(&s->m);
        flush(s);
    }
}

const char *fleet_session_peer_name(struct fleet_session *s, char *buf, size_t n)
{
    const char *name = NULL;

    if (!s->ready || n == 0) {
        if (n) {
            buf[0] = '\0';
        }
        return buf;
    }
    if (s->m.peer_name[0]) {
        name = s->m.peer_name;
    } else if (s->link && s->link->ops->peer_name) {
        name = s->link->ops->peer_name(s->link->ctx, s->m.peer_key);
        if (name) {
            fleet_match_set_peer_name(&s->m, name);
        }
    }
    if (name && name[0]) {
        snprintf(buf, n, "%s", name);
    } else {
        snprintf(buf, n, "%02X%02X", s->m.peer_key[0], s->m.peer_key[1]);
    }
    return buf;
}
