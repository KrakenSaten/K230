/*
 * PacketShark's WHEN face, its words: see psk_in.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_in.h"

#include <stdio.h>
#include <string.h>

#define DOT " \xc2\xb7 "
#define ARROW " \xe2\x86\x92 "

static const char *const metric_names[PPC_IN_METRICS] = { "DNS ANSWER TIME", "DNS ANSWERS SEEN",
                                                          "DNS FAILURES", "DHCP DISCOVERS", "ARP REQUESTS" };

/* The engine's reasons (enum ppc_in_reason, by number), said. */
static const char *const reasons[12] = {
    "",
    "THE CAPTURE FILTER LEFT THESE OUT",
    "THIS CAPTURE COULD NOT ANALYSE THEM",
    "TOO LITTLE OF A WINDOW WAS CAPTURED",
    "THE CAPTURE NO LONGER HOLDS ALL OF A WINDOW",
    "MOST OF A WINDOW WAS ON HOLD OR NOT WATCHED",
    "ONLY ONE DIRECTION IS VISIBLE HERE",
    "FRAMES WERE LOST AND HOW MANY IS NOT KNOWN",
    "TOO FEW TO COMPARE",
    "THE FRAMES LOST COULD ACCOUNT FOR A CHANGE",
    "TOO FEW TO RULE OUT A CHANGE",
    "ANSWERS NOT SEEN COULD CHANGE THE RESULT",
};

const char *psk_in_metric(unsigned metric)
{
    return metric < PPC_IN_METRICS ? metric_names[metric] : "";
}

static void ms_text(uint32_t us, char *b, size_t n)
{
    if (us >= 10000000u) {
        snprintf(b, n, "%u S", (unsigned)(us / 1000000u));
    } else if (us >= 1000u) {
        snprintf(b, n, "%u MS", (unsigned)((us + 500u) / 1000u));
    } else {
        snprintf(b, n, "%u \xc2\xb5S", (unsigned)us);
    }
}

static void rate_text(uint32_t x100, char *b, size_t n)
{
    if (x100 >= 1000u || x100 % 100u == 0) {
        snprintf(b, n, "%u", (unsigned)((x100 + 50u) / 100u));
    } else {
        snprintf(b, n, "%u.%u", (unsigned)(x100 / 100u), (unsigned)((x100 % 100u) / 10u));
    }
}

const char *psk_in_figures(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen)
{
    char a[24];
    char b[24];

    buf[0] = '\0';
    switch (c->metric) {
    case PSK_INM_DNS_TIME:
        if (!c->n[0] && !c->n[1]) {
            snprintf(buf, buflen, "ANSWER TIMES: NONE SEEN");
            break;
        }
        if (c->n[0]) {
            ms_text(c->stat[0], a, sizeof(a));
        } else {
            snprintf(a, sizeof(a), "\xe2\x80\x93");
        }
        if (c->n[1]) {
            ms_text(c->stat[1], b, sizeof(b));
        } else {
            snprintf(b, sizeof(b), "\xe2\x80\x93");
        }
        snprintf(buf, buflen, "MEDIAN %s" ARROW "%s" DOT "%u" ARROW "%u ANSWERS", a, b, (unsigned)c->n[0],
                 (unsigned)c->n[1]);
        break;
    case PSK_INM_DNS_ANSWERED:
        snprintf(buf, buflen, "%u OF %u" ARROW "%u OF %u QUESTIONS", (unsigned)c->k[0], (unsigned)c->n[0],
                 (unsigned)c->k[1], (unsigned)c->n[1]);
        break;
    case PSK_INM_DNS_FAILED:
        snprintf(buf, buflen, "%u OF %u" ARROW "%u OF %u ANSWERS FAILED", (unsigned)c->k[0], (unsigned)c->n[0],
                 (unsigned)c->k[1], (unsigned)c->n[1]);
        break;
    case PSK_INM_DHCP:
    case PSK_INM_ARP:
        rate_text(c->stat[0], a, sizeof(a));
        rate_text(c->stat[1], b, sizeof(b));
        snprintf(buf, buflen, "%s" ARROW "%s A MINUTE" DOT "%u" ARROW "%u SEEN", a, b, (unsigned)c->k[0],
                 (unsigned)c->k[1]);
        break;
    default:
        break;
    }
    return buf;
}

static size_t add(char *buf, size_t buflen, size_t o, const char *s)
{
    int k;

    if (o >= buflen) {
        return o;
    }
    k = snprintf(buf + o, buflen - o, "%s%s", o ? DOT : "", s);
    return k < 0 ? o : o + (size_t)k;
}

