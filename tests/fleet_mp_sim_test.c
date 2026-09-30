/*
 * PocketFleet multiplayer under a hostile network, thousands of times
 * (docs/apps/FLEET_MULTIPLAYER.md, "Tests").
 *
 * Two nodes, each a match state machine with a saved blob and an automatic
 * player (PocketFleet's own AI, handed only its own shots and their answers),
 * play whole matches over a simulated radio. The simulation is discrete
 * time, 100 ms a step, and entirely deterministic from its seed, so any
 * failure prints the seed that reproduces it.
 *
 * The radio: every frame has its airtime (fleet_proto_airtime_ms), a node
 * transmits one frame at a time, and two frames whose airtimes overlap are
 * both lost - LoRa is half duplex and a collision costs both. On top of that,
 * per profile: random loss, duplication, reordering (delivery jitter),
 * partitions of 30 s to 10 minutes, and faults on the nodes themselves: a
 * crash with the last change unsaved, a crash after saving but before
 * sending, the app closed and reopened later (packets wait in a 16-deep
 * inbox, as they do in meshcored), a meshcored restart (the inbox is lost),
 * and a reboot (both).
 *
 * After every step:
 *   I1  the two logs never differ by more than one ply;
 *   I2  they agree on every ply both hold;
 *   I3  a ply, once seen anywhere, never changes (one shot is one ply);
 *   I4  every SHOT or RESULT that reaches a node was already saved by its
 *       sender (nothing leaves that is not on disk);
 *   I5  no node spends more than 90 s of airtime in any hour of one run of
 *       the app;
 * and at the end:
 *   I6  the match finished on both sides, one winner, both fleets verified;
 *   I7  the log is exactly the log of the same match on a perfect network:
 *       faults change when things happen, never what happens;
 *   I8  every answer in it is what the defender's real fleet says.
 *
 * In the talking profiles both players also chat all the way through, a line
 * every 3 to 18 s whenever they have room, and every 5 s of the match:
 *   C1  no line is shown twice in a node's history;
 *   C2  every line of theirs was said by the opponent in this session - a
 *       line of an earlier one is never shown in a later one;
 * and I5-I8 are unchanged: talking changes what the airtime is spent on,
 * never what happens in the match, and the reference it is compared with
 * is the same match played in silence on a perfect network.
 *
 * Then cheating peers, each of which must be caught (or made harmless):
 * lying about one hit, answering from a different fleet than the one
 * committed, refusing to reveal, revealing with the wrong salt, reporting an
 * impossible sinking, firing out of turn and firing twice, replaying a
 * previous match's packets, and a third node spoofing the session.
 *
 * FLEET_SIM_MATCHES sets matches per honest profile (default 300).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_mp_harness.h"

#include <stdlib.h>

#define STEP_MS 100
#define INBOX 16
#define FLIGHTS 2048
#define HOUR_MS 3600000LL

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

struct profile {
    const char *name;
    int loss_pct;
    int dup_pct;
    int jitter_ms;          /* delivery delay spread: reordering */
    int collisions;
    int partitions;         /* mean minutes between outages, 0 = none */
    int crashes;            /* per-step odds x 1e-6 of each fault */
    int closes;
    int restarts;
    int reboots;
    int chat;               /* both players talk throughout */
};

enum cheat {
    HONEST = 0,
    CHEAT_LIE,
    CHEAT_MOVE,
    CHEAT_NOREVEAL,
    CHEAT_SALT,
    CHEAT_IMPOSSIBLE,
    CHEAT_DOUBLE,
    CHEAT_REPLAY,
    CHEAT_SPOOF,
};

struct flight {
    int from;
    int to;
    int64_t start;
    int64_t end;
    int64_t at;
    int lost;
    uint8_t len;
    uint8_t b[FLEET_PROTO_MAX];
};

struct simnode {
    struct mp_node n;
    int open;
    int64_t reopen_at;
    int lose_inbox;
    struct {
        uint8_t len;
        uint8_t b[FLEET_PROTO_MAX];
    } inbox[INBOX];
    int inbox_n;
    int64_t deaf_until;
    int64_t move_at;
    int seen;
    struct fleet_rng trng;
    int64_t tx_free;
    int64_t run_start;       /* when this run of the app began */
    uint32_t minute_air[60];
    int64_t minute_of[60];
    unsigned frames;
    unsigned airtime;
    int hold_send;
    /* talking */
    struct fleet_rng crng;
    int64_t say_at;
    unsigned said;
};

