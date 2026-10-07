/*
 * PocketFleet multiplayer state machine, rule by rule
 * (docs/apps/FLEET_MULTIPLAYER.md). Each test names the rule it holds the
 * code to. The randomized simulator (tests/fleet_mp_sim_test.c) plays
 * thousands of matches through faults; this file pins the individual cases
 * so a regression says which rule broke.
 *
 * The wire here is a queue the test controls packet by packet: deliver,
 * drop, duplicate, hold back and deliver late.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_mp_harness.h"

#include <stdlib.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

struct pkt {
    int from;
    int to;
    uint8_t len;
    uint8_t b[FLEET_PROTO_MAX];
};

#define NODES 3
#define QMAX 512

struct wire {
    struct mp_node n[NODES];
    struct pkt q[QMAX];
    int qn;
    int64_t now;
    unsigned sent[NODES][FLEET_MSG_TYPE_COUNT];
    unsigned airtime[NODES];
};

static int node_of(struct wire *w, const uint8_t *key)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (memcmp(w->n[i].key, key, FLEET_KEY_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

static void pump(struct wire *w)
{
    int i;

    for (i = 0; i < NODES; i++) {
        struct fleet_match_out o;

        mp_save(&w->n[i]);
        while (fleet_match_pop(&w->n[i].m, &o)) {
            struct pkt *p;

            if (w->qn == QMAX) {
                continue;
            }
            p = &w->q[w->qn++];
            p->from = i;
            p->to = node_of(w, o.to);
            p->len = o.len;
            memcpy(p->b, o.bytes, o.len);
            w->sent[i][o.bytes[0] & 0x3F]++;
            w->airtime[i] += fleet_proto_airtime_ms(o.len);
        }
    }
}

static void wire_init(struct wire *w, uint32_t seed)
{
    memset(w, 0, sizeof(*w));
    /* Keys ordered: node 0 < node 1 < node 2. */
    mp_node_init(&w->n[0], 0x10, seed + 1, FLEET_ADMIRAL);
    mp_node_init(&w->n[1], 0x20, seed + 2, FLEET_COMMANDER);
    mp_node_init(&w->n[2], 0x30, seed + 3, FLEET_OFFICER);
    w->now = 1000;
}

static void remove_at(struct wire *w, int i)
{
    memmove(&w->q[i], &w->q[i + 1], (size_t)(w->qn - i - 1) * sizeof(w->q[0]));
    w->qn--;
}

static void deliver_at(struct wire *w, int i)
{
    struct pkt p = w->q[i];

    remove_at(w, i);
    if (p.to >= 0) {
        fleet_match_receive(&w->n[p.to].m, w->n[p.from].key, p.b, p.len, w->now);
    }
    pump(w);
}

static void deliver_all(struct wire *w)
{
    int guard = 0;

    while (w->qn > 0 && guard++ < 10000) {
        deliver_at(w, 0);
    }
}

static void drop_all(struct wire *w)
{
    w->qn = 0;
}

/* The index of the first queued packet of a type, or -1. */
static int find(struct wire *w, int type)
{
    int i;

    for (i = 0; i < w->qn; i++) {
        if ((w->q[i].b[0] & 0x3F) == type) {
            return i;
        }
    }
    return -1;
}

static void advance(struct wire *w, int64_t ms)
{
    int i;

    w->now += ms;
    for (i = 0; i < NODES; i++) {
        fleet_match_tick(&w->n[i].m, w->now);
    }
    pump(w);
}

/* Run with the players playing until time runs out. A player takes
 * THINK_MS over each move, as a person does; the airtime governor is sized
 * for people, not for two programs firing as fast as they can. */
#define THINK_MS 6000

static void play(struct wire *w, int64_t ms, int auto_accept)
{
    int64_t end = w->now + ms;
    int64_t ready[2] = { 0, 0 };
    int seen[2] = { -1, -1 };

    while (w->now < end) {
        int i;

        deliver_all(w);
        for (i = 0; i < 2; i++) {
            int turn = fleet_match_my_turn(&w->n[i].m) ? w->n[i].m.resolved : -1;

            if (turn != seen[i]) {
                seen[i] = turn;
                ready[i] = w->now + THINK_MS;
            }
            if (turn < 0 || w->now >= ready[i]) {
                mp_play(&w->n[i], w->now, auto_accept);
            }
        }
        pump(w);
        deliver_all(w);
        advance(w, 100);
    }
}

static struct fleet_match *A(struct wire *w)
{
    return &w->n[0].m;
}

static struct fleet_match *B(struct wire *w)
{
    return &w->n[1].m;
}

/* A (host) invites B (guest), B accepts, both deploy: a match in BATTLE. */
static void to_battle(struct wire *w)
{
    fleet_match_invite(A(w), w->n[1].key, "B", 0x123456, w->now);
    pump(w);
    deliver_all(w);
    fleet_match_accept(B(w), w->now);
    pump(w);
    deliver_all(w);
    mp_deploy(&w->n[0], w->now);
    mp_deploy(&w->n[1], w->now);
    pump(w);
    deliver_all(w);
}

/* ---- session ---------------------------------------------------------------- */

static void test_invite_accept(void)
{
    struct wire w;

    wire_init(&w, 1);
    check("invite: the host is inviting",
          fleet_match_invite(A(&w), w.n[1].key, "B", 0x123456, w.now) == 0 &&
          A(&w)->phase == FLEET_MP_INVITING);
    pump(&w);
    check("invite: one INVITE on the air", w.sent[0][FLEET_MSG_INVITE] == 1);
    deliver_all(&w);
    check("invite: the guest is asked", B(&w)->phase == FLEET_MP_INVITED &&
          B(&w)->role == FLEET_ROLE_GUEST && B(&w)->sid == A(&w)->sid);
    check("invite: nothing was saved for an invite", !w.n[1].has_blob);
    fleet_match_accept(B(&w), w.now);
    pump(&w);
    deliver_all(&w);
    check("accept: both deploy", A(&w)->phase == FLEET_MP_DEPLOY && B(&w)->phase == FLEET_MP_DEPLOY);
    check("accept: host and guest", A(&w)->role == FLEET_ROLE_HOST && B(&w)->role == FLEET_ROLE_GUEST);
    check("accept: the live session is saved on both sides", w.n[0].has_blob && w.n[1].has_blob);
    check("accept: one ACCEPT and one START", w.sent[1][FLEET_MSG_ACCEPT] == 1 &&
          w.sent[0][FLEET_MSG_START] == 1);
}

static void test_start_lost(void)
{
    struct wire w;
    int i;

    wire_init(&w, 2);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0x42, w.now);
    pump(&w);
    deliver_all(&w);
    fleet_match_accept(B(&w), w.now);
    pump(&w);
    deliver_at(&w, find(&w, FLEET_MSG_ACCEPT));   /* A answers START */
    i = find(&w, FLEET_MSG_START);
    check("start lost: START was sent", i >= 0);
    if (i >= 0) {
        remove_at(&w, i);
    }
    advance(&w, 20000);       /* B asks again */
    deliver_all(&w);
    check("start lost: the retried ACCEPT is answered with START and B deploys",
          B(&w)->phase == FLEET_MP_DEPLOY && w.sent[0][FLEET_MSG_START] == 2);
}

static void test_decline(void)
{
    struct wire w;
    uint32_t sid;

    wire_init(&w, 3);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0x777, w.now);
    pump(&w);
    deliver_all(&w);
    sid = B(&w)->sid;
    check("decline: done", fleet_match_decline(B(&w), w.now) == 0);
    pump(&w);
    deliver_all(&w);
    check("decline: the host is idle and told", A(&w)->phase == FLEET_MP_IDLE &&
          A(&w)->notice == FLEET_NOTICE_DECLINED);
    check("decline: the guest remembers it", B(&w)->tomb[0].sid == sid &&
          B(&w)->tomb[0].kind == FLEET_TOMB_DECLINED);
    {
        struct fleet_msg msg;
        uint8_t buf[16];
        int n;

        memset(&msg, 0, sizeof(msg));
        msg.type = FLEET_MSG_INVITE;
        msg.sid = sid;
        n = fleet_proto_encode(&msg, buf, sizeof(buf));
        fleet_match_receive(B(&w), w.n[0].key, buf, (size_t)n, w.now);
        pump(&w);
        check("decline: a late copy of the invite is declined again, and asks nobody",
              B(&w)->phase == FLEET_MP_IDLE && find(&w, FLEET_MSG_DECLINE) >= 0);
    }
}

