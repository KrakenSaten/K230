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
#define DNS_QR 0x8000u
#define DNS_WAITING 1u
#define DNS_CALLED 2u

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
    /* One allocation for both tables: the flow entries are a multiple of 8
     * bytes long, so the query entries after them are aligned. */
    sg->flows = calloc(flows * sizeof(*sg->flows) + PPC_DNS_QUERIES * sizeof(*sg->dns), 1);
    if (!sg->flows) {
        return -1;
    }
    sg->cap = flows;
    sg->dns = (struct ppc_dns_q *)(void *)(sg->flows + flows);
    sg->dns_cap = PPC_DNS_QUERIES;
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

static uint32_t fnv(uint32_t h, const uint8_t *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static uint32_t us_of(uint64_t ns)
{
    uint64_t us = ns / 1000u;

    return us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
}

/* ---- the connection table ------------------------------------------------------------- */

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
    uint32_t h = fnv(2166136261u, k->a, 16);

    h = fnv(h, k->b, 16);
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
 * probes, else the one silent longest. *fresh says it was made now. */
static struct ppc_sig_flow *flow_of(struct ppc_signals *sg, const struct key *k, uint64_t now_ns, int *fresh)
{
    uint32_t h = hash_key(k);
    struct ppc_sig_flow *spare = NULL;
    uint32_t i;

    *fresh = 0;
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
    *fresh = 1;
    return spare;
}

/* Whether a segment starting at seq begins inside the direction's gap. */
static int in_hole(const struct ppc_sig_dir *d, uint32_t seq)
{
    return d->hole && (int32_t)(seq - d->hole_lo) >= 0 && (int32_t)(d->hole_hi - seq) > 0;
}

/* The gap shrinks by what filled it from either edge; a fill in its middle
 * leaves it as it was (one gap per direction is an approximation). */
static void fill_hole(struct ppc_sig_dir *d, uint32_t seq, uint32_t end)
{
    if (seq == d->hole_lo) {
        d->hole_lo = (int32_t)(end - d->hole_hi) >= 0 ? d->hole_hi : end;
    } else if ((int32_t)(end - d->hole_hi) >= 0) {
        d->hole_hi = seq;
    }
    if ((int32_t)(d->hole_hi - d->hole_lo) <= 0) {
        d->hole = 0;
    }
}

static void timed(uint64_t *sum, uint32_t *max, uint32_t *max_seq, uint32_t us, uint32_t seq)
{
    *sum += us;
    if (us >= *max) {
        *max = us;
        *max_seq = seq;
    }
}

static uint16_t tcp_signals(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq)
{
    struct ppc_diag *st = &sg->st;
    struct key k;
    struct ppc_sig_flow *f;
    struct ppc_sig_dir *d;
    struct ppc_sig_dir *o;
    uint16_t out = 0;
    unsigned diag = PPC_DIAG_NONE;
    uint8_t fl = r->tcp_flags;
    uint32_t space = tcp_len + ((fl & PPC_TCP_SYN) ? 1u : 0u) + ((fl & PPC_TCP_FIN) ? 1u : 0u);
    uint32_t end = r->a + space;
    uint64_t now = r->rel_ns;
    int fresh;

    if (fl & PPC_TCP_RST) {
        out |= PPC_SIG_RST;
    }
    if (!sg->flows) {
        return out;
    }
    make_key(r, &k);
    f = flow_of(sg, &k, now, &fresh);
    f->last_ns = now;
    f->dirs |= (uint8_t)(1u << k.dir);
    if (f->pkts < 0xffffu) {
        f->pkts++;
    }
    d = &f->d[k.dir];
    o = &f->d[k.dir ^ 1];
    if ((fl & PPC_TCP_ACK) && (!d->have_ack || (int32_t)(r->b - d->ack) > 0)) {
        d->ack = r->b;
        d->have_ack = 1;
    }

    /* Anything from the other end answers a waiting SYN. */
    if (f->syn_seq && f->syn_dir != (uint8_t)k.dir) {
        forget_syn(sg, f);
        f->syn_done = 1;
    }
    if ((fl & PPC_TCP_SYN) && !(fl & PPC_TCP_ACK)) {
        if (d->have && r->a + 1u == d->next && f->syn_isn == r->a) {
            out |= PPC_SIG_RETRANS; /* the same SYN again */
            diag = PPC_DIAG_TCP_SYN_AGAIN;
            st->tcp_syn_again++;
        } else {
            /* A new connection on these ports: what was known is void. */
            forget_syn(sg, f);
            memset(f->d, 0, sizeof(f->d));
            f->syn_done = 0;
            f->syn_isn = r->a;
            d->have = 1;
            d->next = end;
            f->hs = PPC_HS_SYN;
            f->hs_dir = (uint8_t)k.dir;
            f->hs_ns = now;
            f->hs_seen = 1;
            f->hs_seq = seq;
            f->irtt_us = 0;
            f->ended = 0;
            diag = PPC_DIAG_TCP_SYN;
            st->tcp_attempts++;
        }
        if (!f->syn_seq && !f->syn_done && seq) {
            f->syn_seq = seq;
            f->syn_dir = (uint8_t)k.dir;
            f->syn_ns = now;
            sg->waiting++;
        }
        return (uint16_t)(out | diag << PPC_DIAG_SHIFT);
    }
    if (fl & PPC_TCP_RST) {
        /* Which reset it is decides whether it is a fault: a refusal or a
         * reset after close is not one. */
        if (fresh) {
            diag = PPC_DIAG_TCP_RST_UNSEEN;
            st->tcp_rst_unseen++;
        } else if (f->ended) {
            diag = PPC_DIAG_TCP_RST_CLOSED;
            st->tcp_rst_closed++;
        } else if (f->hs == PPC_HS_SYN && (uint8_t)k.dir != f->hs_dir) {
            diag = PPC_DIAG_TCP_REFUSED;
            st->tcp_refused++;
        } else {
            diag = PPC_DIAG_TCP_ABORTED;
            st->tcp_aborted++;
        }
        /* The connection is over: the next SYN starts from nothing. */
        f->ended = 2;
        f->d[0].have = 0;
        f->d[1].have = 0;
        f->d[0].hole = 0;
        f->d[1].hole = 0;
        return (uint16_t)(out | diag << PPC_DIAG_SHIFT);
    }

    /* The handshake. */
    if ((fl & PPC_TCP_SYN) && f->hs == PPC_HS_SYN && (uint8_t)k.dir != f->hs_dir && r->b == f->syn_isn + 1u) {
        f->hs = PPC_HS_SYNACK;
        f->synack_isn = r->a;
        diag = PPC_DIAG_TCP_SYNACK;
    } else if ((fl & PPC_TCP_SYN) && f->hs == PPC_HS_NONE) {
        /* A SYN-ACK whose SYN was not seen: the rest can be followed, not
         * timed. */
        f->hs = PPC_HS_SYNACK;
        f->hs_dir = (uint8_t)(k.dir ^ 1);
        f->synack_isn = r->a;
        f->hs_seen = 0;
        diag = PPC_DIAG_TCP_MIDSTREAM;
        st->tcp_midstream++;
    } else if (!(fl & PPC_TCP_SYN) && f->hs == PPC_HS_SYNACK && (uint8_t)k.dir == f->hs_dir && (fl & PPC_TCP_ACK) &&
               (int32_t)(r->b - (f->synack_isn + 1u)) >= 0) {
        f->hs = PPC_HS_OPEN;
        diag = PPC_DIAG_TCP_CONNECTED;
        st->tcp_connected++;
        if (f->hs_seen && now >= f->hs_ns) {
            f->irtt_us = us_of(now - f->hs_ns);
            if (f->irtt_us == 0) {
                f->irtt_us = 1;
            }
            st->tcp_timed++;
            timed(&st->tcp_hs_sum_us, &st->tcp_hs_max_us, &st->tcp_hs_max_seq, f->irtt_us, seq);
        }
    } else if (f->hs == PPC_HS_NONE) {
        f->hs = PPC_HS_MID;
        diag = PPC_DIAG_TCP_MIDSTREAM;
        st->tcp_midstream++;
    }
    if ((fl & PPC_TCP_FIN) && !f->ended) {
        f->ended = 1;
    }
    if (space == 0) {
        return (uint16_t)(out | diag << PPC_DIAG_SHIFT); /* a bare ACK takes no sequence space */
    }
    if (!(fl & PPC_TCP_SYN)) {
        st->tcp_segments++;
    }
    if (!d->have) {
        d->have = 1;
        d->next = end;
    } else if ((int32_t)(end - d->next) > 0) {
        if ((int32_t)(r->a - d->next) > 0) {
            /* Bytes skipped: a gap this capture has not seen (yet). */
            d->hole = 1;
            d->hole_lo = d->next;
            d->hole_hi = r->a;
            d->hole_ns = now;
        }
        d->next = end;
    } else if (tcp_len <= 1 && !(fl & (PPC_TCP_SYN | PPC_TCP_FIN)) && r->a + 1u == d->next) {
        /* a keep-alive: not a repeat of data */
    } else if (in_hole(d, r->a)) {
        uint64_t wait = f->irtt_us ? (uint64_t)f->irtt_us * 1000u : PPC_TCP_REORDER_NS;
        uint64_t since = now >= d->hole_ns ? now - d->hole_ns : 0;

        fill_hole(d, r->a, end);
        if (since < wait) {
            diag = PPC_DIAG_TCP_OUT_OF_ORDER;
            st->tcp_ooo++;
        } else {
            out |= PPC_SIG_RETRANS; /* its original never came by this interface */
            diag = PPC_DIAG_TCP_RETRANS_UNSEEN;
            st->tcp_retrans_unseen++;
        }
    } else {
        out |= PPC_SIG_RETRANS; /* bytes seen before */
        if (fl & PPC_TCP_SYN) {
            diag = PPC_DIAG_TCP_SYN_AGAIN;
            st->tcp_syn_again++;
        } else if (o->have_ack && (int32_t)(o->ack - end) >= 0) {
            diag = PPC_DIAG_TCP_RETRANS_ACKED;
            st->tcp_retrans_acked++;
        } else {
            diag = PPC_DIAG_TCP_RETRANS;
            st->tcp_retrans++;
        }
    }
    return (uint16_t)(out | diag << PPC_DIAG_SHIFT);
}

/* ---- the DNS table --------------------------------------------------------------------- */

struct qkey {
    uint8_t cli[16];
    uint8_t srv[16];
    uint16_t cport;
    uint16_t sport;
    uint16_t id;
    uint8_t v6;
};

static uint32_t hash_q(const struct qkey *k)
{
    uint32_t h = fnv(2166136261u, k->cli, 16);

    h = fnv(h, k->srv, 16);
    h = (h ^ k->cport) * 16777619u;
    h = (h ^ k->sport) * 16777619u;
    h = (h ^ k->id) * 16777619u;
    h = (h ^ k->v6) * 16777619u;
    return h ^ (h >> 15);
}

static int same_q(const struct ppc_dns_q *q, const struct qkey *k)
{
    return q->used && q->id == k->id && q->cport == k->cport && q->sport == k->sport && q->v6 == k->v6 &&
           memcmp(q->cli, k->cli, 16) == 0 && memcmp(q->srv, k->srv, 16) == 0;
}

static void free_q(struct ppc_signals *sg, struct ppc_dns_q *q)
{
    if (q->used) {
        if (q->state == DNS_WAITING && sg->dns_waiting) {
            sg->dns_waiting--;
        }
        if (sg->dns_live) {
            sg->dns_live--;
        }
    }
    memset(q, 0, sizeof(*q));
}

/* The query's entry, or NULL. With `make`, a new one: a free slot among the
 * probes, else the oldest (one already called unanswered first); a waiting
 * query replaced is forgotten, not called unanswered. */
static struct ppc_dns_q *query_of(struct ppc_signals *sg, const struct qkey *k, int make)
{
    uint32_t h = hash_q(k);
    struct ppc_dns_q *spare = NULL;
    uint32_t i;

    for (i = 0; i < PPC_SIG_PROBES; i++) {
        struct ppc_dns_q *q = &sg->dns[(h + i) & (sg->dns_cap - 1u)];

        if (same_q(q, k)) {
            return q;
        }
        if (!spare || (spare->used && (!q->used || (q->state > spare->state) ||
                                       (q->state == spare->state && q->last_ns < spare->last_ns)))) {
            spare = q;
        }
    }
    if (!make) {
        return NULL;
    }
    if (spare->used) {
        if (spare->state == DNS_WAITING) {
            sg->st.dns_unjudged++;
        }
        free_q(sg, spare);
    }
    memcpy(spare->cli, k->cli, 16);
    memcpy(spare->srv, k->srv, 16);
    spare->cport = k->cport;
    spare->sport = k->sport;
    spare->id = k->id;
    spare->v6 = k->v6;
    spare->used = 1;
    sg->dns_live++;
    return spare;
}

static void count_rcode(struct ppc_diag *st, unsigned rcode)
{
    switch (rcode) {
    case 0:
        break;
    case 2:
        st->dns_servfail++;
        break;
    case 3:
        st->dns_nxdomain++;
        break;
    case 5:
        st->dns_refused++;
        break;
    default:
        st->dns_other++;
        break;
    }
}

/* A DNS message on port 53: a query enters the table, an answer leaves it
 * with its time. Returns the diagnosis. */
static unsigned dns_track(struct ppc_signals *sg, const struct ppc_record *r, uint32_t seq)
{
    struct ppc_diag *st = &sg->st;
    unsigned len = r->l3 == PPC_L3_IPV6 ? 16u : 4u;
    int answer = (r->a & DNS_QR) != 0;
    struct ppc_dns_q *q;
    struct qkey k;
    unsigned diag;
    uint32_t us;

    sg->dns_last_us = 0;
    if (!sg->dns || r->l4 != 17 || (r->l3 != PPC_L3_IPV4 && r->l3 != PPC_L3_IPV6)) {
        return PPC_DIAG_NONE;
    }
    memset(&k, 0, sizeof(k));
    k.v6 = r->l3 == PPC_L3_IPV6;
    k.id = (uint16_t)(r->a >> 16);
    memcpy(k.cli, answer ? r->dst : r->src, len);
    memcpy(k.srv, answer ? r->src : r->dst, len);
    k.cport = answer ? r->dport : r->sport;
    k.sport = answer ? r->sport : r->dport;
    if (!answer) {
        q = query_of(sg, &k, 0);
        if (q) {
            /* The resolver asked again: the wait starts over. */
            if (q->state != DNS_WAITING) {
                q->state = DNS_WAITING;
                sg->dns_waiting++;
            }
            if (q->tries < 0xff) {
                q->tries++;
            }
            q->seq = seq;
            q->last_ns = r->rel_ns;
            st->dns_retries++;
            return PPC_DIAG_DNS_RETRY;
        }
        q = query_of(sg, &k, 1);
        q->state = DNS_WAITING;
        q->tries = 1;
        q->seq = seq;
        q->last_ns = r->rel_ns;
        sg->dns_waiting++;
        st->dns_queries++;
        return PPC_DIAG_DNS_QUERY;
    }
    q = query_of(sg, &k, 0);
    if (!q) {
        st->dns_orphan++;
        return PPC_DIAG_DNS_ORPHAN;
    }
    us = r->rel_ns >= q->last_ns ? us_of(r->rel_ns - q->last_ns) : 0;
    timed(&st->dns_rtt_sum_us, &st->dns_rtt_max_us, &st->dns_rtt_max_seq, us, seq);
    sg->dns_last_us = us ? us : 1u; /* an answer in the same microsecond still had its query */
    if (q->state == DNS_CALLED) {
        diag = PPC_DIAG_DNS_LATE;
        st->dns_late++;
    } else {
        diag = PPC_DIAG_DNS_ANSWER;
        st->dns_answered++;
    }
    free_q(sg, q);
    return diag;
}

/* ---- the frame ------------------------------------------------------------------------- */

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
    case PPC_INFO_DNS: {
        uint16_t out = 0;

        /* A response (QR) with a code other than NOERROR. NetBIOS uses the
         * same header for other things and mDNS answers carry no failure. */
        if ((r->app == PPC_APP_DNS || r->app == PPC_APP_LLMNR) && (r->a & DNS_QR) && (r->a & 0xfu) != 0) {
            out = PPC_SIG_DNS_FAIL;
            count_rcode(&sg->st, r->a & 0xfu);
        }
        /* Transactions: unicast DNS only. An LLMNR or mDNS query that no
         * one answers is how those protocols say "not here". */
        if (r->app == PPC_APP_DNS) {
            out = (uint16_t)(out | dns_track(sg, r, seq) << PPC_DIAG_SHIFT);
        }
        return out;
    }
    case PPC_INFO_ICMP:
        return (r->a >> 8) == 3 ? PPC_SIG_UNREACH : 0;
    case PPC_INFO_ICMP6:
        return (r->a >> 8) == 1 ? PPC_SIG_UNREACH : 0;
    default:
        return 0;
    }
}