struct sim {
    const struct profile *p;
    enum cheat cheat;
    struct simnode s[3];
    struct flight fl[FLIGHTS];
    int fn;
    int64_t now;
    struct fleet_rng rng;
    int64_t part_until;
    int64_t next_part;
    uint32_t seed;
    /* the match being checked */
    uint32_t sid;
    uint8_t ledger_cell[FLEET_PROTO_PLY_MAX + 1];
    uint8_t ledger_res[FLEET_PROTO_PLY_MAX + 1];
    int ledger_n;
    int attempts;
    int done;
    char why[160];
    /* cheats */
    uint8_t lie_ply[FLEET_PROTO_PLY_MAX + 1];
    uint8_t lie_res[FLEET_PROTO_PLY_MAX + 1];
    struct fleet_board alt;
    int lies;
    int injected;
    struct flight captured[512];
    int captured_n;
    int replaying;
    /* counters */
    unsigned stat_faults;
    unsigned max_hour_air;
};

static uint32_t r32(struct sim *x)
{
    return fleet_rng_next(&x->rng);
}

static int pct(struct sim *x, int p)
{
    return p > 0 && (int)(r32(x) % 100) < p;
}

static int ppm(struct sim *x, int p)
{
    return p > 0 && (int)(r32(x) % 1000000) < p;
}

static void fail(struct sim *x, const char *why)
{
    if (!x->why[0]) {
        snprintf(x->why, sizeof(x->why), "seed %u, t=%llds: %s", x->seed,
                 (long long)(x->now / 1000), why);
    }
}

/* ---- the radio --------------------------------------------------------------- */

static void air_account(struct simnode *s, int64_t now, uint32_t air, struct sim *x)
{
    int64_t minute = now / 60000;
    int slot = (int)(minute % 60);
    uint32_t total = 0;
    int i;

    if (s->minute_of[slot] != minute) {
        s->minute_of[slot] = minute;
        s->minute_air[slot] = 0;
    }
    s->minute_air[slot] += air;
    for (i = 0; i < 60; i++) {
        if (s->minute_of[i] > minute - 60 && s->minute_of[i] <= minute &&
            s->minute_of[i] * 60000 >= s->run_start - 60000) {
            total += s->minute_air[i];
        }
    }
    if (total > x->max_hour_air) {
        x->max_hour_air = total;
    }
    /* The governor charges in whole minutes of the app's own run; a minute
     * that straddles a restart can hold both runs' frames. */
    if (total > FLEET_GOV_HOUR_MS + 2 * 400 && now - s->run_start > 60000) {
        fail(x, "I5: more than the governor's airtime in an hour");
    }
}

static void transmit(struct sim *x, int from, const uint8_t *b, uint8_t len, int to)
{
    struct simnode *s = &x->s[from];
    uint32_t air = fleet_proto_airtime_ms(len);
    int copies = 1 + pct(x, x->p->dup_pct);
    int64_t start = s->tx_free > x->now ? s->tx_free : x->now;
    int i;
    int c;

    s->tx_free = start + air;
    s->frames++;
    s->airtime += air;
    air_account(s, x->now, air, x);
    if (x->fn + 2 > FLIGHTS) {
        return;
    }
    for (c = 0; c < copies; c++) {
        struct flight *f = &x->fl[x->fn++];

        f->from = from;
        f->to = to;
        f->start = start;
        f->end = start + air;
        f->at = f->end + 50 + (x->p->jitter_ms ? r32(x) % (uint32_t)x->p->jitter_ms : 0) +
                (c ? 200 + r32(x) % 3000 : 0);
        f->len = len;
        memcpy(f->b, b, len);
        f->lost = (x->now < x->part_until) || pct(x, x->p->loss_pct);
        if (x->p->collisions && c == 0) {
            for (i = 0; i < x->fn - 1; i++) {
                struct flight *o = &x->fl[i];

                if (o->from != from && o->start < f->end && f->start < o->end) {
                    o->lost = 1;
                    f->lost = 1;
                }
            }
        }
    }
    if (x->replaying == 0 && x->cheat == CHEAT_REPLAY && x->captured_n < 512) {
        x->captured[x->captured_n] = x->fl[x->fn - 1];
        x->captured[x->captured_n].lost = 0;
        x->captured_n++;
    }
}

/* ---- cheats: rewrite what node 1 says ------------------------------------------- */