static void test_cancel(void)
{
    struct wire w;
    int i;

    wire_init(&w, 4);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0x999, w.now);
    pump(&w);
    deliver_all(&w);
    fleet_match_accept(B(&w), w.now);
    pump(&w);
    i = find(&w, FLEET_MSG_ACCEPT);
    check("cancel: the accept is held back", i >= 0);
    {
        struct pkt held = w.q[i];

        remove_at(&w, i);
        check("cancel: done", fleet_match_cancel(A(&w), w.now) == 0);
        pump(&w);
        check("cancel: CANCEL sent", find(&w, FLEET_MSG_CANCEL) >= 0);
        drop_all(&w);
        w.q[w.qn++] = held;   /* the ACCEPT arrives after the cancel */
        deliver_all(&w);
    }
    check("cancel: a late accept is answered END(cancelled) and the guest stands down",
          B(&w)->phase == FLEET_MP_IDLE && B(&w)->notice == FLEET_NOTICE_CANCELLED &&
          A(&w)->phase == FLEET_MP_IDLE);
}

static void test_no_answer(void)
{
    struct wire w;
    int i;

    wire_init(&w, 5);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0x31, w.now);
    for (i = 0; i < 400; i++) {
        pump(&w);
        drop_all(&w);
        advance(&w, 1000);
    }
    check("no answer: six invites, then the host gives up and says so",
          w.sent[0][FLEET_MSG_INVITE] == FLEET_RETRY_MAX && A(&w)->phase == FLEET_MP_IDLE &&
          A(&w)->notice == FLEET_NOTICE_NO_ANSWER);
}

static void test_crossed_invites(void)
{
    struct wire w;

    wire_init(&w, 6);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0xAAA, w.now);
    fleet_match_invite(B(&w), w.n[0].key, "A", 0xBBB, w.now);
    pump(&w);
    deliver_all(&w);
    advance(&w, 100);
    deliver_all(&w);
    /* A's key is lower, so A's invite survives and B joins it. */
    check("crossed: one session, the lower key's",
          A(&w)->sid == B(&w)->sid && A(&w)->sid == (0xAAA & 0xFFFFFF));
    check("crossed: the higher key joined as guest, without being asked",
          B(&w)->role == FLEET_ROLE_GUEST && A(&w)->role == FLEET_ROLE_HOST &&
          A(&w)->phase == FLEET_MP_DEPLOY && B(&w)->phase == FLEET_MP_DEPLOY);
    check("crossed: and was told why", B(&w)->notice == FLEET_NOTICE_CROSSED);
}

static void test_busy(void)
{
    struct wire w;

    wire_init(&w, 7);
    to_battle(&w);
    fleet_match_invite(&w.n[2].m, w.n[1].key, "B", 0x55, w.now);
    pump(&w);
    deliver_all(&w);
    check("busy: a third node is declined as busy",
          w.n[2].m.phase == FLEET_MP_IDLE && w.n[2].m.notice == FLEET_NOTICE_BUSY);
    check("busy: the match in hand is untouched", B(&w)->phase == FLEET_MP_BATTLE);
}

static void test_busy_with_you(void)
{
    struct wire w;
    uint32_t old;

    wire_init(&w, 8);
    to_battle(&w);
    old = B(&w)->sid;
    /* A loses its match (a state it could not read) and invites B again. */
    mp_node_init(&w.n[0], 0x10, 99, FLEET_ADMIRAL);
    fleet_match_invite(A(&w), w.n[1].key, "B", 0xC0FFEE, w.now);
    pump(&w);
    deliver_all(&w);
    check("busy with you: B's old match is ended as unknown to A",
          B(&w)->phase == FLEET_MP_DONE && B(&w)->outcome == FLEET_OUTCOME_VOID &&
          B(&w)->end_reason == FLEET_END_UNKNOWN);
    advance(&w, 20000);
    deliver_all(&w);
    check("busy with you: A's next invite reaches B", B(&w)->phase == FLEET_MP_INVITED &&
          B(&w)->sid != old);
}

/* ---- battle ---------------------------------------------------------------- */

static void test_commit_then_battle(void)
{
    struct wire w;

    wire_init(&w, 9);
    to_battle(&w);
    check("commit: both in battle", A(&w)->phase == FLEET_MP_BATTLE && B(&w)->phase == FLEET_MP_BATTLE);
    check("commit: each holds the other's", A(&w)->have_peer_commit && B(&w)->have_peer_commit &&
          memcmp(A(&w)->peer_commit, B(&w)->own_commit, FLEET_COMMIT_BYTES) == 0);
    check("commit: the guest fires first", fleet_match_my_turn(B(&w)) && !fleet_match_my_turn(A(&w)));
    check("commit: the host cannot fire out of turn", fleet_match_fire(A(&w), 0, 0, w.now) != 0);
}

static void test_persist_gate(void)
{
    struct wire w;
    struct fleet_match_out o;

    wire_init(&w, 10);
    to_battle(&w);
    fleet_match_fire(B(&w), 4, 4, w.now);
    check("gate: a shot not yet saved does not leave", fleet_match_dirty(B(&w)) &&
          fleet_match_pop(B(&w), &o) == 0);
    fleet_match_saved(B(&w));
    check("gate: once saved, it does", fleet_match_pop(B(&w), &o) == 1 &&
          (o.bytes[0] & 0x3F) == FLEET_MSG_SHOT);
}

static void test_full_match(void)
{
    struct wire w;
    unsigned shots;
    unsigned results;

    wire_init(&w, 11);
    to_battle(&w);
    play(&w, 3600000, 0);
    check("full match: both done", A(&w)->phase == FLEET_MP_DONE && B(&w)->phase == FLEET_MP_DONE);
    check("full match: one winner", (A(&w)->outcome == FLEET_OUTCOME_WIN) !=
          (B(&w)->outcome == FLEET_OUTCOME_WIN) && A(&w)->outcome != FLEET_OUTCOME_VOID);
    check("full match: both fleets verified", A(&w)->verify == FLEET_VERIFY_OK &&
          B(&w)->verify == FLEET_VERIFY_OK);
    check("full match: the logs are identical", A(&w)->resolved == B(&w)->resolved &&
          memcmp(A(&w)->log_cell, B(&w)->log_cell, sizeof(A(&w)->log_cell)) == 0 &&
          memcmp(A(&w)->log_res, B(&w)->log_res, sizeof(A(&w)->log_res)) == 0);
    shots = w.sent[0][FLEET_MSG_SHOT] + w.sent[1][FLEET_MSG_SHOT];
    results = w.sent[0][FLEET_MSG_RESULT] + w.sent[1][FLEET_MSG_RESULT];
    printf("     %u plies, %u SHOT, %u RESULT, %u REVEAL, %u SYNC; airtime A %u ms, B %u ms\n",
           A(&w)->resolved, shots, results,
           w.sent[0][FLEET_MSG_REVEAL] + w.sent[1][FLEET_MSG_REVEAL],
           w.sent[0][FLEET_MSG_SYNC] + w.sent[1][FLEET_MSG_SYNC], w.airtime[0], w.airtime[1]);
    check("full match: on a clean link, exactly one SHOT and one RESULT per ply",
          shots == A(&w)->resolved && results == A(&w)->resolved);
    check("full match: no retry at all", A(&w)->stats.dup_replies == 0 && B(&w)->stats.dup_replies == 0);
}

/* Fire once, drive the exchange with a hook applied to the queue. */
static void one_shot(struct wire *w)
{
    to_battle(w);
    fleet_match_fire(B(w), 5, 5, w->now);
    pump(w);
}

static void test_lost_shot(void)
{
    struct wire w;

    wire_init(&w, 12);
    one_shot(&w);
    drop_all(&w);
    check("lost shot: nothing resolved", A(&w)->resolved == 0 && B(&w)->resolved == 0);
    advance(&w, 6000);
    check("lost shot: no retry before the timeout", w.qn == 0);
    advance(&w, 6000);
    check("lost shot: retried", find(&w, FLEET_MSG_SHOT) >= 0);
    deliver_all(&w);
    check("lost shot: one ply, answered once", A(&w)->resolved == 1 && B(&w)->resolved == 1 &&
          A(&w)->log_cell[1] == 55);
}

