/*
 * N1.2 bench replay: a capture the real pos-capture saved on the bench, and
 * the sidecar of what the helper said while it ran (its state, its
 * lost-frame readings, shedding), replayed into the N1.0 observers and the
 * frozen ppc_intel engine in packet time, as tests/ppc_intel_test.c's
 * player does. Host only, research only.
 *
 *   bench_replay CAPTURE.pcap SIDECAR.txt [MARK_S]
 *
 * Sidecar lines (wall-clock milliseconds):
 *   own <mac>               the device's interface address
 *   start <ms>              START was sent
 *   state <ms> <word>       capturing | hold | stopped | ...
 *   read <ms> <ok> <total>  a lost-frame reading (kernel + interface drops)
 *   shed <ms> 0|1           the helper counted without storing
 *   stop <ms>               STOP was sent
 *
 * A frame's direction comes from the device's address, as the helper
 * classifies it for the engine: its source is the device (OUTGOING), its
 * destination is the device (HOST), broadcast, multicast, or another host.
 * The mark is at MARK_S seconds after START (60 by default), so the pair
 * "after against before" is the held-out interface's base and window.
 * Output: one line per metric, "<metric> <verdict> <reason> <quality>
 * <n0> <n1> <k0> <k1>".
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_evid.h"
#include "pocketpcap/ppc_intel.h"
#include "pocketpcap/ppc_net.h"
#include "pocketpcap/ppc_pcapfile.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_signals.h"
#include "pocketpcap/ppc_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fr {
    uint32_t t_ms;
    uint32_t len, wire;
    uint8_t *data;
};

struct ev {
    uint32_t t_ms;
    uint8_t kind; /* 0 reading, 1 state, 2 shed */
    uint8_t ok;
    uint8_t state;
    uint64_t total;
};

struct player {
    struct ppc_signals sig;
    struct ppc_net net;
    struct ppc_evid ev;
    struct ppc_intel in;
    struct ppc_store store;
    uint32_t lead_ms;
    int have_lead;
    uint8_t self[6];
    unsigned state;
    uint32_t last_seq;
};

static void on_said(void *ctx, const struct ppc_net_said *s)
{
    struct player *p = ctx;
    struct ppc_ev_obs o;

    if (ppc_evid_from_net(s, p->lead_ms, p->self, &o)) {
        ppc_evid_note(&p->ev, &o);
        ppc_intel_note(&p->in, &o);
    }
}

static void late_none(void *ctx, uint32_t seq, uint16_t bit)
{
    (void)ctx;
    (void)seq;
    (void)bit;
}

static unsigned dir_of(const uint8_t *b, uint32_t len, const uint8_t self[6])
{
    if (len < 14) {
        return PPC_PKT_UNKNOWN;
    }
    if (!memcmp(b + 6, self, 6)) {
        return PPC_PKT_OUTGOING;
    }
    if (!memcmp(b, self, 6)) {
        return PPC_PKT_HOST;
    }
    if (b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff && b[4] == 0xff && b[5] == 0xff) {
        return PPC_PKT_BROADCAST;
    }
    return (b[0] & 1u) ? PPC_PKT_MULTICAST : PPC_PKT_OTHERHOST;
}

static void play_frame(struct player *p, const struct fr *x)
{
    struct ppc_frame f;
    struct ppc_record r;
    struct ppc_ev_obs o;
    uint32_t pay;
    uint32_t seq;
    unsigned dir = dir_of(x->data, x->len, p->self);

    memset(&f, 0, sizeof(f));
    f.data = x->data;
    f.cap_len = x->len;
    f.wire_len = x->wire;
    f.pkttype = (uint8_t)dir;
    pay = ppc_decode(&f, &r);
    if (!p->have_lead) {
        p->have_lead = 1;
        p->lead_ms = x->t_ms;
    }
    r.rel_ns = (uint64_t)(x->t_ms - p->lead_ms) * 1000000u;
    seq = p->store.last_seq + 1u;
    ppc_intel_frame(&p->in, x->t_ms, dir, 0);
    r.signal = ppc_signals_frame(&p->sig, &r, 0, seq);
    if (ppc_evid_from_dns(&r, ppc_sig_diag(r.signal), p->sig.dns_last_us, dir == PPC_PKT_HOST, p->lead_ms, seq, &o)) {
        ppc_evid_note(&p->ev, &o);
        ppc_intel_note(&p->in, &o);
    }
    ppc_net_frame(&p->net, &r, x->data, x->len, pay, seq);
    ppc_store_add(&p->store, &r, x->data, ppc_store_len(&r, pay, x->len, PPC_KEEP_HEADERS));
    p->last_seq = p->store.last_seq;
}

