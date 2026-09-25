/*
 * PocketFleet multiplayer save codec. See fleet_match_save.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_match_save.h"

#include <string.h>

static const uint8_t MAGIC[4] = { 'P', 'F', 'M', '1' };

struct cursor {
    uint8_t *w;
    const uint8_t *r;
    size_t at;
    size_t n;
    int bad;
};

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;

    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static void put(struct cursor *c, const void *p, size_t n)
{
    if (c->at + n > c->n) {
        c->bad = 1;
        return;
    }
    memcpy(c->w + c->at, p, n);
    c->at += n;
}

static void put8(struct cursor *c, uint8_t v)
{
    put(c, &v, 1);
}

static void put32(struct cursor *c, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

    put(c, b, 4);
}

static void get(struct cursor *c, void *p, size_t n)
{
    if (c->at + n > c->n) {
        c->bad = 1;
        memset(p, 0, n);
        return;
    }
    memcpy(p, c->r + c->at, n);
    c->at += n;
}

static uint8_t get8(struct cursor *c)
{
    uint8_t v;

    get(c, &v, 1);
    return v;
}

static uint32_t get32(struct cursor *c)
{
    uint8_t b[4];

    get(c, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int fleet_match_save_encode(const struct fleet_match *m, uint8_t *buf, size_t n)
{
    struct cursor c = { buf, NULL, 0, n, 0 };
    struct fleet_match idle;
    const struct fleet_match *s = m;
    int i;

    if (!m || !buf || n < FLEET_MATCH_SAVE_SIZE) {
        return -1;
    }
    if (m->phase < FLEET_MP_DEPLOY) {
        /* Nothing before START is saved: an idle match with the tombstones. */
        memset(&idle, 0, sizeof(idle));
        memcpy(idle.self_key, m->self_key, FLEET_KEY_BYTES);
        memcpy(idle.tomb, m->tomb, sizeof(idle.tomb));
        idle.pending = FLEET_NO_CELL;
        s = &idle;
    }
    put(&c, MAGIC, 4);
    put8(&c, FLEET_MATCH_SAVE_VERSION & 0xFF);
    put8(&c, FLEET_MATCH_SAVE_VERSION >> 8);
    put(&c, s->self_key, FLEET_KEY_BYTES);
    put8(&c, s->role);
    put8(&c, s->phase);
    put32(&c, s->sid);
    put(&c, s->peer_key, FLEET_KEY_BYTES);
    put(&c, s->peer_name, FLEET_MATCH_NAME_MAX);
    put8(&c, s->rules);
    put8(&c, s->committed);
    put(&c, s->own_layout, FLEET_LAYOUT_BYTES);
    put(&c, s->own_salt, FLEET_SALT_BYTES);
    put(&c, s->own_commit, FLEET_COMMIT_BYTES);
    put8(&c, s->have_peer_commit);
    put(&c, s->peer_commit, FLEET_COMMIT_BYTES);
    put8(&c, s->peer_has_commit);
    put8(&c, s->resolved);
    put(&c, s->log_cell + 1, FLEET_PROTO_PLY_MAX);
    put(&c, s->log_res + 1, FLEET_PROTO_PLY_MAX);
    put8(&c, s->pending);
    put8(&c, s->have_peer_reveal);
    put(&c, s->peer_layout, FLEET_LAYOUT_BYTES);
    put(&c, s->peer_salt, FLEET_SALT_BYTES);
    put8(&c, s->peer_has_reveal);
    put8(&c, s->outcome);
    put8(&c, s->verify);
    put8(&c, s->end_reason);
    put8(&c, s->end_by_me);
    put8(&c, s->end_unacked);
    for (i = 0; i < FLEET_MATCH_TOMBSTONES; i++) {
        put32(&c, s->tomb[i].sid);
        put(&c, s->tomb[i].peer, FLEET_MATCH_PEER_PREFIX);
        put8(&c, s->tomb[i].kind);
        put8(&c, s->tomb[i].reason);
    }
    put32(&c, fnv1a(buf, c.at));
    if (c.bad || c.at != FLEET_MATCH_SAVE_SIZE) {
        return -1;
    }
    return (int)c.at;
}

static int flag(uint8_t v)
{
    return v <= 1;
}