static void cheat_rewrite(struct sim *x, uint8_t *b, uint8_t *len)
{
    struct fleet_msg m;
    struct fleet_match *me = &x->s[1].n.m;

    if (fleet_proto_decode(&m, b, *len) != 0) {
        return;
    }
    if (m.type == FLEET_MSG_RESULT) {
        int k = m.ply;

        if (!x->lie_ply[k]) {
            uint8_t res = m.res;

            if (x->cheat == CHEAT_LIE && !x->lies && fleet_res_outcome(res) == 2) {
                res = fleet_res_make(1, 0, 0);
                x->lies++;
            } else if (x->cheat == CHEAT_IMPOSSIBLE && !x->lies && fleet_res_outcome(res) == 2) {
                res = fleet_res_make(3, FLEET_SHIP_CARRIER, 0);
                x->lies++;
            } else if (x->cheat == CHEAT_MOVE) {
                int sunk;
                enum fleet_shot_result r = fleet_board_fire(&x->alt, m.cell / 10, m.cell % 10, &sunk);

                res = r == FLEET_SHOT_INVALID ? res : fleet_res_make(r, sunk, x->alt.ships_afloat == 0);
            }
            x->lie_ply[k] = 1;
            x->lie_res[k] = res;
        }
        m.res = x->lie_res[k];
    } else if (m.type == FLEET_MSG_SHOT && m.ply > 1 && x->lie_ply[m.ply - 1]) {
        m.res = x->lie_res[m.ply - 1];
    } else if (m.type == FLEET_MSG_REVEAL && x->cheat == CHEAT_SALT) {
        m.salt[0] ^= 1;
    } else if (m.type == FLEET_MSG_SHOT && x->cheat == CHEAT_DOUBLE && m.ply > 2 && x->injected < 5) {
        /* The same ply at another square, and a ply two ahead. */
        struct fleet_msg extra = m;
        uint8_t eb[FLEET_PROTO_MAX];
        int n;

        extra.cell = (uint8_t)((m.cell + 37) % FLEET_CELLS);
        n = fleet_proto_encode(&extra, eb, sizeof(eb));
        if (n > 0) {
            transmit(x, 1, eb, (uint8_t)n, 0);
        }
        extra.ply = (uint8_t)(m.ply + 2 <= FLEET_PROTO_PLY_MAX ? m.ply + 2 : m.ply);
        n = fleet_proto_encode(&extra, eb, sizeof(eb));
        if (n > 0) {
            transmit(x, 1, eb, (uint8_t)n, 0);
        }
        x->injected++;
    }
    (void)me;
    {
        uint8_t out[FLEET_PROTO_MAX];
        int n = fleet_proto_encode(&m, out, sizeof(out));

        /* A rewrite the codec will not encode (a lie it cannot even carry)
         * leaves the packet as it was. */
        if (n > 0) {
            memcpy(b, out, (size_t)n);
            *len = (uint8_t)n;
        }
    }
}

static int dropped_by_cheat(struct sim *x, const uint8_t *b)
{
    return x->cheat == CHEAT_NOREVEAL && (b[0] & 0x3F) == FLEET_MSG_REVEAL;
}

/* ---- nodes ----------------------------------------------------------------------- */

static int other(int i)
{
    return i == 0 ? 1 : 0;
}

static void pump(struct sim *x, int i)
{
    struct simnode *s = &x->s[i];
    struct fleet_match_out o;

    mp_save(&s->n);
    if (s->hold_send) {
        return;
    }
    while (fleet_match_pop(&s->n.m, &o)) {
        int to = -1;
        int j;

        for (j = 0; j < 3; j++) {
            if (memcmp(x->s[j].n.key, o.to, FLEET_KEY_BYTES) == 0) {
                to = j;
            }
        }
        if (i == 1 && x->cheat != HONEST && x->cheat != CHEAT_REPLAY && x->cheat != CHEAT_SPOOF) {
            if (dropped_by_cheat(x, o.bytes)) {
                continue;
            }
            cheat_rewrite(x, o.bytes, &o.len);
        }
        if (to >= 0) {
            transmit(x, i, o.bytes, o.len, to);
        }
    }
}

/* I4: what reaches a node was saved by its sender first. */
static void check_saved(struct sim *x, int from, const uint8_t *b, uint8_t len)
{
    struct fleet_msg m;
    struct fleet_match saved;
    struct simnode *s = &x->s[from];

    if (x->cheat != HONEST || fleet_proto_decode(&m, b, len) != 0 ||
        (m.type != FLEET_MSG_SHOT && m.type != FLEET_MSG_RESULT)) {
        return;
    }
    if (!s->n.has_blob) {
        fail(x, "I4: a shot or answer left a node that had saved nothing");
        return;
    }
    fleet_match_init(&saved, s->n.key, 1);
    if (fleet_match_save_decode(&saved, s->n.blob, sizeof(s->n.blob)) != 0 || saved.sid != m.sid) {
        return;   /* the sender has moved on to another session since */
    }
    if (m.type == FLEET_MSG_RESULT && saved.resolved < m.ply) {
        fail(x, "I4: an answer left before its ply was saved");
    }
    if (m.type == FLEET_MSG_SHOT && saved.resolved < m.ply &&
        !(saved.resolved + 1 == m.ply && saved.pending == m.cell)) {
        fail(x, "I4: a shot left before it was saved");
    }
}

static void receive(struct sim *x, int to, int from, const uint8_t *b, uint8_t len)
{
    struct simnode *s = &x->s[to];

    if (x->now < s->deaf_until) {
        return;
    }
    if (!s->open) {
        if (s->inbox_n == INBOX) {
            memmove(&s->inbox[0], &s->inbox[1], sizeof(s->inbox[0]) * (INBOX - 1));
            s->inbox_n--;
        }
        s->inbox[s->inbox_n].len = len;
        memcpy(s->inbox[s->inbox_n].b, b, len);
        s->inbox_n++;
        return;
    }
    fleet_match_receive(&s->n.m, x->s[from].n.key, b, len, x->now);
    pump(x, to);
}