static void tick(struct player *p, uint32_t t)
{
    ppc_evid_tick(&p->ev, t, p->state);
    ppc_intel_tick(&p->in, t, p->state);
    if (p->have_lead && t >= p->lead_ms) {
        ppc_signals_expire(&p->sig, (uint64_t)(t - p->lead_ms) * 1000000u, late_none, NULL);
        ppc_net_expire(&p->net, (uint64_t)(t - p->lead_ms) * 1000000u);
    }
}

int main(int argc, char **argv)
{
    static struct player p;
    struct ppc_pcap_reader rd;
    struct ppc_frame f;
    struct fr *frs = NULL;
    struct ev *evs = NULL;
    size_t nf = 0, cf = 0, ne = 0, ce = 0;
    uint64_t start = 0, stop = 0;
    uint32_t mark_ms;
    char line[256];
    FILE *sc;
    size_t i, j;
    uint32_t next_tick = 0;
    unsigned m;
    int shed = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: bench_replay CAPTURE.pcap SIDECAR.txt [MARK_S]\n");
        return 2;
    }
    mark_ms = (uint32_t)((argc > 3 ? atof(argv[3]) : 60.0) * 1000.0);
    sc = fopen(argv[2], "r");
    if (!sc) {
        perror(argv[2]);
        return 2;
    }
    while (fgets(line, sizeof(line), sc)) {
        unsigned long long t = 0, tot = 0;
        unsigned a[6];
        char w[32];
        int ok = 0;

        if (sscanf(line, "own %x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (j = 0; j < 6; j++) {
                p.self[j] = (uint8_t)a[j];
            }
        } else if (sscanf(line, "start %llu", &t) == 1) {
            start = t;
        } else if (sscanf(line, "stop %llu", &t) == 1) {
            stop = t;
        } else if (start && (sscanf(line, "read %llu %d %llu", &t, &ok, &tot) == 3 ||
                             sscanf(line, "state %llu %31s", &t, w) == 2 || sscanf(line, "shed %llu %d", &t, &ok) == 2)) {
            if (ne == ce) {
                ce = ce ? ce * 2 : 1024;
                evs = realloc(evs, ce * sizeof(*evs));
            }
            memset(&evs[ne], 0, sizeof(evs[ne]));
            evs[ne].t_ms = t > start ? (uint32_t)(t - start) : 0;
            if (line[0] == 'r') {
                evs[ne].kind = 0;
                evs[ne].ok = (uint8_t)ok;
                evs[ne].total = tot;
            } else if (line[0] == 's' && line[1] == 't') {
                evs[ne].kind = 1;
                evs[ne].state = !strcmp(w, "hold") ? PPC_EVS_HELD : PPC_EVS_WATCHED;
                if (strcmp(w, "hold") && strcmp(w, "capturing")) {
                    continue; /* idle, stopped, saving: STOP ends the replay */
                }
            } else {
                evs[ne].kind = 2;
                evs[ne].ok = (uint8_t)ok;
            }
            ne++;
        }
    }
    fclose(sc);
    if (!start || !stop) {
        fprintf(stderr, "sidecar without start or stop\n");
        return 2;
    }
    if (ppc_pcap_open(&rd, argv[1]) != 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    while (ppc_pcap_next(&rd, &f) == 1) {
        uint64_t ms = f.ts_ns / 1000000u;

        if (ms < start || ms > stop) {
            continue;
        }
        if (nf == cf) {
            cf = cf ? cf * 2 : 4096;
            frs = realloc(frs, cf * sizeof(*frs));
        }
        frs[nf].t_ms = (uint32_t)(ms - start);
        frs[nf].len = f.cap_len;
        frs[nf].wire = f.wire_len;
        frs[nf].data = malloc(f.cap_len ? f.cap_len : 1);
        memcpy(frs[nf].data, f.data, f.cap_len);
        nf++;
    }
    ppc_pcap_close(&rd);

    if (ppc_signals_init(&p.sig, PPC_SIG_FLOWS) != 0 || ppc_net_init(&p.net) != 0 || ppc_evid_init(&p.ev) != 0 ||
        ppc_intel_init(&p.in) != 0 || ppc_store_init(&p.store, PPC_STORE_SUMMARIES, PPC_STORE_RING_BYTES) != 0) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    ppc_net_listen(&p.net, on_said, &p);
    ppc_evid_coverage(&p.ev, PPC_CFILTER_ALL, 0);
    ppc_intel_coverage(&p.in, PPC_CFILTER_ALL, 0);
    p.state = PPC_EVS_WATCHED;
    {
        uint32_t t_stop = (uint32_t)(stop - start);
        int marked = 0;

        for (i = 0, j = 0;;) {
            uint32_t tf = i < nf ? frs[i].t_ms : 0xffffffffu;
            uint32_t te = j < ne ? evs[j].t_ms : 0xffffffffu;
            uint32_t tm = marked ? 0xffffffffu : mark_ms;
            uint32_t t = tf < te ? tf : te;

            if (tm <= t && tm <= next_tick && tm < t_stop) {
                const struct ppc_ev_mark *mk;

                tick(&p, tm);
                mk = ppc_evid_mark(&p.ev, tm, p.state == PPC_EVS_HELD ? PPC_STATE_HOLD : PPC_STATE_CAPTURING,
                                   p.last_seq, 0);
                ppc_intel_mark(&p.in, mk->id, mk->at_ms);
                marked = 1;
                continue;
            }
            if (next_tick <= t && next_tick <= t_stop) {
                tick(&p, next_tick);
                next_tick += 250u;
                continue;
            }
            if (t == 0xffffffffu || t > t_stop) {
                break;
            }
            if (te <= tf) {
                const struct ev *e = &evs[j++];

                if (e->kind == 0) {
                    ppc_evid_counters(&p.ev, e->t_ms, e->ok, e->total);
                    ppc_intel_counters(&p.in, e->t_ms, e->ok, e->total);
                } else if (e->kind == 1 && e->state != p.state) {
                    tick(&p, e->t_ms);
                    p.state = e->state;
                    tick(&p, e->t_ms);
                    if (p.state == PPC_EVS_HELD) {
                        ppc_signals_blind(&p.sig);
                        ppc_net_blind(&p.net);
                    }
                } else if (e->kind == 2) {
                    shed = e->ok;
                }
            } else {
                if (p.state == PPC_EVS_WATCHED) {
                    if (shed) {
                        ppc_intel_frame(&p.in, frs[i].t_ms, PPC_PKT_UNKNOWN, 1);
                    } else {
                        play_frame(&p, &frs[i]);
                    }
                }
                i++;
            }
        }
        tick(&p, t_stop);
        ppc_evid_stop(&p.ev, t_stop, 0);
        ppc_intel_stop(&p.in, t_stop);
    }
    for (m = 0; m < PPC_INM_COUNT; m++) {
        struct ppc_in_cmp c;

        if (ppc_intel_compare(&p.in, 1, 1, m, &c) != 0) {
            printf("%s pending - - 0 0 0 0\n", ppc_in_metric_id(m));
            continue;
        }
        printf("%s %s %s %s %u %u %u %u\n", ppc_in_metric_id(m), ppc_in_verdict_id(c.verdict), ppc_in_reason_id(c.reason),
               ppc_in_quality_id(c.quality), c.n[0], c.n[1], c.k[0], c.k[1]);
    }
    return 0;
}
