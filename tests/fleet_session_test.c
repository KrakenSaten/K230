/*
 * PocketFleet multiplayer session (apps/fleet/link/fleet_session.h) against
 * the virtual opponent (fleet_link_loop.h) and an in-memory store: whole
 * matches through the same joint the app uses, including the rules the joint
 * itself keeps - nothing pumped before the player engages, save before send,
 * a failed save played through, a saved match resumed by a new session, and
 * a save from another identity refused.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_ai.h"
#include "fleet_link_loop.h"
#include "fleet_session.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

struct store {
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    int has;
    int fail;
    unsigned saves;
    uint32_t rng;
};

static int store_save(void *user, const uint8_t *blob, size_t n)
{
    struct store *st = user;

    if (st->fail || n != sizeof(st->blob)) {
        return -1;
    }
    memcpy(st->blob, blob, n);
    st->has = 1;
    st->saves++;
    return 0;
}

static int entropy(void *user, void *buf, size_t n)
{
    struct store *st = user;
    uint8_t *b = buf;
    size_t i;

    for (i = 0; i < n; i++) {
        st->rng = st->rng * 1103515245u + 12345u;
        b[i] = (uint8_t)(st->rng >> 16);
    }
    return 0;
}

/* Our player: the AI, from our own shots and their answers. */
static void our_move(struct fleet_session *s, int64_t now)
{
    struct fleet_match *m = &s->m;
    struct fleet_ai ai;
    struct fleet_rng rng;
    int row;
    int col;
    int k;

    if (m->phase == FLEET_MP_DEPLOY && !m->committed) {
        struct fleet_board b;

        fleet_board_clear(&b);
        fleet_rng_seed(&rng, 99);
        fleet_board_autoplace(&b, &rng);
        fleet_session_deploy(s, &b, now);
        return;
    }
    if (!fleet_match_my_turn(m)) {
        return;
    }
    fleet_ai_init(&ai, FLEET_COMMANDER);
    for (k = 1; k <= m->resolved; k++) {
        if (fleet_match_shooter(k) == m->role) {
            fleet_ai_observe(&ai, m->log_cell[k] / 10, m->log_cell[k] % 10,
                             (enum fleet_shot_result)fleet_res_outcome(m->log_res[k]),
                             fleet_res_ship(m->log_res[k]));
        }
    }
    fleet_rng_seed(&rng, 7u + m->resolved);
    if (fleet_ai_next_shot(&ai, &rng, &row, &col) == 0) {
        fleet_session_fire(s, row, col, now);
    }
}

/* Run until the match is done on our side or time runs out. */
static int64_t run(struct fleet_session *s, int64_t now, int64_t ms, int play)
{
    int64_t end = now + ms;
    int64_t next_move = now;

    while (now < end) {
        now += 100;
        fleet_session_poll(s, now);
        if (play && now >= next_move) {
            unsigned before = fleet_session_revision(s);

            our_move(s, now);
            if (fleet_session_revision(s) != before) {
                next_move = now + 2000;
            }
        }
        if (s->ready && s->m.phase == FLEET_MP_DONE && !s->m.end_unacked &&
            s->m.verify != FLEET_VERIFY_PENDING) {
            break;
        }
    }
    return now;
}

static struct fleet_link *open_loop(const char *spec)
{
    struct fleet_link_loop_cfg cfg;

    fleet_link_loop_defaults(&cfg);
    cfg.think_ms = 1500;
    fleet_link_loop_parse(spec, &cfg);
    return fleet_link_loop_open(&cfg);
}

static void peer_key(struct fleet_link *link, uint8_t key[FLEET_KEY_BYTES])
{
    struct fleet_link_peer p;

    link->ops->peers(link->ctx, &p, 1);
    memcpy(key, p.key, FLEET_KEY_BYTES);
}

static void test_not_engaged(void)
{
    struct store st = { .rng = 1 };
    struct fleet_session s;
    struct fleet_link *link = open_loop("invite=500");
    int64_t now = 0;

    fleet_session_init(&s, link, store_save, entropy, &st);
    now = run(&s, now, 10000, 0);
    check("not engaged: an invite waiting for us is not even read", s.received == 0 &&
          s.sent == 0 && s.m.phase == FLEET_MP_IDLE);
    fleet_session_engage(&s, now);
    now = run(&s, now, 3000, 0);
    check("engaged: the opponent's invite arrives", s.m.phase == FLEET_MP_INVITED);
    check("and nothing was sent in answer on its own", s.sent == 0);
    fleet_session_accept(&s, now);
    run(&s, now, 30 * 60000, 1);
    check("accepted and played to the end", s.m.phase == FLEET_MP_DONE &&
          s.m.verify == FLEET_VERIFY_OK && s.m.role == FLEET_ROLE_GUEST);
    link->ops->close(link->ctx);
}

