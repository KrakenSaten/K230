/*
 * pocketpcap intelligence engine, first slice (N1.1): see ppc_intel.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_intel.h"

#include "ppc_record.h"
#include "ppc_store.h"
#include "ppc_view.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SEC_MASK (PPC_IN_SECONDS - 1u)
#define Q_MASK (PPC_IN_QUESTIONS - 1u)
#define EVT_MASK (PPC_IN_EVENTS - 1u)
#define READ_MASK (PPC_IN_READINGS - 1u)
#define LEDGER_MASK (PPC_IN_LEDGER - 1u)
#define MATCH_BACK 64u        /* questions looked through for an answer's or a retry's */
#define ONE_WAY_MIN_OUT 8u    /* frames this device sent before one-sidedness is judged */
#define ONE_WAY_LOG_P 6.907755 /* ln 1000: a silent spell that unlikely at the inbound rate seen */
#define Z_BOUND 1.96          /* the bound put on a base window's own share or rate */

/* ---- life ---------------------------------------------------------------------------------- */

uint32_t ppc_intel_bytes(void)
{
    return PPC_IN_SECONDS * sizeof(struct ppc_in_sec) + PPC_IN_QUESTIONS * sizeof(struct ppc_in_q) +
           PPC_IN_EVENTS * sizeof(struct ppc_in_evt) + PPC_IN_READINGS * sizeof(struct ppc_in_read) +
           PPC_IN_LEDGER * sizeof(struct ppc_in_cmp);
}

int ppc_intel_init(struct ppc_intel *in)
{
    uint8_t *p;

    memset(in, 0, sizeof(*in));
    in->mem = calloc(1, ppc_intel_bytes());
    if (!in->mem) {
        return -1;
    }
    p = in->mem;
    in->secs = (struct ppc_in_sec *)p;
    p += PPC_IN_SECONDS * sizeof(struct ppc_in_sec);
    in->qs = (struct ppc_in_q *)p;
    p += PPC_IN_QUESTIONS * sizeof(struct ppc_in_q);
    in->evts = (struct ppc_in_evt *)p;
    p += PPC_IN_EVENTS * sizeof(struct ppc_in_evt);
    in->reads = (struct ppc_in_read *)p;
    p += PPC_IN_READINGS * sizeof(struct ppc_in_read);
    in->ledger = (struct ppc_in_cmp *)p;
    in->next_id = 1;
    /* START is a reading at which nothing had been lost (as ppc_evid). */
    in->read_ms = 0;
    in->lost_total = 0;
    in->reads_from_ms = 0;
    return 0;
}

void ppc_intel_free(struct ppc_intel *in)
{
    free(in->mem);
    memset(in, 0, sizeof(*in));
}

void ppc_intel_coverage(struct ppc_intel *in, unsigned cfilter, unsigned unanalyzed)
{
    in->cfilter = (uint8_t)cfilter;
    in->unanalyzed = (uint8_t)(unanalyzed & PPC_EV_CLASSES);
}

/* ---- the series ---------------------------------------------------------------------------- */

static struct ppc_in_sec *sec_slot(const struct ppc_intel *in, uint32_t s)
{
    struct ppc_in_sec *x = &in->secs[s & SEC_MASK];

    return x->used && x->sec == s ? x : NULL;
}

static void sec_open(struct ppc_intel *in, uint32_t s, uint8_t flags)
{
    struct ppc_in_sec *x = &in->secs[s & SEC_MASK];

    memset(x, 0, sizeof(*x));
    x->sec = s;
    x->used = 1;
    x->flags = flags;
}

/* Bring the series to second s: every second passed is what the capture was
 * doing then. A jump longer than the ring keeps only its newest seconds. */
static void fill_to(struct ppc_intel *in, uint32_t s)
{
    uint32_t from;

    if (!in->ticking) {
        in->ticking = 1;
        in->cur_sec = s;
        sec_open(in, s, in->cur_flags);
        return;
    }
    if (s <= in->cur_sec) {
        return;
    }
    from = in->cur_sec + 1u;
    if (s - from >= PPC_IN_SECONDS) {
        from = s - (PPC_IN_SECONDS - 1u);
    }
    for (; from <= s; from++) {
        sec_open(in, from, in->cur_flags);
    }
    in->cur_sec = s;
}

/* The slot of the second t_ms falls in, opened if it is new; NULL for a
 * second the ring no longer holds. */
static struct ppc_in_sec *at(struct ppc_intel *in, uint32_t t_ms)
{
    uint32_t s = t_ms / 1000u;

    if (!in->ticking || s > in->cur_sec) {
        fill_to(in, s);
    }
    return sec_slot(in, s);
}

static void bump16(uint16_t *v)
{
    if (*v < 0xffffu) {
        (*v)++;
    }
}

void ppc_intel_frame(struct ppc_intel *in, uint32_t t_ms, unsigned pkttype, int shed)
{
    struct ppc_in_sec *x = at(in, t_ms);

    if (t_ms > in->now_ms) {
        in->now_ms = t_ms;
    }
    if (!x) {
        return;
    }
    x->frames++;
    if (shed) {
        /* Counted, not analysed: its second was not watched whole. */
        x->flags |= PPC_EVS_SHED;
        return;
    }
    if (pkttype == PPC_PKT_OUTGOING) {
        x->own_out++;
    } else if (pkttype == PPC_PKT_HOST) {
        x->own_in++;
    }
}

/* ---- observations -------------------------------------------------------------------------- */

static struct ppc_in_q *q_find(struct ppc_intel *in, uint16_t id, uint32_t t_ms, int unanswered)
{
    uint32_t n = in->nq < MATCH_BACK ? in->nq : MATCH_BACK;
    uint32_t i;

    for (i = 1; i <= n; i++) {
        struct ppc_in_q *q = &in->qs[(in->nq - i) & Q_MASK];

        if (q->t_ms > t_ms || t_ms - q->t_ms > PPC_IN_WAIT_MS) {
            continue;
        }
        if (q->id == id && (!unanswered || !q->answered)) {
            return q;
        }
    }
    return NULL;
}

static void evt_add(struct ppc_intel *in, const struct ppc_ev_obs *o, unsigned metric)
{
    struct ppc_in_evt *e = &in->evts[in->nevt++ & EVT_MASK];

    e->t_ms = o->t_ms;
    e->seq = o->seq;
    e->metric = (uint8_t)metric;
}

void ppc_intel_note(struct ppc_intel *in, const struct ppc_ev_obs *o)
{
    struct ppc_in_sec *x = at(in, o->t_ms);
    uint16_t id = (uint16_t)(o->a & 0xffffu);
    struct ppc_in_q *q;

    if (o->t_ms > in->now_ms) {
        in->now_ms = o->t_ms;
    }
    switch (o->kind) {
    case PPC_EV_DHCP_DISCOVER:
        if (x) {
            bump16(&x->dhcp_disc);
        }
        evt_add(in, o, PPC_INM_DHCP_DISCOVER);
        break;
    case PPC_EV_ARP_REQUEST:
        if (x) {
            bump16(&x->arp_req);
        }
        evt_add(in, o, PPC_INM_ARP_REQUEST);
        break;
    case PPC_EV_DNS_RETRY:
        if (!(o->flags & PPC_EVF_OWN)) {
            break;
        }
        q = q_find(in, id, o->t_ms, 0);
        if (q) {
            if (q->retries < 0xffu) {
                q->retries++;
            }
            if (x) {
                bump16(&x->dns_retry);
            }
            break;
        }
        /* A retry of a question no longer held is a question. */
        /* fall through */
    case PPC_EV_DNS_QUERY:
        if (!(o->flags & PPC_EVF_OWN)) {
            break; /* another device's question: its answer may never pass here */
        }
        q = &in->qs[in->nq++ & Q_MASK];
        memset(q, 0, sizeof(*q));
        q->t_ms = o->t_ms;
        q->seq = o->seq;
        q->id = id;
        if (x) {
            bump16(&x->dns_q);
        }
        break;
    case PPC_EV_DNS_ANSWER:
    case PPC_EV_DNS_FAILURE:
        if (!(o->flags & PPC_EVF_OWN) || (o->flags & PPC_EVF_UNPAIRED)) {
            break; /* not this device's, or nothing to time it by */
        }
        q = q_find(in, id, o->t_ms, 1);
        if (!q) {
            break; /* later than the wait: the question stays unanswered within it */
        }
        q->answered = o->kind == PPC_EV_DNS_FAILURE ? 2u : 1u;
        q->ans_seq = o->seq;
        q->rtt_us = o->b ? o->b : (o->t_ms - q->t_ms) * 1000u;
        if (x) {
            bump16(&x->dns_ans);
            if (q->answered == 2u) {
                bump16(&x->dns_fail);
            }
        }
        break;
    default:
        break;
    }
}