static void reopen(struct sim *x, int i)
{
    struct simnode *s = &x->s[i];
    int k;

    s->open = 1;
    s->run_start = x->now;
    if (s->lose_inbox) {
        s->inbox_n = 0;
    }
    s->lose_inbox = 0;
    if (mp_reload(&s->n, x->now) != 0) {
        fail(x, "a node's own save was refused on reopen");
    }
    for (k = 0; k < s->inbox_n; k++) {
        fleet_match_receive(&s->n.m, x->s[other(i)].n.key, s->inbox[k].b, s->inbox[k].len, x->now);
        pump(x, i);
    }
    s->inbox_n = 0;
    fleet_match_resume(&s->n.m, x->now);
    s->hold_send = 0;
    pump(x, i);
    s->seen = -2;
}

static void close_node(struct sim *x, int i, int64_t down_ms, int lose_inbox)
{
    struct simnode *s = &x->s[i];

    s->open = 0;
    s->reopen_at = x->now + down_ms;
    s->lose_inbox = lose_inbox;
    s->hold_send = 0;
    x->stat_faults++;
}

static void faults(struct sim *x)
{
    const struct profile *p = x->p;
    int i;

    if (p->partitions && x->now >= x->next_part) {
        x->part_until = x->now + 30000 + (int64_t)(r32(x) % 570000);
        x->next_part = x->part_until + (int64_t)p->partitions * 60000 / 2 +
                       (int64_t)(r32(x) % (uint32_t)(p->partitions * 60000));
        x->stat_faults++;
    }
    for (i = 0; i < 2; i++) {
        struct simnode *s = &x->s[i];

        if (!s->open) {
            if (x->now >= s->reopen_at) {
                reopen(x, i);
            }
            continue;
        }
        if (ppm(x, p->crashes)) {
            /* Crash with the latest change unsaved, or saved but not sent. */
            if (r32(x) & 1) {
                s->n.skip_save = 1;
            } else {
                s->hold_send = 1;
            }
            close_node(x, i, 1000 + r32(x) % 60000, 0);
        } else if (ppm(x, p->closes)) {
            close_node(x, i, 10000 + r32(x) % 600000, 0);
        } else if (ppm(x, p->restarts)) {
            s->inbox_n = 0;
            s->deaf_until = x->now + 2000;
            x->stat_faults++;
        } else if (ppm(x, p->reboots)) {
            close_node(x, i, 30000 + r32(x) % 90000, 1);
        }
    }
}

static int think(struct simnode *s)
{
    return 1500 + (int)fleet_rng_below(&s->trng, 10500);
}

static void players(struct sim *x)
{
    int i;

    for (i = 0; i < 2; i++) {
        struct simnode *s = &x->s[i];
        struct fleet_match *m = &s->n.m;
        int key;

        if (!s->open) {
            continue;
        }
        if (x->p->chat && fleet_match_chat_open(m) && x->now >= s->say_at) {
            char text[64];

            /* The session, the speaker and a serial: what C1 and C2 read. */
            snprintf(text, sizeof(text), "%06x %d %u, over.", (unsigned)m->sid, i, ++s->said);
            fleet_match_chat_send(m, text, x->now);
            pump(x, i);
            s->say_at = x->now + 3000 + (int64_t)fleet_rng_below(&s->crng, 15000);
        }
        key = m->phase * 1000 + m->resolved + (fleet_match_my_turn(m) ? 500 : 0);
        if (key != s->seen) {
            s->seen = key;
            s->move_at = x->now + think(s);
        }
        if (x->now < s->move_at) {
            continue;
        }
        if (m->phase == FLEET_MP_DONE && m->outcome == FLEET_OUTCOME_VOID) {
            fleet_match_dismiss(m);     /* this attempt is over; try again */
            pump(x, i);
            continue;
        }
        if (i == 0 && m->phase == FLEET_MP_IDLE) {
            fleet_match_invite(m, x->s[1].n.key, "B", r32(x), x->now);
            x->attempts++;
        } else if (mp_play(&s->n, x->now, 1)) {
            /* acted */
        }
        pump(x, i);
    }
}

/* ---- invariants ------------------------------------------------------------------- */

static int in_game(const struct fleet_match *m)
{
    return m->phase >= FLEET_MP_BATTLE && m->end_reason == FLEET_END_NONE;
}