static void whole_match(const char *spec, const char *label)
{
    struct store st = { .rng = 2 };
    struct fleet_session s;
    struct fleet_link *link = open_loop(spec);
    uint8_t key[FLEET_KEY_BYTES];
    const struct fleet_match *peer;
    char name[64];
    int64_t now = 0;

    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, now);
    peer_key(link, key);
    check(label, fleet_session_invite(&s, key, "SIM OPPONENT", now) == 0);
    now = run(&s, now, 3 * 3600000LL, 1);
    peer = fleet_link_loop_peer(link);
    /* Done on our side; theirs may still be finishing the exchange of fleets,
     * which our side answers from DONE. */
    while (peer->phase != FLEET_MP_DONE && now < 4 * 3600000LL) {
        now += 100;
        fleet_session_poll(&s, now);
    }
    snprintf(name, sizeof(name), "%s: done, one winner, both fleets verified", label);
    check(name, s.m.phase == FLEET_MP_DONE && peer->phase == FLEET_MP_DONE &&
          (s.m.outcome == FLEET_OUTCOME_WIN) != (peer->outcome == FLEET_OUTCOME_WIN) &&
          s.m.verify == FLEET_VERIFY_OK && peer->verify == FLEET_VERIFY_OK);
    snprintf(name, sizeof(name), "%s: the same log on both sides", label);
    check(name, s.m.resolved == peer->resolved &&
          memcmp(s.m.log_cell, peer->log_cell, sizeof(s.m.log_cell)) == 0);
    snprintf(name, sizeof(name), "%s: saved as it went", label);
    check(name, st.has && st.saves > s.m.resolved / 2);
    link->ops->close(link->ctx);
}

static void test_reopen(void)
{
    struct store st = { .rng = 3 };
    struct fleet_session s;
    struct fleet_session again;
    struct fleet_link *link = open_loop("loss=10");
    uint8_t key[FLEET_KEY_BYTES];
    int64_t now = 0;
    int r;

    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, now);
    peer_key(link, key);
    fleet_session_invite(&s, key, NULL, now);
    now = run(&s, now, 4 * 60000, 1);
    r = s.m.resolved;
    check("reopen: some of the match was played", r > 4 && s.m.phase == FLEET_MP_BATTLE);
    /* The app closes: the session is gone, the store is not. */
    fleet_session_init(&again, link, store_save, entropy, &st);
    fleet_session_set_saved(&again, st.blob, sizeof(st.blob), 0);
    now = run(&again, now, 30000, 0);
    check("reopen: the saved match is read, and nothing is sent before Resume",
          again.ready && again.m.phase == FLEET_MP_BATTLE && again.m.resolved >= r - 1 &&
          again.sent == 0);
    check("reopen: resume", fleet_session_resume(&again, now) == 0);
    run(&again, now, 3 * 3600000LL, 1);
    check("reopen: the match finished and verified",
          again.m.phase == FLEET_MP_DONE && again.m.verify == FLEET_VERIFY_OK);
    link->ops->close(link->ctx);
}

static void test_save_failure(void)
{
    struct store st = { .rng = 4, .fail = 1 };
    struct fleet_session s;
    struct fleet_link *link = open_loop("");
    uint8_t key[FLEET_KEY_BYTES];

    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, 0);
    peer_key(link, key);
    fleet_session_invite(&s, key, NULL, 0);
    run(&s, 0, 3 * 3600000LL, 1);
    check("save failure: said so, and played on to the end",
          s.save_failed && s.m.phase == FLEET_MP_DONE && s.m.verify == FLEET_VERIFY_OK);
    link->ops->close(link->ctx);
}

static void test_other_identity(void)
{
    struct store st = { .rng = 5 };
    struct fleet_session s;
    struct fleet_match other;
    struct fleet_link *link = open_loop("");
    uint8_t key[FLEET_KEY_BYTES] = { 1, 2, 3 };
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];

    fleet_match_init(&other, key, 1);
    fleet_match_save_encode(&other, blob, sizeof(blob));
    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_set_saved(&s, blob, sizeof(blob), 0);
    fleet_session_poll(&s, 100);
    check("other identity: a save made under another key is not used",
          s.ready && s.identity_changed && s.store_fault && s.m.phase == FLEET_MP_IDLE);
    link->ops->close(link->ctx);
}

static void test_decline_and_silence(void)
{
    struct store st = { .rng = 6 };
    struct fleet_session s;
    struct fleet_link *link = open_loop("decline");
    uint8_t key[FLEET_KEY_BYTES];

    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, 0);
    peer_key(link, key);
    fleet_session_invite(&s, key, NULL, 0);
    run(&s, 0, 20000, 0);
    check("decline: the host is told", s.m.phase == FLEET_MP_IDLE &&
          s.m.notice == FLEET_NOTICE_DECLINED);
    link->ops->close(link->ctx);

    link = open_loop("silent");
    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, 0);
    peer_key(link, key);
    fleet_session_invite(&s, key, NULL, 0);
    run(&s, 0, 10 * 60000, 0);
    check("silence: no answer, and the host says so", s.m.phase == FLEET_MP_IDLE &&
          s.m.notice == FLEET_NOTICE_NO_ANSWER && s.sent == FLEET_RETRY_MAX);
    link->ops->close(link->ctx);
}

/* ---- chat through the session -------------------------------------------- */