int ppc_signals_pending(const struct ppc_signals *sg)
{
    return sg->flows && (sg->waiting > 0 || sg->dns_live > 0);
}

uint32_t ppc_signals_expire(struct ppc_signals *sg, uint64_t now_ns, ppc_sig_late_fn fn, void *ctx)
{
    uint32_t n = 0;
    uint32_t i;

    if (!sg->flows) {
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
                fn(ctx, seq, PPC_SIG_SYN_UNANSWERED);
            }
        }
    }
    for (i = 0; i < sg->dns_cap && sg->dns_live > 0; i++) {
        struct ppc_dns_q *q = &sg->dns[i];
        uint64_t age;

        if (!q->used) {
            continue;
        }
        age = now_ns >= q->last_ns ? now_ns - q->last_ns : 0;
        if (q->state == DNS_WAITING && age >= PPC_DNS_WAIT_NS) {
            q->state = DNS_CALLED;
            if (sg->dns_waiting) {
                sg->dns_waiting--;
            }
            sg->st.dns_noreply++;
            n++;
            if (fn) {
                fn(ctx, q->seq, PPC_SIG_DNS_NOREPLY);
            }
        } else if (q->state == DNS_CALLED && age >= PPC_DNS_KEEP_NS) {
            free_q(sg, q);
        }
    }
    return n;
}