static void test_lost_result(void)
{
    struct wire w;
    int i;

    wire_init(&w, 13);
    one_shot(&w);
    deliver_at(&w, find(&w, FLEET_MSG_SHOT));
    i = find(&w, FLEET_MSG_RESULT);
    check("lost result: answered", i >= 0 && A(&w)->resolved == 1);
    remove_at(&w, i);
    advance(&w, 12000);
    deliver_all(&w);
    check("lost result: the retried shot fetched the answer, and the ply was taken once",
          B(&w)->resolved == 1 && A(&w)->resolved == 1 && A(&w)->own.shot[55] == 1 &&
          A(&w)->stats.dup_replies == 1);
}

static void test_duplicate_shot(void)
{
    struct wire w;
    struct pkt copy;
    int afloat;

    wire_init(&w, 14);
    one_shot(&w);
    copy = w.q[find(&w, FLEET_MSG_SHOT)];
    w.q[w.qn++] = copy;
    afloat = A(&w)->own.ships_afloat;
    deliver_all(&w);
    check("duplicate shot: two copies at once, one ply, one answer",
          A(&w)->resolved == 1 && B(&w)->resolved == 1 && w.sent[0][FLEET_MSG_RESULT] == 1);
    w.now += 3000;
    w.q[w.qn++] = copy;
    deliver_all(&w);
    check("duplicate shot: a later copy is answered again from the log",
          w.sent[0][FLEET_MSG_RESULT] == 2 && A(&w)->stats.dup_replies == 1);
    w.now += 100;
    w.q[w.qn++] = copy;
    w.q[w.qn++] = copy;
    deliver_all(&w);
    check("duplicate shot: a burst of copies costs at most one answer per 2 s",
          w.sent[0][FLEET_MSG_RESULT] == 2);
    check("duplicate shot: the fleet took it once", A(&w)->own.ships_afloat == afloat &&
          A(&w)->own.shot[55] == 1 && A(&w)->resolved == 1);
}

static void test_duplicate_result(void)
{
    struct wire w;
    struct pkt copy;

    wire_init(&w, 15);
    one_shot(&w);
    deliver_all(&w);
    fleet_match_fire(A(&w), 0, 0, w.now);   /* ply 2, host */
    pump(&w);
    deliver_all(&w);
    copy.from = 0;
    copy.to = 1;
    {
        struct fleet_msg msg;

        memset(&msg, 0, sizeof(msg));
        msg.type = FLEET_MSG_RESULT;
        msg.sid = A(&w)->sid;
        msg.ply = 1;
        msg.cell = 55;
        msg.res = A(&w)->log_res[1];
        copy.len = (uint8_t)fleet_proto_encode(&msg, copy.b, sizeof(copy.b));
    }
    w.q[w.qn++] = copy;
    deliver_all(&w);
    check("duplicate result: nothing changes", B(&w)->resolved == 2 && B(&w)->phase == FLEET_MP_BATTLE &&
          B(&w)->stats.violations == 0);
}

static void test_out_of_order(void)
{
    struct wire w;
    int i;

    wire_init(&w, 16);
    one_shot(&w);
    deliver_at(&w, find(&w, FLEET_MSG_SHOT));
    i = find(&w, FLEET_MSG_RESULT);
    {
        struct pkt result = w.q[i];

        remove_at(&w, i);
        fleet_match_fire(A(&w), 9, 9, w.now);   /* A answers, then fires ply 2 */
        pump(&w);
        deliver_all(&w);                          /* ply 2 arrives before ply 1's answer */
        check("out of order: the next shot carried the answer",
              B(&w)->resolved == 2 && B(&w)->log_res[1] == A(&w)->log_res[1]);
        w.q[w.qn++] = result;
        deliver_all(&w);
        check("out of order: the late answer changes nothing", B(&w)->resolved == 2 &&
              B(&w)->stats.violations == 0);
    }
}

static void test_delayed_old_packet(void)
{
    struct wire w;
    struct pkt old;

    wire_init(&w, 17);
    one_shot(&w);
    old = w.q[find(&w, FLEET_MSG_SHOT)];
    play(&w, 120000, 0);
    {
        int r = A(&w)->resolved;
        int shots = 0;
        int k;

        for (k = 0; k < FLEET_CELLS; k++) {
            shots += A(&w)->own.shot[k];
        }
        w.q[w.qn++] = old;
        deliver_all(&w);
        check("delayed: a shot from long ago is stale", A(&w)->resolved >= r &&
              A(&w)->stats.violations == 0 && A(&w)->stats.rx_stale > 0);
        for (k = 0; k < FLEET_CELLS; k++) {
            shots -= A(&w)->own.shot[k];
        }
        check("delayed: and fired at nothing", shots <= 0);
    }
}

static void test_simultaneous_retry(void)
{
    struct wire w;
    int i;

    wire_init(&w, 18);
    one_shot(&w);
    deliver_at(&w, find(&w, FLEET_MSG_SHOT));
    i = find(&w, FLEET_MSG_RESULT);
    remove_at(&w, i);                 /* B never hears ply 1's answer */
    fleet_match_fire(A(&w), 7, 7, w.now);
    pump(&w);
    drop_all(&w);                     /* nor A's shot for ply 2 */
    advance(&w, 30000);               /* both retry */
    check("simultaneous retry: both sides were retrying",
          w.sent[1][FLEET_MSG_SHOT] >= 2 && w.sent[0][FLEET_MSG_SHOT] >= 2);
    deliver_all(&w);
    advance(&w, 100);
    deliver_all(&w);
    check("simultaneous retry: converged on ply 2 with one answer each",
          A(&w)->resolved == 2 && B(&w)->resolved == 2 &&
          memcmp(A(&w)->log_res, B(&w)->log_res, 3) == 0);
}

static void test_stale_session(void)
{
    struct wire w;
    struct pkt old;
    uint32_t sid;

    wire_init(&w, 19);
    one_shot(&w);
    old = w.q[find(&w, FLEET_MSG_SHOT)];
    sid = A(&w)->sid;
    deliver_all(&w);
    play(&w, 3600000, 0);
    fleet_match_dismiss(A(&w));
    check("stale session: the finished match is a tombstone", A(&w)->tomb[0].sid == sid &&
          A(&w)->phase == FLEET_MP_IDLE);
    w.q[w.qn++] = old;
    deliver_at(&w, w.qn - 1);
    check("stale session: its packet is answered END, and changes nothing",
          find(&w, FLEET_MSG_END) >= 0 && A(&w)->phase == FLEET_MP_IDLE);
    {
        struct fleet_msg msg;
        uint8_t buf[16];
        int n;

        drop_all(&w);
        memset(&msg, 0, sizeof(msg));
        msg.type = FLEET_MSG_SHOT;
        msg.sid = 0x0BADBA;
        msg.ply = 1;
        msg.cell = 3;
        n = fleet_proto_encode(&msg, buf, sizeof(buf));
        fleet_match_receive(&w.n[2].m, w.n[0].key, buf, (size_t)n, w.now);
        pump(&w);
        check("unknown session: END(unknown)", w.qn == 1 && (w.q[0].b[0] & 0x3F) == FLEET_MSG_END &&
              w.q[0].b[5] == FLEET_END_UNKNOWN);
        fleet_match_receive(&w.n[2].m, w.n[0].key, buf, (size_t)n, w.now + 10);
        pump(&w);
        check("unknown session: once, not once per copy", w.qn == 1);
    }
}

static void test_forfeit(void)
{
    struct wire w;

    wire_init(&w, 20);
    to_battle(&w);
    check("forfeit: accepted", fleet_match_forfeit(A(&w), w.now) == 0);
    pump(&w);
    deliver_all(&w);
    check("forfeit: the peer wins, the forfeiter loses",
          B(&w)->phase == FLEET_MP_DONE && B(&w)->outcome == FLEET_OUTCOME_WIN &&
          A(&w)->outcome == FLEET_OUTCOME_LOSS);
    check("forfeit: acknowledged, so it is not sent again", !A(&w)->end_unacked &&
          w.sent[1][FLEET_MSG_END_ACK] == 1);
}

