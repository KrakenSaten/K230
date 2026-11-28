/*
 * The deterministic replay fixtures of the intelligence engine (N1.1;
 * docs/apps/packetshark/neural/N1.1.md): scripted scenes on a virtual
 * clock, each four and a half minutes of a capture with a mark at 200 s.
 *
 *   slow-dns         this device's DNS answers slow from 30 to about 420 ms
 *                    at the mark
 *   one-sided        from 140 s every reply to this device takes another
 *                    path: its questions are seen, nothing comes back
 *   hold-recovery    the resolver stops answering at 170 s; HOLD from 205 to
 *                    225 s; it answers again at 215 s, while nobody watched
 *   loss-healthy     a quarter of all frames is lost in the minute after the
 *                    mark, while DNS answers every question; the counters
 *                    cannot be read from 190 to 258 s, so the loss is known
 *                    in total, not where (a lagged loss signal)
 *   fault-under-loss the resolver stops answering at 205 s while a quarter
 *                    of all frames is lost
 *   loss-unknown     slow answers after the mark; the counters cannot be read
 *                    from 250 s to the end; a small buffer lets the packets
 *                    referenced leave it
 *
 * Background in every scene: a keepalive to and from a server every second
 * (this device's own traffic in both directions), a DNS question every
 * second, an ARP request from the router every ten. Frames are built by
 * the N1.0 fixture builders (tests/ppc_evid_fixtures.h), byte for byte the
 * same on every run; documentation addresses and example.net names only.
 * Loss removes frames by a fixed hash of their place, so the same frames
 * are lost on every run, and the counters report exactly those.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_INTEL_FIXTURES_H
#define PPC_INTEL_FIXTURES_H

#include "ppc_evid_fixtures.h"

#include <stdlib.h>

#define IX_COUNT 6u
#define IX_MARK_MS 200000u
#define IX_STOP_MS 280000u
#define IX_SERVER 0x50u /* a server this device keeps a conversation with, 192.0.2.80 */
#define IX_READ_MS 250u /* the helper reads its counters every 250 ms */

enum ix_op { IX_FRAME = 1, IX_MARK, IX_HOLD_ON, IX_HOLD_OFF, IX_STOP };

struct ix_step {
    uint32_t t_ms;
    uint32_t order;   /* place of creation: ties in time keep it */
    uint8_t op;
    uint8_t dir;
    uint8_t lost;     /* the kernel dropped it: never seen, counted lost */
    uint16_t len;
    uint8_t frame[FX_FRAME_MAX];
};

struct ix {
    const char *name;
    const char *what;
    uint32_t n, cap;
    struct ix_step *step;
    uint32_t fail_from, fail_to;  /* counters cannot be read in [from, to); 0, 0: always can */
    uint32_t store_summaries;     /* 0: the helper's own sizes */
    uint64_t store_bytes;
};

/* The scene's plan, from which its steps are made. */
struct ix_plan {
    uint32_t slow_from;  /* answers slow from here; 0: never */
    uint32_t down_from, down_to; /* the resolver does not answer in [from, to) */
    uint32_t oneway_from;        /* nothing reaches this device from here; 0: never */
    uint32_t hold_on, hold_off;  /* HOLD; 0, 0: none */
    uint32_t loss_from, loss_to, loss_permille;
};

static inline struct ix_step *ix_add(struct ix *x, uint32_t t_ms, unsigned op)
{
    struct ix_step *s;

    if (x->n == x->cap) {
        x->cap = x->cap ? x->cap * 2u : 1024u;
        x->step = realloc(x->step, x->cap * sizeof(*x->step));
        if (!x->step) {
            abort();
        }
    }
    s = &x->step[x->n];
    memset(s, 0, sizeof(*s));
    s->t_ms = t_ms;
    s->order = x->n++;
    s->op = (uint8_t)op;
    return s;
}

/* Copy the one frame an N1.0 builder put in a scratch fixture. */
static inline void ix_take(struct ix *x, const struct fx *scratch)
{
    const struct fx_step *f = &scratch->step[0];
    struct ix_step *s = ix_add(x, f->t_ms, IX_FRAME);

    s->dir = f->dir;
    s->len = f->len;
    memcpy(s->frame, f->frame, sizeof(s->frame));
}

static inline void ix_dns(struct ix *x, uint32_t t_ms, int answer, uint32_t id, unsigned rcode)
{
    static struct fx scratch;

    memset(&scratch, 0, sizeof(scratch));
    fx_dns(&scratch, t_ms, answer, FX_SELF, id, rcode, "www.example.net");
    ix_take(x, &scratch);
}

static inline void ix_arp(struct ix *x, uint32_t t_ms, unsigned tip)
{
    static struct fx scratch;

    memset(&scratch, 0, sizeof(scratch));
    fx_arp(&scratch, t_ms, 1, FX_ROUTER, 1, tip, 0);
    ix_take(x, &scratch);
}

/* A keepalive over UDP (ports 40001 and 7000, neither one the decoder
 * names) between this device and the server, in = to this device. */