/* ---- loss readings ------------------------------------------------------------------------- */

static void read_push(struct ppc_intel *in, uint32_t from, uint32_t to, uint32_t lost)
{
    struct ppc_in_read *r;

    if (in->nread >= PPC_IN_READINGS) {
        /* The oldest interval leaves: nothing before its end is covered any more. */
        in->reads_from_ms = in->reads[in->nread & READ_MASK].to_ms;
    }
    r = &in->reads[in->nread++ & READ_MASK];
    r->from_ms = from;
    r->to_ms = to;
    r->lost = lost;
}

void ppc_intel_counters(struct ppc_intel *in, uint32_t now_ms, int ok, uint64_t lost_total)
{
    uint64_t d;

    if (in->stopped) {
        return;
    }
    if (now_ms > in->now_ms) {
        in->now_ms = now_ms;
    }
    if (!ok) {
        in->failing = 1;
        return;
    }
    if (now_ms < in->read_ms) {
        now_ms = in->read_ms;
    }
    d = lost_total > in->lost_total ? lost_total - in->lost_total : 0;
    if (d > 0xffffffffu) {
        d = 0xffffffffu;
    }
    if (d) {
        /* Only an interval in which frames were lost needs keeping: the rest
         * is known from read_ms and reads_from_ms alone. */
        read_push(in, in->read_ms, now_ms, (uint32_t)d);
    }
    in->read_ms = now_ms;
    in->lost_total = lost_total;
    in->failing = 0;
}

/* ---- statistics ---------------------------------------------------------------------------- */

static uint32_t to_e6(double p)
{
    if (!(p > 0.0)) {
        return 0;
    }
    if (p >= 1.0) {
        return 1000000u;
    }
    return (uint32_t)(p * 1e6 + 0.5);
}

static double log_binom_pmf(uint32_t i, uint32_t n, double lp, double lq)
{
    return lgamma((double)n + 1.0) - lgamma((double)i + 1.0) - lgamma((double)(n - i) + 1.0) + i * lp + (n - i) * lq;
}

static double binom_sum(uint32_t i0, uint32_t i1, uint32_t n, double p)
{
    double lp = log(p);
    double lq = log1p(-p);
    double s = 0.0;
    uint32_t i;

    for (i = i0; i <= i1 && i <= n; i++) {
        s += exp(log_binom_pmf(i, n, lp, lq));
    }
    return s > 1.0 ? 1.0 : s;
}

uint32_t ppc_in_binom_le_e6(uint32_t k, uint32_t n, double p)
{
    if (k >= n || p <= 0.0) {
        return 1000000u;
    }
    if (p >= 1.0) {
        return 0;
    }
    return to_e6(binom_sum(0, k, n, p));
}

uint32_t ppc_in_binom_ge_e6(uint32_t k, uint32_t n, double p)
{
    if (k == 0 || p >= 1.0) {
        return 1000000u;
    }
    if (p <= 0.0 || k > n) {
        return 0;
    }
    return to_e6(binom_sum(k, n, n, p));
}

/* Poisson terms beyond this many standard deviations are left out. */
static double poisson_sum(uint32_t i0, uint32_t i1, double mu)
{
    double lmu = log(mu);
    double sd = sqrt(mu);
    double lo = mu - 40.0 * sd - 40.0;
    double hi = mu + 40.0 * sd + 40.0;
    double s = 0.0;
    uint32_t i;

    if (lo > (double)i0) {
        i0 = (uint32_t)lo;
    }
    if (hi < (double)i1) {
        i1 = (uint32_t)hi;
    }
    for (i = i0; i <= i1; i++) {
        s += exp(-mu + i * lmu - lgamma((double)i + 1.0));
    }
    return s > 1.0 ? 1.0 : s;
}

uint32_t ppc_in_poisson_ge_e6(uint32_t c, double mu)
{
    if (c == 0) {
        return 1000000u;
    }
    if (!(mu > 0.0)) {
        return 0;
    }
    if (isinf(mu)) {
        return 1000000u;
    }
    return to_e6(poisson_sum(c, 0xffffffffu, mu));
}

uint32_t ppc_in_poisson_le_e6(uint32_t c, double mu)
{
    if (!(mu > 0.0) || isinf(mu)) {
        return mu > 0.0 ? 0u : 1000000u;
    }
    return to_e6(poisson_sum(0, c, mu));
}

static void wilson(uint32_t k, uint32_t n, double *lo, double *hi)
{
    double z = Z_BOUND;
    double ph = (double)k / n;
    double d = 1.0 + z * z / n;
    double c = ph + z * z / (2.0 * n);
    double w = z * sqrt(ph * (1.0 - ph) / n + z * z / (4.0 * n * n));

    *lo = (c - w) / d;
    *hi = (c + w) / d;
    if (*lo < 0.0 || k == 0) {
        *lo = 0.0;
    }
    if (*hi > 1.0 || k >= n) {
        *hi = 1.0; /* exactly: a base that answered everything cannot be beaten */
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;

    return x < y ? -1 : x > y;
}

static uint32_t median(uint32_t *v, uint32_t n)
{
    qsort(v, n, sizeof(*v), cmp_u32);
    return n % 2u ? v[n / 2u] : (uint32_t)(((uint64_t)v[n / 2u - 1u] + v[n / 2u]) / 2u);
}

/* Mann-Whitney U, normal approximation with ties and continuity: the
 * one-sided p-values that b's values are greater (gt) and less (lt) than
 * a's. a and b are sorted. */
static void mann_whitney(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb, uint32_t *gt_e6,
                         uint32_t *lt_e6)
{
    double rank_b = 0.0;
    double ties = 0.0;
    double n = (double)na + nb;
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t pos = 1;
    double u, mean, var, z;

    while (i < na || j < nb) {
        uint32_t v = (j >= nb || (i < na && a[i] <= b[j])) ? a[i] : b[j];
        uint32_t ca = 0;
        uint32_t cb = 0;
        double r;

        while (i < na && a[i] == v) {
            i++;
            ca++;
        }
        while (j < nb && b[j] == v) {
            j++;
            cb++;
        }
        r = pos + (ca + cb - 1u) / 2.0;
        rank_b += r * cb;
        ties += (double)(ca + cb) * (ca + cb) * (ca + cb) - (ca + cb);
        pos += ca + cb;
    }
    u = rank_b - nb * (nb + 1.0) / 2.0;
    mean = na * (double)nb / 2.0;
    var = na * (double)nb / 12.0 * ((n + 1.0) - ties / (n * (n - 1.0)));
    if (!(var > 0.0)) {
        *gt_e6 = *lt_e6 = 1000000u;
        return;
    }
    z = (u - mean - 0.5) / sqrt(var);
    *gt_e6 = to_e6(0.5 * erfc(z / sqrt(2.0)));
    z = (u - mean + 0.5) / sqrt(var);
    *lt_e6 = to_e6(0.5 * erfc(-z / sqrt(2.0)));
}

/* ---- windows ------------------------------------------------------------------------------- */

/* The first second whose start lies at or after t_ms: seconds belong to the
 * window their start is in, so adjacent windows share none. */
static uint32_t sec_ceil(uint32_t t_ms)
{
    return t_ms / 1000u + (t_ms % 1000u ? 1u : 0u);
}

static uint32_t obs_end(const struct ppc_intel *in)
{
    return in->stopped ? in->stop_ms : in->now_ms;
}

static void loss_of(const struct ppc_intel *in, uint32_t a, uint32_t b, struct ppc_in_vis *v)
{
    uint32_t n = in->nread < PPC_IN_READINGS ? in->nread : PPC_IN_READINGS;
    uint32_t i;

    v->lost_lo = v->lost_hi = 0;
    v->loss_known = in->reads_from_ms <= a && in->read_ms >= b;
    for (i = 0; i < n; i++) {
        const struct ppc_in_read *r = &in->reads[(in->nread - n + i) & READ_MASK];

        if (!r->lost || r->to_ms <= a || r->from_ms >= b) {
            continue;
        }
        v->lost_hi += r->lost;
        if (r->from_ms >= a && r->to_ms <= b) {
            v->lost_lo += r->lost;
        }
    }
}

/* What the capture saw of [a, b): the series' seconds, and the loss bound of
 * [a, loss_b) (later than b for answers that may still come). */
static void vis_of(const struct ppc_intel *in, uint32_t a, uint32_t b, uint32_t loss_b, struct ppc_in_vis *v,
                   uint32_t sums[6])
{
    uint32_t s;
    uint32_t s1 = sec_ceil(b);

    memset(v, 0, sizeof(*v));
    if (sums) {
        memset(sums, 0, 6 * sizeof(uint32_t));
    }
    v->from_ms = a;
    v->to_ms = b;
    for (s = sec_ceil(a); s < s1; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);

        if (!x) {
            v->gap_s++;
            continue;
        }
        if (x->flags == PPC_EVS_WATCHED) {
            v->watched_s++;
        } else if (x->flags & PPC_EVS_HELD) {
            v->held_s++;
        } else if (x->flags & PPC_EVS_SHED) {
            v->shed_s++;
        } else {
            v->gap_s++; /* before the first tick: nothing known of it */
        }
        v->frames += x->frames;
        v->own_in += x->own_in;
        v->own_out += x->own_out;
        if (sums) {
            sums[0] += x->dhcp_disc;
            sums[1] += x->arp_req;
        }
    }
    loss_of(in, a, loss_b, v);
}