static void test_forfeit_unacked(void)
{
    struct wire w;
    int i;

    wire_init(&w, 21);
    to_battle(&w);
    fleet_match_forfeit(A(&w), w.now);
    for (i = 0; i < 400; i++) {
        pump(&w);
        drop_all(&w);
        advance(&w, 1000);
    }
    check("forfeit, peer gone: END three times, then stop", w.sent[0][FLEET_MSG_END] == 3 &&
          !A(&w)->end_unacked && A(&w)->outcome == FLEET_OUTCOME_LOSS);
}

/* ---- fairness ------------------------------------------------------------------- */

/* Rewrite the answer B receives for its first shot. */
static void lie_first_answer(struct wire *w, uint8_t res)
{
    int i;
    struct fleet_msg msg;

    one_shot(w);
    deliver_at(w, find(w, FLEET_MSG_SHOT));
    i = find(w, FLEET_MSG_RESULT);
    fleet_proto_decode(&msg, w->q[i].b, w->q[i].len);
    msg.res = res;
    w->q[i].len = (uint8_t)fleet_proto_encode(&msg, w->q[i].b, sizeof(w->q[i].b));
    deliver_all(w);
}

static void test_lie_detected_at_reveal(void)
{
    struct wire w;
    uint8_t truth;
    uint8_t lie;

    wire_init(&w, 22);
    to_battle(&w);
    fleet_match_fire(B(&w), 5, 5, w.now);
    pump(&w);
    deliver_at(&w, find(&w, FLEET_MSG_SHOT));
    truth = A(&w)->log_res[1];
    lie = fleet_res_outcome(truth) == 1 ? fleet_res_make(2, 0, 0) : fleet_res_make(1, 0, 0);
    {
        int i = find(&w, FLEET_MSG_RESULT);
        struct fleet_msg msg;

        fleet_proto_decode(&msg, w.q[i].b, w.q[i].len);
        msg.res = lie;
        w.q[i].len = (uint8_t)fleet_proto_encode(&msg, w.q[i].b, sizeof(w.q[i].b));
    }
    deliver_all(&w);
    /* Carry on; A's own log keeps the truth, B's keeps the lie. From here the
     * digests differ, so the first SYNC would void the match - keep the link
     * clean so none is needed, and let the reveal find it. */
    play(&w, 3600000, 0);
    check("lie: B's log holds the answer it was given", B(&w)->log_res[1] == lie);
    check("lie: the reveal shows B that A's answers did not match A's fleet",
          B(&w)->verify == FLEET_VERIFY_MISMATCH);
    check("lie: A, who told the truth, is verified by B's reveal", A(&w)->verify != FLEET_VERIFY_MISMATCH);
}

static void test_impossible_answer(void)
{
    struct wire w;

    wire_init(&w, 23);
    /* "Carrier sunk" on the first shot: no carrier can sink from one hit. */
    lie_first_answer(&w, fleet_res_make(3, FLEET_SHIP_CARRIER, 0));
    check("impossible: the match ends as a violation", B(&w)->phase == FLEET_MP_DONE &&
          B(&w)->end_reason == FLEET_END_VIOLATION && B(&w)->outcome == FLEET_OUTCOME_VOID);
    check("impossible: and the other side is told", A(&w)->phase == FLEET_MP_DONE &&
          A(&w)->outcome == FLEET_OUTCOME_VOID);
}

static void test_changed_answer(void)
{
    struct wire w;
    struct fleet_msg msg;
    struct pkt p;

    wire_init(&w, 24);
    one_shot(&w);
    deliver_all(&w);
    memset(&msg, 0, sizeof(msg));
    msg.type = FLEET_MSG_RESULT;
    msg.sid = A(&w)->sid;
    msg.ply = 1;
    msg.cell = 55;
    msg.res = fleet_res_outcome(A(&w)->log_res[1]) == 1 ? fleet_res_make(2, 0, 0)
                                                          : fleet_res_make(1, 0, 0);
    p.from = 0;
    p.to = 1;
    p.len = (uint8_t)fleet_proto_encode(&msg, p.b, sizeof(p.b));
    w.q[w.qn++] = p;
    deliver_all(&w);
    check("changed answer: an answer that changes after it was given is a violation",
          B(&w)->end_reason == FLEET_END_VIOLATION);
}

static void test_changed_commit(void)
{
    struct wire w;
    struct fleet_msg msg;
    struct pkt p;

    wire_init(&w, 25);
    to_battle(&w);
    memset(&msg, 0, sizeof(msg));
    msg.type = FLEET_MSG_COMMIT;
    msg.sid = A(&w)->sid;
    memcpy(msg.commit, A(&w)->own_commit, FLEET_COMMIT_BYTES);
    msg.commit[0] ^= 1;
    p.from = 0;
    p.to = 1;
    p.len = (uint8_t)fleet_proto_encode(&msg, p.b, sizeof(p.b));
    w.q[w.qn++] = p;
    deliver_all(&w);
    check("changed commit: a second, different fleet commitment is a violation",
          B(&w)->end_reason == FLEET_END_VIOLATION);
}

static void test_wrong_sender(void)
{
    struct wire w;

    wire_init(&w, 26);
    one_shot(&w);
    w.q[0].from = 2;   /* the same bytes, from a third node */
    deliver_all(&w);
    check("wrong sender: a packet for the session from another node is ignored",
          A(&w)->resolved == 0 && A(&w)->stats.rx_foreign == 1);
}

/* ---- resync ---------------------------------------------------------------------- */

static void test_crash_after_save_before_send(void)
{
    struct wire w;

    wire_init(&w, 27);
    one_shot(&w);
    /* A takes the shot, saves the ply, and dies before its answer leaves. */
    fleet_match_receive(A(&w), w.n[1].key, w.q[0].b, w.q[0].len, w.now);
    remove_at(&w, 0);
    mp_save(&w.n[0]);
    check("crash: the ply was saved", A(&w)->resolved == 1 && !fleet_match_dirty(A(&w)));
    check("crash: the saved match reloads", mp_reload(&w.n[0], w.now) == 0 &&
          A(&w)->resolved == 1 && A(&w)->phase == FLEET_MP_BATTLE);
    advance(&w, 15000);
    deliver_all(&w);
    check("crash: B's retry is answered from the log, and the ply is not taken twice",
          B(&w)->resolved == 1 && A(&w)->resolved == 1 && A(&w)->own.shot[55] == 1);
}

static void test_crash_before_save(void)
{
    struct wire w;

    wire_init(&w, 28);
    to_battle(&w);
    w.n[1].skip_save = 1;
    fleet_match_fire(B(&w), 1, 1, w.now);
    pump(&w);
    check("crash before save: the unsaved shot never left", w.qn == 0);
    check("crash before save: reload", mp_reload(&w.n[1], w.now) == 0);
    check("crash before save: the turn is still B's, with nothing pending",
          fleet_match_my_turn(B(&w)) && B(&w)->pending == FLEET_NO_CELL);
}

static void test_resume_resync(void)
{
    struct wire w;

    wire_init(&w, 29);
    one_shot(&w);
    deliver_at(&w, 0);
    remove_at(&w, find(&w, FLEET_MSG_RESULT));
    w.now += 4000;
    mp_reload(&w.n[1], w.now);               /* B closes and reopens */
    check("resume: B still has its shot pending", B(&w)->pending == 55 && B(&w)->resolved == 0);
    fleet_match_resume(B(&w), w.now);
    pump(&w);
    check("resume: B asks with SYNC", find(&w, FLEET_MSG_SYNC) >= 0 &&
          fleet_match_link(B(&w)) == FLEET_LINK_RESYNCING);
    deliver_all(&w);
    check("resume: A was one ply ahead and resent the answer; in step again",
          B(&w)->resolved == 1 && A(&w)->resolved == 1 && fleet_match_link(B(&w)) == FLEET_LINK_OK);
}

static void test_divergent_logs(void)
{
    struct wire w;

    wire_init(&w, 30);
    one_shot(&w);
    deliver_all(&w);
    B(&w)->log_res[1] ^= 3;    /* corrupt B's record of ply 1 in memory */
    fleet_match_resume(B(&w), w.now);
    pump(&w);
    deliver_all(&w);
    check("divergent: digests differ, the match is void on both sides",
          A(&w)->outcome == FLEET_OUTCOME_VOID && B(&w)->outcome == FLEET_OUTCOME_VOID &&
          A(&w)->end_reason == FLEET_END_VOID);
}