const char *psk_in_evidence(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen, bool *limited)
{
    char t[48];
    size_t o = 0;

    buf[0] = '\0';
    *limited = c->quality != PSK_INQ_GOOD || c->verdict == PSK_INV_CANT_TELL;
    if (c->verdict == PSK_INV_PENDING) {
        snprintf(buf, buflen, "NOT YET COMPARED");
        *limited = true;
        return buf;
    }
    if (c->verdict == PSK_INV_CANT_TELL || c->quality == PSK_INQ_INSUFFICIENT) {
        o = add(buf, buflen, o, "NOT ENOUGH EVIDENCE");
        if (c->reason == 9 && !(c->notes & 0x02u)) {
            /* The engine's "frames lost could account for it" on a pair
             * that lost nothing: only the test without the practical bound
             * saw a rise (the N1.2 bench, finding E1). Said as what it is. */
            o = add(buf, buflen, o, "TOO BURSTY TO TELL A CHANGE FROM ORDINARY VARIATION");
        } else if (c->reason > 0 && c->reason < 12) {
            o = add(buf, buflen, o, reasons[c->reason]);
        }
    } else if (c->quality == PSK_INQ_DEGRADED) {
        o = add(buf, buflen, o, "WATCHED WITH LIMITS");
    } else {
        o = add(buf, buflen, o, "WATCHED WHOLE");
    }
    if (c->notes & 0x01u) {
        o = add(buf, buflen, o, "NOT ALL OF IT WATCHED");
    }
    if (c->notes & 0x02u) {
        o = add(buf, buflen, o, "FRAMES LOST");
    }
    if ((c->notes & 0x04u) && (c->und[0] || c->und[1])) {
        snprintf(t, sizeof(t), "%u QUESTIONS LEFT OUT", (unsigned)(c->und[0] + c->und[1]));
        o = add(buf, buflen, o, t);
    }
    if (c->notes & 0x10u) {
        o = add(buf, buflen, o, "THE CAPTURE STOPPED INSIDE THE WINDOW");
    }
    if (c->notes & 0x20u) {
        o = add(buf, buflen, o, "ANSWERS NOT SEEN COUNTED AS THE SLOWEST");
    }
    if (c->metric == PSK_INM_DNS_ANSWERED && c->n[0] && c->n[1] &&
        (uint64_t)c->k[1] * c->n[0] < (uint64_t)c->k[0] * c->n[1]) {
        /* Fewer answers seen is not fewer answers: one by another path
         * does not pass this interface (the N1.2 bench, finding E2). */
        o = add(buf, buflen, o, "AN ANSWER BY ANOTHER PATH IS NOT SEEN HERE");
        *limited = true;
    }
    (void)o;
    return buf;
}

const char *psk_in_verdict(const struct ppc_evt_in_cmp *c, bool final)
{
    static const char *const worse[PPC_IN_METRICS] = { "SLOWER", "FEWER ANSWERED", "MORE FAILURES",
                                                       "MORE DISCOVERS", "MORE ARP REQUESTS" };
    static const char *const better[PPC_IN_METRICS] = { "FASTER", "MORE ANSWERED", "FEWER FAILURES",
                                                        "FEWER DISCOVERS", "FEWER ARP REQUESTS" };

    if (!PSK_IN_VERDICTS || !final || c->metric >= PPC_IN_METRICS) {
        return "";
    }
    switch (c->verdict) {
    case PSK_INV_WORSE:
        return worse[c->metric];
    case PSK_INV_BETTER:
        return better[c->metric];
    case PSK_INV_NO_CHANGE:
        return "NO CHANGE";
    case PSK_INV_SMALL:
        return "SMALL CHANGE";
    case PSK_INV_CANT_TELL:
        return "CAN'T TELL";
    default:
        return "";
    }
}

const char *psk_in_refs(const struct ppc_evt_in_cmp *c, char *buf, size_t buflen, bool *expired)
{
    static const char *const alive[3] = { "GONE (EXPIRED)", "SUMMARY ONLY", "WHOLE" };
    size_t o = 0;
    unsigned i;

    buf[0] = '\0';
    *expired = false;
    for (i = 0; i < PPC_IN_REF_MAX; i++) {
        int k;

        if (!c->ref[i]) {
            continue;
        }
        *expired |= c->alive[i] == 0;
        k = snprintf(buf + o, buflen - o, "%s#%u %s", o ? DOT : "PACKETS ", (unsigned)c->ref[i],
                     alive[c->alive[i] <= 2 ? c->alive[i] : 0]);
        if (k < 0 || (size_t)k >= buflen - o) {
            break;
        }
        o += (size_t)k;
    }
    return buf;
}