/* The lost frames of [a, b) by the second: each loss interval's count
 * spread over the seconds it covers by the frames seen in them (by time
 * where none were seen). out[i] is second sec_ceil(a) + i. Returns the
 * number of seconds, at most max. */
static uint32_t loss_by_second(const struct ppc_intel *in, uint32_t a, uint32_t b, double *out, uint32_t max)
{
    uint32_t s0 = sec_ceil(a);
    uint32_t s1 = sec_ceil(b);
    uint32_t n = in->nread < PPC_IN_READINGS ? in->nread : PPC_IN_READINGS;
    uint32_t ns = s1 > s0 ? s1 - s0 : 0u;
    uint32_t i;
    uint32_t s;

    if (ns > max) {
        ns = max;
    }
    for (i = 0; i < ns; i++) {
        out[i] = 0.0;
    }
    for (i = 0; i < n; i++) {
        const struct ppc_in_read *r = &in->reads[(in->nread - n + i) & READ_MASK];
        uint32_t r0 = r->from_ms / 1000u;
        uint32_t r1 = sec_ceil(r->to_ms);
        double seen = 0.0;

        if (!r->lost || r->to_ms <= a || r->from_ms >= b) {
            continue;
        }
        for (s = r0; s < r1; s++) {
            const struct ppc_in_sec *x = sec_slot(in, s);

            seen += x ? x->frames : 0u;
        }
        for (s = r0 > s0 ? r0 : s0; s < r1 && s < s0 + ns; s++) {
            const struct ppc_in_sec *x = sec_slot(in, s);
            double part = seen > 0.0 ? (x ? x->frames : 0u) / seen : 1.0 / (double)(r1 - r0);

            out[s - s0] += r->lost * part;
        }
    }
    return ns;
}

/* What a stream counted by the second (DHCP DISCOVERs: 0, ARP requests: 1)
 * may have lost in [a, b), at its own arrival times: in each second, the
 * second's lost frames times the stream's part of the frames seen in it.
 * Kernel drops are by arrival time, not by frame kind, so a stream spread
 * over the minute loses what its own seconds lost, not the share that
 * bursts of other traffic put on the window (G0.md §6). A second that lost
 * frames and saw none is given the stream's part of the whole window.
 * Infinite when the losses are not known. */
static double stream_lost(const struct ppc_intel *in, uint32_t a, uint32_t b, unsigned which, int known)
{
    double lost[PPC_IN_WINDOW_MS / 1000u + 2u];
    uint32_t s0 = sec_ceil(a);
    uint32_t ns;
    uint32_t i;
    double fw = 0.0;
    double nw = 0.0;
    double e = 0.0;

    if (!known) {
        return INFINITY;
    }
    ns = loss_by_second(in, a, b, lost, (uint32_t)(sizeof(lost) / sizeof(lost[0])));
    for (i = 0; i < ns; i++) {
        const struct ppc_in_sec *x = sec_slot(in, s0 + i);

        if (x) {
            fw += x->frames;
            nw += which ? x->arp_req : x->dhcp_disc;
        }
    }
    for (i = 0; i < ns; i++) {
        const struct ppc_in_sec *x = sec_slot(in, s0 + i);
        double f = x ? x->frames : 0.0;
        double k = x ? (which ? x->arp_req : x->dhcp_disc) : 0.0;

        if (lost[i] <= 0.0) {
            continue;
        }
        e += f > 0.0 ? k * lost[i] / f : (fw > 0.0 ? lost[i] * nw / fw : lost[i]);
    }
    return e;
}

static uint32_t span_s(const struct ppc_in_vis *v)
{
    return v->watched_s + v->held_s + v->shed_s + v->gap_s;
}

/* The loss share's bounds: lost frames among all that arrived. Unknown
 * losses bound it by 1 above. */
static void loss_share(const struct ppc_in_vis *v, double *lo, double *hi)
{
    double f = v->frames;

    *lo = v->lost_lo ? v->lost_lo / (v->lost_lo + f) : 0.0;
    *hi = !v->loss_known ? 1.0 : v->lost_hi ? v->lost_hi / (v->lost_hi + f) : 0.0;
}

/* A question whose answer was not seen is called unanswered only when its
 * wait was watched whole, every second from it to its end, and the way back
 * to this device was seen working in it: some frame other than a DNS answer
 * reached this device. Without that witness, a resolver that did not answer
 * and replies that took another path look the same, and the question is
 * left undecided (E3a-R: one-sided visibility starting inside a window). */
static int wait_watched(const struct ppc_intel *in, uint32_t t_ms)
{
    uint32_t s;
    int back = 0;

    for (s = t_ms / 1000u; s <= (t_ms + PPC_IN_WAIT_MS) / 1000u; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);

        if (!x || x->flags != PPC_EVS_WATCHED) {
            return 0;
        }
        back |= x->own_in > x->dns_ans;
    }
    return back;
}

struct qside {
    uint32_t npairs;
    uint32_t decided, answered, answers, failed, undecided, retries;
    uint32_t oldest_ok; /* the ring still held the window's start */
    uint32_t rtt[PPC_IN_QUESTIONS];
};

