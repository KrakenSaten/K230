/*
 * PocketFleet multiplayer match state machine. See fleet_match.h and
 * docs/apps/FLEET_MULTIPLAYER.md, which this follows rule for rule.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_match.h"

#include "fleet_sha256.h"

#include <string.h>

#define OUTCOME_MISS 1
#define OUTCOME_HIT 2
#define OUTCOME_SUNK 3

static const char COMMIT_DOMAIN[] = "DOORS-FLEET-COMMIT-1";

/* ---- small helpers ------------------------------------------------------ */

static int key_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, FLEET_KEY_BYTES) == 0;
}

static void touch(struct fleet_match *m)
{
    m->revision++;
}

static void mark_dirty(struct fleet_match *m)
{
    m->dirty = 1;
    m->revision++;
}

enum fleet_mp_role fleet_match_shooter(int ply)
{
    return (ply & 1) ? FLEET_ROLE_GUEST : FLEET_ROLE_HOST;
}

static uint8_t peer_role(const struct fleet_match *m)
{
    return m->role == FLEET_ROLE_HOST ? FLEET_ROLE_GUEST : FLEET_ROLE_HOST;
}

static int mine(const struct fleet_match *m, int ply)
{
    return fleet_match_shooter(ply) == m->role;
}

static uint32_t jitter(struct fleet_match *m, uint32_t span)
{
    return span ? fleet_rng_below(&m->rng, span + 1) : 0;
}

const char *fleet_match_phase_name(enum fleet_mp_phase phase)
{
    static const char *names[FLEET_MP_PHASE_COUNT] = {
        "idle", "inviting", "invited", "accepting", "deploy", "committed",
        "battle", "reveal", "done",
    };

    return (unsigned)phase < FLEET_MP_PHASE_COUNT ? names[phase] : "?";
}

int fleet_match_active(const struct fleet_match *m)
{
    return m && m->phase >= FLEET_MP_DEPLOY && m->phase <= FLEET_MP_REVEAL;
}

int fleet_match_saved_phase(const struct fleet_match *m)
{
    return m && m->phase >= FLEET_MP_DEPLOY;
}

int fleet_match_my_turn(const struct fleet_match *m)
{
    return m && m->phase == FLEET_MP_BATTLE && m->pending == FLEET_NO_CELL &&
           m->resolved < FLEET_PROTO_PLY_MAX && mine(m, m->resolved + 1);
}

int fleet_match_shots_by(const struct fleet_match *m, enum fleet_mp_role side)
{
    int k;
    int n = 0;

    for (k = 1; k <= m->resolved; k++) {
        n += fleet_match_shooter(k) == side;
    }
    return n;
}

enum fleet_mp_link fleet_match_link(const struct fleet_match *m)
{
    if (m->resyncing) {
        return FLEET_LINK_RESYNCING;
    }
    if (m->lost) {
        return FLEET_LINK_LOST;
    }
    if (m->throttled) {
        return FLEET_LINK_THROTTLED;
    }
    if (m->attempts >= 2 && (m->ob == FLEET_OB_COMMIT || m->ob == FLEET_OB_SHOT ||
                             m->ob == FLEET_OB_REVEAL || m->ob == FLEET_OB_INVITE ||
                             m->ob == FLEET_OB_ACCEPT)) {
        return FLEET_LINK_RETRYING;
    }
    return FLEET_LINK_OK;
}

/* ---- the commitment ----------------------------------------------------- */

int fleet_layout_encode(const struct fleet_board *board, uint8_t out[FLEET_LAYOUT_BYTES])
{
    int i;

    if (!board || !fleet_board_complete(board)) {
        return -1;
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        const struct fleet_ship_state *s = &board->ships[i];

        out[i] = (uint8_t)((s->orient == FLEET_VERTICAL ? 0x80 : 0) |
                           (s->row * FLEET_GRID + s->col));
    }
    return 0;
}

int fleet_layout_decode(const uint8_t in[FLEET_LAYOUT_BYTES], struct fleet_board *board)
{
    int i;

    fleet_board_clear(board);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        int cell = in[i] & 0x7F;

        if (cell >= FLEET_CELLS ||
            fleet_board_place(board, (enum fleet_ship)i, cell / FLEET_GRID, cell % FLEET_GRID,
                              (in[i] & 0x80) ? FLEET_VERTICAL : FLEET_HORIZONTAL) != 0) {
            fleet_board_clear(board);
            return -1;
        }
    }
    return 0;
}

