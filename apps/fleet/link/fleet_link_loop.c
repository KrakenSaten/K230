/*
 * The virtual opponent. See fleet_link_loop.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_link_loop.h"

#include "../engine/fleet_ai.h"
#include "../net/fleet_match.h"
#include "../net/fleet_match_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE 64
#define PEER_NAME "SIM OPPONENT"
#define PEER_REPLY "Copy that."

struct packet {
    int64_t at;
    uint8_t len;
    uint8_t b[FLEET_PROTO_MAX];
};

struct queue {
    struct packet p[QUEUE];
    int n;
};

struct loop {
    struct fleet_link link;
    struct fleet_link_loop_cfg cfg;
    uint8_t self[FLEET_KEY_BYTES];
    uint8_t peer_key[FLEET_KEY_BYTES];
    struct fleet_match peer;
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    struct fleet_rng rng;
    struct queue to_self;
    struct queue to_peer;
    int64_t now;
    int64_t move_at;
    int seen;
    int64_t idle_since;
    int64_t done_since;
    int64_t opened;
    int cut;
    unsigned answered;      /* lines of ours the opponent has answered */
    int64_t answer_at;
    /* The list peers() gives, top first: 0 the opponent, 1..crowd the
     * bystanders, and when each bystander was last heard. */
    int order[1 + FLEET_LINK_LOOP_CROWD_MAX];
    int64_t heard[1 + FLEET_LINK_LOOP_CROWD_MAX];
};

void fleet_link_loop_defaults(struct fleet_link_loop_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->delay_ms = 400;
    cfg->think_ms = 2500;
    cfg->difficulty = FLEET_OFFICER;
    cfg->seed = 20260925u;
}

static int word(const char **p, const char *w)
{
    size_t n = strlen(w);

    if (strncmp(*p, w, n) == 0) {
        *p += n;
        return 1;
    }
    return 0;
}

int fleet_link_loop_parse(const char *spec, struct fleet_link_loop_cfg *cfg)
{
    const char *p = spec;

    while (p && *p) {
        if (word(&p, "loss=")) {
            cfg->loss_pct = atoi(p);
        } else if (word(&p, "dup=")) {
            cfg->dup_pct = atoi(p);
        } else if (word(&p, "delay=")) {
            cfg->delay_ms = atoi(p);
        } else if (word(&p, "invite=")) {
            cfg->invite_after_ms = atoi(p);
        } else if (word(&p, "think=")) {
            cfg->think_ms = atoi(p);
        } else if (word(&p, "level=")) {
            cfg->difficulty = atoi(p) % FLEET_DIFFICULTY_COUNT;
        } else if (word(&p, "seed=")) {
            cfg->seed = (uint32_t)strtoul(p, NULL, 10);
        } else if (word(&p, "decline")) {
            cfg->decline = 1;
        } else if (word(&p, "silent")) {
            cfg->silent = 1;
        } else if (word(&p, "cut=")) {
            cfg->cut_after_ms = atoi(p);
        } else if (word(&p, "chat")) {
            cfg->chat = 1;
        } else if (word(&p, "crowd=")) {
            cfg->crowd = atoi(p);
        }
        p = strchr(p, ',');
        if (p) {
            p++;
        }
    }
    if (cfg->loss_pct < 0 || cfg->loss_pct > 90) {
        cfg->loss_pct = 0;
    }
    if (cfg->delay_ms < 0) {
        cfg->delay_ms = 0;
    }
    if (cfg->crowd < 0 || cfg->crowd > FLEET_LINK_LOOP_CROWD_MAX) {
        cfg->crowd = 0;
    }
    return 0;
}

static struct loop *of(void *ctx)
{
    return ctx;
}

static void push(struct loop *l, struct queue *q, const uint8_t *b, size_t n)
{
    int copies = 1;
    int c;

    if (l->cfg.silent && q == &l->to_self) {
        return;
    }
    if (!l->opened) {
        l->opened = l->now ? l->now : 1;
    }
    if (l->cut || (l->cfg.cut_after_ms > 0 && l->now - l->opened >= l->cfg.cut_after_ms)) {
        return;
    }
    if ((int)fleet_rng_below(&l->rng, 100) < l->cfg.loss_pct) {
        return;
    }
    if ((int)fleet_rng_below(&l->rng, 100) < l->cfg.dup_pct) {
        copies = 2;
    }
    for (c = 0; c < copies && q->n < QUEUE; c++) {
        struct packet *p = &q->p[q->n++];

        p->at = l->now + l->cfg.delay_ms +
                (l->cfg.delay_ms ? fleet_rng_below(&l->rng, (uint32_t)l->cfg.delay_ms) : 0);
        p->len = (uint8_t)n;
        memcpy(p->b, b, n);
    }
}