static int theirs_saying(const struct fleet_chat *c, const char *text)
{
    int n = 0;
    int i;

    for (i = 0; i < c->count; i++) {
        n += !c->line[i].mine && strcmp(c->line[i].text, text) == 0;
    }
    return n;
}

static void test_chat_session(void)
{
    struct store st = { .rng = 7 };
    struct fleet_session s;
    struct fleet_session again;
    struct fleet_link *link = open_loop("chat,think=1000");
    const struct fleet_match *peer = fleet_link_loop_peer(link);
    uint8_t key[FLEET_KEY_BYTES];
    unsigned saves;
    int64_t now = 0;
    int i;

    fleet_session_init(&s, link, store_save, entropy, &st);
    fleet_session_engage(&s, now);
    peer_key(link, key);
    fleet_session_invite(&s, key, "SIM OPPONENT", now);
    for (i = 0; i < 600 && s.m.phase != FLEET_MP_BATTLE; i++) {
        now = run(&s, now, 100, 1);
    }
    check("chat session: in battle", s.m.phase == FLEET_MP_BATTLE);
    now = run(&s, now, 30000, 0);           /* the governor's burst refills */
    saves = st.saves;
    check("chat session: a line goes through the session",
          fleet_session_chat_send(&s, "Hello sim", now) == 0);
    check("chat session: and needs no save to go", st.saves == saves && s.sent > 0);
    now = run(&s, now, 5000, 0);
    check("chat session: the opponent has it, from us, once", theirs_saying(&peer->chat, "Hello sim") == 1);
    check("chat session: and confirmed it", s.m.chat.line[0].state == FLEET_CHAT_DELIVERED);
    check("chat session: its answer is shown, as theirs, once",
          theirs_saying(&s.m.chat, "Copy that.") == 1 && s.m.chat.unread == 1);
    fleet_link_loop_say(link, "Nice shot");
    now = run(&s, now, 3000, 0);
    check("chat session: the opponent speaking first is heard too",
          theirs_saying(&s.m.chat, "Nice shot") == 1);

    /* The opponent goes out of reach while we are typing, and after. */
    fleet_link_loop_set_cut(link, 1);
    fleet_session_chat_send(&s, "Are you there?", now);
    now = run(&s, now, 120000, 0);
    check("chat session: a line to an opponent out of reach is given up, not sent for ever",
          fleet_chat_last(&s.m.chat)->state == FLEET_CHAT_FAILED &&
          fleet_chat_pending(&s.m.chat) == 0);
    fleet_link_loop_set_cut(link, 0);
    fleet_session_chat_send(&s, "Back again", now);
    now = run(&s, now, 5000, 0);
    check("chat session: once back, the next line gets through",
          theirs_saying(&peer->chat, "Back again") == 1 &&
          theirs_saying(&peer->chat, "Are you there?") == 0);

    /* Leave with a line still waiting, and come back. */
    fleet_link_loop_set_cut(link, 1);
    fleet_session_chat_send(&s, "Left behind", now);
    now = run(&s, now, 1000, 0);
    check("chat session: a line is waiting as the app closes", fleet_chat_pending(&s.m.chat) == 1);
    for (i = 0; i < 20; i++) {
        /* Closed and opened again, over and over: the session is rebuilt
         * from the saved match each time, and nothing said survives it. */
        fleet_session_init(&again, link, store_save, entropy, &st);
        fleet_session_set_saved(&again, st.blob, sizeof(st.blob), 0);
        now = run(&again, now, 500, 0);
        fleet_session_resume(&again, now);
        if (again.m.chat.count != 0 || again.m.phase != FLEET_MP_BATTLE) {
            break;
        }
        now = run(&again, now, 500, 0);
    }
    check("chat session: reopened twenty times, the match is back and its chat is not",
          i == 20 && again.m.chat.count == 0);
    fleet_link_loop_set_cut(link, 0);
    now = run(&again, now, 60000, 1);
    check("chat session: the line left behind is never sent by the next run",
          theirs_saying(&peer->chat, "Left behind") == 0);

    /* Played out, put away, and a new match: a new chat. */
    now = run(&again, now, 3 * 3600000LL, 1);
    check("chat session: the match finished", again.m.phase == FLEET_MP_DONE);
    fleet_session_dismiss(&again, now);
    check("chat session: put away, no line is left", again.m.chat.count == 0);
    for (i = 0; i < 400 && peer->phase != FLEET_MP_IDLE; i++) {
        now = run(&again, now, 100, 0);
    }
    fleet_session_invite(&again, key, "SIM OPPONENT", now);
    for (i = 0; i < 600 && again.m.phase != FLEET_MP_BATTLE; i++) {
        now = run(&again, now, 100, 1);
    }
    check("chat session: a new match starts with an empty chat on both sides",
          again.m.phase == FLEET_MP_BATTLE && again.m.chat.count == 0 && peer->chat.count == 0);
    link->ops->close(link->ctx);
}

int main(void)
{
    test_not_engaged();
    whole_match("", "clean link");
    whole_match("loss=25,dup=10,delay=1500", "25% loss, duplicates, slow");
    test_reopen();
    test_save_failure();
    test_other_identity();
    test_decline_and_silence();
    test_chat_session();
    printf("fleet_session_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
