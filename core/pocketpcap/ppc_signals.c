/*
 * pocketpcap signals. See ppc_signals.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_signals.h"

#include <stdlib.h>
#include <string.h>

#define ETH_HDR 14u
#define VLAN_TAGS_MAX 2

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

int ppc_signals_init(struct ppc_signals *sg, uint32_t flows)
{
    memset(sg, 0, sizeof(*sg));
    if (flows == 0 || (flows & (flows - 1u)) != 0) {
        return -1;
    }
    sg->flows = calloc(flows, sizeof(*sg->flows));
    if (!sg->flows) {
        return -1;
    }
    sg->cap = flows;
    return 0;
}

void ppc_signals_free(struct ppc_signals *sg)
{
    free(sg->flows);
    memset(sg, 0, sizeof(*sg));
}

uint32_t ppc_tcp_payload_len(const uint8_t *d, uint32_t n, uint32_t pay_off)
{
    uint32_t off = ETH_HDR;
    uint32_t end;
    uint16_t et;
    int tags = 0;

    if (!d || n < ETH_HDR) {
        return 0;
    }
    et = rd16(d + 12);
    while ((et == 0x8100 || et == 0x88a8 || et == 0x9100) && tags < VLAN_TAGS_MAX) {
        if (n < off + 4) {
            return 0;
        }
        et = rd16(d + off + 2);
        off += 4;
        tags++;
    }
    if (et == 0x0800) {
        if (n < off + 20) {
            return 0;
        }
        end = off + rd16(d + off + 2);
    } else if (et == 0x86dd) {
        if (n < off + 40) {
            return 0;
        }
        end = off + 40u + rd16(d + off + 4);
    } else {
        return 0;
    }
    return end > pay_off ? end - pay_off : 0;
}

/* ---- the table ------------------------------------------------------------------------ */

struct key {
    uint8_t a[16];
    uint8_t b[16];
    uint16_t pa;
    uint16_t pb;
    uint8_t v6;
    int dir; /* 0: the packet goes a to b */
};

static void make_key(const struct ppc_record *r, struct key *k)
{
    unsigned len = r->l3 == PPC_L3_IPV6 ? 16u : 4u;
    int c = memcmp(r->src, r->dst, len);

    memset(k, 0, sizeof(*k));
    k->v6 = r->l3 == PPC_L3_IPV6;
    if (c < 0 || (c == 0 && r->sport <= r->dport)) {
        memcpy(k->a, r->src, len);
        memcpy(k->b, r->dst, len);
        k->pa = r->sport;
        k->pb = r->dport;
        k->dir = 0;
    } else {
        memcpy(k->a, r->dst, len);
        memcpy(k->b, r->src, len);
        k->pa = r->dport;
        k->pb = r->sport;
        k->dir = 1;
    }
}

static uint32_t hash_key(const struct key *k)
{
    uint32_t h = 2166136261u;
    unsigned i;

    for (i = 0; i < 16; i++) {
        h = (h ^ k->a[i]) * 16777619u;
    }
    for (i = 0; i < 16; i++) {
        h = (h ^ k->b[i]) * 16777619u;
    }
    h = (h ^ k->pa) * 16777619u;
    h = (h ^ k->pb) * 16777619u;
    h = (h ^ k->v6) * 16777619u;
    return h ^ (h >> 15);
}

static int same(const struct ppc_sig_flow *f, const struct key *k)
{
    return f->used && f->pa == k->pa && f->pb == k->pb && f->v6 == k->v6 && memcmp(f->a, k->a, 16) == 0 &&
           memcmp(f->b, k->b, 16) == 0;
}

static void forget_syn(struct ppc_signals *sg, struct ppc_sig_flow *f)
{
    if (f->syn_seq) {
        f->syn_seq = 0;
        if (sg->waiting) {
            sg->waiting--;
        }
    }
}

/* The connection's entry, made when it is not there: a free slot among the
 * probes, else the one silent longest. */
static struct ppc_sig_flow *flow_of(struct ppc_signals *sg, const struct key *k, uint64_t now_ns)
{
    uint32_t h = hash_key(k);
    struct ppc_sig_flow *spare = NULL;
    uint32_t i;

    for (i = 0; i < PPC_SIG_PROBES; i++) {
        struct ppc_sig_flow *f = &sg->flows[(h + i) & (sg->cap - 1u)];

        if (same(f, k)) {
            return f;
        }
        if (!spare || (spare->used && (!f->used || f->last_ns < spare->last_ns))) {
            spare = f;
        }
    }
    if (spare->used) {
        forget_syn(sg, spare);
        sg->replaced++;
    }
    memset(spare, 0, sizeof(*spare));
    memcpy(spare->a, k->a, 16);
    memcpy(spare->b, k->b, 16);
    spare->pa = k->pa;
    spare->pb = k->pb;
    spare->v6 = k->v6;
    spare->used = 1;
    spare->last_ns = now_ns;
    return spare;
}

