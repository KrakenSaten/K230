/*
 * pocketpcap synthetic source: a deterministic traffic generator at a chosen
 * rate, for the benchmark, the simulator and the UI and lifecycle tests. It
 * never touches a network: frames are built in memory.
 *
 * Every address is a documentation address (192.0.2.0/24, 198.51.100.0/24,
 * 2001:db8::/32) or a locally administered MAC (02:...), and every name is
 * under example.net, so nothing it produces can be mistaken for real traffic.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "ppc_source.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define TICK_NS 10000000L
#define SYNTH_PPS_MAX 1000000u

struct synth_src {
    int fd;
    uint32_t pps;
    uint32_t seed;
    uint64_t crash_after;  /* 0: never */
    uint64_t step_after;   /* 0: never */
    int64_t step_ns;
    uint64_t start_mono;
    uint64_t start_wall;
    uint64_t made;
    uint8_t buf[1600];
};

static uint64_t clock_ns(clockid_t id)
{
    struct timespec t;

    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- frames ----------------------------------------------------------------------- */

static const uint8_t mac_self[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };
static const uint8_t mac_peer[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x02 };
static const uint8_t mac_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t mac_lldp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };
static const uint8_t mac_v6mc[6] = { 0x33, 0x33, 0xff, 0x00, 0x00, 0x02 };

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t eth(uint8_t *b, const uint8_t *dst, const uint8_t *src, uint16_t et)
{
    memcpy(b, dst, 6);
    memcpy(b + 6, src, 6);
    put16(b + 12, et);
    return 14;
}

static uint32_t ip4(uint8_t *b, uint8_t proto, uint32_t l4len, uint8_t s, uint8_t d, int out)
{
    memset(b, 0, 20);
    b[0] = 0x45;
    put16(b + 2, 20 + l4len);
    b[8] = 64;
    b[9] = proto;
    b[12] = 192;
    b[14] = 2;
    b[15] = out ? s : d;
    b[16] = 192;
    b[18] = 2;
    b[19] = out ? d : s;
    return 20;
}

static uint32_t udp(uint8_t *b, uint16_t sp, uint16_t dp, uint32_t paylen)
{
    put16(b, sp);
    put16(b + 2, dp);
    put16(b + 4, 8 + paylen);
    put16(b + 6, 0);
    return 8;
}

/* example.net, www.example.net or api.example.net, type A. */
static uint32_t dns(uint8_t *b, uint16_t id, int response, unsigned which)
{
    static const char *const names[] = { "\3www\7example\3net", "\3api\7example\3net",
                                         "\7example\3net" };
    const char *name = names[which % 3];
    uint32_t n = 12;
    size_t nl = strlen(name) + 1;

    memset(b, 0, 12);
    put16(b, id);
    put16(b + 2, response ? 0x8180 : 0x0100);
    put16(b + 4, 1);
    put16(b + 6, response ? 1 : 0);
    memcpy(b + n, name, nl);
    n += (uint32_t)nl;
    put16(b + n, 1);
    put16(b + n + 2, 1);
    n += 4;
    if (response) {
        put16(b + n, 0xc00c);
        put16(b + n + 2, 1);
        put16(b + n + 4, 1);
        put32(b + n + 6, 300);
        put16(b + n + 10, 4);
        b[n + 12] = 198;
        b[n + 13] = 51;
        b[n + 14] = 100;
        b[n + 15] = (uint8_t)(10 + which % 3);
        n += 16;
    }
    return n;
}

static uint32_t mix(uint64_t n, uint32_t seed)
{
    uint64_t x = n * 0x9e3779b97f4a7c15ull + seed;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x;
}

/* One TCP segment between 192.0.2.10 and 192.0.2.<peer>. */
static uint32_t tcp_seg(uint8_t *b, int out, uint8_t peer, uint16_t sp, uint16_t dp, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint32_t pay)
{
    uint32_t l = eth(b, out ? mac_peer : mac_self, out ? mac_self : mac_peer, 0x0800);

    l += ip4(b + l, 6, 20 + pay, 10, peer, out);
    put16(b + l, sp);
    put16(b + l + 2, dp);
    put32(b + l + 4, seq);
    put32(b + l + 8, ack);
    b[l + 12] = 0x50;
    b[l + 13] = flags;
    put16(b + l + 14, 65535);
    put32(b + l + 16, 0);
    l += 20;
    memset(b + l, 'x', pay);
    return l + pay;
}