static void invariants(struct sim *x)
{
    struct fleet_match *a = &x->s[0].n.m;
    struct fleet_match *b = &x->s[1].n.m;
    int i;
    int k;

    if (x->cheat != HONEST && x->cheat != CHEAT_REPLAY && x->cheat != CHEAT_SPOOF) {
        return;
    }
    for (i = 0; i < 2; i++) {
        struct fleet_match *m = &x->s[i].n.m;

        if (m->phase < FLEET_MP_BATTLE) {
            continue;
        }
        if (m->sid != x->sid) {
            x->sid = m->sid;
            x->ledger_n = 0;
        }
        for (k = 1; k <= m->resolved; k++) {
            if (k > x->ledger_n) {
                x->ledger_cell[k] = m->log_cell[k];
                x->ledger_res[k] = m->log_res[k];
                x->ledger_n = k;
            } else if (x->ledger_cell[k] != m->log_cell[k] || x->ledger_res[k] != m->log_res[k]) {
                fail(x, "I3: a ply changed");
            }
        }
        if (m->end_reason == FLEET_END_VOID || m->end_reason == FLEET_END_VIOLATION) {
            fail(x, "an honest match was voided or called a violation");
        }
    }
    if (x->p->chat && x->now % 5000 == 0) {
        for (i = 0; i < 2; i++) {
            const struct fleet_chat *c = &x->s[i].n.m.chat;
            char want[8];
            int j;

            snprintf(want, sizeof(want), "%06x", (unsigned)x->s[i].n.m.sid);
            for (k = 0; k < c->count; k++) {
                if (!c->line[k].mine &&
                    (strncmp(c->line[k].text, want, 6) != 0 || c->line[k].text[7] - '0' != other(i))) {
                    fail(x, "C2: a line not said by the opponent in this session");
                }
                for (j = k + 1; j < c->count; j++) {
                    if (strcmp(c->line[j].text, c->line[k].text) == 0) {
                        fail(x, "C1: a line shown twice");
                    }
                }
            }
        }
    }
    if (a->sid == b->sid && in_game(a) && in_game(b)) {
        int d = (int)a->resolved - (int)b->resolved;

        if (d > 1 || d < -1) {
            fail(x, "I1: the logs are more than one ply apart");
        }
        for (k = 1; k <= a->resolved && k <= b->resolved; k++) {
            if (a->log_cell[k] != b->log_cell[k] || a->log_res[k] != b->log_res[k]) {
                fail(x, "I2: the logs disagree");
                break;
            }
        }
    }
}

/* I8: every answer in the log is what the defender's committed fleet says. */
static int answers_true(struct sim *x)
{
    struct fleet_board fleet[2];
    int i;
    int k;

    for (i = 0; i < 2; i++) {
        if (fleet_layout_decode(x->s[i].n.m.own_layout, &fleet[i]) != 0) {
            return 0;
        }
    }
    for (k = 1; k <= x->s[0].n.m.resolved; k++) {
        /* ply k is fired by the guest (node 1) when odd, at the host's fleet. */
        struct fleet_board *target = &fleet[fleet_match_shooter(k) == FLEET_ROLE_GUEST ? 0 : 1];
        int cell = x->s[0].n.m.log_cell[k];
        int sunk;
        enum fleet_shot_result r = fleet_board_fire(target, cell / 10, cell % 10, &sunk);

        if (fleet_res_make(r, sunk, target->ships_afloat == 0) != x->s[0].n.m.log_res[k]) {
            return 0;
        }
    }
    return 1;
}

/* ---- one match ----------------------------------------------------------------------- */

struct result {
    int finished;
    int plies;
    int attempts;
    unsigned frames[2];
    unsigned airtime[2];
    int64_t duration;
    uint8_t log_cell[FLEET_PROTO_PLY_MAX + 1];
    uint8_t log_res[FLEET_PROTO_PLY_MAX + 1];
    uint8_t outcome[2];
    uint8_t verify[2];
    uint8_t end_reason[2];
    unsigned violations[2];
    unsigned rx_foreign;
    unsigned max_hour_air;
    unsigned faults;
    unsigned chat_shown;        /* lines of the other side shown, both nodes */
    unsigned chat_said;
    char why[160];
};

static void sim_init(struct sim *x, const struct profile *p, uint32_t seed, enum cheat cheat)
{
    int i;

    memset(x, 0, sizeof(*x));
    x->p = p;
    x->seed = seed;
    x->cheat = cheat;
    fleet_rng_seed(&x->rng, seed * 7919u + 17);
    mp_node_init(&x->s[0].n, 0x10, seed * 3 + 1, (int)(seed % 4));
    mp_node_init(&x->s[1].n, 0x20, seed * 3 + 2, (int)((seed / 4) % 4));
    mp_node_init(&x->s[2].n, 0x30, seed * 3 + 3, 0);
    for (i = 0; i < 3; i++) {
        x->s[i].open = 1;
        x->s[i].seen = -1;
        x->s[i].run_start = 0;
        fleet_rng_seed(&x->s[i].trng, seed * 31u + (uint32_t)i);
        fleet_rng_seed(&x->s[i].crng, seed * 53u + (uint32_t)i);
        {
            int j;

            for (j = 0; j < 60; j++) {
                x->s[i].minute_of[j] = -1000;
            }
        }
    }
    x->next_part = p->partitions ? (int64_t)(r32(x) % (uint32_t)(p->partitions * 60000)) : 0;
    x->now = 0;
    if (cheat == CHEAT_MOVE) {
        struct fleet_rng r;

        fleet_rng_seed(&r, seed ^ 0xA17);
        fleet_board_clear(&x->alt);
        fleet_board_autoplace(&x->alt, &r);
    }
}