static uint16_t tcp_signals(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq)
{
    struct key k;
    struct ppc_sig_flow *f;
    struct ppc_sig_dir *d;
    uint16_t out = 0;
    uint8_t fl = r->tcp_flags;
    uint32_t space = tcp_len + ((fl & PPC_TCP_SYN) ? 1u : 0u) + ((fl & PPC_TCP_FIN) ? 1u : 0u);
    uint32_t end = r->a + space;

    if (fl & PPC_TCP_RST) {
        out |= PPC_SIG_RST;
    }
    if (!sg->flows) {
        return out;
    }
    make_key(r, &k);
    f = flow_of(sg, &k, r->rel_ns);
    f->last_ns = r->rel_ns;
    d = &f->d[k.dir];

    /* Anything from the other end answers a waiting SYN. */
    if (f->syn_seq && f->syn_dir != (uint8_t)k.dir) {
        forget_syn(sg, f);
        f->syn_done = 1;
    }
    if ((fl & PPC_TCP_SYN) && !(fl & PPC_TCP_ACK)) {
        if (d->have && r->a + 1u == d->next && f->syn_isn == r->a) {
            out |= PPC_SIG_RETRANS; /* the same SYN again */
        } else {
            /* A new connection on these ports: what was known is void. */
            forget_syn(sg, f);
            f->d[0].have = 0;
            f->d[1].have = 0;
            f->syn_done = 0;
            f->syn_isn = r->a;
            d->have = 1;
            d->next = end;
        }
        if (!f->syn_seq && !f->syn_done && seq) {
            f->syn_seq = seq;
            f->syn_dir = (uint8_t)k.dir;
            f->syn_ns = r->rel_ns;
            sg->waiting++;
        }
        return out;
    }
    if (fl & PPC_TCP_RST) {
        /* The connection is over: the next SYN starts from nothing. */
        f->d[0].have = 0;
        f->d[1].have = 0;
        return out;
    }
    if (space == 0) {
        return out; /* a bare ACK takes no sequence space */
    }
    if (!d->have) {
        d->have = 1;
        d->next = end;
    } else if ((int32_t)(end - d->next) > 0) {
        d->next = end;
    } else if (!(tcp_len <= 1 && !(fl & (PPC_TCP_SYN | PPC_TCP_FIN)) && r->a + 1u == d->next)) {
        out |= PPC_SIG_RETRANS; /* not a keep-alive: bytes seen before */
    }
    return out;
}

uint16_t ppc_signals_frame(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq)
{
    if (r->flags & (PPC_F_MALFORMED | PPC_F_NONFIRST)) {
        return 0;
    }
    switch (r->info) {
    case PPC_INFO_TCP:
        if ((r->l3 == PPC_L3_IPV4 || r->l3 == PPC_L3_IPV6) && r->l4 == 6) {
            return tcp_signals(sg, r, tcp_len, seq);
        }
        return 0;
    case PPC_INFO_DNS:
        /* A response (QR) with a code other than NOERROR. NetBIOS uses the
         * same header for other things and mDNS answers carry no failure. */
        if ((r->app == PPC_APP_DNS || r->app == PPC_APP_LLMNR) && (r->a & 0x8000u) && (r->a & 0xfu) != 0) {
            return PPC_SIG_DNS_FAIL;
        }
        return 0;
    case PPC_INFO_ICMP:
        return (r->a >> 8) == 3 ? PPC_SIG_UNREACH : 0;
    case PPC_INFO_ICMP6:
        return (r->a >> 8) == 1 ? PPC_SIG_UNREACH : 0;
    default:
        return 0;
    }
}

uint32_t ppc_signals_expire(struct ppc_signals *sg, uint64_t now_ns, ppc_sig_late_fn fn, void *ctx)
{
    uint32_t n = 0;
    uint32_t i;

    if (!sg->flows || sg->waiting == 0) {
        return 0;
    }
    for (i = 0; i < sg->cap && sg->waiting > 0; i++) {
        struct ppc_sig_flow *f = &sg->flows[i];

        if (f->used && f->syn_seq && now_ns >= f->syn_ns && now_ns - f->syn_ns >= PPC_SIG_SYN_WAIT_NS) {
            uint32_t seq = f->syn_seq;

            forget_syn(sg, f);
            f->syn_done = 1;
            n++;
            if (fn) {
                fn(ctx, seq);
            }
        }
    }
    return n;
}

void ppc_signals_blind(struct ppc_signals *sg)
{
    uint32_t i;

    for (i = 0; sg->flows && i < sg->cap && sg->waiting > 0; i++) {
        forget_syn(sg, &sg->flows[i]);
    }
    sg->waiting = 0;
}

/* By bit index: RETRANS 0, RST 1, DNS_FAIL 2, UNREACH 3, SYN_UNANSWERED 4. */
const char *ppc_signal_word(unsigned index)
{
    static const char *const words[PPC_SIGNAL_COUNT] = { "RETRANS", "TCP RST", "DNS FAIL", "UNREACH", "NO ANSWER" };

    return index < PPC_SIGNAL_COUNT ? words[index] : "?";
}

const char *ppc_signal_name(unsigned index)
{
    static const char *const names[PPC_SIGNAL_COUNT] = { "RETRANSMISSION", "CONNECTION RESET", "DNS FAILURE",
                                                         "DESTINATION UNREACHABLE", "SYN NOT ANSWERED" };

    return index < PPC_SIGNAL_COUNT ? names[index] : "?";
}