/* Take the first packet due, in order of arrival. */
static int take(struct queue *q, int64_t now, uint8_t *b, size_t *n)
{
    int best = -1;
    int i;

    for (i = 0; i < q->n; i++) {
        if (q->p[i].at <= now && (best < 0 || q->p[i].at < q->p[best].at)) {
            best = i;
        }
    }
    if (best < 0) {
        return 0;
    }
    memcpy(b, q->p[best].b, q->p[best].len);
    *n = q->p[best].len;
    q->p[best] = q->p[--q->n];
    return 1;
}

static void peer_save(struct loop *l)
{
    if (fleet_match_dirty(&l->peer) &&
        fleet_match_save_encode(&l->peer, l->blob, sizeof(l->blob)) > 0) {
        fleet_match_saved(&l->peer);
    }
}

static void peer_flush(struct loop *l)
{
    struct fleet_match_out o;

    peer_save(l);
    while (fleet_match_pop(&l->peer, &o)) {
        push(l, &l->to_self, o.bytes, o.len);
    }
}

static int peer_choose(struct loop *l, int *row, int *col)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int k;

    fleet_ai_init(&ai, (enum fleet_difficulty)l->cfg.difficulty);
    for (k = 1; k <= l->peer.resolved; k++) {
        uint8_t res = l->peer.log_res[k];

        if (fleet_match_shooter(k) == l->peer.role) {
            fleet_ai_observe(&ai, l->peer.log_cell[k] / FLEET_GRID,
                             l->peer.log_cell[k] % FLEET_GRID,
                             (enum fleet_shot_result)fleet_res_outcome(res), fleet_res_ship(res));
        }
    }
    fleet_rng_seed(&rng, l->cfg.seed * 2654435761u + l->peer.resolved * 40503u + 1);
    return fleet_ai_next_shot(&ai, &rng, row, col);
}

static void peer_play(struct loop *l)
{
    struct fleet_match *m = &l->peer;
    int key = m->phase * 1000 + m->resolved + (fleet_match_my_turn(m) ? 500 : 0);
    int row;
    int col;

    /* A line of ours is answered once, a moment later. */
    if (l->cfg.chat && m->stats.chat_rx > l->answered) {
        if (!l->answer_at) {
            l->answer_at = l->now + l->cfg.think_ms;
        } else if (l->now >= l->answer_at) {
            fleet_match_chat_send(m, PEER_REPLY, l->now);
            l->answered = m->stats.chat_rx;
            l->answer_at = 0;
        }
    }
    if (key != l->seen) {
        l->seen = key;
        l->move_at = l->now + l->cfg.think_ms +
                     (l->cfg.think_ms ? fleet_rng_below(&l->rng, (uint32_t)l->cfg.think_ms) : 0);
    }
    if (m->phase == FLEET_MP_IDLE) {
        if (!l->idle_since) {
            l->idle_since = l->now;
        }
        if (l->cfg.invite_after_ms > 0 && l->now - l->idle_since >= l->cfg.invite_after_ms) {
            fleet_match_invite(m, l->self, PEER_NAME, fleet_rng_next(&l->rng), l->now);
            l->idle_since = 0;
        }
        return;
    }
    l->idle_since = 0;
    if (m->phase == FLEET_MP_DONE) {
        if (!l->done_since) {
            l->done_since = l->now;
        }
        if (l->now - l->done_since > 30000) {
            fleet_match_dismiss(m);
            l->done_since = 0;
        }
        return;
    }
    l->done_since = 0;
    if (l->now < l->move_at) {
        return;
    }
    if (m->phase == FLEET_MP_INVITED) {
        if (l->cfg.decline) {
            fleet_match_decline(m, l->now);
        } else {
            fleet_match_accept(m, l->now);
        }
    } else if (m->phase == FLEET_MP_DEPLOY && !m->committed) {
        struct fleet_board b;
        struct fleet_rng r;
        uint8_t salt[FLEET_SALT_BYTES];
        int i;

        fleet_board_clear(&b);
        fleet_rng_seed(&r, l->cfg.seed ^ m->sid);
        fleet_board_autoplace(&b, &r);
        for (i = 0; i < FLEET_SALT_BYTES; i++) {
            salt[i] = (uint8_t)fleet_rng_next(&r);
        }
        fleet_match_deploy(m, &b, salt, l->now);
    } else if (fleet_match_my_turn(m) && peer_choose(l, &row, &col) == 0) {
        fleet_match_fire(m, row, col, l->now);
    }
}

static void loop_poll(void *ctx, int64_t now)
{
    struct loop *l = of(ctx);
    uint8_t b[FLEET_PROTO_MAX];
    size_t n;

    l->now = now;
    while (take(&l->to_peer, now, b, &n)) {
        fleet_match_receive(&l->peer, l->self, b, n, now);
        peer_flush(l);
    }
    fleet_match_tick(&l->peer, now);
    peer_play(l);
    peer_flush(l);
}