static void test_lost_link(void)
{
    struct wire w;
    int i;
    unsigned probes;

    wire_init(&w, 31);
    one_shot(&w);
    for (i = 0; i < 300; i++) {
        drop_all(&w);
        advance(&w, 1000);
    }
    check("lost: six tries, then lost", w.sent[1][FLEET_MSG_SHOT] == FLEET_RETRY_MAX &&
          fleet_match_link(B(&w)) == FLEET_LINK_LOST);
    probes = w.sent[1][FLEET_MSG_SYNC];
    for (i = 0; i < 600; i++) {
        drop_all(&w);
        advance(&w, 1000);
    }
    check("lost: probed by SYNC about every two minutes, and nothing else",
          w.sent[1][FLEET_MSG_SYNC] - probes >= 4 && w.sent[1][FLEET_MSG_SYNC] - probes <= 6 &&
          w.sent[1][FLEET_MSG_SHOT] == FLEET_RETRY_MAX);
    advance(&w, 150000);
    deliver_all(&w);
    advance(&w, 100);
    deliver_all(&w);
    check("lost: the link came back and the shot went through", B(&w)->resolved == 1 &&
          fleet_match_link(B(&w)) == FLEET_LINK_OK);
}

static void test_governor(void)
{
    struct wire w;
    struct fleet_msg msg;
    uint8_t buf[16];
    int n;
    int i;
    unsigned before;

    wire_init(&w, 32);
    one_shot(&w);
    deliver_all(&w);
    memset(&msg, 0, sizeof(msg));
    msg.type = FLEET_MSG_SYNC;
    msg.sid = A(&w)->sid;
    msg.ply = A(&w)->resolved;
    msg.flags = FLEET_SYNC_HAVE_COMMIT | (FLEET_SYNC_BATTLE << FLEET_SYNC_PHASE_SHIFT);
    msg.cell = FLEET_NO_CELL;
    msg.digest = fleet_match_digest(A(&w), A(&w)->resolved);
    n = fleet_proto_encode(&msg, buf, sizeof(buf));
    before = w.sent[0][FLEET_MSG_SYNC];
    for (i = 0; i < 100; i++) {
        w.now += 2100;   /* just past the duplicate-reply limit each time */
        fleet_match_receive(A(&w), w.n[1].key, buf, (size_t)n, w.now);
        pump(&w);
        drop_all(&w);
    }
    /* 210 s: a burst of 6, then one every 5 s. */
    check("governor: a flood of requests is answered within the burst and refill",
          w.sent[0][FLEET_MSG_SYNC] - before <= 6 + 210000 / FLEET_GOV_REFILL_MS + 1);
    check("governor: the refused replies were dropped, not queued", A(&w)->stats.gov_drops > 0);
}

/* ---- save codec -------------------------------------------------------------------- */

static void test_save_codec(void)
{
    struct wire w;
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    struct fleet_match copy;
    int n;

    wire_init(&w, 33);
    to_battle(&w);
    play(&w, 60000, 0);
    n = fleet_match_save_encode(A(&w), blob, sizeof(blob));
    check("save: fixed size", n == FLEET_MATCH_SAVE_SIZE);
    printf("     match.v1 is %d bytes\n", n);
    fleet_match_init(&copy, w.n[0].key, 1);
    check("save: decodes and restores", fleet_match_save_decode(&copy, blob, sizeof(blob)) == 0 &&
          fleet_match_restore(&copy, w.now) == 0);
    check("save: the same match", copy.resolved == A(&w)->resolved && copy.sid == A(&w)->sid &&
          memcmp(copy.own_commit, A(&w)->own_commit, FLEET_COMMIT_BYTES) == 0 &&
          memcmp(&copy.own, &A(&w)->own, sizeof(copy.own)) == 0 &&
          memcmp(copy.target.shot, A(&w)->target.shot, FLEET_CELLS) == 0);
    fleet_match_init(&copy, w.n[1].key, 1);
    check("save: another node's save is refused as such",
          fleet_match_save_decode(&copy, blob, sizeof(blob)) == -2);
    fleet_match_init(&copy, w.n[0].key, 1);
    blob[100] ^= 1;
    check("save: a flipped bit is refused", fleet_match_save_decode(&copy, blob, sizeof(blob)) == -1);
    check("save: a short blob is refused", fleet_match_save_decode(&copy, blob, sizeof(blob) - 1) == -1);
    {
        /* A log the own fleet contradicts, re-sealed so only restore can catch it. */
        struct fleet_match bad = *A(&w);
        int k;

        for (k = 1; k <= bad.resolved; k++) {
            if (fleet_match_shooter(k) != bad.role) {
                bad.log_res[k] = fleet_res_outcome(bad.log_res[k]) == 1 ? fleet_res_make(2, 0, 0)
                                                                         : fleet_res_make(1, 0, 0);
                break;
            }
        }
        fleet_match_save_encode(&bad, blob, sizeof(blob));
        fleet_match_init(&copy, w.n[0].key, 1);
        check("save: a log the own fleet contradicts is refused on restore",
              fleet_match_save_decode(&copy, blob, sizeof(blob)) == 0 &&
              fleet_match_restore(&copy, w.now) == -1);
    }
    {
        struct fleet_match inviting;

        fleet_match_init(&inviting, w.n[2].key, 1);
        fleet_match_invite(&inviting, w.n[0].key, "A", 7, w.now);
        fleet_match_save_encode(&inviting, blob, sizeof(blob));
        fleet_match_init(&copy, w.n[2].key, 1);
        check("save: an invite is not saved; it restores as idle",
              fleet_match_save_decode(&copy, blob, sizeof(blob)) == 0 &&
              fleet_match_restore(&copy, w.now) == 0 && copy.phase == FLEET_MP_IDLE);
    }
}

static void test_layout_commit(void)
{
    struct fleet_board b;
    struct fleet_board back;
    struct fleet_rng rng;
    uint8_t layout[FLEET_LAYOUT_BYTES];
    uint8_t key[FLEET_KEY_BYTES];
    uint8_t salt[FLEET_SALT_BYTES] = { 1 };
    uint8_t c1[FLEET_COMMIT_BYTES];
    uint8_t c2[FLEET_COMMIT_BYTES];

    fleet_board_clear(&b);
    fleet_rng_seed(&rng, 5);
    fleet_board_autoplace(&b, &rng);
    mp_key(key, 1);
    check("layout: encodes", fleet_layout_encode(&b, layout) == 0);
    check("layout: decodes to the same fleet", fleet_layout_decode(layout, &back) == 0 &&
          memcmp(back.ship_at, b.ship_at, FLEET_CELLS) == 0);
    layout[1] = layout[0];
    check("layout: two ships on one square is refused", fleet_layout_decode(layout, &back) != 0);
    fleet_layout_encode(&b, layout);
    fleet_commit_compute(1, key, layout, salt, c1);
    salt[0] = 2;
    fleet_commit_compute(1, key, layout, salt, c2);
    check("commit: the salt changes it", memcmp(c1, c2, FLEET_COMMIT_BYTES) != 0);
    salt[0] = 1;
    fleet_commit_compute(2, key, layout, salt, c2);
    check("commit: the session changes it", memcmp(c1, c2, FLEET_COMMIT_BYTES) != 0);
    key[5] ^= 1;
    fleet_commit_compute(1, key, layout, salt, c2);
    check("commit: the owner changes it", memcmp(c1, c2, FLEET_COMMIT_BYTES) != 0);
}

/* ---- chat ---------------------------------------------------------------------- */

static int count_type(struct wire *w, int type)
{
    int n = 0;
    int i;

    for (i = 0; i < w->qn; i++) {
        n += (w->q[i].b[0] & 0x3F) == type;
    }
    return n;
}