/* About one frame in a hundred is something PacketShark marks as a finding
 * (M7), so the simulator and the tests have every kind to show: a failed
 * lookup, a reset, a segment sent again, an unreachable, and a SYN nobody
 * answers. Returns 0 for the other frames. */
static uint32_t finding_frame(uint64_t n, uint32_t r, uint8_t *b)
{
    uint32_t l;

    if ((r >> 16) % 97u != 0) {
        return 0;
    }
    switch ((r >> 24) % 5u) {
    case 0: {
        /* NXDOMAIN from the resolver: the query's name, no answer. */
        uint8_t m[128];
        uint32_t dl = dns(m, (uint16_t)(n & 0xffffu), 0, r >> 4);

        put16(m + 2, 0x8183);
        l = eth(b, mac_self, mac_peer, 0x0800);
        l += ip4(b + l, 17, 8 + dl, 1, 53, 0);
        l += udp(b + l, 53, 50000 + (r & 255u), dl);
        memcpy(b + l, m, dl);
        return l + dl;
    }
    case 1:
        return tcp_seg(b, 0, 21, 443, (uint16_t)(49200 + (r & 7u)), (uint32_t)n, 0, 0x14, 0);
    case 2:
        /* The same hundred bytes again and again: every one after the
         * first is a retransmission. */
        return tcp_seg(b, 1, 22, 50001, 443, 1000, 1, 0x18, 100);
    case 3:
        /* Destination unreachable (port), quoting nothing more than zeros. */
        l = eth(b, mac_self, mac_peer, 0x0800);
        l += ip4(b + l, 1, 8 + 28, 10, 1, 0);
        memset(b + l, 0, 36);
        b[l] = 3;
        b[l + 1] = 3;
        return l + 36;
    default:
        /* A SYN to a host that never answers; a new port most times. */
        return tcp_seg(b, 1, 99, (uint16_t)(51000 + ((r >> 4) & 1023u)), 443, (uint32_t)(n * 2654435761u), 0, 0x02, 0);
    }
}