static void questions(const struct ppc_intel *in, uint32_t a, uint32_t b, struct qside *q)
{
    uint32_t n = in->nq < PPC_IN_QUESTIONS ? in->nq : PPC_IN_QUESTIONS;
    uint32_t end = obs_end(in);
    uint32_t i;

    memset(q, 0, offsetof(struct qside, rtt));
    q->oldest_ok = in->nq <= PPC_IN_QUESTIONS || in->qs[in->nq & Q_MASK].t_ms <= a;
    for (i = 0; i < n; i++) {
        const struct ppc_in_q *x = &in->qs[(in->nq - n + i) & Q_MASK];

        if (x->t_ms < a || x->t_ms >= b) {
            continue;
        }
        if (x->retries) {
            q->retries++;
        }
        if (x->answered) {
            q->decided++;
            q->answered++;
            q->answers++;
            q->failed += x->answered == 2u;
            q->rtt[q->npairs++] = x->rtt_us;
        } else if (x->t_ms + PPC_IN_WAIT_MS <= end && wait_watched(in, x->t_ms)) {
            q->decided++;
        } else {
            q->undecided++;
        }
    }
}

/* ---- comparisons --------------------------------------------------------------------------- */

static unsigned metric_class(unsigned m)
{
    return m == PPC_INM_DHCP_DISCOVER ? PPC_CLASS_DHCP : m == PPC_INM_ARP_REQUEST ? PPC_CLASS_ARP : PPC_CLASS_DNS;
}

static const struct ppc_in_mark *mark_of(const struct ppc_intel *in, uint32_t id)
{
    unsigned i;

    for (i = 0; i < PPC_IN_MARKS; i++) {
        if (in->marks[i].id && in->marks[i].id == id) {
            return &in->marks[i];
        }
    }
    return NULL;
}

static void windows(const struct ppc_in_mark *m, unsigned pair, uint32_t end, uint32_t w[2][2])
{
    uint32_t at = m->at_ms;
    int64_t b0 = pair ? (int64_t)at - PPC_IN_WINDOW_MS : (int64_t)at - PPC_IN_WINDOW_MS - PPC_IN_REF_MS;
    int64_t b1 = pair ? (int64_t)at : (int64_t)at - PPC_IN_WINDOW_MS;
    int64_t c1 = pair ? (int64_t)at + PPC_IN_WINDOW_MS : (int64_t)at;
    unsigned i;

    w[0][0] = b0 < 0 ? 0u : (uint32_t)b0;
    w[0][1] = b1 < 0 ? 0u : (uint32_t)b1;
    w[1][0] = w[0][1];
    w[1][1] = c1 < 0 ? 0u : (uint32_t)c1;
    for (i = 0; i < 2; i++) {
        if (w[i][1] > end) {
            w[i][1] = end;
        }
        if (w[i][0] > w[i][1]) {
            w[i][0] = w[i][1];
        }
    }
}

/* Refs: the slowest answers, the questions not answered, the failures. */
static void dns_refs(const struct ppc_intel *in, uint32_t a, uint32_t b, unsigned metric, struct ppc_in_cmp *c)
{
    uint32_t n = in->nq < PPC_IN_QUESTIONS ? in->nq : PPC_IN_QUESTIONS;
    uint32_t end = obs_end(in);
    uint32_t best[PPC_IN_REFS] = { 0 };
    unsigned nr = 0;
    uint32_t i;
    unsigned j;

    for (i = 0; i < n; i++) {
        const struct ppc_in_q *x = &in->qs[(in->nq - n + i) & Q_MASK];

        if (x->t_ms < a || x->t_ms >= b) {
            continue;
        }
        if (metric == PPC_INM_DNS_TIME && x->answered) {
            /* keep the PPC_IN_REFS slowest, slowest first, the earlier on a tie */
            for (j = 0; j < nr; j++) {
                if (x->rtt_us > best[j]) {
                    break;
                }
            }
            if (j < PPC_IN_REFS) {
                unsigned k = nr < PPC_IN_REFS ? nr : PPC_IN_REFS - 1u;

                for (; k > j; k--) {
                    best[k] = best[k - 1u];
                    c->refs[k] = c->refs[k - 1u];
                }
                best[j] = x->rtt_us;
                c->refs[j] = x->ans_seq;
                if (nr < PPC_IN_REFS) {
                    nr++;
                }
            }
        } else if (metric == PPC_INM_DNS_ANSWERED && !x->answered && nr < PPC_IN_REFS &&
                   x->t_ms + PPC_IN_WAIT_MS <= end && wait_watched(in, x->t_ms)) {
            c->refs[nr++] = x->seq;
        } else if (metric == PPC_INM_DNS_FAILED && x->answered == 2u && nr < PPC_IN_REFS) {
            c->refs[nr++] = x->ans_seq;
        }
    }
}

static void evt_refs(const struct ppc_intel *in, uint32_t a, uint32_t b, unsigned metric, struct ppc_in_cmp *c)
{
    uint32_t n = in->nevt < PPC_IN_EVENTS ? in->nevt : PPC_IN_EVENTS;
    unsigned nr = 0;
    uint32_t i;

    for (i = 0; i < n && nr < PPC_IN_REFS; i++) {
        const struct ppc_in_evt *e = &in->evts[(in->nevt - n + i) & EVT_MASK];

        /* by its second, as the counts are */
        if (e->metric == metric && e->t_ms / 1000u >= sec_ceil(a) && e->t_ms / 1000u < sec_ceil(b)) {
            c->refs[nr++] = e->seq;
        }
    }
}

/* A loss share is an interval [lo, hi] (ppc_intel.h). Each test is run at
 * both ends of it: at the end that favours no change (cons) and at the end
 * that favours a change (aggr). A change is claimed only when cons rejects;
 * when only aggr does, the loss could explain - or hide - the change, and it
 * abstains. No change is said only when neither end rejects. A loss not
 * known is the bound [lo, 1]: it abstains only where everything lost could
 * hide a change still possible, and then says the losses are not known. */
static void settle(struct ppc_in_cmp *c, uint32_t cons_w, uint32_t cons_b, uint32_t aggr_w, uint32_t aggr_b,
                   uint32_t rule_out)
{
    int known = c->vis[0].loss_known && c->vis[1].loss_known;

    if (cons_w < PPC_IN_ALPHA_E6) {
        c->verdict = PPC_INV_WORSE;
        c->p_e6 = cons_w;
    } else if (cons_b < PPC_IN_ALPHA_E6) {
        c->verdict = PPC_INV_BETTER;
        c->p_e6 = cons_b;
    } else if (aggr_w < PPC_IN_ALPHA_E6 || aggr_b < PPC_IN_ALPHA_E6) {
        c->verdict = PPC_INV_CANT_TELL;
        c->reason = known ? PPC_INR_LOSS_EXPLAINS : PPC_INR_LOSS_UNKNOWN;
        c->p_e6 = cons_w < cons_b ? cons_w : cons_b;
    } else if (rule_out >= PPC_IN_ALPHA_E6) {
        /* No test rejected, but the window cannot rule out a material
         * worsening either: saying "no change" would reassure falsely. */
        c->verdict = PPC_INV_CANT_TELL;
        c->reason = PPC_INR_WEAK;
        c->p_e6 = rule_out;
    } else {
        c->verdict = PPC_INV_NO_CHANGE;
        c->p_e6 = cons_w < cons_b ? cons_w : cons_b;
    }
}

/* What a share or a rate seen at loss f1 in the window would be expected
 * to be, given the base's x0 seen at loss f0: x0 (1 - f1) / (1 - f0).
 * Everything lost in the base (f0 = 1) leaves it unbounded. */
static double scaled(double x0, double f0, double f1)
{
    return f0 >= 1.0 ? INFINITY : x0 * (1.0 - f1) / (1.0 - f0);
}

static double cap1(double p)
{
    return p > 1.0 ? 1.0 : p;
}

