/*
 * G0 on the C engine itself: replay windows of the held-out interface
 * (research/packetshark-neural/g0-heldout/INTERFACE.md) into ppc_intel.c,
 * the code the helper would run, and print its five verdicts. Host only,
 * research only; never built into a product.
 *
 * Input (stdin), one block a window, numbers separated by spaces:
 *   W
 *   S <125 watched 0|1>     F <125 frames>      I <125 own_in>
 *   D <125 own_in_dns>      O <125 own_out>     A <125 arp>   H <125 dhcp>
 *   L <500 lost_slot>       R <500 read_ok 0|1>
 *   Q <n>, then n lines: <tq_us> <seen_answer> <failed> <rtt_us>
 *   E
 * Output (stdout), one line a window: five "verdict:reason" words in the
 * engine's metric order (dns.time dns.answered dns.failed dhcp.discover
 * arp.request).
 *
 * The replay, as the helper would feed the engine:
 *   - a counter reading at 0 ms (nothing lost), then one at the end of each
 *     250 ms slot: good with the running total, or failed;
 *   - a tick at each second's start: watched, or HOLD;
 *   - the second's frames spread evenly over it: this device's own out,
 *     its own in, the rest broadcast; ARP requests and DHCP DISCOVERs as
 *     observations spread over the second; a question as an observation at
 *     its time, its answer, when seen, at the question's time plus its
 *     answer time;
 *   - a mark at 60 s, STOP at 125 s, then the "after against before" pair.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_intel.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T 125
#define SLOTS 500
#define QMAX 4096

enum { EV_TICK, EV_READ, EV_FRAME, EV_NOTE };

struct ev {
    uint32_t t_ms;
    uint8_t type;
    uint8_t order; /* within one millisecond: tick, then notes and frames, then the reading */
    uint8_t pkttype;
    uint8_t ok;
    uint64_t lost;
    struct ppc_ev_obs o;
};

static int win_watched[T], win_frames[T], win_in[T], win_dns[T], win_out[T], win_arp[T], win_dhcp[T];
static long win_lost[SLOTS];
static int win_ok[SLOTS];
static struct {
    long tq_us, rtt_us;
    int seen, failed;
} qs[QMAX];
static int nq;

static struct ev *evs;
static size_t nev, cap_ev;

static struct ev *push(uint32_t t_ms, uint8_t type, uint8_t order)
{
    struct ev *e;

    if (nev == cap_ev) {
        cap_ev = cap_ev ? cap_ev * 2 : 65536;
        evs = realloc(evs, cap_ev * sizeof(*evs));
        if (!evs) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    e = &evs[nev++];
    memset(e, 0, sizeof(*e));
    e->t_ms = t_ms;
    e->type = type;
    e->order = order;
    return e;
}

static int cmp_ev(const void *a, const void *b)
{
    const struct ev *x = a;
    const struct ev *y = b;

    if (x->t_ms != y->t_ms) {
        return x->t_ms < y->t_ms ? -1 : 1;
    }
    return (int)x->order - (int)y->order;
}

static int read_ints(char tag, int *v, int n)
{
    char c;
    int i;

    if (scanf(" %c", &c) != 1 || c != tag) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (scanf("%d", &v[i]) != 1) {
            return -1;
        }
    }
    return 0;
}

static int read_window(void)
{
    char c;
    int i;

    if (scanf(" %c", &c) != 1) {
        return 0;
    }
    if (c != 'W' || read_ints('S', win_watched, T) || read_ints('F', win_frames, T) || read_ints('I', win_in, T) ||
        read_ints('D', win_dns, T) || read_ints('O', win_out, T) || read_ints('A', win_arp, T) ||
        read_ints('H', win_dhcp, T)) {
        return -1;
    }
    if (scanf(" %c", &c) != 1 || c != 'L') {
        return -1;
    }
    for (i = 0; i < SLOTS; i++) {
        if (scanf("%ld", &win_lost[i]) != 1) {
            return -1;
        }
    }
    if (read_ints('R', win_ok, SLOTS) || scanf(" %c %d", &c, &nq) != 2 || c != 'Q' || nq < 0 || nq > QMAX) {
        return -1;
    }
    for (i = 0; i < nq; i++) {
        if (scanf("%ld %d %d %ld", &qs[i].tq_us, &qs[i].seen, &qs[i].failed, &qs[i].rtt_us) != 4) {
            return -1;
        }
    }
    if (scanf(" %c", &c) != 1 || c != 'E') {
        return -1;
    }
    return 1;
}