static enum fleet_link_send loop_send(void *ctx, const uint8_t to[FLEET_KEY_BYTES],
                                      const uint8_t *buf, size_t n)
{
    struct loop *l = of(ctx);

    if (memcmp(to, l->peer_key, FLEET_KEY_BYTES) != 0) {
        return FLEET_LINK_SENT;     /* nobody else is out there */
    }
    push(l, &l->to_peer, buf, n);
    return FLEET_LINK_SENT;
}

static int loop_recv(void *ctx, uint8_t from[FLEET_KEY_BYTES], uint8_t *buf, size_t *n)
{
    struct loop *l = of(ctx);

    if (!take(&l->to_self, l->now, buf, n)) {
        return 0;
    }
    memcpy(from, l->peer_key, FLEET_KEY_BYTES);
    return 1;
}

static int loop_self_key(void *ctx, uint8_t key[FLEET_KEY_BYTES])
{
    memcpy(key, of(ctx)->self, FLEET_KEY_BYTES);
    return 0;
}

static enum fleet_link_state loop_state(void *ctx)
{
    (void)ctx;
    return FLEET_LINK_UP;
}

/* Bystander n's key: first byte n, so none is the opponent's or ours. */
static void bystander_key(int n, uint8_t key[FLEET_KEY_BYTES])
{
    int i;

    for (i = 0; i < FLEET_KEY_BYTES; i++) {
        key[i] = (uint8_t)(0x60 + i);
    }
    key[0] = (uint8_t)n;
}

static int loop_peers(void *ctx, struct fleet_link_peer *out, int max)
{
    struct loop *l = of(ctx);
    int count = 0;
    int who;
    int k;

    for (k = 0; k <= l->cfg.crowd && count < max; k++, count++) {
        who = l->order[k];
        memset(&out[count], 0, sizeof(out[count]));
        if (who == 0) {
            memcpy(out[count].key, l->peer_key, FLEET_KEY_BYTES);
            strncpy(out[count].name, PEER_NAME, sizeof(out[count].name) - 1);
            out[count].heard_ms = l->now;
            out[count].hops = 0;
        } else {
            bystander_key(who, out[count].key);
            snprintf(out[count].name, sizeof(out[count].name), "BYSTANDER %d", who);
            out[count].heard_ms = l->heard[who];
            out[count].hops = 1;
        }
    }
    return count;
}

static const char *loop_peer_name(void *ctx, const uint8_t key[FLEET_KEY_BYTES])
{
    return memcmp(key, of(ctx)->peer_key, FLEET_KEY_BYTES) == 0 ? PEER_NAME : NULL;
}

static int loop_advertise(void *ctx)
{
    (void)ctx;
    return 0;
}

static uint32_t loop_retry_base(void *ctx)
{
    (void)ctx;
    return 0;
}

static void loop_close(void *ctx)
{
    free(ctx);
}

static const struct fleet_link_ops LOOP_OPS = {
    loop_poll, loop_send, loop_recv, loop_self_key, loop_state, loop_peers,
    loop_peer_name, loop_advertise, loop_retry_base, loop_close,
};

struct fleet_link *fleet_link_loop_open(const struct fleet_link_loop_cfg *cfg)
{
    struct loop *l = calloc(1, sizeof(*l));
    int i;

    if (!l) {
        return NULL;
    }
    l->cfg = *cfg;
    for (i = 0; i < FLEET_KEY_BYTES; i++) {
        /* The local key sorts above the opponent's, so in crossed invites
         * the opponent's survives - the case worth seeing in the lobby. */
        l->self[i] = (uint8_t)(0xD0 + i);
        l->peer_key[i] = (uint8_t)(0x50 + i);
    }
    for (i = 0; i <= FLEET_LINK_LOOP_CROWD_MAX; i++) {
        l->order[i] = i;
    }
    fleet_rng_seed(&l->rng, cfg->seed ^ 0x100F);
    fleet_match_init(&l->peer, l->peer_key, cfg->seed);
    l->seen = -1;
    l->link.ops = &LOOP_OPS;
    l->link.ctx = l;
    return &l->link;
}

const struct fleet_match *fleet_link_loop_peer(const struct fleet_link *link)
{
    return link ? &((const struct loop *)link->ctx)->peer : NULL;
}

int fleet_link_loop_say(struct fleet_link *link, const char *text)
{
    struct loop *l = link ? link->ctx : NULL;
    int rc;

    if (!l) {
        return -1;
    }
    rc = fleet_match_chat_send(&l->peer, text, l->now);
    peer_flush(l);
    return rc;
}

void fleet_link_loop_hear(struct fleet_link *link, int who)
{
    struct loop *l = link ? link->ctx : NULL;
    int k = 0;

    if (!l || who < 0 || who > l->cfg.crowd) {
        return;
    }
    while (l->order[k] != who) {
        k++;
    }
    for (; k > 0; k--) {
        l->order[k] = l->order[k - 1];
    }
    l->order[0] = who;
    l->heard[who] = l->now;
}

void fleet_link_loop_set_cut(struct fleet_link *link, int cut)
{
    if (link) {
        ((struct loop *)link->ctx)->cut = cut;
    }
}