/* A CHAT as node `from` would send it, handed straight to `to`. */
static void say_raw(struct wire *w, int from, int to, uint32_t sid, uint8_t id, const char *text)
{
    struct fleet_msg msg;
    uint8_t buf[FLEET_PROTO_MAX];
    int n;

    memset(&msg, 0, sizeof(msg));
    msg.type = FLEET_MSG_CHAT;
    msg.sid = sid;
    msg.ply = id;
    msg.text_len = (uint8_t)strlen(text);
    memcpy(msg.text, text, msg.text_len);
    n = fleet_proto_encode(&msg, buf, sizeof(buf));
    if (n > 0) {
        fleet_match_receive(&w->n[to].m, w->n[from].key, buf, (size_t)n, w->now);
    }
    pump(w);
}

static int lines_mine_waiting(const struct fleet_match *m)
{
    return fleet_chat_pending(&m->chat);
}

/* to_battle() sets a match up in no time at all, which leaves the governor's
 * burst at the tokens chat never takes. People take half a minute to get
 * there, and the burst refills meanwhile. */
static void to_battle_settled(struct wire *w)
{
    to_battle(w);
    advance(w, 30000);
    deliver_all(w);
}

static void test_chat_line(void)
{
    struct wire w;
    const struct fleet_chat_line *l;
    int rc;

    wire_init(&w, 40);
    check("chat: nothing can be said without a match",
          fleet_match_chat_send(A(&w), "Anyone there?", w.now) == -1 && A(&w)->chat.count == 0);
    to_battle_settled(&w);
    rc = fleet_match_chat_send(A(&w), "Hello there", w.now);
    check("chat: a line is taken once the match is live", rc == 0);
    check("chat: and it is not saved - nothing waits on the disk for it", !fleet_match_dirty(A(&w)));
    pump(&w);
    check("chat: it goes on the air at once when the game owes nothing",
          count_type(&w, FLEET_MSG_CHAT) == 1 && A(&w)->chat.line[0].mine &&
          A(&w)->chat.line[0].state == FLEET_CHAT_SENDING);
    deliver_at(&w, find(&w, FLEET_MSG_CHAT));
    l = fleet_chat_last(&B(&w)->chat);
    check("chat: the opponent has it as theirs, word for word",
          l && !l->mine && strcmp(l->text, "Hello there") == 0 && B(&w)->chat.unread == 1);
    check("chat: and answers with a receipt", count_type(&w, FLEET_MSG_CHAT_ACK) == 1);
    deliver_all(&w);
    check("chat: the receipt marks ours delivered", A(&w)->chat.line[0].state == FLEET_CHAT_DELIVERED);
    check("chat: nothing of the match moved", A(&w)->resolved == 0 && B(&w)->resolved == 0 &&
          fleet_match_my_turn(B(&w)) && A(&w)->phase == FLEET_MP_BATTLE);
    fleet_match_chat_seen(B(&w));
    check("chat: seen, nothing is new", B(&w)->chat.unread == 0);
    /* A third node, speaking into this session, is not the opponent. */
    say_raw(&w, 2, 1, A(&w)->sid, 99, "I am A, honest");
    check("chat: a line from anyone but the opponent is not shown", B(&w)->chat.count == 1 &&
          B(&w)->stats.rx_foreign > 0 && count_type(&w, FLEET_MSG_CHAT_ACK) == 0);
}

static void test_chat_duplicates(void)
{
    struct wire w;
    struct pkt copy;
    int i;

    wire_init(&w, 41);
    to_battle_settled(&w);
    fleet_match_chat_send(A(&w), "Once only", w.now);
    pump(&w);
    i = find(&w, FLEET_MSG_CHAT);
    copy = w.q[i];
    w.q[w.qn++] = copy;
    deliver_all(&w);
    check("chat duplicate: a line that arrives twice is shown once",
          B(&w)->chat.count == 1 && B(&w)->stats.chat_dup == 1 && B(&w)->chat.unread == 1);
    check("chat duplicate: the copy within 2 s is not answered again", w.sent[1][FLEET_MSG_CHAT_ACK] == 1);
    w.now += FLEET_DUP_REPLY_MS + 100;
    w.q[w.qn++] = copy;
    deliver_all(&w);
    check("chat duplicate: a later copy is answered again, still shown once",
          w.sent[1][FLEET_MSG_CHAT_ACK] == 2 && B(&w)->chat.count == 1);

    /* The receipt lost: the retry is a copy to the receiver. */
    fleet_match_chat_send(A(&w), "Receipt lost", w.now);
    pump(&w);
    deliver_at(&w, find(&w, FLEET_MSG_CHAT));
    i = find(&w, FLEET_MSG_CHAT_ACK);
    check("chat receipt lost: it was sent", i >= 0);
    if (i >= 0) {
        remove_at(&w, i);
    }
    advance(&w, 20000);
    check("chat receipt lost: ours is sent again", w.sent[0][FLEET_MSG_CHAT] == 3);
    deliver_all(&w);
    check("chat receipt lost: the retry confirms it and is not shown twice",
          A(&w)->chat.line[1].state == FLEET_CHAT_DELIVERED && B(&w)->chat.count == 2);
}

static void test_chat_lost(void)
{
    struct wire w;
    int i;

    wire_init(&w, 42);
    to_battle_settled(&w);
    fleet_match_chat_send(A(&w), "Into the void", w.now);
    pump(&w);
    for (i = 0; i < 200; i++) {
        drop_all(&w);
        advance(&w, 1000);
    }
    check("chat lost: tried three times, then given up",
          w.sent[0][FLEET_MSG_CHAT] == FLEET_CHAT_TRIES &&
          A(&w)->chat.line[0].state == FLEET_CHAT_FAILED && A(&w)->stats.chat_failed == 1);
    check("chat lost: the other side never saw it", B(&w)->chat.count == 0);
    check("chat lost: a line going unanswered is not a link problem",
          fleet_match_link(A(&w)) == FLEET_LINK_OK && !A(&w)->lost);
    fleet_match_chat_send(A(&w), "Second try", w.now);
    pump(&w);
    check("chat lost: the next line still goes", count_type(&w, FLEET_MSG_CHAT) == 1);
    deliver_all(&w);
    check("chat lost: and arrives", B(&w)->chat.count == 1 &&
          strcmp(B(&w)->chat.line[0].text, "Second try") == 0);
}

static void test_chat_reordered(void)
{
    struct wire w;
    struct pkt late;
    int i;

    wire_init(&w, 43);
    to_battle_settled(&w);
    fleet_match_chat_send(A(&w), "First", w.now);
    fleet_match_chat_send(A(&w), "Second", w.now);
    pump(&w);
    check("chat order: one line on the air at a time", count_type(&w, FLEET_MSG_CHAT) == 1 &&
          A(&w)->chat.line[1].state == FLEET_CHAT_QUEUED);
    i = find(&w, FLEET_MSG_CHAT);
    late = w.q[i];
    remove_at(&w, i);           /* held up somewhere on the mesh */
    advance(&w, 20000);         /* the retry of the first */
    deliver_all(&w);            /* ... its receipt, then the second, its receipt */
    advance(&w, 100);
    deliver_all(&w);
    check("chat order: both arrive, in order", B(&w)->chat.count == 2 &&
          strcmp(B(&w)->chat.line[0].text, "First") == 0 &&
          strcmp(B(&w)->chat.line[1].text, "Second") == 0);
    w.q[w.qn++] = late;
    deliver_all(&w);
    check("chat order: the first, arriving late after the second, is not shown again",
          B(&w)->chat.count == 2 && strcmp(fleet_chat_last(&B(&w)->chat)->text, "Second") == 0);
    check("chat order: both of ours are delivered",
          A(&w)->chat.line[0].state == FLEET_CHAT_DELIVERED &&
          A(&w)->chat.line[1].state == FLEET_CHAT_DELIVERED);
}