static void side_text(const struct ppc_evt_in_vis *v, const char *name, char *b, size_t n, bool *limited)
{
    size_t o;
    int k;

    k = snprintf(b, n, "%s %u S WATCHED", name, (unsigned)v->watched_s);
    o = k > 0 ? (size_t)k : 0;
    if (v->held_s && o < n) {
        k = snprintf(b + o, n - o, ", %u S ON HOLD", (unsigned)v->held_s);
        o += k > 0 ? (size_t)k : 0;
        *limited = true;
    }
    if (v->shed_s && o < n) {
        k = snprintf(b + o, n - o, ", %u S COUNTED ONLY", (unsigned)v->shed_s);
        o += k > 0 ? (size_t)k : 0;
        *limited = true;
    }
    if (v->gap_s && o < n) {
        k = snprintf(b + o, n - o, ", %u S NOT HELD", (unsigned)v->gap_s);
        o += k > 0 ? (size_t)k : 0;
        *limited = true;
    }
    if (!v->loss_known && o < n) {
        snprintf(b + o, n - o, ", LOSSES NOT KNOWN");
        *limited = true;
    } else if (v->lost_hi && o < n) {
        if (v->lost_lo == v->lost_hi) {
            snprintf(b + o, n - o, ", %u FRAMES LOST", (unsigned)v->lost_hi);
        } else {
            snprintf(b + o, n - o, ", UP TO %u FRAMES LOST", (unsigned)v->lost_hi);
        }
        *limited = true;
    }
}

const char *psk_in_seeing(const struct psk_insight *in, char *buf, size_t buflen, bool *limited)
{
    char s0[96];
    char s1[96];
    unsigned i;

    *limited = false;
    buf[0] = '\0';
    if (in->nvis < 2) {
        return buf;
    }
    side_text(&in->vis[0], in->head.pair ? "BEFORE" : "REFERENCE", s0, sizeof(s0), limited);
    side_text(&in->vis[1], in->head.pair ? "AFTER" : "BEFORE", s1, sizeof(s1), limited);
    snprintf(buf, buflen, "%s" DOT "%s", s0, s1);
    for (i = 0; i < 2; i++) {
        if (in->vis[i].own_out >= 8u && in->vis[i].own_in == 0) {
            size_t o = strlen(buf);

            snprintf(buf + o, buflen - o, DOT "NOTHING ADDRESSED TO THIS DEVICE SEEN %s",
                     i ? (in->head.pair ? "AFTER" : "BEFORE") : (in->head.pair ? "BEFORE" : "IN THE REFERENCE"));
            *limited = true;
        }
    }
    return buf;
}

const char *psk_in_waiting(void)
{
    return "Comparing the minute before the mark with the minute after.";
}

const char *psk_in_head(const struct psk_insight *in, char *buf, size_t buflen)
{
    char s[96];

    snprintf(buf, buflen, "%s" DOT "OBSERVATIONS ONLY, NO VERDICTS YET", psk_in_summary(in, s, sizeof(s)));
    return buf;
}

const char *psk_in_pair_a(const struct psk_insight *in, char *buf, size_t buflen)
{
    char s[96];

    snprintf(buf, buflen, "BEFORE THE MARK, AGAINST THE TWO MINUTES BEFORE IT" DOT "%s", psk_in_summary(in, s, sizeof(s)));
    return buf;
}

bool psk_in_complete(const struct psk_insight *in)
{
    return in->head.mark && in->nvis == 2 && in->have == (1u << PPC_IN_METRICS) - 1u;
}

const char *psk_in_summary(const struct psk_insight *in, char *buf, size_t buflen)
{
    unsigned limited = 0;
    unsigned i;

    buf[0] = '\0';
    if (!psk_in_complete(in)) {
        return buf;
    }
    for (i = 0; i < PPC_IN_METRICS; i++) {
        bool lim;
        char t[160];

        psk_in_evidence(&in->cmp[i], t, sizeof(t), &lim);
        limited += lim;
    }
    if (!in->head.final) {
        snprintf(buf, buflen, "WINDOW STILL FILLING" DOT "%u WITH LIMITS SO FAR", limited);
    } else if (limited) {
        snprintf(buf, buflen, "%u OBSERVED" DOT "%u WITH LIMITS", (unsigned)PPC_IN_METRICS, limited);
    } else {
        snprintf(buf, buflen, "%u OBSERVED" DOT "ALL WATCHED WHOLE", (unsigned)PPC_IN_METRICS);
    }
    return buf;
}