void ppc_signals_blind(struct ppc_signals *sg)
{
    uint32_t i;

    if (!sg->flows) {
        return;
    }
    sg->st.blinded++;
    for (i = 0; i < sg->cap; i++) {
        struct ppc_sig_flow *f = &sg->flows[i];

        if (!f->used) {
            continue;
        }
        forget_syn(sg, f);
        f->d[0].hole = 0;
        f->d[1].hole = 0;
        if (f->hs == PPC_HS_SYN || f->hs == PPC_HS_SYNACK) {
            f->hs = PPC_HS_MID; /* its next step may pass unseen */
        }
    }
    sg->waiting = 0;
    for (i = 0; i < sg->dns_cap && sg->dns_live > 0; i++) {
        if (sg->dns[i].used) {
            if (sg->dns[i].state == DNS_WAITING) {
                sg->st.dns_unjudged++;
            }
            free_q(sg, &sg->dns[i]);
        }
    }
    sg->dns_waiting = 0;
}

void ppc_signals_diag(const struct ppc_signals *sg, struct ppc_diag *out)
{
    uint32_t i;

    *out = sg->st;
    out->dns_waiting = sg->dns_waiting;
    out->tcp_replaced = sg->replaced > 0xffffffffu ? 0xffffffffu : (uint32_t)sg->replaced;
    out->tcp_flows = 0;
    out->tcp_oneway = 0;
    for (i = 0; sg->flows && i < sg->cap; i++) {
        const struct ppc_sig_flow *f = &sg->flows[i];

        if (!f->used) {
            continue;
        }
        out->tcp_flows++;
        /* One way only, and more than a SYN waiting: the other direction
         * reached another interface, or this capture missed it. */
        if (f->dirs != 3u && f->pkts >= 3u && f->hs != PPC_HS_SYN) {
            out->tcp_oneway++;
        }
    }
}

/* By bit index: RETRANS 0, RST 1, DNS_FAIL 2, UNREACH 3, SYN_UNANSWERED 4,
 * DNS_NOREPLY 5. */
const char *ppc_signal_word(unsigned index)
{
    static const char *const words[PPC_SIGNAL_COUNT] = { "RETRANS", "TCP RST",   "DNS FAIL",
                                                         "UNREACH", "NO ANSWER", "NO DNS REPLY" };

    return index < PPC_SIGNAL_COUNT ? words[index] : "?";
}

const char *ppc_signal_name(unsigned index)
{
    static const char *const names[PPC_SIGNAL_COUNT] = { "RETRANSMISSION",          "CONNECTION RESET",
                                                         "DNS FAILURE",             "DESTINATION UNREACHABLE",
                                                         "SYN NOT ANSWERED",        "DNS QUERY NOT ANSWERED" };

    return index < PPC_SIGNAL_COUNT ? names[index] : "?";
}