/* A share whose harmful direction is lower (dir -1: answered) or higher
 * (+1: failed), with loss bounds f[side][lo, hi]; all 0 when loss leaves
 * the share as it was. "No change" also needs the window to rule out a
 * material worsening (PPC_IN_MATERIAL_*), at the loss that favours it.
 * unseen[side] are the questions or answers the share could not judge,
 * counted as works against each verdict (G0.md §6):
 *   answered  questions left undecided (no watched wait, no way back
 *             seen) may have been answered or not. Only an answered
 *             question is always decided, so leaving them out would
 *             favour "answered": "fewer" counts the window's as answered
 *             and the base's as not; "more" and "no change" the reverse;
 *   failed    answers not seen (lost, or never sent) may all have been
 *             failures: "fewer failures" and "no change" count them so. */
static void decide_share(struct ppc_in_cmp *c, int dir, const double f[2][2], const uint32_t unseen[2])
{
    double lo0, hi0;
    uint32_t k = c->k[1];
    uint32_t n = c->n[1];
    uint32_t u0 = unseen[0];
    uint32_t u1 = unseen[1];

    wilson(c->k[0], c->n[0], &lo0, &hi0);
    if (dir < 0) {
        /* The base's true share, corrected for its loss, is at most 1: a
         * window seen under loss can be expected to show at most 1 - f1 of
         * it. worse = fewer: cons expects the fewest, aggr the most. better
         * is impossible against a base whose true share may already be 1. */
        double lo0w, hi0w, lo0a, hi0a;
        double t_cw, t_aw, t_cb, t_ab, t_ro, alt;

        wilson(c->k[0], c->n[0] + u0, &lo0w, &hi0w);      /* the base's lowest */
        wilson(c->k[0] + u0, c->n[0] + u0, &lo0a, &hi0a); /* the base's highest */
        t_cw = cap1(scaled(lo0w, f[0][0], 0.0));
        t_aw = cap1(scaled(lo0, f[0][1], 0.0));
        t_cb = cap1(scaled(hi0a, f[0][1], 0.0));
        t_ab = cap1(scaled(hi0, f[0][0], 0.0));
        t_ro = cap1(scaled(hi0a, f[0][0], 0.0));
        alt = t_ro * PPC_IN_MATERIAL_SHARE_PCT / 100.0 * (1.0 - f[1][0]);
        settle(c, ppc_in_binom_le_e6(k + u1, n + u1, t_cw * (1.0 - f[1][1])),
               t_cb < 1.0 ? ppc_in_binom_ge_e6(k, n + u1, t_cb * (1.0 - f[1][0])) : 1000000u,
               ppc_in_binom_le_e6(k, n, t_aw * (1.0 - f[1][0])),
               t_ab < 1.0 ? ppc_in_binom_ge_e6(k, n, t_ab * (1.0 - f[1][1])) : 1000000u,
               ppc_in_binom_ge_e6(k, n + u1, alt));
    } else {
        double alt = cap1(hi0 + PPC_IN_MATERIAL_FAIL_PCT / 100.0);

        settle(c, ppc_in_binom_ge_e6(k, n, hi0), ppc_in_binom_le_e6(k + u1, n + u1, lo0),
               ppc_in_binom_ge_e6(k, n, hi0), ppc_in_binom_le_e6(k, n, lo0),
               alt >= 1.0 ? 1000000u : ppc_in_binom_le_e6(k + u1, n + u1, alt));
    }
}

/* A rate whose harmful direction is higher: events over watched seconds.
 * Loss hides events, so a rise can be hidden and a fall can be made. Each
 * side's true count lies between what was seen and that plus what the
 * stream may have lost (lost[side]: the larger of its own seconds' share
 * and the traffic's share; infinite when unknown). A verdict must hold at
 * the bound that works against it (G0.md §6):
 *   worse      the window's count as seen (it can only have been more)
 *              against the base's highest;
 *   better     the window's highest against the base as seen;
 *   no change  the window's highest below a material rise over the base
 *              as seen (PPC_IN_MATERIAL_RATE times, or
 *              PPC_IN_MATERIAL_RATE_MIN more a minute, whichever is more).
 * The window's losses never make a rise: a claim of more events stands on
 * events seen. Where only the traffic-weighted share (loss spread evenly
 * over kinds and times) would decide, the verdict is CAN'T TELL. */
static void decide_rate(struct ppc_in_cmp *c, const double f[2][2], const double lost[2])
{
    double c0 = c->k[0];
    double w0 = c->n[0];
    double w1 = c->n[1];
    double hi0 = c0 + lost[0];
    double up = (hi0 + Z_BOUND * sqrt(hi0 + 1.0) + 1.0) / w0 * w1;
    double up_seen = (c0 + Z_BOUND * sqrt(c0 + 1.0) + 1.0) / w0 * w1;
    double dn = c0 - Z_BOUND * sqrt(c0 + 1.0);
    double r0 = c0 / w0;
    double alt = r0 * PPC_IN_MATERIAL_RATE;
    double hi1 = c->k[1] + lost[1];
    uint32_t c1 = c->k[1];
    uint32_t c1hi = isfinite(hi1) && hi1 < 4.0e9 ? (uint32_t)ceil(hi1) : 0xffffffffu;

    if (alt < r0 + PPC_IN_MATERIAL_RATE_MIN / 60.0) {
        alt = r0 + PPC_IN_MATERIAL_RATE_MIN / 60.0;
    }
    dn = dn > 0.0 ? dn / w0 * w1 : 0.0;
    settle(c, isfinite(up) ? ppc_in_poisson_ge_e6(c1, up) : 1000000u,
           dn > 0.0 && c1hi != 0xffffffffu ? ppc_in_poisson_le_e6(c1hi, dn) : 1000000u,
           f[1][1] >= 1.0 ? 0u : ppc_in_poisson_ge_e6(c1, scaled(up_seen, f[0][0], f[1][1])),
           dn > 0.0 ? ppc_in_poisson_le_e6(c1, scaled(dn, f[0][1], f[1][0])) : 1000000u,
           c1hi == 0xffffffffu ? 1000000u : ppc_in_poisson_le_e6(c1hi, alt * w1));
}

/* Answer times: medians and Mann-Whitney over the answers seen. The answers
 * not seen - a question decided unanswered: its answer lost, or slower than
 * the wait - may be exactly the slow ones (selective loss: frames drop when
 * the path is busiest; a slow resolver times out). So every verdict must
 * survive counting them as slower than anything seen:
 *   slower   the base's unseen answers count as slow (it cannot have been
 *            faster than its seen answers say);
 *   faster   the window's unseen answers count as slow;
 *   no change  fewer than half the window's answers above the practical
 *            bound, its unseen answers counted above it (sign test).
 * A difference the answers seen show but the unseen ones could undo is
 * CAN'T TELL with PPC_INR_UNSEEN. A significant difference below the
 * practical bound is a small change, never "no change". */
static uint32_t augment(const uint32_t *seen, uint32_t n, uint32_t missing, uint32_t *out)
{
    uint32_t i;

    memcpy(out, seen, n * sizeof(*out));
    for (i = 0; i < missing && n + i < PPC_IN_QUESTIONS; i++) {
        out[n + i] = 0xffffffffu; /* slower than any answer seen */
    }
    return n + i;
}

static int slower(uint32_t gt, uint64_t m0, uint64_t m1)
{
    return gt < PPC_IN_ALPHA_E6 && m1 * 100u >= m0 * PPC_IN_SLOWER_PCT && m1 >= m0 + PPC_IN_SLOWER_US;
}