static void obs(uint32_t t_ms, uint8_t kind, uint8_t flags, uint32_t a, uint32_t b)
{
    struct ev *e = push(t_ms, EV_NOTE, 1);

    e->o.t_ms = t_ms;
    e->o.kind = kind;
    e->o.flags = flags;
    e->o.a = a;
    e->o.b = b;
}

static void spread(int s, int n, uint8_t type, uint8_t pkttype, uint8_t kind)
{
    int k;

    for (k = 0; k < n; k++) {
        uint32_t t = (uint32_t)s * 1000u + (uint32_t)((k + 0.5) * 1000.0 / n);

        if (type == EV_FRAME) {
            push(t, EV_FRAME, 1)->pkttype = pkttype;
        } else {
            obs(t, kind, 0, 0, 0);
        }
    }
}

static void build(void)
{
    uint64_t total = 0;
    int s;
    int j;
    int i;

    nev = 0;
    push(0, EV_READ, 0)->ok = 1;
    for (s = 0; s < T; s++) {
        int other = win_frames[s] - win_in[s] - win_out[s];

        push((uint32_t)s * 1000u, EV_TICK, 0)->ok = (uint8_t)win_watched[s];
        if (!win_watched[s]) {
            continue;
        }
        spread(s, win_out[s], EV_FRAME, PPC_PKT_OUTGOING, 0);
        spread(s, win_in[s], EV_FRAME, PPC_PKT_HOST, 0);
        spread(s, other > 0 ? other : 0, EV_FRAME, PPC_PKT_BROADCAST, 0);
        spread(s, win_arp[s], EV_NOTE, 0, PPC_EV_ARP_REQUEST);
        spread(s, win_dhcp[s], EV_NOTE, 0, PPC_EV_DHCP_DISCOVER);
    }
    for (j = 0; j < SLOTS; j++) {
        struct ev *e;

        total += (uint64_t)(win_lost[j] > 0 ? win_lost[j] : 0);
        e = push((uint32_t)(j + 1) * 250u, EV_READ, 2);
        e->ok = (uint8_t)win_ok[j];
        e->lost = total;
    }
    for (i = 0; i < nq; i++) {
        uint32_t tq = (uint32_t)(qs[i].tq_us / 1000);
        uint32_t id = (uint32_t)(i + 1) & 0xffffu;

        obs(tq, PPC_EV_DNS_QUERY, PPC_EVF_OWN, id, 0);
        if (qs[i].seen) {
            uint32_t ta = (uint32_t)((qs[i].tq_us + qs[i].rtt_us) / 1000);

            obs(ta, qs[i].failed ? PPC_EV_DNS_FAILURE : PPC_EV_DNS_ANSWER, PPC_EVF_OWN,
                id | (qs[i].failed ? (2u << 16) : 0u), (uint32_t)qs[i].rtt_us);
        }
    }
    qsort(evs, nev, sizeof(*evs), cmp_ev);
}

static int replay(struct ppc_intel *in)
{
    struct ppc_in_cmp c;
    size_t i;
    unsigned m;
    int marked = 0;

    if (ppc_intel_init(in) != 0) {
        return -1;
    }
    ppc_intel_coverage(in, PPC_CFILTER_ALL, 0);
    for (i = 0; i < nev; i++) {
        const struct ev *e = &evs[i];

        if (!marked && e->t_ms >= 60000u) {
            ppc_intel_mark(in, 1, 60000u);
            marked = 1;
        }
        switch (e->type) {
        case EV_TICK:
            ppc_intel_tick(in, e->t_ms, e->ok ? PPC_EVS_WATCHED : PPC_EVS_HELD);
            break;
        case EV_READ:
            ppc_intel_counters(in, e->t_ms, e->ok, e->lost);
            break;
        case EV_FRAME:
            ppc_intel_frame(in, e->t_ms, e->pkttype, 0);
            break;
        default:
            ppc_intel_note(in, &e->o);
            break;
        }
    }
    ppc_intel_stop(in, T * 1000u);
    for (m = 0; m < PPC_INM_COUNT; m++) {
        if (ppc_intel_compare(in, 1, 1, m, &c) != 0) {
            printf("%spending:-", m ? " " : "");
            continue;
        }
        printf("%s%s:%s", m ? " " : "", ppc_in_verdict_id(c.verdict), ppc_in_reason_id(c.reason));
    }
    printf("\n");
    ppc_intel_free(in);
    return 0;
}

int main(void)
{
    struct ppc_intel in;
    int r;

    while ((r = read_window()) == 1) {
        build();
        if (replay(&in) != 0) {
            fprintf(stderr, "engine init failed\n");
            return 2;
        }
    }
    if (r < 0) {
        fprintf(stderr, "bad input\n");
        return 2;
    }
    return 0;
}