static void deliver_due(struct sim *x)
{
    int i = 0;

    while (i < x->fn) {
        struct flight f = x->fl[i];

        if (f.at > x->now) {
            i++;
            continue;
        }
        memmove(&x->fl[i], &x->fl[i + 1], (size_t)(x->fn - i - 1) * sizeof(x->fl[0]));
        x->fn--;
        if (!f.lost) {
            check_saved(x, f.from, f.b, f.len);
            receive(x, f.to, f.from, f.b, f.len);
        }
    }
}

static void spoof(struct sim *x)
{
    /* Node 2 sends node 1's next plausible shot for the live session. */
    struct fleet_match *a = &x->s[0].n.m;
    struct fleet_msg m;
    uint8_t b[FLEET_PROTO_MAX];
    int n;

    if (a->phase != FLEET_MP_BATTLE || (x->now % 20000) != 0) {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.type = FLEET_MSG_SHOT;
    m.sid = a->sid;
    m.ply = (uint8_t)(a->resolved + 1);
    m.cell = (uint8_t)(r32(x) % FLEET_CELLS);
    m.res = a->resolved ? a->log_res[a->resolved] : 0;
    n = fleet_proto_encode(&m, b, sizeof(b));
    if (n > 0) {
        transmit(x, 2, b, (uint8_t)n, 0);
    }
}

static void replay_captured(struct sim *x)
{
    int i;

    x->replaying = 1;
    for (i = 0; i < x->captured_n; i++) {
        if (x->captured[i].from == 1 && x->s[0].n.m.phase == FLEET_MP_BATTLE &&
            (x->now / STEP_MS) % 97 == (int64_t)i % 97) {
            receive(x, 0, 1, x->captured[i].b, x->captured[i].len);
        }
    }
}

static void run(struct sim *x, struct result *out, int64_t cap_ms)
{
    struct fleet_match *a = &x->s[0].n.m;
    struct fleet_match *b = &x->s[1].n.m;
    int i;

    while (x->now < cap_ms && !x->why[0]) {
        x->now += STEP_MS;
        faults(x);
        deliver_due(x);
        for (i = 0; i < 2; i++) {
            if (x->s[i].open) {
                fleet_match_tick(&x->s[i].n.m, x->now);
                pump(x, i);
            }
        }
        players(x);
        if (x->cheat == CHEAT_SPOOF) {
            spoof(x);
        }
        if (x->cheat == CHEAT_REPLAY && x->replaying) {
            replay_captured(x);
        }
        invariants(x);
        if (a->phase == FLEET_MP_DONE && b->phase == FLEET_MP_DONE && a->sid == b->sid &&
            a->outcome != FLEET_OUTCOME_VOID && b->outcome != FLEET_OUTCOME_VOID &&
            x->s[0].open && x->s[1].open && !a->end_unacked && !b->end_unacked) {
            break;
        }
        if (x->cheat != HONEST && x->cheat != CHEAT_REPLAY && x->cheat != CHEAT_SPOOF &&
            a->phase == FLEET_MP_DONE) {
            break;
        }
    }
    memset(out, 0, sizeof(*out));
    out->finished = a->phase == FLEET_MP_DONE && b->phase == FLEET_MP_DONE && a->sid == b->sid;
    out->plies = a->resolved;
    out->attempts = x->attempts;
    for (i = 0; i < 2; i++) {
        out->frames[i] = x->s[i].frames;
        out->airtime[i] = x->s[i].airtime;
        out->outcome[i] = x->s[i].n.m.outcome;
        out->verify[i] = x->s[i].n.m.verify;
        out->end_reason[i] = x->s[i].n.m.end_reason;
        out->violations[i] = x->s[i].n.m.stats.violations;
    }
    out->rx_foreign = a->stats.rx_foreign;
    out->duration = x->now;
    memcpy(out->log_cell, a->log_cell, sizeof(out->log_cell));
    memcpy(out->log_res, a->log_res, sizeof(out->log_res));
    out->max_hour_air = x->max_hour_air;
    out->faults = x->stat_faults;
    out->chat_shown = a->stats.chat_rx + b->stats.chat_rx;
    out->chat_said = x->s[0].said + x->s[1].said;
    memcpy(out->why, x->why, sizeof(out->why));
}

static const struct profile CLEAN = { "clean", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static const struct profile PROFILES[] = {
    { "clean", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "10% loss, collisions", 10, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { "30% loss, 10% duplicates, reordering", 30, 10, 6000, 1, 0, 0, 0, 0, 0, 0 },
    { "20% loss, outages of 30 s to 10 min", 20, 5, 2000, 1, 8, 0, 0, 0, 0, 0 },
    { "crashes, app closed and reopened, service restarts", 10, 5, 2000, 1, 0, 40, 60, 40, 0, 0 },
    { "everything at once, reboots too", 25, 10, 6000, 1, 12, 30, 40, 30, 15, 0 },
    { "talking throughout, 10% loss, collisions", 10, 0, 0, 1, 0, 0, 0, 0, 0, 1 },
    { "talking throughout, everything at once", 25, 10, 6000, 1, 12, 30, 40, 30, 15, 1 },
};

static void honest_profile(const struct profile *p, int matches, uint32_t seed0)
{
    long plies = 0;
    long frames = 0;
    unsigned long airtime = 0;
    unsigned max_air = 0;
    unsigned max_hour = 0;
    long attempts = 0;
    int finished = 0;
    int verified = 0;
    int oracle = 1;
    int truth = 1;
    int winners = 1;
    int64_t longest = 0;
    unsigned faults = 0;
    unsigned long chat_shown = 0;
    unsigned long chat_said = 0;
    int64_t total_ms = 0;
    char first_why[200] = "";
    int i;

    for (i = 0; i < matches; i++) {
        static struct sim x;
        static struct sim ref;
        struct result r;
        struct result rr;
        uint32_t seed = seed0 + (uint32_t)i;

        sim_init(&x, p, seed, HONEST);
        run(&x, &r, 24 * HOUR_MS);
        if (r.why[0] && !first_why[0]) {
            snprintf(first_why, sizeof(first_why), "%s", r.why);
        }
        if (!r.finished) {
            if (!first_why[0]) {
                snprintf(first_why, sizeof(first_why), "seed %u did not finish in 24 h", seed);
            }
            continue;
        }
        finished++;
        verified += r.verify[0] == FLEET_VERIFY_OK && r.verify[1] == FLEET_VERIFY_OK;
        winners &= (r.outcome[0] == FLEET_OUTCOME_WIN) != (r.outcome[1] == FLEET_OUTCOME_WIN) &&
                   r.outcome[0] != FLEET_OUTCOME_VOID && r.outcome[1] != FLEET_OUTCOME_VOID;
        if (!answers_true(&x)) {
            truth = 0;
        }
        sim_init(&ref, &CLEAN, seed, HONEST);
        run(&ref, &rr, 24 * HOUR_MS);
        if (!rr.finished || rr.plies != r.plies ||
            memcmp(rr.log_cell, r.log_cell, sizeof(r.log_cell)) != 0 ||
            memcmp(rr.log_res, r.log_res, sizeof(r.log_res)) != 0) {
            if (oracle) {
                printf("     oracle mismatch at seed %u (%d vs %d plies)\n", seed, r.plies, rr.plies);
            }
            oracle = 0;
        }
        plies += r.plies;
        frames += r.frames[0] + r.frames[1];
        airtime += r.airtime[0] + r.airtime[1];
        if (r.airtime[0] > max_air) max_air = r.airtime[0];
        if (r.airtime[1] > max_air) max_air = r.airtime[1];
        if (r.max_hour_air > max_hour) max_hour = r.max_hour_air;
        attempts += r.attempts;
        faults += r.faults;
        chat_shown += r.chat_shown;
        chat_said += r.chat_said;
        if (r.duration > longest) longest = r.duration;
        total_ms += r.duration;
    }
    printf("---- %s: %d matches\n", p->name, matches);
    printf("     finished %d, both verified %d, faults injected %u\n", finished, verified, faults);
    if (finished) {
        printf("     %.1f plies/match, %.2f frames/ply, airtime %.1f s/device/match (max %.1f s), "
               "worst hour %.1f s, invites/match %.2f, mean %.0f min, longest %.0f min\n",
               (double)plies / finished, (double)frames / (double)(plies ? plies : 1),
               airtime / 2000.0 / finished, max_air / 1000.0, max_hour / 1000.0,
               (double)attempts / finished, total_ms / 60000.0 / finished, longest / 60000.0);
    }
    if (finished && p->chat) {
        printf("     chat: %.1f lines shown/match of %.1f tried (the rest waited for room, or were "
               "given up)\n", (double)chat_shown / finished, (double)chat_said / finished);
    }
    if (first_why[0]) {
        printf("     first problem: %s\n", first_why);
    }
    {
        char label[160];

        snprintf(label, sizeof(label), "%s: every match finished, invariants I1-I5 held", p->name);
        check(label, finished == matches && !first_why[0]);
        snprintf(label, sizeof(label), "%s: one winner each time, and every fleet verified (I6)", p->name);
        check(label, winners && verified == finished);
        snprintf(label, sizeof(label), "%s: the same match as on a perfect network (I7)", p->name);
        check(label, oracle);
        snprintf(label, sizeof(label), "%s: every answer was true to the defender's fleet (I8)", p->name);
        check(label, truth);
        if (p->chat) {
            snprintf(label, sizeof(label), "%s: and the players were heard (C1, C2 held)", p->name);
            check(label, chat_shown > (unsigned long)finished * 10);
        }
    }
}

/* ---- cheaters ------------------------------------------------------------------------ */

static void cheat_case(const char *name, enum cheat cheat, int matches)
{
    int caught = 0;
    int harmless = 0;
    int honest_blamed = 0;
    int i;

    for (i = 0; i < matches; i++) {
        static struct sim x;
        struct result r;
        const struct fleet_match *a;

        sim_init(&x, &PROFILES[1], 9000 + (uint32_t)i * 13, cheat);
        if (cheat == CHEAT_REPLAY) {
            /* Play one match, capture everything, then replay it into the next. */
            run(&x, &r, 24 * HOUR_MS);
            fleet_match_dismiss(&x.s[0].n.m);
            fleet_match_dismiss(&x.s[1].n.m);
            x.replaying = 1;
            x.attempts = 0;
            x.sid = 0;
            x.ledger_n = 0;
        }
        run(&x, &r, 24 * HOUR_MS);
        a = &x.s[0].n.m;
        switch (cheat) {
        case CHEAT_LIE:
        case CHEAT_MOVE:
        case CHEAT_SALT:
            caught += a->verify == FLEET_VERIFY_MISMATCH || a->end_reason == FLEET_END_VIOLATION ||
                      a->end_reason == FLEET_END_VOID;
            break;
        case CHEAT_NOREVEAL:
            caught += a->phase == FLEET_MP_DONE && a->verify == FLEET_VERIFY_NONE &&
                      a->outcome != FLEET_OUTCOME_VOID;
            break;
        case CHEAT_IMPOSSIBLE:
            caught += a->end_reason == FLEET_END_VIOLATION;
            break;
        case CHEAT_DOUBLE: {
            /* Harmless when every ply of theirs hit our fleet once, at one square. */
            int k;
            int theirs = 0;
            int shot = 0;

            for (k = 1; k <= a->resolved; k++) {
                theirs += fleet_match_shooter(k) != a->role;
            }
            for (k = 0; k < FLEET_CELLS; k++) {
                shot += a->own.shot[k];
            }
            harmless += shot == theirs;
            caught += a->end_reason == FLEET_END_VIOLATION || a->end_reason == FLEET_END_VOID;
            break;
        }
        case CHEAT_REPLAY:
        case CHEAT_SPOOF:
            harmless += r.finished && !r.why[0] && r.verify[0] == FLEET_VERIFY_OK &&
                        r.verify[1] == FLEET_VERIFY_OK && answers_true(&x);
            break;
        default:
            break;
        }
        /* The honest side's own fleet must never be judged false by the cheater. */
        if (cheat != CHEAT_REPLAY && cheat != CHEAT_SPOOF &&
            x.s[1].n.m.verify == FLEET_VERIFY_MISMATCH && cheat != CHEAT_SALT && cheat != CHEAT_MOVE &&
            cheat != CHEAT_LIE) {
            honest_blamed++;
        }
    }
    {
        char label[200];

        if (cheat == CHEAT_DOUBLE) {
            snprintf(label, sizeof(label), "cheat, %s: never more than one square per ply (%d/%d harmless, %d ended)",
                     name, harmless, matches, caught);
            check(label, harmless == matches);
        } else if (cheat == CHEAT_REPLAY || cheat == CHEAT_SPOOF) {
            snprintf(label, sizeof(label), "cheat, %s: no effect on the match (%d/%d)", name, harmless, matches);
            check(label, harmless == matches);
        } else {
            snprintf(label, sizeof(label), "cheat, %s: caught %d/%d", name, caught, matches);
            check(label, caught == matches && honest_blamed == 0);
        }
    }
}

int main(void)
{
    const char *env = getenv("FLEET_SIM_MATCHES");
    int matches = env ? atoi(env) : 300;
    size_t i;

    if (matches < 1) {
        matches = 1;
    }
    for (i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        honest_profile(&PROFILES[i], matches, 1 + (uint32_t)i * 100000);
    }
    cheat_case("lies about one hit", CHEAT_LIE, 40);
    cheat_case("answers from a fleet it never committed", CHEAT_MOVE, 40);
    cheat_case("refuses to reveal", CHEAT_NOREVEAL, 20);
    cheat_case("reveals with the wrong salt", CHEAT_SALT, 40);
    cheat_case("reports an impossible sinking", CHEAT_IMPOSSIBLE, 40);
    cheat_case("fires twice and out of turn", CHEAT_DOUBLE, 40);
    cheat_case("replays a previous match", CHEAT_REPLAY, 20);
    cheat_case("a third node spoofs the session", CHEAT_SPOOF, 20);
    printf("fleet_mp_sim_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