static void decide_time(struct ppc_in_cmp *c, struct qside q[2])
{
    static uint32_t a0[PPC_IN_QUESTIONS], a1[PPC_IN_QUESTIONS];
    uint32_t miss0 = q[0].decided - q[0].answered;
    uint32_t miss1 = q[1].decided - q[1].answered;
    uint32_t gt, lt, cgt, clt, dummy;
    uint32_t m0, m1, n0, n1, cm0, cm1;
    uint64_t bound;
    uint32_t above = 0;
    uint32_t i;

    m0 = median(q[0].rtt, q[0].npairs); /* sorts both */
    m1 = median(q[1].rtt, q[1].npairs);
    c->stat[0] = m0;
    c->stat[1] = m1;
    mann_whitney(q[0].rtt, q[0].npairs, q[1].rtt, q[1].npairs, &gt, &lt);
    /* conservative: slower against the base with its unseen answers slow */
    n0 = augment(q[0].rtt, q[0].npairs, miss0, a0);
    cm0 = n0 % 2u ? a0[n0 / 2u] : (uint32_t)(((uint64_t)a0[n0 / 2u - 1u] + a0[n0 / 2u]) / 2u);
    mann_whitney(a0, n0, q[1].rtt, q[1].npairs, &cgt, &dummy);
    /* conservative: faster with the window's unseen answers slow */
    n1 = augment(q[1].rtt, q[1].npairs, miss1, a1);
    cm1 = n1 % 2u ? a1[n1 / 2u] : (uint32_t)(((uint64_t)a1[n1 / 2u - 1u] + a1[n1 / 2u]) / 2u);
    mann_whitney(q[0].rtt, q[0].npairs, a1, n1, &dummy, &clt);
    bound = (uint64_t)m0 * PPC_IN_SLOWER_PCT / 100u;
    if (bound < (uint64_t)m0 + PPC_IN_SLOWER_US) {
        bound = (uint64_t)m0 + PPC_IN_SLOWER_US;
    }
    for (i = 0; i < q[1].npairs; i++) {
        above += q[1].rtt[i] > bound;
    }
    if (miss0 || miss1) {
        c->notes |= PPC_INN_UNSEEN;
    }
    if (slower(cgt, cm0, m1)) {
        c->verdict = PPC_INV_WORSE;
        c->p_e6 = cgt;
    } else if (cm1 != 0xffffffffu && slower(clt, cm1, m0)) {
        c->verdict = PPC_INV_BETTER;
        c->p_e6 = clt;
    } else if (slower(gt, m0, m1) || slower(lt, m1, m0)) {
        c->verdict = PPC_INV_CANT_TELL;
        c->reason = PPC_INR_UNSEEN;
        c->p_e6 = gt < lt ? gt : lt;
    } else if (cgt < PPC_IN_ALPHA_E6 || clt < PPC_IN_ALPHA_E6) {
        c->verdict = PPC_INV_SMALL;
        c->p_e6 = cgt < clt ? cgt : clt;
    } else if (ppc_in_binom_le_e6(above + miss1, q[1].npairs + miss1, 0.5) >= PPC_IN_ALPHA_E6) {
        c->verdict = PPC_INV_CANT_TELL;
        c->reason = PPC_INR_WEAK;
        c->p_e6 = ppc_in_binom_le_e6(above + miss1, q[1].npairs + miss1, 0.5);
    } else {
        c->verdict = PPC_INV_NO_CHANGE;
        c->p_e6 = gt < lt ? gt : lt;
    }
}

static int one_way(const struct ppc_in_vis *v)
{
    return v->own_out >= ONE_WAY_MIN_OUT && v->own_in == 0;
}

/* One-sided visibility that starts inside a window: a run of watched
 * seconds in which this device sent at least ONE_WAY_MIN_OUT frames and
 * received none, longer than its own inbound rate makes plausible
 * (P(no frame) <= 1/1000). The rate is the busier window's of [a, m) and
 * [m, b), so a silence that fills much of one window does not lower its own
 * yardstick, thinned by the most the window may have lost. E3a-R found the
 * whole-window ratio alone misses this, and a rate over both windows too
 * (N1.1.md). */
static int one_way_spell(const struct ppc_intel *in, uint32_t a, uint32_t m, uint32_t b, double f_hi)
{
    uint32_t s1 = sec_ceil(b);
    uint32_t sm = sec_ceil(m);
    uint64_t tin[2] = { 0, 0 };
    uint64_t tother[2] = { 0, 0 };
    uint32_t ws[2] = { 0, 0 };
    uint32_t run = 0;
    uint32_t out = 0;
    double lam = 0.0;
    uint32_t s;
    int other;
    unsigned w;

    for (s = sec_ceil(a); s < s1; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);

        if (x && x->flags == PPC_EVS_WATCHED) {
            w = s >= sm;
            tin[w] += x->own_in;
            tother[w] += x->own_in > x->dns_ans ? x->own_in - x->dns_ans : 0u;
            ws[w]++;
        }
    }
    /* Inbound other than DNS answers, where there is any, so that a resolver
     * that stops answering is not taken for a path that stops delivering. */
    other = tother[0] + tother[1] > 0;
    for (w = 0; w < 2; w++) {
        double r = ws[w] ? (double)(other ? tother[w] : tin[w]) / ws[w] : 0.0;

        if (r > lam) {
            lam = r;
        }
    }
    if (!(lam > 0.0) || f_hi >= 1.0) {
        return 0;
    }
    lam *= 1.0 - f_hi;
    for (s = sec_ceil(a); s < s1; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);
        uint32_t got;

        if (!x || x->flags != PPC_EVS_WATCHED) {
            continue; /* not watched: neither breaks nor lengthens a run */
        }
        got = other ? (x->own_in > x->dns_ans ? x->own_in - x->dns_ans : 0u) : x->own_in;
        if (got) {
            run = out = 0;
            continue;
        }
        run++;
        out += x->own_out;
        if (out >= ONE_WAY_MIN_OUT && lam * run >= ONE_WAY_LOG_P) {
            return 1;
        }
    }
    return 0;
}

/* Gaps in what reached the capture at all: runs of watched seconds in
 * which no frame arrived except this device's own, and none was lost
 * either (the readings say so), longer than the busier window's arrival
 * rate makes plausible (P(no frame) <= 1/1000), thinned only by the loss
 * the readings prove. A receive path that stopped (one-sided visibility)
 * hides broadcasts as well as this device's replies, and the lost-frame
 * counter does not see it: the frames never reached the interface
 * (G0.md §6). Such seconds were not seen, as seconds on HOLD were not:
 * gap[i] is set for second sec_ceil(a) + i of every such run, whole. A
 * second whose silence the readings explain (frames lost in it), or whose
 * losses are not known, neither lengthens nor breaks a run: the rate's
 * loss bounds speak for those. Returns the number of gap seconds. */
static uint32_t recv_gaps(const struct ppc_intel *in, uint32_t a, uint32_t m, uint32_t b, const double f[2][2],
                          uint8_t *gap, uint32_t max)
{
    double lost[2u * (PPC_IN_WINDOW_MS / 1000u) + 2u];
    uint32_t s0 = sec_ceil(a);
    uint32_t s1 = sec_ceil(b);
    uint32_t sm = sec_ceil(m);
    uint32_t nl = loss_by_second(in, a, b, lost, (uint32_t)(sizeof(lost) / sizeof(lost[0])));
    uint64_t got[2] = { 0, 0 };
    uint32_t ws[2] = { 0, 0 };
    uint32_t run_s[2u * (PPC_IN_WINDOW_MS / 1000u) + 2u];
    uint32_t run = 0;
    uint32_t total = 0;
    double lam = 0.0;
    uint32_t s;
    uint32_t i;
    unsigned w;

    for (i = 0; i < max; i++) {
        gap[i] = 0;
    }
    for (s = s0; s < s1; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);

        if (x && x->flags == PPC_EVS_WATCHED) {
            w = s >= sm;
            got[w] += x->frames > x->own_out ? x->frames - x->own_out : 0u;
            ws[w]++;
        }
    }
    for (w = 0; w < 2; w++) {
        double r = ws[w] ? (double)got[w] / ws[w] * (1.0 - f[w][0]) : 0.0;

        if (r > lam) {
            lam = r;
        }
    }
    if (!(lam > 0.0)) {
        return 0;
    }
    for (s = s0; s < s1 && s - s0 < max; s++) {
        const struct ppc_in_sec *x = sec_slot(in, s);

        if (!x || x->flags != PPC_EVS_WATCHED) {
            continue;
        }
        if (x->frames > x->own_out) {
            run = 0;
            continue;
        }
        if ((s - s0 < nl && lost[s - s0] > 0.0) || s * 1000u < in->reads_from_ms || (s + 1u) * 1000u > in->read_ms) {
            continue; /* explained by loss, or its losses not known */
        }
        if (run < sizeof(run_s) / sizeof(run_s[0])) {
            run_s[run] = s;
        }
        run++;
        if (lam * run >= ONE_WAY_LOG_P) {
            uint32_t n = run < sizeof(run_s) / sizeof(run_s[0]) ? run : (uint32_t)(sizeof(run_s) / sizeof(run_s[0]));

            for (i = 0; i < n; i++) {
                if (!gap[run_s[i] - s0]) {
                    gap[run_s[i] - s0] = 1;
                    total++;
                }
            }
        }
    }
    return total;
}