void fleet_commit_compute(uint32_t sid, const uint8_t owner[FLEET_KEY_BYTES],
                          const uint8_t layout[FLEET_LAYOUT_BYTES],
                          const uint8_t salt[FLEET_SALT_BYTES],
                          uint8_t out[FLEET_COMMIT_BYTES])
{
    struct fleet_sha256 s;
    uint8_t digest[FLEET_SHA256_BYTES];
    uint8_t sidb[3] = { (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid };

    fleet_sha256_init(&s);
    fleet_sha256_update(&s, COMMIT_DOMAIN, sizeof(COMMIT_DOMAIN) - 1);
    fleet_sha256_update(&s, sidb, 3);
    fleet_sha256_update(&s, owner, FLEET_KEY_BYTES);
    fleet_sha256_update(&s, layout, FLEET_LAYOUT_BYTES);
    fleet_sha256_update(&s, salt, FLEET_SALT_BYTES);
    fleet_sha256_final(&s, digest);
    memcpy(out, digest, FLEET_COMMIT_BYTES);
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

uint32_t fleet_match_digest(const struct fleet_match *m, int n)
{
    uint8_t sidb[3] = { (uint8_t)(m->sid >> 16), (uint8_t)(m->sid >> 8), (uint8_t)m->sid };
    static const uint8_t zero[FLEET_COMMIT_BYTES];
    const uint8_t *own = m->committed ? m->own_commit : zero;
    const uint8_t *peer = m->have_peer_commit ? m->peer_commit : zero;
    uint32_t h = 2166136261u;
    int k;

    h = fnv(h, sidb, 3);
    h = fnv(h, m->role == FLEET_ROLE_HOST ? own : peer, FLEET_COMMIT_BYTES);
    h = fnv(h, m->role == FLEET_ROLE_HOST ? peer : own, FLEET_COMMIT_BYTES);
    for (k = 1; k <= n && k <= m->resolved; k++) {
        uint8_t e[2] = { m->log_cell[k], m->log_res[k] };

        h = fnv(h, e, 2);
    }
    return h;
}

/* ---- what we know of their fleet ------------------------------------------ */

/* Every run of `len` cells in a line through `cell` whose squares are all
 * hits of ours not yet attributed to another ship. Returns how many; the
 * last one found is left in run[]. An honest opponent's sunk ship is always
 * one of them, so none at all means the answer was impossible. */
static int hull_candidates(const uint8_t *hitmap, const uint8_t *owner, int cell, int len,
                           int run[5])
{
    int count = 0;
    int vertical;

    for (vertical = 0; vertical < 2; vertical++) {
        int off;

        for (off = 0; off < len; off++) {
            int r0 = cell / FLEET_GRID - (vertical ? off : 0);
            int c0 = cell % FLEET_GRID - (vertical ? 0 : off);
            int n;
            int ok = 1;

            for (n = 0; n < len && ok; n++) {
                int r = r0 + (vertical ? n : 0);
                int c = c0 + (vertical ? 0 : n);
                int idx = fleet_index(r, c);

                ok = idx >= 0 && (idx == cell || (hitmap[idx] && owner[idx] == 0xFF));
            }
            if (ok) {
                for (n = 0; n < len; n++) {
                    run[n] = (r0 + (vertical ? n : 0)) * FLEET_GRID + c0 + (vertical ? 0 : n);
                }
                count++;
            }
            if (len == 1) {
                break;
            }
        }
    }
    return count;
}

/* Rebuild the target board from our own shots and their answers, and check
 * the answers could all be true. Returns 0, or -1 when they could not. */
static int build_target(struct fleet_match *m)
{
    struct fleet_board *t = &m->target;
    uint8_t hitmap[FLEET_CELLS] = { 0 };
    uint8_t owner[FLEET_CELLS];
    uint8_t sunk[FLEET_SHIP_COUNT] = { 0 };
    int hits = 0;
    int sunk_len = 0;
    int sunk_count = 0;
    int shots = 0;
    int destroyed = 0;
    int bad = 0;
    int placeholder = -1;
    int k;
    int i;

    fleet_board_clear(t);
    memset(owner, 0xFF, sizeof(owner));
    for (k = 1; k <= m->resolved; k++) {
        int cell = m->log_cell[k];
        uint8_t res = m->log_res[k];
        int outcome = fleet_res_outcome(res);

        if (!mine(m, k)) {
            continue;
        }
        shots++;
        if (t->shot[cell] || destroyed) {
            bad = 1;
        }
        t->shot[cell] = 1;
        if (outcome == OUTCOME_MISS) {
            continue;
        }
        hits++;
        if (outcome == OUTCOME_HIT) {
            hitmap[cell] = 1;
            continue;
        }
        {
            int ship = fleet_res_ship(res);
            int len = fleet_ship_length((enum fleet_ship)ship);
            int run[5];
            int n;

            if (sunk[ship]) {
                bad = 1;
            }
            sunk[ship] = 1;
            sunk_count++;
            sunk_len += len;
            n = hull_candidates(hitmap, owner, cell, len, run);
            if (n == 0) {
                bad = 1;
            }
            if (n == 1) {
                for (i = 0; i < len; i++) {
                    owner[run[i]] = (uint8_t)ship;
                }
            } else {
                owner[cell] = (uint8_t)ship;
            }
            hitmap[cell] = 1;
            if (fleet_res_destroyed(res)) {
                destroyed = 1;
            }
        }
        if (fleet_res_destroyed(res) != (sunk_count == FLEET_SHIP_COUNT)) {
            bad = 1;
        }
    }
    if (hits > FLEET_HULL_CELLS || sunk_len > hits || (shots >= FLEET_CELLS && !destroyed)) {
        bad = 1;
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        if (sunk[i]) {
            t->ships[i].placed = 1;
            t->ships[i].hits = fleet_ship_length((enum fleet_ship)i);
        } else if (placeholder < 0) {
            placeholder = i;
        }
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        if (owner[i] != 0xFF) {
            t->ship_at[i] = owner[i];
        } else if (hitmap[i]) {
            /* A hit on a ship not yet known: any index whose ship is still
             * afloat draws as a hit. */
            t->ship_at[i] = (uint8_t)(placeholder >= 0 ? placeholder : 0);
        }
    }
    t->ships_afloat = (uint8_t)(FLEET_SHIP_COUNT - sunk_count);
    return bad ? -1 : 0;
}

/* Rebuild our own waters from the layout and the fire at it. Returns 0, or
 * -1 when a logged answer is not what our own fleet says. */
static int build_own(struct fleet_match *m)
{
    int k;

    fleet_board_clear(&m->own);
    if (!m->committed) {
        return 0;
    }
    if (fleet_layout_decode(m->own_layout, &m->own) != 0) {
        return -1;
    }
    for (k = 1; k <= m->resolved; k++) {
        int sunk;
        int cell = m->log_cell[k];
        enum fleet_shot_result r;

        if (mine(m, k)) {
            continue;
        }
        r = fleet_board_fire(&m->own, cell / FLEET_GRID, cell % FLEET_GRID, &sunk);
        if (r == FLEET_SHOT_INVALID ||
            fleet_res_make(r, sunk, m->own.ships_afloat == 0) != m->log_res[k]) {
            return -1;
        }
    }
    return 0;
}

/* Their revealed fleet against its commitment and against every answer they
 * gave us. */
static uint8_t verify_peer(struct fleet_match *m)
{
    struct fleet_board b;
    uint8_t c[FLEET_COMMIT_BYTES];
    int k;

    if (!m->have_peer_reveal) {
        return FLEET_VERIFY_NONE;
    }
    if (!m->have_peer_commit || fleet_layout_decode(m->peer_layout, &b) != 0) {
        return FLEET_VERIFY_MISMATCH;
    }
    fleet_commit_compute(m->sid, m->peer_key, m->peer_layout, m->peer_salt, c);
    if (memcmp(c, m->peer_commit, FLEET_COMMIT_BYTES) != 0) {
        return FLEET_VERIFY_MISMATCH;
    }
    for (k = 1; k <= m->resolved; k++) {
        int cell = m->log_cell[k];
        int sunk;
        enum fleet_shot_result r;

        if (!mine(m, k)) {
            continue;
        }
        r = fleet_board_fire(&b, cell / FLEET_GRID, cell % FLEET_GRID, &sunk);
        if (r == FLEET_SHOT_INVALID ||
            fleet_res_make(r, sunk, b.ships_afloat == 0) != m->log_res[k]) {
            return FLEET_VERIFY_MISMATCH;
        }
    }
    m->peer_board = b;
    return FLEET_VERIFY_OK;
}

/* ---- the airtime governor ------------------------------------------------ */

static void gov_refill(struct fleet_match *m, int64_t now)
{
    while (m->tokens < FLEET_GOV_BURST && now >= m->refill_at) {
        m->tokens++;
        m->refill_at += FLEET_GOV_REFILL_MS;
    }
    if (m->tokens >= FLEET_GOV_BURST && m->refill_at < now) {
        m->refill_at = now;
    }
}

static uint32_t hour_sum(const struct fleet_match *m, const uint32_t *per_minute, int64_t now)
{
    int64_t minute = now / 60000;
    uint32_t total = 0;
    int i;

    for (i = 0; i < 60; i++) {
        if (m->minute_of[i] > minute - 60 && m->minute_of[i] <= minute) {
            total += per_minute[i];
        }
    }
    return total;
}

/* Fleet's airtime in the rolling hour, chat included. */
static uint32_t gov_hour(const struct fleet_match *m, int64_t now)
{
    return hour_sum(m, m->minute_ms, now);
}

/* The part of it that was chat. */
static uint32_t gov_chat_hour(const struct fleet_match *m, int64_t now)
{
    return hour_sum(m, m->chat_minute_ms, now);
}

/* A fresh reply - the answer the peer is waiting for right now - needs only
 * room in the hour: dropping it would only make the peer ask again, which
 * costs more airtime than it saves. Everything else also needs a token. */
static int gov_allow(struct fleet_match *m, uint32_t air, int64_t now, int fresh)
{
    gov_refill(m, now);
    if (gov_hour(m, now) + air > FLEET_GOV_HOUR_MS) {
        return 0;
    }
    return fresh || m->tokens > 0;
}

/* The airtime into the rolling hour, and nothing from the burst. */
static void gov_charge_hour(struct fleet_match *m, uint32_t air, int64_t now, int chat)
{
    int64_t minute = now / 60000;
    int slot = (int)(minute % 60);

    if (m->minute_of[slot] != minute) {
        m->minute_of[slot] = minute;
        m->minute_ms[slot] = 0;
        m->chat_minute_ms[slot] = 0;
    }
    m->minute_ms[slot] += air;
    if (chat) {
        m->chat_minute_ms[slot] += air;
    }
}

static void gov_take_token(struct fleet_match *m, int64_t now)
{
    if (m->tokens == FLEET_GOV_BURST) {
        m->refill_at = now + FLEET_GOV_REFILL_MS;
    }
    if (m->tokens > 0) {
        m->tokens--;
    }
}

static void gov_charge(struct fleet_match *m, uint32_t air, int64_t now)
{
    gov_take_token(m, now);
    gov_charge_hour(m, air, now, 0);
}

/* When the governor will next let a frame go. */
static int64_t gov_ready(struct fleet_match *m, int64_t now)
{
    if (m->tokens > 0) {
        return now + 60000;   /* the hour is full: look again in a minute */
    }
    return m->refill_at > now ? m->refill_at : now + 1;
}

/* ---- sending -------------------------------------------------------------- */

enum { SENT = 0, DEFERRED = 1, DROPPED = -1 };
enum { AS_OBLIGATION, AS_FRESH, AS_DUP };

/* Into the outbox, already paid for. A full outbox loses its oldest packet,
 * which only the game's own traffic can make happen: chat never queues
 * behind anything (chat_enqueue). */
static void outbox_put(struct fleet_match *m, const uint8_t *to, const uint8_t *buf, int n,
                       uint8_t ob, uint32_t air)
{
    struct fleet_match_out *o;

    if (m->out_len == FLEET_MATCH_OUTBOX) {
        m->out_head = (uint8_t)((m->out_head + 1) % FLEET_MATCH_OUTBOX);
        m->out_len--;
    }
    o = &m->out[(m->out_head + m->out_len) % FLEET_MATCH_OUTBOX];
    memcpy(o->to, to, FLEET_KEY_BYTES);
    o->len = (uint8_t)n;
    memcpy(o->bytes, buf, (size_t)n);
    o->obligation = ob;
    m->out_len++;
    m->stats.tx++;
    m->stats.tx_airtime_ms += air;
    if (ob) {
        m->stats.tx_obligation++;
    }
}

static int enqueue_as(struct fleet_match *m, const uint8_t *to, const struct fleet_msg *msg,
                      uint8_t ob, int as)
{
    uint8_t buf[FLEET_PROTO_MAX];
    uint32_t air;
    int n = fleet_proto_encode(msg, buf, sizeof(buf));

    if (n < 0) {
        return DROPPED;
    }
    air = fleet_proto_airtime_ms((size_t)n);
    if (!gov_allow(m, air, m->now, as == AS_FRESH)) {
        if (as == AS_OBLIGATION) {
            m->stats.gov_waits++;
            return DEFERRED;
        }
        m->stats.gov_drops++;
        return DROPPED;
    }
    gov_charge(m, air, m->now);
    outbox_put(m, to, buf, n, ob, air);
    return SENT;
}

/* A chat frame on the game's terms: only into an empty outbox, and never
 * past Fleet's hour.
 *
 * A line also needs a token above FLEET_CHAT_TOKEN_RESERVE, Fleet's hour
 * under FLEET_CHAT_HOUR_MS and chat's own share of it under
 * FLEET_CHAT_SHARE_MS: that is what keeps chat second to the game.
 *
 * A receipt is held to the whole hour alone, as the game's own first answers
 * are, and never spends one of the reserved tokens. The line it confirms is
 * already on the other screen; withholding the receipt would only make its
 * sender try again and then call it not delivered, which costs more and says
 * the wrong thing. There is at most one receipt per line, and the lines are
 * held to the rules above by the side that says them. */
static int chat_enqueue(struct fleet_match *m, const struct fleet_msg *msg)
{
    uint8_t buf[FLEET_PROTO_MAX];
    uint32_t air;
    int receipt = msg->type == FLEET_MSG_CHAT_ACK;
    int n = fleet_proto_encode(msg, buf, sizeof(buf));

    if (n < 0) {
        return DROPPED;
    }
    air = fleet_proto_airtime_ms((size_t)n);
    gov_refill(m, m->now);
    if (m->out_len > 0 || gov_hour(m, m->now) + air > FLEET_GOV_HOUR_MS) {
        return DEFERRED;
    }
    if (!receipt && (m->tokens <= FLEET_CHAT_TOKEN_RESERVE ||
                     gov_hour(m, m->now) + air > FLEET_CHAT_HOUR_MS ||
                     gov_chat_hour(m, m->now) + air > FLEET_CHAT_SHARE_MS)) {
        return DEFERRED;
    }
    if (m->tokens > FLEET_CHAT_TOKEN_RESERVE) {
        gov_take_token(m, m->now);
    }
    gov_charge_hour(m, air, m->now, 1);
    outbox_put(m, m->peer_key, buf, n, 0, air);
    return SENT;
}

static int enqueue(struct fleet_match *m, const uint8_t *to, const struct fleet_msg *msg,
                   uint8_t ob)
{
    return enqueue_as(m, to, msg, ob, AS_OBLIGATION);
}

/* A reply. `dup` marks one that answers a copy of something already
 * answered: those are rate limited, so a burst of copies costs one frame. */
static void reply(struct fleet_match *m, const uint8_t *to, const struct fleet_msg *msg, int dup)
{
    if (dup) {
        if (m->last_reply[msg->type] && m->now - m->last_reply[msg->type] < FLEET_DUP_REPLY_MS) {
            return;
        }
        m->stats.dup_replies++;
    }
    if (enqueue_as(m, to, msg, 0, dup ? AS_DUP : AS_FRESH) == SENT) {
        m->last_reply[msg->type] = m->now ? m->now : 1;
    }
}

static void msg_init(const struct fleet_match *m, struct fleet_msg *msg, int type, uint32_t sid)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = (uint8_t)type;
    msg->sid = sid ? sid : m->sid;
}

static uint8_t sync_phase(const struct fleet_match *m)
{
    switch (m->phase) {
    case FLEET_MP_INVITING:
    case FLEET_MP_INVITED:
    case FLEET_MP_ACCEPTING:
        return FLEET_SYNC_PRESTART;
    case FLEET_MP_DEPLOY:
        return FLEET_SYNC_DEPLOY;
    case FLEET_MP_COMMITTED:
        return FLEET_SYNC_COMMITTED;
    case FLEET_MP_BATTLE:
        return FLEET_SYNC_BATTLE;
    case FLEET_MP_REVEAL:
        return FLEET_SYNC_REVEAL;
    case FLEET_MP_DONE:
        return FLEET_SYNC_DONE;
    default:
        return FLEET_SYNC_NONE;
    }
}

static void build_sync(const struct fleet_match *m, struct fleet_msg *msg, int is_reply)
{
    msg_init(m, msg, FLEET_MSG_SYNC, 0);
    msg->ply = m->resolved;
    msg->flags = (uint8_t)((is_reply ? FLEET_SYNC_REPLY : 0) |
                           (m->have_peer_commit ? FLEET_SYNC_HAVE_COMMIT : 0) |
                           (m->have_peer_reveal ? FLEET_SYNC_HAVE_REVEAL : 0) |
                           (sync_phase(m) << FLEET_SYNC_PHASE_SHIFT));
    msg->cell = m->pending;
    msg->digest = fleet_match_digest(m, m->resolved);
}

static void build_commit(const struct fleet_match *m, struct fleet_msg *msg)
{
    msg_init(m, msg, FLEET_MSG_COMMIT, 0);
    msg->flags = m->have_peer_commit ? FLEET_FLAG_HAVE_PEER : 0;
    memcpy(msg->commit, m->own_commit, FLEET_COMMIT_BYTES);
}

static void build_reveal(const struct fleet_match *m, struct fleet_msg *msg)
{
    msg_init(m, msg, FLEET_MSG_REVEAL, 0);
    msg->flags = m->have_peer_reveal ? FLEET_FLAG_HAVE_PEER : 0;
    memcpy(msg->layout, m->own_layout, FLEET_LAYOUT_BYTES);
    memcpy(msg->salt, m->own_salt, FLEET_SALT_BYTES);
}

static void build_shot(const struct fleet_match *m, struct fleet_msg *msg)
{
    msg_init(m, msg, FLEET_MSG_SHOT, 0);
    msg->ply = (uint8_t)(m->resolved + 1);
    msg->cell = m->pending;
    msg->res = m->resolved ? m->log_res[m->resolved] : 0;
}

static void build_result(const struct fleet_match *m, struct fleet_msg *msg, int ply)
{
    msg_init(m, msg, FLEET_MSG_RESULT, 0);
    msg->ply = (uint8_t)ply;
    msg->cell = m->log_cell[ply];
    msg->res = m->log_res[ply];
}

static void build_end(const struct fleet_match *m, struct fleet_msg *msg, int type,
                      uint32_t sid, uint8_t reason)
{
    msg_init(m, msg, type, sid);
    msg->ply = sid == m->sid ? m->resolved : 0;
    msg->reason = reason;
}

/* The newest packet this device holds for the peer at or after ply k: the
 * pending shot (which carries the answer to ply R), or the answer to R. */
static void resend_latest(struct fleet_match *m, int dup)
{
    struct fleet_msg msg;

    if (m->phase == FLEET_MP_BATTLE && m->pending != FLEET_NO_CELL) {
        build_shot(m, &msg);
    } else if (m->resolved > 0 && !mine(m, m->resolved)) {
        build_result(m, &msg, m->resolved);
    } else {
        return;
    }
    reply(m, m->peer_key, &msg, dup);
}

/* ---- tombstones ------------------------------------------------------------ */

static void tomb_push(struct fleet_match *m, uint32_t sid, const uint8_t *peer, uint8_t kind,
                      uint8_t reason)
{
    int i;

    if (!sid) {
        return;
    }
    for (i = FLEET_MATCH_TOMBSTONES - 1; i > 0; i--) {
        m->tomb[i] = m->tomb[i - 1];
        m->tomb_reply[i] = m->tomb_reply[i - 1];
    }
    m->tomb[0].sid = sid;
    memcpy(m->tomb[0].peer, peer, FLEET_MATCH_PEER_PREFIX);
    m->tomb[0].kind = kind;
    m->tomb[0].reason = reason;
    m->tomb_reply[0] = 0;
    m->dirty = 1;
}

static int tomb_find(const struct fleet_match *m, uint32_t sid, const uint8_t *from)
{
    int i;

    for (i = 0; i < FLEET_MATCH_TOMBSTONES; i++) {
        if (m->tomb[i].kind != FLEET_TOMB_EMPTY && m->tomb[i].sid == sid &&
            memcmp(m->tomb[i].peer, from, FLEET_MATCH_PEER_PREFIX) == 0) {
            return i;
        }
    }
    return -1;
}

/* Forget the session in hand, keeping the tombstones and the identity. */
static void clear_session(struct fleet_match *m)
{
    uint8_t self[FLEET_KEY_BYTES];
    struct fleet_tombstone tomb[FLEET_MATCH_TOMBSTONES];

    memcpy(self, m->self_key, sizeof(self));
    memcpy(tomb, m->tomb, sizeof(tomb));
    m->role = FLEET_ROLE_NONE;
    m->phase = FLEET_MP_IDLE;
    m->sid = 0;
    memset(m->peer_key, 0, sizeof(m->peer_key));
    memset(m->peer_name, 0, sizeof(m->peer_name));
    m->rules = 0;
    m->committed = 0;
    memset(m->own_layout, 0, sizeof(m->own_layout));
    memset(m->own_salt, 0, sizeof(m->own_salt));
    memset(m->own_commit, 0, sizeof(m->own_commit));
    m->have_peer_commit = 0;
    memset(m->peer_commit, 0, sizeof(m->peer_commit));
    m->peer_has_commit = 0;
    m->resolved = 0;
    memset(m->log_cell, 0, sizeof(m->log_cell));
    memset(m->log_res, 0, sizeof(m->log_res));
    m->pending = FLEET_NO_CELL;
    m->have_peer_reveal = 0;
    memset(m->peer_layout, 0, sizeof(m->peer_layout));
    memset(m->peer_salt, 0, sizeof(m->peer_salt));
    m->peer_has_reveal = 0;
    m->outcome = FLEET_OUTCOME_NONE;
    m->verify = FLEET_VERIFY_PENDING;
    m->end_reason = FLEET_END_NONE;
    m->end_by_me = 0;
    m->end_unacked = 0;
    fleet_board_clear(&m->own);
    fleet_board_clear(&m->target);
    fleet_board_clear(&m->peer_board);
    m->resyncing = 0;
    m->lost = 0;
    m->probes = 0;
    m->attempts = 0;
    m->ob = FLEET_OB_NONE;
    m->silence_probed = 0;
    /* Nothing said in one match is shown in the next. Our ids run on, so
     * the next session never starts on the numbers this one just used. */
    fleet_chat_reset(&m->chat, m->chat.next_id);
    memcpy(m->self_key, self, sizeof(self));
    memcpy(m->tomb, tomb, sizeof(tomb));
    m->dirty = 1;
    m->revision++;
}

/* A finished match becomes a tombstone before anything replaces it. */
static void retire_done(struct fleet_match *m)
{
    if (m->phase == FLEET_MP_DONE) {
        tomb_push(m, m->sid, m->peer_key, FLEET_TOMB_ENDED,
                  m->end_reason ? m->end_reason : FLEET_END_FINISHED);
        clear_session(m);
    }
}

/* ---- ending ---------------------------------------------------------------- */

static void finish(struct fleet_match *m, uint8_t outcome, uint8_t reason, int by_me)
{
    m->phase = FLEET_MP_DONE;
    m->outcome = outcome;
    m->end_reason = reason;
    m->end_by_me = (uint8_t)by_me;
    m->end_unacked = (uint8_t)(by_me && reason != FLEET_END_NONE);
    m->pending = FLEET_NO_CELL;
    m->resyncing = 0;
    m->lost = 0;
    m->event = FLEET_EV_PHASE;
    mark_dirty(m);
}

static void violation(struct fleet_match *m)
{
    m->stats.violations++;
    finish(m, FLEET_OUTCOME_VOID, FLEET_END_VIOLATION, 1);
}

static void void_match(struct fleet_match *m)
{
    finish(m, FLEET_OUTCOME_VOID, FLEET_END_VOID, 1);
}

static void game_over(struct fleet_match *m)
{
    int winner_is_me = mine(m, m->resolved);

    m->phase = FLEET_MP_REVEAL;
    m->outcome = winner_is_me ? FLEET_OUTCOME_WIN : FLEET_OUTCOME_LOSS;
    m->pending = FLEET_NO_CELL;
    m->event = FLEET_EV_PHASE;
    mark_dirty(m);
}

static void maybe_done_reveal(struct fleet_match *m)
{
    if (m->phase == FLEET_MP_REVEAL && m->have_peer_reveal && m->peer_has_reveal) {
        m->phase = FLEET_MP_DONE;
        mark_dirty(m);
    }
}

/* ---- obligations ------------------------------------------------------------ */

static void obligation_now(const struct fleet_match *m, uint8_t *kind, uint8_t *ply)
{
    *ply = 0;
    if (m->phase == FLEET_MP_DONE && m->end_unacked) {
        *kind = FLEET_OB_END;
    } else if (m->resyncing) {
        *kind = FLEET_OB_SYNC;
    } else if (m->phase == FLEET_MP_INVITING) {
        *kind = FLEET_OB_INVITE;
    } else if (m->phase == FLEET_MP_ACCEPTING) {
        *kind = FLEET_OB_ACCEPT;
    } else if (m->cancel_left) {
        *kind = FLEET_OB_CANCEL;
    } else if ((m->phase == FLEET_MP_COMMITTED || m->phase == FLEET_MP_BATTLE) &&
               m->committed && !m->peer_has_commit) {
        *kind = FLEET_OB_COMMIT;
    } else if (m->phase == FLEET_MP_BATTLE && m->pending != FLEET_NO_CELL) {
        *kind = FLEET_OB_SHOT;
        *ply = (uint8_t)(m->resolved + 1);
    } else if (m->phase == FLEET_MP_REVEAL && (!m->have_peer_reveal || !m->peer_has_reveal)) {
        *kind = FLEET_OB_REVEAL;
    } else {
        *kind = FLEET_OB_NONE;
    }
}

static int ob_max(uint8_t kind)
{
    switch (kind) {
    case FLEET_OB_CANCEL:
        return 2;
    case FLEET_OB_END:
    case FLEET_OB_SYNC:
        return 3;
    default:
        return FLEET_RETRY_MAX;
    }
}

static int send_obligation(struct fleet_match *m, uint8_t kind)
{
    struct fleet_msg msg;

    switch (kind) {
    case FLEET_OB_END:
        build_end(m, &msg, FLEET_MSG_END, 0, m->end_reason);
        break;
    case FLEET_OB_SYNC:
        build_sync(m, &msg, 0);
        break;
    case FLEET_OB_INVITE:
        msg_init(m, &msg, FLEET_MSG_INVITE, 0);
        msg.rules = m->rules;
        break;
    case FLEET_OB_ACCEPT:
        msg_init(m, &msg, FLEET_MSG_ACCEPT, 0);
        break;
    case FLEET_OB_CANCEL:
        msg_init(m, &msg, FLEET_MSG_CANCEL, m->cancel_sid);
        return enqueue(m, m->cancel_to, &msg, kind);
    case FLEET_OB_COMMIT:
        /* Theirs in hand, ours unconfirmed: ask, rather than resend 22 bytes
         * that a peer already holding them would not answer. */
        if (m->have_peer_commit) {
            build_sync(m, &msg, 0);
        } else {
            build_commit(m, &msg);
        }
        break;
    case FLEET_OB_SHOT:
        build_shot(m, &msg);
        break;
    case FLEET_OB_REVEAL:
        if (m->have_peer_reveal) {
            build_sync(m, &msg, 0);
        } else {
            build_reveal(m, &msg);
        }
        break;
    default:
        return DROPPED;
    }
    return enqueue(m, m->peer_key, &msg, kind);
}

static void exhausted(struct fleet_match *m, uint8_t kind)
{
    switch (kind) {
    case FLEET_OB_INVITE:
        tomb_push(m, m->sid, m->peer_key, FLEET_TOMB_CANCELLED, FLEET_END_CANCELLED);
        clear_session(m);
        m->notice = FLEET_NOTICE_NO_ANSWER;
        break;
    case FLEET_OB_ACCEPT:
        clear_session(m);
        m->notice = FLEET_NOTICE_NO_ANSWER;
        break;
    case FLEET_OB_CANCEL:
        m->cancel_left = 0;
        break;
    case FLEET_OB_END:
        m->end_unacked = 0;
        mark_dirty(m);
        break;
    case FLEET_OB_SYNC:
        m->resyncing = 0;
        m->lost = 1;
        m->probes = 0;
        m->next_probe = m->now + FLEET_PROBE_MS + jitter(m, FLEET_PROBE_MS / 4);
        touch(m);
        break;
    default:
        m->lost = 1;
        m->probes = 0;
        m->next_probe = m->now + FLEET_PROBE_MS + jitter(m, FLEET_PROBE_MS / 4);
        touch(m);
        break;
    }
}

static uint32_t backoff(struct fleet_match *m, int attempts)
{
    uint32_t base = m->retry_base ? m->retry_base : FLEET_RETRY_BASE_MS;
    uint32_t d = base;
    int i;

    for (i = 1; i < attempts && d < FLEET_RETRY_CAP_MS; i++) {
        d *= 2;
    }
    if (d > FLEET_RETRY_CAP_MS) {
        d = FLEET_RETRY_CAP_MS;
    }
    return d + jitter(m, d / 2);
}

static void service_game(struct fleet_match *m)
{
    uint8_t kind;
    uint8_t ply;
    int rc;

    obligation_now(m, &kind, &ply);
    if (kind != m->ob || ply != m->ob_ply) {
        m->ob = kind;
        m->ob_ply = ply;
        m->attempts = 0;
        m->busy_run = 0;
        m->next_retry = m->now;
        touch(m);
    }
    if (kind == FLEET_OB_NONE) {
        m->throttled = 0;
        return;
    }
    if (kind == FLEET_OB_REVEAL && m->reveal_sends >= FLEET_REVEAL_SENDS) {
        /* The winner is known; only the proof is missing. After this many
         * requests - however often the peer answered something else, and
         * however long the airtime governor held them back - it is not
         * coming: settle, unverified. */
        m->phase = FLEET_MP_DONE;
        m->lost = 0;
        if (!m->have_peer_reveal) {
            m->verify = FLEET_VERIFY_NONE;
        }
        mark_dirty(m);
        return;
    }
    if (m->lost) {
        struct fleet_msg msg;

        if (m->probes < FLEET_PROBE_MAX && m->now >= m->next_probe) {
            build_sync(m, &msg, 0);
            if (enqueue(m, m->peer_key, &msg, FLEET_OB_SYNC) == SENT) {
                m->probes++;
                m->reveal_sends += kind == FLEET_OB_REVEAL;
                m->next_probe = m->now + FLEET_PROBE_MS + jitter(m, FLEET_PROBE_MS / 4);
            } else {
                m->next_probe = gov_ready(m, m->now);
            }
        }
        return;
    }
    if (m->now < m->next_retry) {
        return;
    }
    if (m->attempts >= ob_max(kind)) {
        exhausted(m, kind);
        return;
    }
    rc = send_obligation(m, kind);
    if (rc == DEFERRED) {
        m->next_retry = gov_ready(m, m->now);
        if (!m->throttled) {
            m->throttled = 1;
            touch(m);
        }
        return;
    }
    if (m->throttled) {
        m->throttled = 0;
        touch(m);
    }
    m->reveal_sends += kind == FLEET_OB_REVEAL;
    m->attempts++;
    m->next_retry = m->now + backoff(m, m->attempts);
    if (m->attempts >= 2) {
        touch(m);
    }
}

/* ---- chat -------------------------------------------------------------------- */

int fleet_match_chat_open(const struct fleet_match *m)
{
    return m && ((m->phase >= FLEET_MP_DEPLOY && m->phase <= FLEET_MP_REVEAL) ||
                 (m->phase == FLEET_MP_DONE && m->end_reason == FLEET_END_NONE));
}

/* Our next line, when the game can spare the air for it. The game owes
 * nothing and is owed nothing: no obligation in force (a shot waiting for its
 * answer included, so a line is never on the air when the answer comes), no
 * resync, the peer not out of reach, and nothing of the game's in the outbox.
 * Waiting costs the line nothing - a try is only counted once it went. */
static void chat_service(struct fleet_match *m)
{
    struct fleet_chat *c = &m->chat;
    struct fleet_chat_line *l;
    struct fleet_msg msg;
    int n;

    if (!fleet_match_chat_open(m)) {
        n = fleet_chat_fail_pending(c);
        if (n > 0) {
            m->stats.chat_failed += (unsigned)n;
            touch(m);
        }
        return;
    }
    l = fleet_chat_outgoing(c);
    if (l && l->state == FLEET_CHAT_SENDING) {
        if (m->now < c->next_try) {
            return;
        }
        if (c->tries >= FLEET_CHAT_TRIES) {
            l->state = FLEET_CHAT_FAILED;
            c->tries = 0;
            m->stats.chat_failed++;
            touch(m);
            l = fleet_chat_outgoing(c);
        }
    }
    if (!l || m->ob != FLEET_OB_NONE || m->lost || m->resyncing || m->throttled ||
        m->out_len > 0) {
        return;
    }
    msg_init(m, &msg, FLEET_MSG_CHAT, 0);
    msg.ply = l->id;
    msg.text_len = l->len;
    memcpy(msg.text, l->text, l->len);
    if (chat_enqueue(m, &msg) != SENT) {
        if (!c->held) {
            c->held = 1;
            m->stats.chat_held++;
        }
        return;
    }
    c->held = 0;
    if (l->state == FLEET_CHAT_QUEUED) {
        l->state = FLEET_CHAT_SENDING;
        c->tries = 0;
    }
    c->tries++;
    c->next_try = m->now + backoff(m, c->tries);
    m->stats.chat_tx++;
    touch(m);
}

/* Everything due: the game's obligation first, then, with what it leaves,
 * a line of chat. */
static void service(struct fleet_match *m)
{
    service_game(m);
    chat_service(m);
}

static void handle_chat(struct fleet_match *m, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int r;

    if (!fleet_match_chat_open(m)) {
        m->stats.rx_stale++;
        return;
    }
    r = fleet_chat_add_theirs(&m->chat, in->ply, in->text, in->text_len);
    if (r < 0) {
        m->stats.rx_bad++;
        return;
    }
    if (r == 1) {
        m->stats.chat_rx++;
        touch(m);
    } else {
        /* Our receipt was lost, or the line came twice. It is answered
         * again, at most once per FLEET_DUP_REPLY_MS, and not shown again. */
        m->stats.chat_dup++;
        if (m->last_reply[FLEET_MSG_CHAT_ACK] &&
            m->now - m->last_reply[FLEET_MSG_CHAT_ACK] < FLEET_DUP_REPLY_MS) {
            return;
        }
    }
    msg_init(m, &msg, FLEET_MSG_CHAT_ACK, 0);
    msg.ply = in->ply;
    msg.check = fleet_chat_check(in->ply, in->text, in->text_len);
    if (chat_enqueue(m, &msg) == SENT) {
        m->last_reply[FLEET_MSG_CHAT_ACK] = m->now ? m->now : 1;
    }
}

static void handle_chat_ack(struct fleet_match *m, const struct fleet_msg *in)
{
    if (fleet_chat_acked(&m->chat, in->ply, in->check)) {
        touch(m);
    }
}

int fleet_match_chat_send(struct fleet_match *m, const char *text, int64_t now)
{
    int rc;

    if (!m || !fleet_match_chat_open(m)) {
        return -1;
    }
    m->now = now;
    rc = fleet_chat_add_mine(&m->chat, text);
    if (rc != 0) {
        return rc;
    }
    touch(m);
    service(m);
    return 0;
}

void fleet_match_chat_seen(struct fleet_match *m)
{
    if (m && m->chat.unread) {
        fleet_chat_seen(&m->chat);
        touch(m);
    }
}

/* ---- contact ----------------------------------------------------------------- */

static void heard(struct fleet_match *m)
{
    m->last_heard = m->now;
    m->silence_probed = 0;
    if (m->lost) {
        m->lost = 0;
        m->probes = 0;
        m->attempts = 0;
        m->next_retry = m->now;
        touch(m);
    }
}

/* ---- incoming -------------------------------------------------------------------- */

static void handle_invite(struct fleet_match *m, const uint8_t *from, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int t = tomb_find(m, in->sid, from);

    if (t >= 0) {
        if (m->tomb[t].kind == FLEET_TOMB_DECLINED &&
            (!m->tomb_reply[t] || m->now - m->tomb_reply[t] >= FLEET_TOMB_REPLY_MS)) {
            msg_init(m, &msg, FLEET_MSG_DECLINE, in->sid);
            msg.reason = FLEET_DECLINE_USER;
            reply(m, from, &msg, 0);
            m->tomb_reply[t] = m->now ? m->now : 1;
        }
        m->stats.rx_stale++;
        return;
    }
    if (m->phase == FLEET_MP_DONE) {
        retire_done(m);
    }
    switch (m->phase) {
    case FLEET_MP_IDLE:
        if (in->rules != FLEET_RULES_CLASSIC) {
            msg_init(m, &msg, FLEET_MSG_DECLINE, in->sid);
            msg.reason = FLEET_DECLINE_RULES;
            reply(m, from, &msg, 1);
            return;
        }
        m->phase = FLEET_MP_INVITED;
        m->role = FLEET_ROLE_GUEST;
        m->sid = in->sid;
        m->rules = in->rules;
        memcpy(m->peer_key, from, FLEET_KEY_BYTES);
        m->peer_name[0] = '\0';
        m->notice = FLEET_NOTICE_NONE;
        m->last_heard = m->now;
        touch(m);
        return;
    case FLEET_MP_INVITED:
        if (key_eq(from, m->peer_key)) {
            if (in->sid != m->sid) {
                m->sid = in->sid;   /* the host asked again, afresh */
                touch(m);
            }
            return;
        }
        break;
    case FLEET_MP_INVITING:
        if (key_eq(from, m->peer_key)) {
            /* Crossed invites: the lower key's invite survives. */
            if (memcmp(from, m->self_key, FLEET_KEY_BYTES) < 0) {
                m->role = FLEET_ROLE_GUEST;
                m->sid = in->sid;
                m->rules = in->rules;
                m->phase = FLEET_MP_ACCEPTING;
                m->notice = FLEET_NOTICE_CROSSED;
                touch(m);
                service(m);
            }
            return;
        }
        break;
    case FLEET_MP_ACCEPTING:
        if (key_eq(from, m->peer_key) && in->sid == m->sid) {
            return;
        }
        break;
    default:
        if (key_eq(from, m->peer_key)) {
            if (in->sid == m->sid) {
                return;
            }
            msg_init(m, &msg, FLEET_MSG_DECLINE, in->sid);
            msg.reason = FLEET_DECLINE_BUSY_WITH_YOU;
            msg.other_sid = m->sid;
            reply(m, from, &msg, 1);
            return;
        }
        break;
    }
    msg_init(m, &msg, FLEET_MSG_DECLINE, in->sid);
    msg.reason = FLEET_DECLINE_BUSY;
    reply(m, from, &msg, 1);
}

/* A packet for a session this device does not have in hand. */
static void handle_stranger(struct fleet_match *m, const uint8_t *from, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int t = tomb_find(m, in->sid, from);

    m->stats.rx_stale++;
    /* A receipt for a line of a session gone asks nothing, like END_ACK. A
     * line of one is answered as any packet of it is: the peer learns the
     * session is over, and nothing it said is shown in another. */
    if (in->type == FLEET_MSG_END_ACK || in->type == FLEET_MSG_DECLINE ||
        in->type == FLEET_MSG_CANCEL || in->type == FLEET_MSG_CHAT_ACK) {
        return;
    }
    if (t >= 0) {
        uint8_t kind = m->tomb[t].kind;

        if (m->tomb_reply[t] && m->now - m->tomb_reply[t] < FLEET_TOMB_REPLY_MS &&
            in->type != FLEET_MSG_END) {
            return;
        }
        if (in->type == FLEET_MSG_END) {
            build_end(m, &msg, FLEET_MSG_END_ACK, in->sid, in->reason);
        } else if (kind == FLEET_TOMB_CANCELLED) {
            build_end(m, &msg, FLEET_MSG_END, in->sid, FLEET_END_CANCELLED);
        } else if (kind == FLEET_TOMB_ENDED) {
            build_end(m, &msg, FLEET_MSG_END, in->sid, m->tomb[t].reason);
        } else {
            return;
        }
        reply(m, from, &msg, 0);
        m->tomb_reply[t] = m->now ? m->now : 1;
        return;
    }
    if (in->type == FLEET_MSG_END) {
        build_end(m, &msg, FLEET_MSG_END_ACK, in->sid, in->reason);
        reply(m, from, &msg, 1);
        return;
    }
    if (m->unknown_sid == in->sid && m->unknown_reply &&
        m->now - m->unknown_reply < FLEET_TOMB_REPLY_MS) {
        return;
    }
    build_end(m, &msg, FLEET_MSG_END, in->sid,
              m->store_fault ? FLEET_END_ABANDON : FLEET_END_UNKNOWN);
    reply(m, from, &msg, 0);
    m->unknown_sid = in->sid;
    m->unknown_reply = m->now ? m->now : 1;
}

static void enter_battle_if_ready(struct fleet_match *m)
{
    if (m->phase == FLEET_MP_COMMITTED && m->committed && m->have_peer_commit) {
        m->phase = FLEET_MP_BATTLE;
        m->event = FLEET_EV_PHASE;
        mark_dirty(m);
    }
}

static void handle_commit(struct fleet_match *m, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int first = !m->have_peer_commit;

    if (m->phase < FLEET_MP_DEPLOY) {
        return;
    }
    if (!first) {
        if (memcmp(m->peer_commit, in->commit, FLEET_COMMIT_BYTES) != 0) {
            if (m->phase != FLEET_MP_DONE) {
                violation(m);
            }
            return;
        }
    } else {
        memcpy(m->peer_commit, in->commit, FLEET_COMMIT_BYTES);
        m->have_peer_commit = 1;
        mark_dirty(m);
    }
    if ((in->flags & FLEET_FLAG_HAVE_PEER) && m->committed && !m->peer_has_commit) {
        m->peer_has_commit = 1;
        mark_dirty(m);
    }
    enter_battle_if_ready(m);
    if (!(in->flags & FLEET_FLAG_HAVE_PEER) || first) {
        if (m->committed) {
            build_commit(m, &msg);
        } else {
            build_sync(m, &msg, 1);
        }
        reply(m, m->peer_key, &msg, !first);
    }
}

/* Take the answer to our pending shot. Returns 0, or -1 after ending the
 * match over an answer that could not be true. */
static int apply_answer(struct fleet_match *m, int ply, uint8_t cell, uint8_t res)
{
    m->resolved = (uint8_t)ply;
    m->log_cell[ply] = cell;
    m->log_res[ply] = res;
    m->pending = FLEET_NO_CELL;
    if (build_target(m) != 0) {
        violation(m);
        return -1;
    }
    m->event = FLEET_EV_ANSWER;
    m->event_cell = cell;
    m->event_res = res;
    mark_dirty(m);
    if (fleet_res_destroyed(res)) {
        game_over(m);
    }
    return 0;
}

static void handle_shot(struct fleet_match *m, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int k = in->ply;

    if (m->phase < FLEET_MP_COMMITTED) {
        return;
    }
    if (fleet_match_shooter(k) != peer_role(m)) {
        m->stats.rx_stale++;
        return;
    }
    if (!m->have_peer_commit) {
        /* They cannot fire without holding ours; we are missing theirs. */
        build_sync(m, &msg, 1);
        reply(m, m->peer_key, &msg, 1);
        return;
    }
    if (m->committed && !m->peer_has_commit) {
        m->peer_has_commit = 1;
        mark_dirty(m);
    }
    enter_battle_if_ready(m);
    if (k <= m->resolved) {
        if (m->log_cell[k] != in->cell) {
            if (m->phase != FLEET_MP_DONE) {
                violation(m);
            }
            return;
        }
        if (k == m->resolved) {
            resend_latest(m, 1);
        } else {
            m->stats.rx_stale++;
        }
        return;
    }
    if (m->phase != FLEET_MP_BATTLE) {
        return;
    }
    if (k == m->resolved + 2 && m->pending != FLEET_NO_CELL) {
        /* Their next shot overtook the answer to ours; it carries it. */
        if (apply_answer(m, m->resolved + 1, m->pending, in->res) != 0 ||
            m->phase != FLEET_MP_BATTLE) {
            return;
        }
    }
    if (k != m->resolved + 1 || m->pending != FLEET_NO_CELL) {
        m->resyncing = 1;
        m->stats.resyncs++;
        touch(m);
        return;
    }
    {
        int sunk;
        enum fleet_shot_result r;

        if (m->own.shot[in->cell]) {
            violation(m);
            return;
        }
        r = fleet_board_fire(&m->own, in->cell / FLEET_GRID, in->cell % FLEET_GRID, &sunk);
        m->resolved = (uint8_t)k;
        m->log_cell[k] = in->cell;
        m->log_res[k] = fleet_res_make(r, sunk, m->own.ships_afloat == 0);
        m->event = FLEET_EV_INCOMING;
        m->event_cell = in->cell;
        m->event_res = m->log_res[k];
        mark_dirty(m);
        build_result(m, &msg, k);
        reply(m, m->peer_key, &msg, 0);
        if (fleet_res_destroyed(m->log_res[k])) {
            game_over(m);
        }
    }
}

static void handle_result(struct fleet_match *m, const struct fleet_msg *in)
{
    int k = in->ply;

    if (m->phase < FLEET_MP_BATTLE || fleet_match_shooter(k) != m->role) {
        m->stats.rx_stale++;
        return;
    }
    if (!m->peer_has_commit) {
        m->peer_has_commit = 1;
        mark_dirty(m);
    }
    if (k <= m->resolved) {
        /* A copy. The answer to a ply may never change. */
        if ((m->log_cell[k] != in->cell || m->log_res[k] != in->res) &&
            m->phase != FLEET_MP_DONE) {
            violation(m);
        }
        return;
    }
    if (m->phase == FLEET_MP_BATTLE && k == m->resolved + 1 && m->pending != FLEET_NO_CELL) {
        if (in->cell != m->pending) {
            violation(m);
            return;
        }
        apply_answer(m, k, in->cell, in->res);
        return;
    }
    m->resyncing = 1;
    m->stats.resyncs++;
    touch(m);
}

static void handle_sync(struct fleet_match *m, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int is_reply = (in->flags & FLEET_SYNC_REPLY) != 0;
    int peer_phase = (in->flags >> FLEET_SYNC_PHASE_SHIFT) & 7;
    int rp = in->ply;
    int r = m->resolved;

    if (is_reply) {
        m->resyncing = 0;
    }
    if (m->committed && m->phase < FLEET_MP_DONE) {
        uint8_t has = (in->flags & FLEET_SYNC_HAVE_COMMIT) != 0;

        /* "I do not hold yours" can be old news; once a ply has been played
         * they certainly hold it, so it is only believed before that. */
        if (has && !m->peer_has_commit) {
            m->peer_has_commit = 1;
            mark_dirty(m);
        } else if (!has && m->peer_has_commit && m->resolved == 0) {
            m->peer_has_commit = 0;
            mark_dirty(m);
        }
    }
    if ((in->flags & FLEET_SYNC_HAVE_REVEAL) && m->phase >= FLEET_MP_REVEAL &&
        !m->peer_has_reveal) {
        m->peer_has_reveal = 1;
        mark_dirty(m);
    }
    if (m->phase == FLEET_MP_DONE && m->end_reason != FLEET_END_NONE) {
        build_end(m, &msg, FLEET_MSG_END, 0, m->end_reason == FLEET_END_FORFEIT && !m->end_by_me
                                                 ? FLEET_END_FINISHED : m->end_reason);
        reply(m, m->peer_key, &msg, 1);
        return;
    }
    /* Compare the logs once both sides are past deployment. A SYNC may have
     * been a long time in flight, so a peer that looks behind may simply be
     * speaking from the past: only the digest of the plies both hold is
     * evidence, and "behind" never is. */
    if ((m->phase == FLEET_MP_BATTLE || m->phase == FLEET_MP_REVEAL) &&
        peer_phase >= FLEET_SYNC_BATTLE) {
        if (rp <= r) {
            if (fleet_match_digest(m, rp) != in->digest) {
                void_match(m);
                return;
            }
            if (rp == r - 1 && !mine(m, r)) {
                /* They may be missing our answer to their last shot. */
                resend_latest(m, 1);
            }
        } else if (rp == r + 1 && m->pending != FLEET_NO_CELL) {
            /* They answered our pending shot; its answer is on its way. */
        } else {
            /* Ahead of anything this device has ever sent them. */
            void_match(m);
            return;
        }
    } else if (m->phase >= FLEET_MP_DEPLOY && m->phase < FLEET_MP_BATTLE && rp > 0) {
        void_match(m);
        return;
    }
    /* Give them what they said they lack. */
    if (m->committed && !(in->flags & FLEET_SYNC_HAVE_COMMIT) && m->phase < FLEET_MP_REVEAL &&
        m->resolved == 0) {
        build_commit(m, &msg);
        reply(m, m->peer_key, &msg, 1);
    }
    if (m->phase >= FLEET_MP_REVEAL && !(in->flags & FLEET_SYNC_HAVE_REVEAL) &&
        m->end_reason == FLEET_END_NONE) {
        build_reveal(m, &msg);
        reply(m, m->peer_key, &msg, 1);
    }
    if (!is_reply) {
        build_sync(m, &msg, 1);
        reply(m, m->peer_key, &msg, 1);
    }
    maybe_done_reveal(m);
}

static void handle_reveal(struct fleet_match *m, const struct fleet_msg *in)
{
    struct fleet_msg msg;
    int first = !m->have_peer_reveal;

    if (m->phase < FLEET_MP_REVEAL) {
        if (m->phase == FLEET_MP_BATTLE) {
            /* They think the game is over and we do not. */
            m->resyncing = 1;
            m->stats.resyncs++;
            touch(m);
        }
        return;
    }
    if (m->end_reason != FLEET_END_NONE) {
        return;
    }
    if (first) {
        memcpy(m->peer_layout, in->layout, FLEET_LAYOUT_BYTES);
        memcpy(m->peer_salt, in->salt, FLEET_SALT_BYTES);
        m->have_peer_reveal = 1;
        m->verify = verify_peer(m);
        mark_dirty(m);
    } else if (memcmp(m->peer_layout, in->layout, FLEET_LAYOUT_BYTES) != 0 ||
               memcmp(m->peer_salt, in->salt, FLEET_SALT_BYTES) != 0) {
        m->verify = FLEET_VERIFY_MISMATCH;
        mark_dirty(m);
    }
    if ((in->flags & FLEET_FLAG_HAVE_PEER) && !m->peer_has_reveal) {
        m->peer_has_reveal = 1;
        mark_dirty(m);
    }
    if (!(in->flags & FLEET_FLAG_HAVE_PEER) || first) {
        build_reveal(m, &msg);
        reply(m, m->peer_key, &msg, !first);
    }
    maybe_done_reveal(m);
}

static void handle_end(struct fleet_match *m, const uint8_t *from, const struct fleet_msg *in)
{
    struct fleet_msg msg;

    if (m->phase <= FLEET_MP_ACCEPTING) {
        clear_session(m);
        m->notice = FLEET_NOTICE_CANCELLED;
    } else if (m->phase != FLEET_MP_DONE) {
        if (in->reason == FLEET_END_FORFEIT) {
            finish(m, FLEET_OUTCOME_WIN, FLEET_END_FORFEIT, 0);
        } else if (in->reason == FLEET_END_FINISHED && m->phase == FLEET_MP_REVEAL) {
            /* They are done with a game we both saw end; stop asking. */
            if (!m->have_peer_reveal) {
                m->verify = FLEET_VERIFY_NONE;
            }
            finish(m, m->outcome, FLEET_END_NONE, 0);
        } else {
            finish(m, FLEET_OUTCOME_VOID,
                   in->reason == FLEET_END_FINISHED ? FLEET_END_VOID : in->reason, 0);
        }
    }
    build_end(m, &msg, FLEET_MSG_END_ACK, in->sid, in->reason);
    reply(m, from, &msg, 1);
}

void fleet_match_receive(struct fleet_match *m, const uint8_t from[FLEET_KEY_BYTES],
                         const uint8_t *buf, size_t n, int64_t now)
{
    struct fleet_msg in;
    int session;

    if (!m || !from) {
        return;
    }
    m->now = now;
    if (fleet_proto_decode(&in, buf, n) != 0) {
        m->stats.rx_bad++;
        return;
    }
    m->stats.rx++;
    if (key_eq(from, m->self_key)) {
        m->stats.rx_foreign++;
        return;
    }
    if (in.type == FLEET_MSG_INVITE) {
        handle_invite(m, from, &in);
        service(m);
        return;
    }
    session = m->phase != FLEET_MP_IDLE && in.sid == m->sid;
    if (session && !key_eq(from, m->peer_key)) {
        m->stats.rx_foreign++;
        return;
    }
    if (!session) {
        /* The host's own cancel and a DECLINE answer our invite to a node
         * whose reply may arrive after we gave up; both are harmless. */
        if (in.type == FLEET_MSG_END && m->phase == FLEET_MP_IDLE && m->cancel_left &&
            in.sid == m->cancel_sid) {
            m->cancel_left = 0;
        }
        handle_stranger(m, from, &in);
        service(m);
        return;
    }
    heard(m);
    if (m->phase == FLEET_MP_DONE && m->end_reason != FLEET_END_NONE &&
        in.type != FLEET_MSG_END && in.type != FLEET_MSG_END_ACK && in.type != FLEET_MSG_SYNC) {
        /* It is over; say so to whatever still arrives, or a peer that missed
         * the END would ask into silence for ever. */
        struct fleet_msg msg;

        build_end(m, &msg, FLEET_MSG_END, 0, m->end_reason == FLEET_END_FORFEIT && !m->end_by_me
                                                  ? FLEET_END_FINISHED : m->end_reason);
        reply(m, m->peer_key, &msg, 1);
        service(m);
        return;
    }
    if (in.type != FLEET_MSG_SYNC && m->resyncing && m->phase >= FLEET_MP_BATTLE &&
        (in.type == FLEET_MSG_SHOT || in.type == FLEET_MSG_RESULT)) {
        /* Ordinary play resumed; it carries what a SYNC would have. */
        m->resyncing = 0;
    }
    switch (in.type) {
    case FLEET_MSG_ACCEPT: {
        int fresh = m->phase == FLEET_MP_INVITING;

        if (fresh) {
            m->role = FLEET_ROLE_HOST;
            m->phase = FLEET_MP_DEPLOY;
            m->event = FLEET_EV_PHASE;
            mark_dirty(m);
        }
        if (m->role == FLEET_ROLE_HOST && m->phase >= FLEET_MP_DEPLOY) {
            struct fleet_msg msg;

            msg_init(m, &msg, FLEET_MSG_START, 0);
            reply(m, m->peer_key, &msg, !fresh);
        }
        break;
    }
    case FLEET_MSG_DECLINE:
        if (m->phase == FLEET_MP_INVITING) {
            if (in.reason == FLEET_DECLINE_BUSY_WITH_YOU) {
                /* They hold a match with us that we no longer have. */
                struct fleet_msg msg;
                int t = tomb_find(m, in.other_sid, from);

                build_end(m, &msg, FLEET_MSG_END, in.other_sid,
                          t >= 0 && m->tomb[t].kind == FLEET_TOMB_ENDED
                              ? m->tomb[t].reason : FLEET_END_UNKNOWN);
                msg.ply = 0;
                reply(m, from, &msg, 1);
                break;
            }
            clear_session(m);
            m->notice = in.reason == FLEET_DECLINE_USER ? FLEET_NOTICE_DECLINED
                      : in.reason == FLEET_DECLINE_BUSY ? FLEET_NOTICE_BUSY
                                                          : FLEET_NOTICE_INCOMPATIBLE;
        }
        break;
    case FLEET_MSG_START:
        if (m->phase == FLEET_MP_ACCEPTING) {
            m->phase = FLEET_MP_DEPLOY;
            m->event = FLEET_EV_PHASE;
            mark_dirty(m);
        }
        break;
    case FLEET_MSG_CANCEL:
        if (m->phase == FLEET_MP_INVITED || m->phase == FLEET_MP_ACCEPTING) {
            clear_session(m);
            m->notice = FLEET_NOTICE_CANCELLED;
        }
        break;
    case FLEET_MSG_COMMIT:
    case FLEET_MSG_SYNC:
        if (m->phase == FLEET_MP_ACCEPTING) {
            /* The host has moved on: START was lost, the session is live. */
            m->phase = FLEET_MP_DEPLOY;
            mark_dirty(m);
        }
        if (in.type == FLEET_MSG_COMMIT) {
            handle_commit(m, &in);
        } else if (m->phase >= FLEET_MP_DEPLOY) {
            handle_sync(m, &in);
        }
        break;
    case FLEET_MSG_SHOT:
        handle_shot(m, &in);
        break;
    case FLEET_MSG_RESULT:
        handle_result(m, &in);
        break;
    case FLEET_MSG_REVEAL:
        handle_reveal(m, &in);
        break;
    case FLEET_MSG_END:
        handle_end(m, from, &in);
        break;
    case FLEET_MSG_END_ACK:
        if (m->phase == FLEET_MP_DONE && m->end_unacked) {
            m->end_unacked = 0;
            mark_dirty(m);
        }
        break;
    case FLEET_MSG_CHAT:
        handle_chat(m, &in);
        break;
    case FLEET_MSG_CHAT_ACK:
        handle_chat_ack(m, &in);
        break;
    default:
        break;
    }
    service(m);
}

/* ---- time ---------------------------------------------------------------------- */

void fleet_match_tick(struct fleet_match *m, int64_t now)
{
    if (!m) {
        return;
    }
    m->now = now;
    gov_refill(m, now);
    /* A probe after each long silence on the opponent's turn, a few times:
     * one probe, or its answer, can be lost, and a peer that ended the match
     * while we waited would otherwise never be heard from again. */
    if (m->phase == FLEET_MP_BATTLE && !m->lost && !m->resyncing && m->ob == FLEET_OB_NONE &&
        !fleet_match_my_turn(m) && m->silence_probed < FLEET_SILENCE_PROBES &&
        now - m->last_heard >= (int64_t)FLEET_SILENCE_PROBE_MS * (m->silence_probed + 1)) {
        struct fleet_msg msg;

        build_sync(m, &msg, 0);
        if (enqueue(m, m->peer_key, &msg, FLEET_OB_SYNC) == SENT) {
            m->silence_probed++;
        }
    }
    service(m);
}

void fleet_match_tx_refused(struct fleet_match *m, const struct fleet_match_out *o, int64_t now)
{
    if (!m || !o) {
        return;
    }
    m->now = now;
    m->stats.busy++;
    if (o->obligation && o->obligation == m->ob && m->attempts > 0 && m->busy_run < 20) {
        m->attempts--;
        m->busy_run++;
        m->next_retry = now + FLEET_BUSY_RETRY_MS + jitter(m, FLEET_BUSY_RETRY_MS / 2);
    }
}

void fleet_match_set_retry_base(struct fleet_match *m, uint32_t ms)
{
    if (m) {
        m->retry_base = ms < FLEET_RETRY_BASE_MS ? FLEET_RETRY_BASE_MS : ms;
    }
}

/* ---- outputs --------------------------------------------------------------------- */

int fleet_match_pop(struct fleet_match *m, struct fleet_match_out *o)
{
    if (!m || !o || m->out_len == 0 || (m->dirty && !m->save_off)) {
        return 0;
    }
    *o = m->out[m->out_head];
    m->out_head = (uint8_t)((m->out_head + 1) % FLEET_MATCH_OUTBOX);
    m->out_len--;
    return 1;
}

int fleet_match_dirty(const struct fleet_match *m)
{
    return m && m->dirty;
}

void fleet_match_saved(struct fleet_match *m)
{
    if (m) {
        m->dirty = 0;
    }
}

void fleet_match_save_failed(struct fleet_match *m)
{
    if (m) {
        m->save_off = 1;
        m->dirty = 0;
        touch(m);
    }
}

void fleet_match_set_store_fault(struct fleet_match *m, int fault)
{
    if (m) {
        m->store_fault = (uint8_t)(fault != 0);
    }
}

/* ---- lifecycle ------------------------------------------------------------------- */

void fleet_match_init(struct fleet_match *m, const uint8_t self_key[FLEET_KEY_BYTES],
                      uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    memcpy(m->self_key, self_key, FLEET_KEY_BYTES);
    m->pending = FLEET_NO_CELL;
    m->tokens = FLEET_GOV_BURST;
    fleet_rng_seed(&m->rng, seed);
    /* From the seed rather than the generator, so the jitter a match draws
     * is the same with or without chat. */
    fleet_chat_reset(&m->chat, (uint8_t)(seed ^ seed >> 8 ^ seed >> 16 ^ seed >> 24));
    fleet_board_clear(&m->own);
    fleet_board_clear(&m->target);
    fleet_board_clear(&m->peer_board);
    {
        int i;

        for (i = 0; i < 60; i++) {
            m->minute_of[i] = -1000;
        }
    }
}

int fleet_match_restore(struct fleet_match *m, int64_t now)
{
    int k;

    m->now = now;
    m->last_heard = now;
    m->refill_at = now;
    m->out_len = 0;
    m->out_head = 0;
    m->ob = FLEET_OB_NONE;
    m->lost = 0;
    m->resyncing = 0;
    if (m->phase < FLEET_MP_DEPLOY) {
        clear_session(m);
        m->dirty = 0;
        return 0;
    }
    if (m->phase >= FLEET_MP_PHASE_COUNT ||
        (m->role != FLEET_ROLE_HOST && m->role != FLEET_ROLE_GUEST) || m->sid == 0 ||
        m->sid > 0xFFFFFF || m->resolved > FLEET_PROTO_PLY_MAX) {
        return -1;
    }
    for (k = 1; k <= m->resolved; k++) {
        if (m->log_cell[k] >= FLEET_CELLS || !fleet_res_valid(m->log_res[k])) {
            return -1;
        }
    }
    if (m->phase >= FLEET_MP_COMMITTED && m->phase <= FLEET_MP_REVEAL && !m->committed) {
        return -1;
    }
    if (m->committed) {
        uint8_t c[FLEET_COMMIT_BYTES];

        fleet_commit_compute(m->sid, m->self_key, m->own_layout, m->own_salt, c);
        if (memcmp(c, m->own_commit, FLEET_COMMIT_BYTES) != 0) {
            return -1;
        }
    }
    if (m->resolved > 0 && (!m->committed || !m->have_peer_commit)) {
        return -1;
    }
    if (build_own(m) != 0) {
        return -1;
    }
    if (build_target(m) != 0 && m->end_reason != FLEET_END_VIOLATION) {
        return -1;
    }
    if (m->pending != FLEET_NO_CELL) {
        if (m->phase != FLEET_MP_BATTLE || m->pending >= FLEET_CELLS ||
            !mine(m, m->resolved + 1) || m->target.shot[m->pending]) {
            return -1;
        }
    }
    if (m->have_peer_reveal) {
        m->verify = verify_peer(m);
    }
    m->dirty = 0;
    touch(m);
    return 0;
}

/* ---- user actions ------------------------------------------------------------------ */

int fleet_match_invite(struct fleet_match *m, const uint8_t peer[FLEET_KEY_BYTES],
                       const char *peer_name, uint32_t sid_entropy, int64_t now)
{
    uint32_t sid = sid_entropy & 0xFFFFFF;
    int i;

    if (!m || !peer || key_eq(peer, m->self_key)) {
        return -1;
    }
    m->now = now;
    if (m->phase == FLEET_MP_DONE) {
        retire_done(m);
    }
    if (m->phase != FLEET_MP_IDLE) {
        return -1;
    }
    /* Never 0, never a sid still remembered. */
    for (i = 0; i < 64; i++) {
        int j;
        int taken = sid == 0;

        for (j = 0; j < FLEET_MATCH_TOMBSTONES && !taken; j++) {
            taken = m->tomb[j].sid == sid;
        }
        if (!taken) {
            break;
        }
        sid = (sid * 2654435761u + 0x9E3779B9u) & 0xFFFFFF;
    }
    m->phase = FLEET_MP_INVITING;
    m->role = FLEET_ROLE_HOST;
    m->sid = sid;
    m->rules = FLEET_RULES_CLASSIC;
    memcpy(m->peer_key, peer, FLEET_KEY_BYTES);
    fleet_match_set_peer_name(m, peer_name);
    m->notice = FLEET_NOTICE_NONE;
    m->cancel_left = 0;
    m->last_heard = now;
    touch(m);
    service(m);
    return 0;
}

int fleet_match_cancel(struct fleet_match *m, int64_t now)
{
    if (!m || m->phase != FLEET_MP_INVITING) {
        return -1;
    }
    m->now = now;
    tomb_push(m, m->sid, m->peer_key, FLEET_TOMB_CANCELLED, FLEET_END_CANCELLED);
    m->cancel_sid = m->sid;
    memcpy(m->cancel_to, m->peer_key, FLEET_KEY_BYTES);
    clear_session(m);
    m->cancel_left = 1;
    service(m);
    return 0;
}

int fleet_match_accept(struct fleet_match *m, int64_t now)
{
    if (!m || m->phase != FLEET_MP_INVITED) {
        return -1;
    }
    m->now = now;
    m->phase = FLEET_MP_ACCEPTING;
    touch(m);
    service(m);
    return 0;
}

int fleet_match_decline(struct fleet_match *m, int64_t now)
{
    struct fleet_msg msg;
    uint8_t to[FLEET_KEY_BYTES];

    if (!m || m->phase != FLEET_MP_INVITED) {
        return -1;
    }
    m->now = now;
    memcpy(to, m->peer_key, sizeof(to));
    msg_init(m, &msg, FLEET_MSG_DECLINE, 0);
    msg.reason = FLEET_DECLINE_USER;
    tomb_push(m, m->sid, m->peer_key, FLEET_TOMB_DECLINED, 0);
    clear_session(m);
    reply(m, to, &msg, 0);
    return 0;
}

int fleet_match_deploy(struct fleet_match *m, const struct fleet_board *board,
                       const uint8_t salt[FLEET_SALT_BYTES], int64_t now)
{
    if (!m || !board || !salt || m->phase != FLEET_MP_DEPLOY) {
        return -1;
    }
    if (fleet_layout_encode(board, m->own_layout) != 0) {
        return -1;
    }
    m->now = now;
    memcpy(m->own_salt, salt, FLEET_SALT_BYTES);
    fleet_commit_compute(m->sid, m->self_key, m->own_layout, m->own_salt, m->own_commit);
    m->committed = 1;
    m->peer_has_commit = 0;
    m->phase = FLEET_MP_COMMITTED;
    if (build_own(m) != 0) {
        m->committed = 0;
        m->phase = FLEET_MP_DEPLOY;
        return -1;
    }
    mark_dirty(m);
    enter_battle_if_ready(m);
    service(m);
    return 0;
}

int fleet_match_fire(struct fleet_match *m, int row, int col, int64_t now)
{
    int idx = fleet_index(row, col);

    if (!m || idx < 0 || !fleet_match_my_turn(m) || m->target.shot[idx]) {
        return -1;
    }
    m->now = now;
    m->pending = (uint8_t)idx;
    mark_dirty(m);
    service(m);
    return 0;
}

int fleet_match_forfeit(struct fleet_match *m, int64_t now)
{
    if (!m || m->phase < FLEET_MP_DEPLOY || m->phase > FLEET_MP_BATTLE) {
        return -1;
    }
    m->now = now;
    finish(m, FLEET_OUTCOME_LOSS, FLEET_END_FORFEIT, 1);
    service(m);
    return 0;
}

int fleet_match_resume(struct fleet_match *m, int64_t now)
{
    if (!m || !(fleet_match_active(m) || (m->phase == FLEET_MP_DONE && m->end_unacked))) {
        return -1;
    }
    m->now = now;
    m->lost = 0;
    m->probes = 0;
    if (m->phase == FLEET_MP_DONE) {
        m->attempts = 0;
        m->next_retry = now;
    } else {
        m->resyncing = 1;
        m->stats.resyncs++;
    }
    touch(m);
    service(m);
    return 0;
}

void fleet_match_dismiss(struct fleet_match *m)
{
    if (!m) {
        return;
    }
    if (m->phase == FLEET_MP_DONE) {
        retire_done(m);
    }
    m->notice = FLEET_NOTICE_NONE;
    touch(m);
}

void fleet_match_set_peer_name(struct fleet_match *m, const char *name)
{
    size_t i;

    if (!m) {
        return;
    }
    memset(m->peer_name, 0, sizeof(m->peer_name));
    if (!name) {
        return;
    }
    for (i = 0; i + 1 < sizeof(m->peer_name) && name[i]; i++) {
        m->peer_name[i] = name[i];
    }
    touch(m);
}