uint32_t ppc_synth_frame(uint64_t n, uint32_t seed, uint8_t *b)
{
    uint32_t r = mix(n, seed);
    unsigned kind = r % 16u;
    int out = (r >> 8) & 1u;
    const uint8_t *src = out ? mac_self : mac_peer;
    const uint8_t *dst = out ? mac_peer : mac_self;
    uint32_t len = finding_frame(n, r, b);

    if (len) {
        return len;
    }
    switch (kind) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5: {
        /* TCP: a bulk segment, or an ACK. */
        uint32_t pay = (r >> 9) & 1u ? 1460u : 0u;
        uint32_t l = eth(b, dst, src, 0x0800);

        l += ip4(b + l, 6, 20 + pay, 10, 20, out);
        put16(b + l, out ? 49152 + (r & 7u) : 443);
        put16(b + l + 2, out ? 443 : 49152 + (r & 7u));
        put32(b + l + 4, (uint32_t)(n * 1460u));
        put32(b + l + 8, (uint32_t)(n * 7u));
        b[l + 12] = 0x50;
        b[l + 13] = pay ? 0x18 : 0x10;
        put16(b + l + 14, 65535);
        put32(b + l + 16, 0);
        l += 20;
        memset(b + l, 'x', pay);
        len = l + pay;
        break;
    }
    case 6:
    case 7:
    case 8: {
        uint32_t pay = 64u + (r >> 12) % 400u;
        uint32_t l = eth(b, dst, src, 0x0800);

        l += ip4(b + l, 17, 8 + pay, 30, 40, out);
        l += udp(b + l, 40000 + (r & 15u), 4500, pay);
        memset(b + l, 'u', pay);
        len = l + pay;
        break;
    }
    case 9:
    case 10: {
        uint8_t m[128];
        int resp = kind == 10;
        uint32_t dl = dns(m, (uint16_t)(n & 0xffffu), resp, r >> 4);
        uint32_t l = eth(b, resp ? mac_self : mac_peer, resp ? mac_peer : mac_self, 0x0800);

        /* client 192.0.2.1, resolver 192.0.2.53 */
        l += ip4(b + l, 17, 8 + dl, 1, 53, !resp);
        l += udp(b + l, resp ? 53 : 50000 + (r & 255u), resp ? 50000 + (r & 255u) : 53, dl);
        memcpy(b + l, m, dl);
        len = l + dl;
        break;
    }
    case 11: {
        /* DHCP Discover from a client that has no address yet. */
        uint32_t l = eth(b, mac_bcast, mac_peer, 0x0800);
        uint32_t ipo = l;
        uint32_t pay = 240 + 4;

        l += ip4(b + l, 17, 8 + pay, 0, 0, 0);
        memset(b + ipo + 12, 0, 4);
        memset(b + ipo + 16, 0xff, 4);
        l += udp(b + l, 68, 67, pay);
        memset(b + l, 0, pay);
        b[l] = 1;
        b[l + 1] = 1;
        b[l + 2] = 6;
        put32(b + l + 4, 0x5e100000u | (r & 0xffffu));
        memcpy(b + l + 28, mac_peer, 6);
        put32(b + l + 236, 0x63825363u);
        b[l + 240] = 53;
        b[l + 241] = 1;
        b[l + 242] = 1;
        b[l + 243] = 255;
        len = l + pay;
        break;
    }
    case 12: {
        uint32_t l = eth(b, mac_bcast, mac_peer, 0x0806);

        put16(b + l, 1);
        put16(b + l + 2, 0x0800);
        b[l + 4] = 6;
        b[l + 5] = 4;
        put16(b + l + 6, 1);
        memcpy(b + l + 8, mac_peer, 6);
        b[l + 14] = 192;
        b[l + 16] = 2;
        b[l + 17] = 20;
        memset(b + l + 18, 0, 6);
        b[l + 24] = 192;
        b[l + 26] = 2;
        b[l + 27] = (uint8_t)(1 + r % 30u);
        len = l + 28;
        break;
    }
    case 13: {
        uint32_t l = eth(b, dst, src, 0x0800);

        l += ip4(b + l, 1, 8 + 56, 10, 1, out);
        b[l] = out ? 8 : 0;
        b[l + 1] = 0;
        put16(b + l + 2, 0);
        put16(b + l + 4, 0x5e10);
        put16(b + l + 6, (uint16_t)n);
        memset(b + l + 8, 'p', 56);
        len = l + 64;
        break;
    }
    case 14: {
        /* ICMPv6 neighbour solicitation for 2001:db8::2. */
        uint32_t l = eth(b, mac_v6mc, mac_self, 0x86dd);
        uint8_t *ip = b + l;

        memset(ip, 0, 40);
        ip[0] = 0x60;
        put16(ip + 4, 32);
        ip[6] = 58;
        ip[7] = 255;
        ip[8] = 0x20;
        ip[9] = 0x01;
        ip[10] = 0x0d;
        ip[11] = 0xb8;
        ip[23] = 1;
        ip[24] = 0xff;
        ip[25] = 0x02;
        ip[35] = 1;
        ip[36] = 0xff;
        ip[39] = 2;
        l += 40;
        memset(b + l, 0, 32);
        b[l] = 135;
        b[l + 8] = 0x20;
        b[l + 9] = 0x01;
        b[l + 10] = 0x0d;
        b[l + 11] = 0xb8;
        b[l + 23] = 2;
        b[l + 24] = 1;
        b[l + 25] = 1;
        memcpy(b + l + 26, mac_self, 6);
        len = l + 32;
        break;
    }
    default: {
        /* LLDP from a switch: an ethertype the decoder only names. */
        uint32_t l = eth(b, mac_lldp, mac_peer, 0x88cc);

        memset(b + l, 0, 46);
        len = l + 46;
        break;
    }
    }
    if (len < 60) {
        memset(b + len, 0, 60 - len);
        len = 60;
    }
    return len;
}

/* ---- the source -------------------------------------------------------------------- */

static int synth_fd(struct ppc_source *s)
{
    return ((struct synth_src *)s->impl)->fd;
}

static uint64_t synth_wall(struct ppc_source *s)
{
    struct synth_src *g = s->impl;
    uint64_t w = clock_ns(CLOCK_REALTIME);

    if (g->step_after && g->made >= g->step_after) {
        w = (uint64_t)((int64_t)w + g->step_ns);
    }
    return w;
}