static void compute(const struct ppc_intel *in, const struct ppc_in_mark *m, unsigned pair, unsigned metric,
                    struct ppc_in_cmp *c)
{
    static struct qside q[2]; /* large; used by one comparison at a time (no threads here) */
    uint32_t w[2][2];
    uint32_t sums[2][6];
    double f[2][2];
    uint8_t gap[2u * (PPC_IN_WINDOW_MS / 1000u) + 2u];
    uint32_t gaps[2] = { 0, 0 };
    unsigned cls = metric_class(metric);
    int dns = cls == PPC_CLASS_DNS;
    uint32_t end = obs_end(in);
    unsigned i;

    memset(c, 0, sizeof(*c));
    c->mark_id = m->id;
    c->pair = (uint8_t)pair;
    c->metric = (uint8_t)metric;
    c->p_e6 = 1000000u;
    windows(m, pair, end, w);
    for (i = 0; i < 2; i++) {
        uint32_t lb = w[i][1];

        if (metric == PPC_INM_DNS_ANSWERED) {
            lb = w[i][1] + PPC_IN_WAIT_MS < end ? w[i][1] + PPC_IN_WAIT_MS : end;
            if (lb < w[i][1]) {
                lb = w[i][1];
            }
        }
        vis_of(in, w[i][0], w[i][1], lb, &c->vis[i], sums[i]);
        if (dns) {
            questions(in, w[i][0], w[i][1], &q[i]);
            c->undecided[i] = q[i].undecided;
        }
    }
    if (pair == 1 && in->stopped && w[1][1] < m->at_ms + PPC_IN_WINDOW_MS) {
        c->notes |= PPC_INN_STOPPED;
    }
    for (i = 0; i < 2; i++) {
        if (c->vis[i].held_s || c->vis[i].shed_s) {
            c->notes |= PPC_INN_UNWATCHED;
        }
        if (c->vis[i].lost_hi || !c->vis[i].loss_known) {
            c->notes |= PPC_INN_LOSS;
        }
        if (dns && q[i].undecided) {
            c->notes |= PPC_INN_UNDECIDED;
        }
        if (dns && q[i].retries) {
            c->notes |= PPC_INN_RETRIES;
        }
        loss_share(&c->vis[i], &f[i][0], &f[i][1]);
    }
    /* what was counted */
    switch (metric) {
    case PPC_INM_DNS_TIME:
        for (i = 0; i < 2; i++) {
            c->n[i] = q[i].npairs;
        }
        break;
    case PPC_INM_DNS_ANSWERED:
        for (i = 0; i < 2; i++) {
            c->n[i] = q[i].decided;
            c->k[i] = q[i].answered;
            c->stat[i] = q[i].decided ? q[i].answered * 1000u / q[i].decided : 0;
        }
        break;
    case PPC_INM_DNS_FAILED:
        for (i = 0; i < 2; i++) {
            c->n[i] = q[i].answers;
            c->k[i] = q[i].failed;
            c->stat[i] = q[i].answers ? q[i].failed * 1000u / q[i].answers : 0;
        }
        break;
    default:
        for (i = 0; i < 2; i++) {
            c->n[i] = c->vis[i].watched_s;
            c->k[i] = sums[i][metric == PPC_INM_DHCP_DISCOVER ? 0 : 1];
        }
        if (recv_gaps(in, w[0][0], w[1][0], w[1][1], (const double(*)[2])f, gap,
                      (uint32_t)(sizeof(gap) / sizeof(gap[0])))) {
            /* the gaps' seconds and events leave both counts */
            uint32_t s0 = sec_ceil(w[0][0]);
            uint32_t sm = sec_ceil(w[1][0]);
            uint32_t s;

            for (s = s0; s < sec_ceil(w[1][1]) && s - s0 < sizeof(gap); s++) {
                const struct ppc_in_sec *x = sec_slot(in, s);

                if (gap[s - s0] && x) {
                    unsigned side = s >= sm;

                    c->n[side]--;
                    c->k[side] -= metric == PPC_INM_DHCP_DISCOVER ? x->dhcp_disc : x->arp_req;
                    gaps[side]++;
                }
            }
            c->notes |= PPC_INN_UNWATCHED;
        }
        for (i = 0; i < 2; i++) {
            c->stat[i] = c->n[i] ? c->k[i] * 6000u / c->n[i] : 0;
        }
        break;
    }
    if (dns) {
        dns_refs(in, w[1][0], w[1][1], metric, c);
    } else {
        evt_refs(in, w[1][0], w[1][1], metric, c);
    }
    /* why it cannot compare, the first that applies */
    if (ppc_cfilter_excludes((enum ppc_cfilter)in->cfilter) & (1u << cls)) {
        c->reason = PPC_INR_NOT_CAPTURED;
    } else if (in->unanalyzed & (1u << cls)) {
        c->reason = PPC_INR_NOT_ANALYZED;
    } else if (span_s(&c->vis[0]) < PPC_IN_MIN_WATCHED_S || span_s(&c->vis[1]) < PPC_IN_MIN_WATCHED_S) {
        c->reason = PPC_INR_CUT;
    } else if (c->vis[0].gap_s || c->vis[1].gap_s || in->reads_from_ms > w[0][0] ||
               (dns && (!q[0].oldest_ok || !q[1].oldest_ok))) {
        c->reason = PPC_INR_HISTORY;
    } else if (c->vis[0].watched_s * 2u < span_s(&c->vis[0]) || c->vis[1].watched_s * 2u < span_s(&c->vis[1])) {
        c->reason = PPC_INR_NOT_WATCHED;
    } else if (dns && (one_way(&c->vis[0]) || one_way(&c->vis[1]) || one_way_spell(in, w[0][0], w[1][0], w[1][1], f[1][1]))) {
        c->reason = PPC_INR_ONE_WAY;
    } else if (!dns && ((gaps[0] && c->n[0] * 2u < span_s(&c->vis[0])) ||
                        (gaps[1] && c->n[1] * 2u < span_s(&c->vis[1])))) {
        c->reason = PPC_INR_ONE_WAY; /* a receive gap over half a window */
    } else if (metric == PPC_INM_DNS_TIME && (c->n[0] < PPC_IN_MIN_PAIRS || c->n[1] < PPC_IN_MIN_PAIRS)) {
        c->reason = PPC_INR_TOO_FEW;
    } else if (metric == PPC_INM_DNS_ANSWERED && (c->n[0] < PPC_IN_MIN_QUESTIONS || c->n[1] < PPC_IN_MIN_QUESTIONS)) {
        c->reason = PPC_INR_TOO_FEW;
    } else if (metric == PPC_INM_DNS_FAILED && (c->n[0] < PPC_IN_MIN_ANSWERS || c->n[1] < PPC_IN_MIN_ANSWERS)) {
        c->reason = PPC_INR_TOO_FEW;
    } else if (!dns && (c->n[0] < PPC_IN_MIN_WATCHED_S || c->n[1] < PPC_IN_MIN_WATCHED_S)) {
        c->reason = PPC_INR_TOO_FEW;
    }
    if (c->reason) {
        c->verdict = PPC_INV_CANT_TELL;
        if (metric == PPC_INM_DNS_TIME && q[0].npairs && q[1].npairs) {
            c->stat[0] = median(q[0].rtt, q[0].npairs);
            c->stat[1] = median(q[1].rtt, q[1].npairs);
        }
    } else if (metric == PPC_INM_DNS_TIME) {
        decide_time(c, q);
    } else if (metric == PPC_INM_DNS_ANSWERED) {
        const uint32_t und[2] = { q[0].undecided, q[1].undecided };

        decide_share(c, -1, f, und);
    } else if (metric == PPC_INM_DNS_FAILED) {
        /* uniform loss leaves the failure share of the answers seen as it
         * was; the answers not seen are counted against "fewer" and "no change" */
        const double none[2][2] = { { 0.0, 0.0 }, { 0.0, 0.0 } };
        const uint32_t unseen[2] = { q[0].decided - q[0].answered + q[0].undecided,
                                     q[1].decided - q[1].answered + q[1].undecided };

        decide_share(c, +1, none, unseen);
    } else {
        unsigned which = metric == PPC_INM_ARP_REQUEST;
        double lost[2];

        for (i = 0; i < 2; i++) {
            double own = stream_lost(in, w[i][0], w[i][1], which, c->vis[i].loss_known);
            double share = f[i][1] >= 1.0 ? INFINITY : c->k[i] * f[i][1] / (1.0 - f[i][1]);

            lost[i] = own > share ? own : share;
        }
        decide_rate(c, f, lost);
    }
    c->quality = c->verdict == PPC_INV_CANT_TELL                                              ? PPC_INQ_INSUFFICIENT
                 : (c->notes & (PPC_INN_UNWATCHED | PPC_INN_LOSS | PPC_INN_UNDECIDED)) != 0u ? PPC_INQ_DEGRADED
                                                                                             : PPC_INQ_GOOD;
}