int fleet_match_save_decode(struct fleet_match *m, const uint8_t *buf, size_t n)
{
    struct cursor c = { NULL, buf, 0, n, 0 };
    struct fleet_match d;
    uint8_t magic[4];
    uint32_t stored;
    int i;

    if (!m || !buf || n != FLEET_MATCH_SAVE_SIZE) {
        return -1;
    }
    stored = (uint32_t)buf[n - 4] | (uint32_t)buf[n - 3] << 8 | (uint32_t)buf[n - 2] << 16 |
             (uint32_t)buf[n - 1] << 24;
    if (fnv1a(buf, n - 4) != stored) {
        return -1;
    }
    d = *m;
    get(&c, magic, 4);
    if (memcmp(magic, MAGIC, 4) != 0) {
        return -1;
    }
    {
        uint8_t lo = get8(&c);
        uint8_t hi = get8(&c);

        if ((lo | (hi << 8)) != FLEET_MATCH_SAVE_VERSION) {
            return -1;
        }
    }
    get(&c, d.self_key, FLEET_KEY_BYTES);
    d.role = get8(&c);
    d.phase = get8(&c);
    d.sid = get32(&c);
    get(&c, d.peer_key, FLEET_KEY_BYTES);
    get(&c, d.peer_name, FLEET_MATCH_NAME_MAX);
    d.rules = get8(&c);
    d.committed = get8(&c);
    get(&c, d.own_layout, FLEET_LAYOUT_BYTES);
    get(&c, d.own_salt, FLEET_SALT_BYTES);
    get(&c, d.own_commit, FLEET_COMMIT_BYTES);
    d.have_peer_commit = get8(&c);
    get(&c, d.peer_commit, FLEET_COMMIT_BYTES);
    d.peer_has_commit = get8(&c);
    d.resolved = get8(&c);
    d.log_cell[0] = 0;
    d.log_res[0] = 0;
    get(&c, d.log_cell + 1, FLEET_PROTO_PLY_MAX);
    get(&c, d.log_res + 1, FLEET_PROTO_PLY_MAX);
    d.pending = get8(&c);
    d.have_peer_reveal = get8(&c);
    get(&c, d.peer_layout, FLEET_LAYOUT_BYTES);
    get(&c, d.peer_salt, FLEET_SALT_BYTES);
    d.peer_has_reveal = get8(&c);
    d.outcome = get8(&c);
    d.verify = get8(&c);
    d.end_reason = get8(&c);
    d.end_by_me = get8(&c);
    d.end_unacked = get8(&c);
    for (i = 0; i < FLEET_MATCH_TOMBSTONES; i++) {
        d.tomb[i].sid = get32(&c);
        get(&c, d.tomb[i].peer, FLEET_MATCH_PEER_PREFIX);
        d.tomb[i].kind = get8(&c);
        d.tomb[i].reason = get8(&c);
        if (d.tomb[i].kind > FLEET_TOMB_ENDED || d.tomb[i].reason >= FLEET_END_REASON_COUNT ||
            d.tomb[i].sid > 0xFFFFFF) {
            return -1;
        }
    }
    if (c.bad || c.at != n - 4) {
        return -1;
    }
    /* Field ranges; fleet_match_restore() checks that they hold together. */
    if (d.phase >= FLEET_MP_PHASE_COUNT || (d.phase > FLEET_MP_IDLE && d.phase < FLEET_MP_DEPLOY) ||
        d.role > FLEET_ROLE_GUEST || !flag(d.committed) || !flag(d.have_peer_commit) ||
        !flag(d.peer_has_commit) || !flag(d.have_peer_reveal) || !flag(d.peer_has_reveal) ||
        d.outcome > FLEET_OUTCOME_VOID || d.verify > FLEET_VERIFY_NONE ||
        d.end_reason >= FLEET_END_REASON_COUNT || !flag(d.end_by_me) || !flag(d.end_unacked) ||
        d.resolved > FLEET_PROTO_PLY_MAX || d.peer_name[FLEET_MATCH_NAME_MAX - 1] != '\0') {
        return -1;
    }
    if (memcmp(d.self_key, m->self_key, FLEET_KEY_BYTES) != 0) {
        return -2;
    }
    *m = d;
    return 0;
}