static inline void ix_keepalive(struct ix *x, uint32_t t_ms, int in)
{
    struct ix_step *s = ix_add(x, t_ms, IX_FRAME);
    uint8_t *b = s->frame;

    fx_mac(b, in ? FX_SELF : IX_SERVER);
    fx_mac(b + 6, in ? IX_SERVER : FX_SELF);
    fx_put16(b + 12, 0x0800);
    b[14] = 0x45;
    b[22] = 64;
    b[23] = 17;
    fx_ip(b + 26, in ? IX_SERVER : FX_SELF);
    fx_ip(b + 30, in ? FX_SELF : IX_SERVER);
    fx_put16(b + 34, in ? 7000u : 40001u);
    fx_put16(b + 36, in ? 40001u : 7000u);
    fx_put16(b + 16, 60 - 14);
    fx_put16(b + 38, 60 - 34);
    s->len = 60;
    s->dir = in ? PPC_PKT_HOST : PPC_PKT_OUTGOING;
}

static inline uint32_t ix_hash(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

static inline int ix_cmp(const void *a, const void *b)
{
    const struct ix_step *x = a;
    const struct ix_step *y = b;

    if (x->t_ms != y->t_ms) {
        return x->t_ms < y->t_ms ? -1 : 1;
    }
    return x->order < y->order ? -1 : x->order > y->order;
}

/* The answer time of question k: about 30 ms, or about 420 ms once slow. */
static inline uint32_t ix_rtt(const struct ix_plan *p, uint32_t k, uint32_t t_ms)
{
    if (p->slow_from && t_ms >= p->slow_from) {
        return 360u + (k * 53u) % 121u;
    }
    return 22u + (k * 37u) % 17u;
}

static inline void ix_make(struct ix *x, const struct ix_plan *p)
{
    uint32_t t;
    uint32_t k;
    uint32_t i;

    for (t = 1000; t < IX_STOP_MS; t += 1000) {
        int reach = !p->oneway_from || t + 100u < p->oneway_from;

        if (reach) {
            ix_keepalive(x, t + 100u, 1);
        }
        ix_keepalive(x, t + 600u, 0);
    }
    for (k = 1; k * 1000u + 300u < IX_STOP_MS; k++) {
        uint32_t q = k * 1000u + 300u;
        uint32_t a = q + ix_rtt(p, k, q);
        int down = q >= p->down_from && q < p->down_to;
        int reach = !p->oneway_from || a < p->oneway_from;

        ix_dns(x, q, 0, 0x1000u + k, 0);
        if (!down && reach && a < IX_STOP_MS) {
            ix_dns(x, a, 1, 0x1000u + k, 0);
        }
    }
    for (t = 10000; t < IX_STOP_MS; t += 10000) {
        ix_arp(x, t + 700u, 0x20u + (t / 10000u) % 8u);
    }
    ix_add(x, IX_MARK_MS, IX_MARK);
    if (p->hold_on) {
        ix_add(x, p->hold_on, IX_HOLD_ON);
        ix_add(x, p->hold_off, IX_HOLD_OFF);
    }
    ix_add(x, IX_STOP_MS, IX_STOP);
    qsort(x->step, x->n, sizeof(*x->step), ix_cmp);
    for (i = 0; i < x->n; i++) {
        struct ix_step *s = &x->step[i];

        if (s->op == IX_FRAME && s->t_ms >= p->loss_from && s->t_ms < p->loss_to &&
            ix_hash(i * 2654435761u + 17u) % 1000u < p->loss_permille) {
            s->lost = 1;
        }
    }
}

static inline void ix_build(unsigned i, struct ix *x)
{
    struct ix_plan p;

    memset(x, 0, sizeof(*x));
    memset(&p, 0, sizeof(p));
    switch (i) {
    case 0:
        x->name = "slow-dns";
        x->what = "this device's DNS answers slow from about 30 to about 420 ms at the mark";
        p.slow_from = IX_MARK_MS;
        break;
    case 1:
        x->name = "one-sided";
        x->what = "from 140 s every reply to this device takes another path";
        p.oneway_from = 140000u;
        break;
    case 2:
        x->name = "hold-recovery";
        x->what = "the resolver stops at 170 s and is back at 215 s, inside a HOLD from 205 to 225 s";
        p.down_from = 170000u;
        p.down_to = 215000u;
        p.hold_on = 205000u;
        p.hold_off = 225000u;
        break;
    case 3:
        x->name = "loss-healthy";
        x->what = "a quarter of all frames lost after the mark, DNS answering; counters unreadable 190-258 s";
        x->fail_from = 190000u;
        x->fail_to = 258000u;
        p.loss_from = IX_MARK_MS;
        p.loss_to = IX_MARK_MS + 60000u;
        p.loss_permille = 250u;
        break;
    case 4:
        x->name = "fault-under-loss";
        x->what = "the resolver stops at 205 s while a quarter of all frames is lost";
        p.down_from = 205000u;
        p.down_to = IX_STOP_MS;
        p.loss_from = IX_MARK_MS;
        p.loss_to = IX_MARK_MS + 60000u;
        p.loss_permille = 250u;
        break;
    default:
        x->name = "loss-unknown";
        x->what = "slow answers after the mark; counters unreadable from 250 s; a small buffer";
        p.slow_from = IX_MARK_MS;
        x->fail_from = 250000u;
        x->fail_to = IX_STOP_MS + 1u;
        x->store_summaries = 128u;
        x->store_bytes = 8192u;
        break;
    }
    ix_make(x, &p);
}

static inline void ix_free(struct ix *x)
{
    free(x->step);
    memset(x, 0, sizeof(*x));
}

#endif