static void test_chat_bounded(void)
{
    struct wire w;
    char text[16];
    int i;
    int rc;

    wire_init(&w, 44);
    to_battle_settled(&w);
    for (i = 0; i < 40; i++) {
        snprintf(text, sizeof(text), "line %d", i);
        say_raw(&w, 0, 1, A(&w)->sid, (uint8_t)i, text);
    }
    check("chat bounded: the history keeps the newest sixteen",
          B(&w)->chat.count == FLEET_CHAT_HISTORY &&
          strcmp(B(&w)->chat.line[0].text, "line 24") == 0 &&
          strcmp(B(&w)->chat.line[FLEET_CHAT_HISTORY - 1].text, "line 39") == 0);
    drop_all(&w);
    for (i = 0; i < FLEET_CHAT_OUTGOING; i++) {
        snprintf(text, sizeof(text), "mine %d", i);
        fleet_match_chat_send(A(&w), text, w.now);
    }
    rc = fleet_match_chat_send(A(&w), "one too many", w.now);
    check("chat bounded: a fifth line waiting is refused, not queued", rc == -2 &&
          lines_mine_waiting(A(&w)) == FLEET_CHAT_OUTGOING);
    drop_all(&w);
    for (i = 0; i < 30; i++) {
        snprintf(text, sizeof(text), "theirs %d", i);
        say_raw(&w, 1, 0, A(&w)->sid, (uint8_t)(100 + i), text);
        drop_all(&w);
    }
    check("chat bounded: a flood of theirs never pushes out ours still waiting",
          A(&w)->chat.count == FLEET_CHAT_HISTORY && lines_mine_waiting(A(&w)) == FLEET_CHAT_OUTGOING);
    check("chat bounded: the outbox never holds more than it can",
          A(&w)->out_len <= FLEET_MATCH_OUTBOX);
}

static void test_chat_limits(void)
{
    struct wire w;
    char longest[FLEET_CHAT_TEXT_MAX + 2];

    wire_init(&w, 45);
    to_battle_settled(&w);
    memset(longest, 'x', sizeof(longest));
    longest[FLEET_CHAT_TEXT_MAX] = '\0';
    check("chat limits: 37 bytes is a line", fleet_match_chat_send(A(&w), longest, w.now) == 0);
    longest[FLEET_CHAT_TEXT_MAX] = 'x';
    longest[FLEET_CHAT_TEXT_MAX + 1] = '\0';
    check("chat limits: 38 is refused", fleet_match_chat_send(A(&w), longest, w.now) == -1);
    check("chat limits: an empty line is refused", fleet_match_chat_send(A(&w), "", w.now) == -1);
    check("chat limits: so is one of spaces", fleet_match_chat_send(A(&w), "   \t ", w.now) == -1);
    check("chat limits: a newline is refused", fleet_match_chat_send(A(&w), "a\nb", w.now) == -1);
    check("chat limits: broken UTF-8 is refused", fleet_match_chat_send(A(&w), "a\xff", w.now) == -1);
    check("chat limits: spaces round a line are dropped",
          fleet_match_chat_send(A(&w), "  hi  ", w.now) == 0 &&
          strcmp(fleet_chat_last(&A(&w)->chat)->text, "hi") == 0);
    check("chat limits: a line of 37 with spaces round it fits",
          fleet_match_chat_send(A(&w), " xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx ", w.now) == 0);
    check("chat limits: Norwegian is text",
          fleet_match_chat_send(A(&w), "Bl\xc3\xa5" "b\xc3\xa6" "rsyltet\xc3\xb8" "y", w.now) == 0);
}

static void test_chat_malformed(void)
{
    struct wire w;
    uint8_t buf[16] = { (uint8_t)((1 << 6) | FLEET_MSG_CHAT), 0, 0, 0, 5, 'a', '\n', 'b' };
    unsigned bad;

    wire_init(&w, 46);
    to_battle_settled(&w);
    buf[1] = (uint8_t)(A(&w)->sid >> 16);
    buf[2] = (uint8_t)(A(&w)->sid >> 8);
    buf[3] = (uint8_t)A(&w)->sid;
    bad = B(&w)->stats.rx_bad;
    fleet_match_receive(B(&w), w.n[0].key, buf, 8, w.now);
    pump(&w);
    check("chat malformed: a line with a control character is refused, unanswered",
          B(&w)->chat.count == 0 && B(&w)->stats.rx_bad == bad + 1 && w.qn == 0);
    buf[0] = (uint8_t)((1 << 6) | 40);          /* a type from a later build */
    fleet_match_receive(B(&w), w.n[0].key, buf, 8, w.now);
    pump(&w);
    check("chat malformed: a type this build does not know is dropped, unanswered",
          B(&w)->stats.rx_bad == bad + 2 && w.qn == 0 && B(&w)->phase == FLEET_MP_BATTLE);
    buf[0] = (uint8_t)((1 << 6) | FLEET_MSG_CHAT_ACK);
    fleet_match_receive(A(&w), w.n[1].key, buf, 7, w.now);
    pump(&w);
    check("chat malformed: a receipt for a line never sent changes nothing",
          A(&w)->chat.count == 0 && w.qn == 0);
}

static void test_chat_stale_session(void)
{
    struct wire w;
    struct pkt old_line;
    uint32_t old_sid;
    int i;

    wire_init(&w, 47);
    to_battle_settled(&w);
    old_sid = A(&w)->sid;
    fleet_match_chat_send(A(&w), "old news", w.now);
    pump(&w);
    old_line = w.q[find(&w, FLEET_MSG_CHAT)];
    deliver_all(&w);
    check("chat stale: said in the old match", B(&w)->chat.count == 1);
    fleet_match_forfeit(A(&w), w.now);
    pump(&w);
    deliver_all(&w);
    fleet_match_dismiss(A(&w));
    fleet_match_dismiss(B(&w));
    check("chat stale: putting a match away forgets its chat",
          A(&w)->chat.count == 0 && B(&w)->chat.count == 0 && B(&w)->chat.unread == 0);
    to_battle_settled(&w);
    check("chat stale: a new match, a new session", A(&w)->sid != old_sid &&
          B(&w)->phase == FLEET_MP_BATTLE && B(&w)->chat.count == 0);
    w.q[w.qn++] = old_line;
    i = w.qn - 1;
    deliver_at(&w, i);
    check("chat stale: a line of the old session is not shown in the new one",
          B(&w)->chat.count == 0 && B(&w)->phase == FLEET_MP_BATTLE);
    check("chat stale: it is answered as the old session's, with its END",
          find(&w, FLEET_MSG_END) >= 0 && count_type(&w, FLEET_MSG_CHAT_ACK) == 0);
    drop_all(&w);
    {
        uint8_t ack[7] = { (uint8_t)((1 << 6) | FLEET_MSG_CHAT_ACK), (uint8_t)(old_sid >> 16),
                           (uint8_t)(old_sid >> 8), (uint8_t)old_sid, 1, 0, 0 };

        fleet_match_receive(A(&w), w.n[1].key, ack, sizeof(ack), w.now);
        pump(&w);
        check("chat stale: an old session's receipt is dropped without a word", w.qn == 0 &&
              A(&w)->phase == FLEET_MP_BATTLE);
    }
}

static void test_chat_second_to_the_game(void)
{
    struct wire w;
    int i;

    wire_init(&w, 48);
    to_battle_settled(&w);
    /* The guest fires and says something in the same breath. */
    fleet_match_fire(B(&w), 5, 5, w.now);
    fleet_match_chat_send(B(&w), "Take that", w.now);
    pump(&w);
    check("chat priority: the shot goes, the line waits for its answer",
          count_type(&w, FLEET_MSG_SHOT) == 1 && count_type(&w, FLEET_MSG_CHAT) == 0);
    deliver_at(&w, find(&w, FLEET_MSG_SHOT));
    check("chat priority: the answer comes first", count_type(&w, FLEET_MSG_RESULT) == 1 &&
          count_type(&w, FLEET_MSG_CHAT) == 0);
    deliver_at(&w, find(&w, FLEET_MSG_RESULT));
    check("chat priority: then the line", B(&w)->resolved == 1 && count_type(&w, FLEET_MSG_CHAT) == 1);
    deliver_all(&w);

    /* Chat never takes the last tokens of the burst. */
    B(&w)->tokens = FLEET_CHAT_TOKEN_RESERVE;
    B(&w)->refill_at = w.now + 600000;
    fleet_match_chat_send(B(&w), "held back", w.now);
    pump(&w);
    check("chat priority: at the reserve, a line waits", count_type(&w, FLEET_MSG_CHAT) == 0 &&
          B(&w)->stats.chat_held == 1);
    fleet_match_fire(A(&w), 0, 0, w.now);   /* ply 2 is the host's */
    pump(&w);
    deliver_all(&w);
    check("chat priority: the game still moves on what chat left it",
          A(&w)->resolved == 2 && B(&w)->resolved == 2 && count_type(&w, FLEET_MSG_CHAT) == 0);
    fleet_match_fire(B(&w), 6, 6, w.now);
    pump(&w);
    check("chat priority: and our own shot takes a reserved token",
          count_type(&w, FLEET_MSG_SHOT) == 1);
    deliver_all(&w);

    /* Nor Fleet's last 40 s of the hour. */
    B(&w)->tokens = FLEET_GOV_BURST;
    B(&w)->minute_of[(w.now / 60000) % 60] = w.now / 60000;
    B(&w)->minute_ms[(w.now / 60000) % 60] = FLEET_CHAT_HOUR_MS;
    advance(&w, 100);
    check("chat priority: past 50 s in the hour, a line waits", count_type(&w, FLEET_MSG_CHAT) == 0);
    fleet_match_fire(A(&w), 0, 1, w.now);
    pump(&w);
    deliver_all(&w);
    check("chat priority: while the game's answer still goes", A(&w)->resolved == 4);
    for (i = 0; i < 70; i++) {
        advance(&w, 60000);
        deliver_all(&w);
    }
    check("chat priority: and the line goes once the hour has room",
          B(&w)->chat.line[B(&w)->chat.count - 1].state == FLEET_CHAT_DELIVERED);
}