int ppc_intel_compare(const struct ppc_intel *in, uint32_t mark_id, unsigned pair, unsigned metric,
                      struct ppc_in_cmp *out)
{
    const struct ppc_in_mark *m = mark_of(in, mark_id);
    uint32_t i;

    if (!m || pair > 1u || metric >= PPC_INM_COUNT) {
        return -1;
    }
    if (m->done[pair]) {
        uint32_t n = in->nledger < PPC_IN_LEDGER ? in->nledger : PPC_IN_LEDGER;

        for (i = 0; i < n; i++) {
            const struct ppc_in_cmp *c = &in->ledger[(in->nledger - n + i) & LEDGER_MASK];

            if (c->mark_id == mark_id && c->pair == pair && c->metric == metric) {
                *out = *c;
                return 0;
            }
        }
    }
    compute(in, m, pair, metric, out);
    return 0;
}

/* ---- making it final ----------------------------------------------------------------------- */

static void finalize(struct ppc_intel *in, struct ppc_in_mark *m, unsigned pair)
{
    unsigned metric;

    for (metric = 0; metric < PPC_INM_COUNT; metric++) {
        struct ppc_in_cmp *c = &in->ledger[in->nledger++ & LEDGER_MASK];

        compute(in, m, pair, metric, c);
        c->id = in->next_id++;
    }
    m->done[pair] = 1;
}

/* A pair is final once its window, and the wait of its last questions, has
 * passed, and a good reading at or after that end has told the losses; or
 * once the capture has stopped. */
static void advance(struct ppc_intel *in)
{
    unsigned i;
    unsigned pair;

    for (i = 0; i < PPC_IN_MARKS; i++) {
        struct ppc_in_mark *m = &in->marks[(in->mark_slot + i) % PPC_IN_MARKS];

        if (!m->id) {
            continue;
        }
        for (pair = 0; pair < 2; pair++) {
            uint32_t due = m->at_ms + (pair ? PPC_IN_WINDOW_MS : 0u) + PPC_IN_WAIT_MS;

            if (!m->done[pair] && (in->stopped || (in->now_ms >= due && in->read_ms >= due))) {
                finalize(in, m, pair);
            }
        }
    }
}

void ppc_intel_tick(struct ppc_intel *in, uint32_t now_ms, unsigned flags)
{
    struct ppc_in_sec *x;

    if (in->stopped) {
        return;
    }
    if (now_ms > in->now_ms) {
        in->now_ms = now_ms;
    }
    fill_to(in, in->now_ms / 1000u);
    x = sec_slot(in, in->cur_sec);
    if (x) {
        x->flags |= in->cur_flags;
        x->flags |= (uint8_t)flags;
    }
    in->cur_flags = (uint8_t)flags;
    advance(in);
}

void ppc_intel_stop(struct ppc_intel *in, uint32_t now_ms)
{
    if (in->stopped) {
        return;
    }
    if (now_ms > in->now_ms) {
        in->now_ms = now_ms;
    }
    fill_to(in, in->now_ms / 1000u);
    in->stopped = 1;
    in->stop_ms = in->now_ms;
    advance(in);
}

void ppc_intel_mark(struct ppc_intel *in, uint32_t mark_id, uint32_t at_ms)
{
    struct ppc_in_mark *m = &in->marks[in->mark_slot];

    memset(m, 0, sizeof(*m));
    m->id = mark_id;
    m->at_ms = at_ms;
    in->mark_slot = (in->mark_slot + 1u) % PPC_IN_MARKS;
    if (in->stopped) {
        advance(in);
    }
}

const struct ppc_in_cmp *ppc_intel_ledger(const struct ppc_intel *in, uint32_t i)
{
    uint32_t n = in->nledger < PPC_IN_LEDGER ? in->nledger : PPC_IN_LEDGER;

    return i < n ? &in->ledger[(in->nledger - n + i) & LEDGER_MASK] : NULL;
}

uint32_t ppc_intel_ledger_count(const struct ppc_intel *in)
{
    return in->nledger < PPC_IN_LEDGER ? in->nledger : PPC_IN_LEDGER;
}

/* ---- references ---------------------------------------------------------------------------- */

unsigned ppc_intel_alive(const struct ppc_store *s, uint32_t seq)
{
    const struct ppc_pin *pin = NULL;
    const struct ppc_record *r;

    if (!s || !seq) {
        return PPC_EV_GONE;
    }
    r = ppc_view_record(s, seq, &pin);
    if (!r) {
        return PPC_EV_GONE;
    }
    return pin || r->cap_len == 0 || ppc_store_has_bytes(s, r) ? PPC_EV_WHOLE : PPC_EV_SUMMARY;
}

/* ---- words for the ledger's text (tests, the docs) -------------------------------------------- */

const char *ppc_in_metric_id(unsigned m)
{
    static const char *const id[PPC_INM_COUNT] = { "dns.time", "dns.answered", "dns.failed", "dhcp.discover",
                                                   "arp.request" };

    return m < PPC_INM_COUNT ? id[m] : "?";
}

const char *ppc_in_verdict_id(unsigned v)
{
    static const char *const id[] = { "pending", "no-change", "worse", "better", "cant-tell", "small" };

    return v <= PPC_INV_SMALL ? id[v] : "?";
}

const char *ppc_in_quality_id(unsigned q)
{
    static const char *const id[] = { "good", "degraded", "insufficient" };

    return q <= PPC_INQ_INSUFFICIENT ? id[q] : "?";
}

const char *ppc_in_reason_id(unsigned r)
{
    static const char *const id[PPC_INR_COUNT] = { "-",        "not-captured", "not-analyzed", "cut",
                                                   "history",  "not-watched",  "one-way",      "loss-unknown",
                                                   "too-few",  "loss-explains", "weak",        "unseen" };

    return r < PPC_INR_COUNT ? id[r] : "?";
}