static int synth_dispatch(struct ppc_source *s, int max, ppc_frame_fn fn, void *ctx)
{
    struct synth_src *g = s->impl;
    uint64_t tick;
    uint64_t due = (clock_ns(CLOCK_MONOTONIC) - g->start_mono) * g->pps / 1000000000u;
    int n = 0;

    if (read(g->fd, &tick, sizeof(tick)) < 0 && errno != EAGAIN) {
        /* the next tick reads it */
    }
    while (g->made < due && n < max) {
        struct ppc_frame f;
        uint64_t ts = g->start_wall + g->made * 1000000000u / g->pps;

        if (g->crash_after && g->made >= g->crash_after) {
            /* The test asked for a crash: a real one, so the helper's crash
             * handler and the shell's view of a signal are both exercised. */
            raise(SIGSEGV);
        }
        if (g->step_after && g->made >= g->step_after) {
            ts = (uint64_t)((int64_t)ts + g->step_ns);
        }
        f.data = g->buf;
        f.cap_len = ppc_synth_frame(g->made, g->seed, g->buf);
        f.wire_len = f.cap_len;
        f.ts_ns = ts;
        f.pkttype = PPC_PKT_UNKNOWN;
        if (memcmp(g->buf + 6, mac_self, 6) == 0) {
            f.pkttype = PPC_PKT_OUTGOING;
        }
        g->made++;
        fn(ctx, &f);
        n++;
    }
    return n;
}

static int synth_stats(struct ppc_source *s, uint64_t *kdrop, uint64_t *ifdrop)
{
    (void)s;
    *kdrop = 0;
    *ifdrop = 0;
    return 0;
}

static void synth_close(struct ppc_source *s)
{
    struct synth_src *g = s->impl;

    if (g) {
        if (g->fd >= 0) {
            close(g->fd);
        }
        free(g);
    }
}

static const struct ppc_source_ops synth_ops = {
    .name = "synth",
    .fd = synth_fd,
    .dispatch = synth_dispatch,
    .stats = synth_stats,
    .wall_ns = synth_wall,
    .close = synth_close,
};

/* synth:<pps>[:<seed>][:crash=<n>][:step=<n>,<s>] */
enum ppc_open_err ppc_source_open_synth(struct ppc_source *s, const char *arg)
{
    char buf[128];
    char *save = NULL;
    char *tok;
    struct synth_src *g;
    struct itimerspec it = { { 0, TICK_NS }, { 0, TICK_NS } };
    int i = 0;

    if (strlen(arg) >= sizeof(buf)) {
        snprintf(s->err, sizeof(s->err), "synth: arguments too long");
        return PPC_OPEN_BADARG;
    }
    g = calloc(1, sizeof(*g));
    if (!g) {
        snprintf(s->err, sizeof(s->err), "out of memory");
        return PPC_OPEN_NOMEM;
    }
    g->fd = -1;
    g->pps = 100;
    g->seed = 1;
    snprintf(buf, sizeof(buf), "%s", arg);
    for (tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save), i++) {
        char *end = NULL;

        if (strncmp(tok, "crash=", 6) == 0) {
            g->crash_after = strtoull(tok + 6, &end, 10);
        } else if (strncmp(tok, "step=", 5) == 0) {
            g->step_after = strtoull(tok + 5, &end, 10);
            if (end && *end == ',') {
                g->step_ns = strtoll(end + 1, &end, 10) * 1000000000LL;
            } else {
                end = tok; /* refused below */
            }
        } else if (i == 0) {
            g->pps = (uint32_t)strtoul(tok, &end, 10);
        } else if (i == 1) {
            g->seed = (uint32_t)strtoul(tok, &end, 10);
        } else {
            end = tok;
        }
        if (!end || *end) {
            snprintf(s->err, sizeof(s->err), "synth: cannot read '%s'", tok);
            free(g);
            return PPC_OPEN_BADARG;
        }
    }
    if (g->pps == 0 || g->pps > SYNTH_PPS_MAX) {
        snprintf(s->err, sizeof(s->err), "synth: rate must be 1..%u", SYNTH_PPS_MAX);
        free(g);
        return PPC_OPEN_BADARG;
    }
    g->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g->fd < 0) {
        snprintf(s->err, sizeof(s->err), "timer: %s", strerror(errno));
        free(g);
        return PPC_OPEN_OTHER;
    }
    timerfd_settime(g->fd, 0, &it, NULL);
    g->start_mono = clock_ns(CLOCK_MONOTONIC);
    g->start_wall = clock_ns(CLOCK_REALTIME);
    s->ops = &synth_ops;
    s->impl = g;
    s->live = 0;
    return PPC_OPEN_OK;
}