static void test_chat_ending(void)
{
    struct wire w;
    int i;

    wire_init(&w, 49);
    to_battle_settled(&w);
    fleet_match_chat_send(B(&w), "one", w.now);
    fleet_match_chat_send(B(&w), "two", w.now);
    pump(&w);
    drop_all(&w);
    fleet_match_forfeit(A(&w), w.now);
    pump(&w);
    deliver_at(&w, find(&w, FLEET_MSG_END));
    check("chat ending: the match ended by forfeit", B(&w)->phase == FLEET_MP_DONE &&
          B(&w)->end_reason == FLEET_END_FORFEIT);
    check("chat ending: lines still waiting are given up, not sent into the ending",
          B(&w)->chat.line[0].state == FLEET_CHAT_FAILED &&
          B(&w)->chat.line[1].state == FLEET_CHAT_FAILED && count_type(&w, FLEET_MSG_CHAT) == 0);
    check("chat ending: and nothing more can be said", fleet_match_chat_send(B(&w), "three", w.now) == -1);
    {
        unsigned before = w.sent[1][FLEET_MSG_CHAT];

        for (i = 0; i < 100; i++) {
            advance(&w, 1000);
            deliver_all(&w);
        }
        check("chat ending: nothing of it goes afterwards", w.sent[1][FLEET_MSG_CHAT] == before);
    }

    /* A match played to its end keeps the chat for the word after it. */
    wire_init(&w, 50);
    to_battle_settled(&w);
    play(&w, 3600000, 0);
    check("chat after the end: the match is over", A(&w)->phase == FLEET_MP_DONE &&
          B(&w)->phase == FLEET_MP_DONE && A(&w)->end_reason == FLEET_END_NONE);
    check("chat after the end: good game", fleet_match_chat_send(A(&w), "gg", w.now) == 0);
    pump(&w);
    deliver_all(&w);
    check("chat after the end: arrives and is confirmed",
          B(&w)->chat.count == 1 && strcmp(B(&w)->chat.line[0].text, "gg") == 0 &&
          A(&w)->chat.line[0].state == FLEET_CHAT_DELIVERED);
}

/* A whole match with both players talking all the way through it. */
static void test_chat_under_load(void)
{
    struct wire quiet;
    struct wire w;
    int64_t start;
    int64_t quiet_ms = 0;
    int64_t busy_ms = 0;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        struct wire *p = pass ? &w : &quiet;
        int64_t end;
        int64_t ready[2] = { 0, 0 };
        int seen[2] = { -1, -1 };
        int64_t next_say = 0;
        int said = 0;

        wire_init(p, 51);
        to_battle(p);
        start = p->now;
        end = p->now + 3600000;
        while (p->now < end && !(A(p)->phase == FLEET_MP_DONE && B(p)->phase == FLEET_MP_DONE)) {
            int i;

            deliver_all(p);
            for (i = 0; i < 2; i++) {
                int turn = fleet_match_my_turn(&p->n[i].m) ? p->n[i].m.resolved : -1;

                if (turn != seen[i]) {
                    seen[i] = turn;
                    ready[i] = p->now + THINK_MS;
                }
                if (turn < 0 || p->now >= ready[i]) {
                    mp_play(&p->n[i], p->now, 0);
                }
            }
            if (pass && p->now >= next_say) {
                /* Each side, every two seconds, as long as it has room. */
                char text[64];

                /* 30-odd bytes: a three-block frame, the dearest there is. */
                snprintf(text, sizeof(text), "Line %d, long enough to fill it!", said++);
                fleet_match_chat_send(A(p), text, p->now);
                fleet_match_chat_send(B(p), text, p->now);
                next_say = p->now + 2000;
            }
            pump(p);
            deliver_all(p);
            advance(p, 100);
        }
        if (pass) {
            busy_ms = p->now - start;
        } else {
            quiet_ms = p->now - start;
        }
    }
    printf("     under load: %u plies in %lld s against %u in %lld s quiet; chat %u+%u lines "
           "shown, %u+%u frames, %u+%u held; airtime A %u ms, B %u ms\n",
           A(&w)->resolved, (long long)(busy_ms / 1000), A(&quiet)->resolved,
           (long long)(quiet_ms / 1000), A(&w)->stats.chat_rx, B(&w)->stats.chat_rx,
           A(&w)->stats.chat_tx, B(&w)->stats.chat_tx, A(&w)->stats.chat_held,
           B(&w)->stats.chat_held, w.airtime[0], w.airtime[1]);
    check("chat load: the match is played to its end", A(&w)->phase == FLEET_MP_DONE &&
          B(&w)->phase == FLEET_MP_DONE && A(&w)->verify == FLEET_VERIFY_OK &&
          B(&w)->verify == FLEET_VERIFY_OK);
    check("chat load: the same match, shot for shot", A(&w)->resolved == A(&quiet)->resolved &&
          memcmp(A(&w)->log_cell, A(&quiet)->log_cell, sizeof(A(&w)->log_cell)) == 0);
    check("chat load: every ply still one SHOT and one RESULT - no retry of the game's",
          w.sent[0][FLEET_MSG_SHOT] + w.sent[1][FLEET_MSG_SHOT] == A(&w)->resolved &&
          w.sent[0][FLEET_MSG_RESULT] + w.sent[1][FLEET_MSG_RESULT] == A(&w)->resolved);
    check("chat load: a lot was said", A(&w)->stats.chat_rx > 20 && B(&w)->stats.chat_rx > 20);
    check("chat load: and the match took no more than a quarter longer",
          busy_ms * 4 <= quiet_ms * 5);
    check("chat load: within Fleet's hour", w.airtime[0] <= FLEET_GOV_HOUR_MS * (busy_ms / 3600000 + 1) &&
          w.airtime[1] <= FLEET_GOV_HOUR_MS * (busy_ms / 3600000 + 1));
}

int main(void)
{
    test_invite_accept();
    test_start_lost();
    test_decline();
    test_cancel();
    test_no_answer();
    test_crossed_invites();
    test_busy();
    test_busy_with_you();
    test_commit_then_battle();
    test_persist_gate();
    test_full_match();
    test_lost_shot();
    test_lost_result();
    test_duplicate_shot();
    test_duplicate_result();
    test_out_of_order();
    test_delayed_old_packet();
    test_simultaneous_retry();
    test_stale_session();
    test_forfeit();
    test_forfeit_unacked();
    test_lie_detected_at_reveal();
    test_impossible_answer();
    test_changed_answer();
    test_changed_commit();
    test_wrong_sender();
    test_crash_after_save_before_send();
    test_crash_before_save();
    test_resume_resync();
    test_divergent_logs();
    test_lost_link();
    test_governor();
    test_save_codec();
    test_layout_commit();
    test_chat_line();
    test_chat_duplicates();
    test_chat_lost();
    test_chat_reordered();
    test_chat_bounded();
    test_chat_limits();
    test_chat_malformed();
    test_chat_stale_session();
    test_chat_second_to_the_game();
    test_chat_ending();
    test_chat_under_load();
    printf("fleet_match_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
