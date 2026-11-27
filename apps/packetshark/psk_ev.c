/*
 * PacketShark's marks and their evidence in words. See psk_ev.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L /* localtime_r */

#include "psk_ev.h"

#include "psk_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DOT " \xc2\xb7 "

/* ---- small things ----------------------------------------------------------------------- */

static void more(char *buf, size_t buflen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void more(char *buf, size_t buflen, const char *fmt, ...)
{
    size_t o = strlen(buf);
    va_list ap;

    if (o + 1 >= buflen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + o, buflen - o, fmt, ap);
    va_end(ap);
}

/* One more part of a line of parts, after a middle dot when it is not the
 * first. */
static void part(char *buf, size_t buflen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void part(char *buf, size_t buflen, const char *fmt, ...)
{
    size_t o = strlen(buf);
    va_list ap;

    if (o && o + sizeof(DOT) < buflen) {
        memcpy(buf + o, DOT, sizeof(DOT));
        o += sizeof(DOT) - 1;
    }
    if (o + 1 >= buflen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + o, buflen - o, fmt, ap);
    va_end(ap);
}

/* A sentence more, on a line of its own after what is there. */
static void line(char *buf, size_t buflen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void line(char *buf, size_t buflen, const char *fmt, ...)
{
    size_t o = strlen(buf);
    va_list ap;

    if (o && o + 2 < buflen) {
        buf[o++] = '\n';
        buf[o] = '\0';
    }
    if (o + 1 >= buflen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + o, buflen - o, fmt, ap);
    va_end(ap);
}

/* A window's length as it is said: "60 S", "8 S", "UNDER 1 S". */
static const char *span_text(uint32_t ms, char *buf, size_t buflen)
{
    if (ms < 1000u) {
        snprintf(buf, buflen, "UNDER 1 S");
    } else {
        snprintf(buf, buflen, "%u S", (unsigned)((ms + 500u) / 1000u));
    }
    return buf;
}

/* The same inside a sentence: "8 s", "under a second". */
static const char *span_lower(uint32_t ms, char *buf, size_t buflen)
{
    if (ms < 1000u) {
        snprintf(buf, buflen, "under a second");
    } else {
        snprintf(buf, buflen, "%u s", (unsigned)((ms + 500u) / 1000u));
    }
    return buf;
}

/* A figure's unit as a sentence writes it: "31 ms", "24 h". */
static const char *lower(const char *text, char *buf)
{
    char *p;

    if (text != buf) {
        return text;
    }
    for (p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    return buf;
}

/* A response time inside a sentence: "31 ms". */
static const char *ms_lower(uint64_t us, char *buf, size_t buflen)
{
    return lower(psk_fmt_ms(us, buf, buflen), buf);
}

static int has(const struct ppc_evt_ev_obs *o, unsigned flag)
{
    return (o->flags & flag) != 0;
}

/* Who sent a client's message or an ARP frame, as a row names it. */
static const char *who(const struct ppc_evt_ev_obs *o)
{
    return has(o, PPC_EVF_OWN) ? "THIS DEVICE" : o->mac[0] ? o->mac : "A DEVICE";
}

/* The same inside a sentence. */
static const char *who_lower(const struct ppc_evt_ev_obs *o)
{
    return has(o, PPC_EVF_OWN) ? "this device" : o->mac[0] ? o->mac : "a device";
}

static uint32_t total(const struct ppc_evt_ev_win *w)
{
    uint32_t n = 0;
    unsigned i;

    for (i = 1; i < PPC_EV_KINDS; i++) {
        n += w->count[i];
    }
    return n;
}

/* ---- what the capture could see ---------------------------------------------------------- */

static const uint8_t ev_order[3] = { PPC_CLASS_DHCP, PPC_CLASS_ARP, PPC_CLASS_DNS };

/* The protocols the capture filter kept out of the capture. */
static uint8_t excluded_of(const struct psk_evidence *ev)
{
    return (uint8_t)(ppc_cfilter_excludes((enum ppc_cfilter)ev->head.cfilter) & PPC_EV_CLASSES);
}

/* The protocols captured but not analysed. */
static uint8_t unanalyzed_of(const struct psk_evidence *ev)
{
    return (uint8_t)(ev->head.unanalyzed & PPC_EV_CLASSES & ~excluded_of(ev));
}

static uint8_t blind_of(const struct psk_evidence *ev)
{
    return (uint8_t)(excluded_of(ev) | unanalyzed_of(ev));
}

/* "DHCP, ARP OR DNS", "DHCP OR DNS", "DNS": the protocols of mask, in their
 * order, joined by word ("OR", "AND"; "or", "and" inside a sentence). */
static const char *class_list(uint8_t mask, const char *word, char *buf, size_t buflen)
{
    unsigned n = 0;
    unsigned k = 0;
    unsigned i;

    buf[0] = '\0';
    for (i = 0; i < 3; i++) {
        n += (mask >> ev_order[i]) & 1u;
    }
    for (i = 0; i < 3; i++) {
        if (!((mask >> ev_order[i]) & 1u)) {
            continue;
        }
        k++;
        if (k > 1 && k == n) {
            more(buf, buflen, " %s ", word);
        } else if (k > 1) {
            more(buf, buflen, ", ");
        }
        more(buf, buflen, "%s", psk_class_word(ev_order[i]));
    }
    return buf;
}

/* What is not known of a window's losses: a spell without a counter
 * reading still open in it (or around its mark), or spells the record did
 * not keep. */
static int loss_unknown(const struct psk_evidence *ev, unsigned side, uint32_t *from)
{
    const struct ppc_evt_ev_win *w = &ev->win[side ? 1 : 0];

    if (w->unknown) {
        *from = w->unknown_from_ms;
        return 1;
    }
    if (ev->head.edge == PPC_EV_EDGE_UNKNOWN && w->span_ms) {
        *from = ev->head.edge_from_ms;
        return 1;
    }
    return 0;
}

/* Whether anything limits what a window says of its frames. */
static int loss_limited(const struct psk_evidence *ev, unsigned side)
{
    const struct ppc_evt_ev_win *w = &ev->win[side ? 1 : 0];
    uint32_t from;

    return w->lost || w->lost_near || (w->flags & PPC_EVW_LOSSES_DROPPED) || loss_unknown(ev, side, &from) ||
           (ev->head.edge == PPC_EV_EDGE_COUNTED && ev->head.edge_lost) ||
           (ev->head.edge == PPC_EV_EDGE_UNKNOWN);
}

/* When the counters stopped being read, in a sentence: "04:10", or "the
 * start" when they never were. A window's own line says only LOSSES
 * UNKNOWN: a clock time in a narrow caption would break at its colon. */
static const char *since_text(uint32_t from, char *buf, size_t buflen)
{
    if (from == 0) {
        snprintf(buf, buflen, "the start");
        return buf;
    }
    return psk_fmt_elapsed(from, buf, buflen);
}

const char *psk_ev_filter(const struct psk_evidence *ev, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "CAPTURE FILTER" DOT "%s", psk_cfilter_label((enum ppc_cfilter)ev->head.cfilter));
    return buf;
}

uint8_t psk_ev_classes(void)
{
    return (uint8_t)(1u << PPC_CLASS_DHCP | 1u << PPC_CLASS_ARP | 1u << PPC_CLASS_DNS);
}

/* ---- the head ---------------------------------------------------------------------------- */

const char *psk_ev_title(const struct psk_evidence *ev, char *buf, size_t buflen)
{
    const struct ppc_evt_ev_head *h = &ev->head;

    if (h->last > h->first && h->id >= h->first) {
        snprintf(buf, buflen, "Mark %u of %u", (unsigned)(h->id - h->first + 1u), (unsigned)(h->last - h->first + 1u));
    } else {
        snprintf(buf, buflen, "Mark");
    }
    return buf;
}

const char *psk_ev_when(const struct psk_evidence *ev, char *buf, size_t buflen)
{
    const struct ppc_evt_ev_head *h = &ev->head;
    char a[24];

    buf[0] = '\0';
    part(buf, buflen, "%s INTO THE CAPTURE", psk_fmt_elapsed(h->at_ms, a, sizeof(a)));
    if (h->wall_s) {
        time_t t = (time_t)h->wall_s;
        struct tm tm;

        if (localtime_r(&t, &tm)) {
            part(buf, buflen, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
    }
    if (h->state == PPC_STATE_HOLD) {
        part(buf, buflen, "ON HOLD");
    } else if (h->state == PPC_STATE_STOPPED || h->state == PPC_STATE_SAVING) {
        part(buf, buflen, "CAPTURE STOPPED");
    }
    return buf;
}

/* ---- the windows ------------------------------------------------------------------------- */

unsigned psk_ev_window(const struct psk_evidence *ev, unsigned side, char *head, size_t headlen, char *why,
                       size_t whylen, char *seen, size_t seenlen)
{
    const struct ppc_evt_ev_win *w = &ev->win[side ? 1 : 0];
    uint32_t whole = w->span_ms < 1000u ? 1u : (w->span_ms + 500u) / 1000u; /* as its length is said */
    uint8_t excluded = excluded_of(ev);
    uint8_t unanalyzed = unanalyzed_of(ev);
    unsigned sev;
    char a[24];
    char c[40];

    head[0] = '\0';
    why[0] = '\0';
    seen[0] = '\0';
    if (w->span_ms == 0) {
        /* Nothing on this side: say why, and claim nothing about it. */
        if (side && (w->flags & PPC_EVW_OPEN)) {
            /* Just set: the window after it has begun. */
            snprintf(head, headlen, "0 S AFTER");
            snprintf(why, whylen, "STILL OPEN");
        } else if (side) {
            snprintf(head, headlen, "NOTHING AFTER");
            snprintf(why, whylen, "THE CAPTURE HAD STOPPED");
        } else {
            snprintf(head, headlen, "NOTHING BEFORE");
            snprintf(why, whylen, "SET AS THE CAPTURE STARTED");
        }
        return PSK_SEV_NOTE;
    }
    snprintf(head, headlen, "%s %s", span_text(w->span_ms, a, sizeof(a)), side ? "AFTER" : "BEFORE");
    if (w->flags & PPC_EVW_CUT_START) {
        snprintf(why, whylen, "THE CAPTURE STARTED THEN");
    } else if (w->flags & PPC_EVW_OPEN) {
        /* Open, not "watching": a capture on HOLD is not, and says so below. */
        snprintf(why, whylen, "STILL OPEN");
    } else if (w->flags & PPC_EVW_CUT_STOP) {
        snprintf(why, whylen, "THE CAPTURE STOPPED THEN");
    }
    if (w->lost) {
        part(seen, seenlen, "%s FRAME%s LOST", psk_fmt_count(w->lost, a, sizeof(a)), w->lost == 1 ? "" : "S");
    }
    /* Lost in a spell that reaches past the window's far end: apart. */
    if (w->lost_near) {
        part(seen, seenlen, "%s LOST NEAR ITS %s", psk_fmt_count(w->lost_near, a, sizeof(a)), side ? "END" : "START");
    }
    /* Frames lost while no reading could be made over the mark: both windows
     * touch them. Their number is told once, in the limits. */
    if (ev->head.edge == PPC_EV_EDGE_COUNTED && ev->head.edge_lost) {
        part(seen, seenlen, "FRAMES LOST AT THE MARK");
    }
    {
        uint32_t from;

        /* Since when is told in the limits. */
        if (loss_unknown(ev, side, &from)) {
            part(seen, seenlen, "LOSSES UNKNOWN");
        }
    }
    if (w->flags & PPC_EVW_LOSSES_DROPPED) {
        part(seen, seenlen, "LOSS RECORD INCOMPLETE");
    }
    /* Seconds are counted whole, so a window that is not on a second's edge
     * touches one more than it is long: never said as more than the length
     * the window's own heading gives. */
    if (w->shed_s) {
        part(seen, seenlen, "COUNTING ONLY %u S", (unsigned)(w->shed_s < whole ? w->shed_s : whole));
    }
    if (w->held_s) {
        part(seen, seenlen, "ON HOLD %u S", (unsigned)(w->held_s < whole ? w->held_s : whole));
    }
    if (w->flags & PPC_EVW_HISTORY) {
        part(seen, seenlen, "OLDEST NOT KEPT");
    }
    if (seen[0]) {
        sev = w->shed_s || loss_limited(ev, side) ? PSK_SEV_WARN : PSK_SEV_NOTE;
    } else {
        /* Every frame there was to see: under a capture filter, every frame
         * it let in. */
        snprintf(seen, seenlen, "%s", ev->head.cfilter != PPC_CFILTER_ALL ? "EVERY CAPTURED FRAME SEEN" : "EVERY FRAME SEEN");
        sev = PSK_SEV_OK;
    }
    /* And what the capture could not see, whatever the frames. */
    if (excluded) {
        part(seen, seenlen, "%s NOT CAPTURED", class_list(excluded, "AND", c, sizeof(c)));
    }
    if (unanalyzed) {
        part(seen, seenlen, "%s NOT ANALYZED", class_list(unanalyzed, "AND", c, sizeof(c)));
    }
    if ((excluded || unanalyzed) && sev == PSK_SEV_OK) {
        sev = PSK_SEV_NOTE;
    }
    return sev;
}

/* ---- the counts -------------------------------------------------------------------------- */

static const struct {
    uint8_t cls;
    uint8_t core;     /* of its protocol's ordinary exchange: listed at zero too */
    const char *word; /* under its protocol's name */
} kinds[PPC_EV_KINDS] = {
    [PPC_EV_DHCP_DISCOVER] = { PPC_CLASS_DHCP, 1, "DISCOVER" },
    [PPC_EV_DHCP_OFFER] = { PPC_CLASS_DHCP, 1, "OFFER" },
    [PPC_EV_DHCP_REQUEST] = { PPC_CLASS_DHCP, 1, "REQUEST" },
    [PPC_EV_DHCP_ACK] = { PPC_CLASS_DHCP, 1, "ACK" },
    [PPC_EV_DHCP_NAK] = { PPC_CLASS_DHCP, 0, "NAK" },
    [PPC_EV_DHCP_DECLINE] = { PPC_CLASS_DHCP, 0, "DECLINE" },
    [PPC_EV_DHCP_RELEASE] = { PPC_CLASS_DHCP, 0, "RELEASE" },
    [PPC_EV_DHCP_INFORM] = { PPC_CLASS_DHCP, 0, "INFORM" },
    [PPC_EV_ARP_REQUEST] = { PPC_CLASS_ARP, 1, "REQUEST" },
    [PPC_EV_ARP_REPLY] = { PPC_CLASS_ARP, 1, "REPLY" },
    [PPC_EV_ARP_PROBE] = { PPC_CLASS_ARP, 0, "PROBE" },
    [PPC_EV_ARP_ANNOUNCE] = { PPC_CLASS_ARP, 0, "ANNOUNCEMENT" },
    [PPC_EV_ARP_CHANGED] = { PPC_CLASS_ARP, 0, "ADDRESS CHANGED MAC" },
    [PPC_EV_ARP_TWO_MACS] = { PPC_CLASS_ARP, 0, "TWO MACS, ONE ADDRESS" },
    [PPC_EV_DNS_QUERY] = { PPC_CLASS_DNS, 1, "QUERY" },
    [PPC_EV_DNS_RETRY] = { PPC_CLASS_DNS, 0, "QUERY AGAIN" },
    [PPC_EV_DNS_ANSWER] = { PPC_CLASS_DNS, 1, "ANSWER" },
    [PPC_EV_DNS_FAILURE] = { PPC_CLASS_DNS, 0, "FAILURE" },
};

unsigned psk_ev_counts(const struct psk_evidence *ev, struct psk_ev_count *out, unsigned max)
{
    uint8_t excluded = excluded_of(ev);
    uint8_t unanalyzed = unanalyzed_of(ev);
    unsigned n = 0;
    unsigned g;
    unsigned k;

    for (g = 0; g < sizeof(ev_order) / sizeof(ev_order[0]); g++) {
        uint8_t bit = (uint8_t)(1u << ev_order[g]);
        uint32_t seen = 0;
        unsigned lines = 0;

        if ((excluded | unanalyzed) & bit) {
            /* Not a count of none: the capture could not see it. */
            if (n < max) {
                struct psk_ev_count *o = &out[n++];

                memset(o, 0, sizeof(*o));
                o->cls = ev_order[g];
                o->first = 1;
                o->blind = 1;
                snprintf(o->name, sizeof(o->name), "%s", (excluded & bit) ? "NOT CAPTURED" : "NOT ANALYZED");
                snprintf(o->before, sizeof(o->before), "-");
                snprintf(o->after, sizeof(o->after), "-");
            }
            continue;
        }
        for (k = 1; k < PPC_EV_KINDS; k++) {
            if (kinds[k].cls == ev_order[g]) {
                seen += ev->win[0].count[k] + ev->win[1].count[k];
            }
        }
        for (k = 1; k < PPC_EV_KINDS && seen && n < max; k++) {
            uint32_t b = ev->win[0].count[k];
            uint32_t f = ev->win[1].count[k];
            struct psk_ev_count *o = &out[n];
            char a[24];

            if (kinds[k].cls != ev_order[g] || (!kinds[k].core && b == 0 && f == 0)) {
                continue;
            }
            memset(o, 0, sizeof(*o));
            o->cls = ev_order[g];
            o->first = lines == 0;
            snprintf(o->name, sizeof(o->name), "%s", kinds[k].word);
            snprintf(o->before, sizeof(o->before), "%s", psk_fmt_count(b, a, sizeof(a)));
            snprintf(o->after, sizeof(o->after), "%s", psk_fmt_count(f, a, sizeof(a)));
            lines++;
            n++;
        }
        if (!seen && n < max) {
            struct psk_ev_count *o = &out[n++];

            memset(o, 0, sizeof(*o));
            o->cls = ev_order[g];
            o->first = 1;
            o->none = 1;
            snprintf(o->name, sizeof(o->name), "NONE SEEN");
            snprintf(o->before, sizeof(o->before), "0");
            snprintf(o->after, sizeof(o->after), "0");
        }
    }
    return n;
}

/* ---- the rows ---------------------------------------------------------------------------- */

static const char *rcode_word(unsigned rcode, char *buf, size_t buflen)
{
    switch (rcode) {
    case 1:
        return "FORMAT ERROR";
    case 2:
        return "SERVFAIL";
    case 3:
        return "NXDOMAIN";
    case 5:
        return "REFUSED";
    default:
        snprintf(buf, buflen, "CODE %u", rcode);
        return buf;
    }
}

static const char *rcode_meaning(unsigned rcode)
{
    switch (rcode) {
    case 1:
        return "The resolver could not read the question.";
    case 2:
        return "The resolver could not get an answer itself.";
    case 3:
        return "The resolver says the name does not exist.";
    case 5:
        return "The resolver will not answer this question.";
    default:
        return "The resolver answered with a failure code.";
    }
}

/* Whether the reply to a client's request could be seen from here: the one
 * sentence that keeps "not seen" from being read as "not sent". */
static void reply_note(const struct ppc_evt_ev_obs *o, char *buf, size_t buflen)
{
    if (has(o, PPC_EVF_OWN)) {
        more(buf, buflen, "Its reply comes back to this port.");
    } else if (has(o, PPC_EVF_REPLY_HERE)) {
        more(buf, buflen, "It asks for a broadcast reply, so the reply would pass this port.");
    } else {
        more(buf, buflen, "Its reply goes to that device alone: not seeing one here says nothing.");
    }
}

static void row_of(const struct psk_evidence *ev, const struct ppc_evt_ev_obs *o, struct psk_ev_row *r)
{
    int64_t d = (int64_t)o->t_ms - (int64_t)ev->head.at_ms;
    uint64_t ad = (uint64_t)(d < 0 ? -d : d);
    char a[32];

    memset(r, 0, sizeof(*r));
    r->side = o->side;
    r->cls = kinds[o->kind].cls;
    r->alive = o->alive;
    r->seq = o->seq;
    /* Tenths of a second from the mark; the mark's own instant has no sign. */
    snprintf(r->when, sizeof(r->when), "%s%u.%u S", ad < 50 ? "" : d < 0 ? "-" : "+", (unsigned)((ad + 50u) / 1000u),
             (unsigned)((ad + 50u) % 1000u / 100u));
    snprintf(r->what, sizeof(r->what), "%s", kinds[o->kind].word);
    /* What is left of the packet behind it: said on the row, so evidence
     * that can no longer be opened is not offered as if it could. */
    if (o->seq == 0 || o->alive == PPC_EV_GONE) {
        snprintf(r->held, sizeof(r->held), "ITS PACKET HAS LEFT THE BUFFER");
    } else if (o->alive == PPC_EV_SUMMARY) {
        snprintf(r->held, sizeof(r->held), "ITS BYTES HAVE LEFT THE BUFFER" DOT "THE SUMMARY REMAINS");
    }
    switch (o->kind) {
    case PPC_EV_DHCP_DISCOVER:
        part(r->what, sizeof(r->what), "%s", who(o));
        if (o->ip[0]) {
            more(r->sub, sizeof(r->sub), "Asks for %s. ", o->ip);
        }
        reply_note(o, r->sub, sizeof(r->sub));
        break;
    case PPC_EV_DHCP_REQUEST:
        part(r->what, sizeof(r->what), "%s", who(o));
        if (o->ip[0] && o->ip2[0]) {
            more(r->sub, sizeof(r->sub), "Asks server %s for %s. ", o->ip2, o->ip);
        } else if (o->ip[0]) {
            more(r->sub, sizeof(r->sub), "Asks for %s. ", o->ip);
        }
        reply_note(o, r->sub, sizeof(r->sub));
        break;
    case PPC_EV_DHCP_OFFER:
    case PPC_EV_DHCP_ACK:
        if (o->ip[0]) {
            part(r->what, sizeof(r->what), "%s FOR %s", o->ip, who(o));
        } else {
            part(r->what, sizeof(r->what), "FOR %s", who(o));
        }
        more(r->sub, sizeof(r->sub), "%s%s %s", o->ip2[0] ? "Server " : "A server", o->ip2,
             o->kind == PPC_EV_DHCP_OFFER ? "offers the address" : "confirms the address");
        if (o->b) {
            more(r->sub, sizeof(r->sub), " for %s", lower(psk_fmt_lease(o->b, a, sizeof(a)), a));
        }
        more(r->sub, sizeof(r->sub), ".%s", has(o, PPC_EVF_BCAST) ? " Sent to every station." : "");
        break;
    case PPC_EV_DHCP_NAK:
        part(r->what, sizeof(r->what), "TO %s", who(o));
        more(r->sub, sizeof(r->sub), "%s%s refuses the address that device asked for. Routine for a device arriving "
                                      "from another network.",
             o->ip2[0] ? "Server " : "A server", o->ip2);
        break;
    case PPC_EV_DHCP_DECLINE:
        part(r->what, sizeof(r->what), "%s", who(o));
        more(r->sub, sizeof(r->sub), "It gives %s back: its own check found the address in use.",
             o->ip[0] ? o->ip : "its address");
        break;
    case PPC_EV_DHCP_RELEASE:
        part(r->what, sizeof(r->what), "%s", who(o));
        more(r->sub, sizeof(r->sub), "It gives up %s.", o->ip[0] ? o->ip : "its address");
        break;
    case PPC_EV_DHCP_INFORM:
        part(r->what, sizeof(r->what), "%s", who(o));
        more(r->sub, sizeof(r->sub), "It has an address and asks only for the network's settings.");
        break;
    case PPC_EV_ARP_REQUEST:
        part(r->what, sizeof(r->what), "WHO HAS %s", o->ip2[0] ? o->ip2 : "AN ADDRESS");
        if (has(o, PPC_EVF_OWN)) {
            more(r->sub, sizeof(r->sub), "Asked by this device. The reply comes back to this port.");
        } else {
            more(r->sub, sizeof(r->sub), "Asked by %s%s%s%s. The reply goes to the asker alone.", who_lower(o),
                 o->ip[0] ? " (" : "", o->ip, o->ip[0] ? ")" : "");
        }
        break;
    case PPC_EV_ARP_REPLY:
        part(r->what, sizeof(r->what), "%s IS AT %s", o->ip[0] ? o->ip : "AN ADDRESS", o->mac[0] ? o->mac : "A MAC");
        more(r->sub, sizeof(r->sub), "%s", has(o, PPC_EVF_OWN)      ? "Sent by this device."
                                           : has(o, PPC_EVF_BCAST) ? "Sent to every station."
                                                                   : "A reply that passed this port.");
        break;
    case PPC_EV_ARP_PROBE:
        part(r->what, sizeof(r->what), "FOR %s", o->ip2[0] ? o->ip2 : "AN ADDRESS");
        more(r->sub, sizeof(r->sub), "%s checks that nobody uses the address before taking it.", who_lower(o));
        break;
    case PPC_EV_ARP_ANNOUNCE:
        part(r->what, sizeof(r->what), "%s IS AT %s", o->ip[0] ? o->ip : "AN ADDRESS", o->mac[0] ? o->mac : "A MAC");
        more(r->sub, sizeof(r->sub), "It tells every station which MAC holds the address, as a device does after "
                                      "getting or changing one.");
        break;
    case PPC_EV_ARP_CHANGED:
        part(r->what, sizeof(r->what), "%s", o->ip[0] ? o->ip : "AN ADDRESS");
        more(r->sub, sizeof(r->sub), "%s now claims an address another MAC answered for before. A device may have "
                                      "been replaced, or two may hold one address.",
             who_lower(o));
        break;
    case PPC_EV_ARP_TWO_MACS:
        part(r->what, sizeof(r->what), "%s", o->ip[0] ? o->ip : "AN ADDRESS");
        more(r->sub, sizeof(r->sub), "%s claims it within a minute of another MAC. Two devices may hold the same "
                                      "address.",
             who_lower(o));
        break;
    case PPC_EV_DNS_QUERY:
    case PPC_EV_DNS_RETRY:
        if (has(o, PPC_EVF_OWN)) {
            part(r->what, sizeof(r->what), "THIS DEVICE ASKS %s", o->ip[0] ? o->ip : "A RESOLVER");
        } else {
            part(r->what, sizeof(r->what), "TO %s", o->ip[0] ? o->ip : "A RESOLVER");
        }
        if (o->kind == PPC_EV_DNS_RETRY) {
            more(r->sub, sizeof(r->sub), "The same question again. ");
        }
        if (has(o, PPC_EVF_OWN)) {
            more(r->sub, sizeof(r->sub), "Its answer comes back to this port.");
        } else {
            more(r->sub, sizeof(r->sub), "Another device's question: its answer goes to that device and may never "
                                          "pass this port.");
        }
        break;
    case PPC_EV_DNS_ANSWER:
    case PPC_EV_DNS_FAILURE:
        if (o->kind == PPC_EV_DNS_FAILURE) {
            part(r->what, sizeof(r->what), "%s", rcode_word(o->a >> 16, a, sizeof(a)));
        } else if (o->b) {
            part(r->what, sizeof(r->what), "%s", psk_fmt_ms(o->b, a, sizeof(a)));
        }
        part(r->what, sizeof(r->what), "FROM %s", o->ip[0] ? o->ip : "A RESOLVER");
        if (o->kind == PPC_EV_DNS_FAILURE) {
            more(r->sub, sizeof(r->sub), "%s ", rcode_meaning(o->a >> 16));
        }
        if (has(o, PPC_EVF_UNPAIRED)) {
            more(r->sub, sizeof(r->sub), "No matching question is available to calculate response time.");
        } else if (o->b) {
            more(r->sub, sizeof(r->sub), "Answered %s after the question.", ms_lower(o->b, a, sizeof(a)));
        } else {
            more(r->sub, sizeof(r->sub), "An answer to a question this capture saw.");
        }
        break;
    default:
        break;
    }
    if (has(o, PPC_EVF_V6)) {
        more(r->sub, sizeof(r->sub), " Over IPv6.");
    }
}

unsigned psk_ev_rows(const struct psk_evidence *ev, struct psk_ev_row *out, unsigned max)
{
    unsigned n = 0;
    unsigned side;
    unsigned i;

    for (side = 0; side < 2; side++) {
        for (i = 0; i < ev->nobs[side] && i < PPC_EV_KEEP && n < max; i++) {
            const struct ppc_evt_ev_obs *o = &ev->obs[side][i];

            if (o->kind == PPC_EV_NONE || o->kind >= PPC_EV_KINDS) {
                continue;
            }
            row_of(ev, o, &out[n++]);
        }
    }
    return n;
}

const char *psk_ev_side(const struct psk_evidence *ev, unsigned side, char *buf, size_t buflen)
{
    const struct ppc_evt_ev_win *w = &ev->win[side ? 1 : 0];
    uint32_t all = total(w);
    uint32_t rows = ev->nobs[side ? 1 : 0];
    uint8_t blind = blind_of(ev);
    uint8_t visible = (uint8_t)(PPC_EV_CLASSES & ~blind);
    const char *yet = (side && (w->flags & PPC_EVW_OPEN)) ? " YET" : "";
    char a[24];
    char c[40];

    snprintf(buf, buflen, "%s", side ? "AFTER THE MARK" : "BEFORE THE MARK");
    if (all == 0) {
        if (!visible) {
            part(buf, buflen, "NOTHING COULD BE OBSERVED");
        } else if (blind) {
            part(buf, buflen, "NO %s SEEN%s", class_list(visible, "OR", c, sizeof(c)), yet);
        } else {
            part(buf, buflen, "NONE SEEN%s", yet);
        }
    } else if (rows < all) {
        part(buf, buflen, "%s OF %s", psk_fmt_count(rows, a, sizeof(a)), psk_fmt_count(all, c, sizeof(c)));
        part(buf, buflen, "NEAREST THE MARK");
    } else {
        part(buf, buflen, "%s SEEN", psk_fmt_count(all, a, sizeof(a)));
    }
    return buf;
}

/* ---- what it can and cannot tell --------------------------------------------------------- */

const char *psk_ev_limits(const struct psk_evidence *ev, char *buf, size_t buflen)
{
    const struct ppc_evt_ev_win *b = &ev->win[0];
    const struct ppc_evt_ev_win *f = &ev->win[1];
    uint8_t excluded = excluded_of(ev);
    uint8_t unanalyzed = unanalyzed_of(ev);
    int unseen = 0;
    char a[24];
    char c[40];
    unsigned side;
    unsigned i;

    buf[0] = '\0';
    line(buf, buflen, "This is what one interface saw: this device's own traffic and what is sent to every station. "
                      "Traffic between other devices never arrives at a switch port or a Wi-Fi station.");
    if (excluded) {
        line(buf, buflen, "The capture filter (%s) kept %s out of this capture: no such message was captured, so "
                          "none is counted or listed, and nothing is said of it.",
             psk_cfilter_label((enum ppc_cfilter)ev->head.cfilter), class_list(excluded, "and", c, sizeof(c)));
    }
    if (unanalyzed) {
        line(buf, buflen, "The %s analysis could not be set up for this capture: those packets were captured and "
                          "are in PACKETS, but none was examined, so none is counted or listed here.",
             class_list(unanalyzed, "and", c, sizeof(c)));
    }
    for (side = 0; side < 2; side++) {
        for (i = 0; i < ev->nobs[side] && i < PPC_EV_KEEP; i++) {
            const struct ppc_evt_ev_obs *o = &ev->obs[side][i];

            if ((o->kind == PPC_EV_DHCP_DISCOVER || o->kind == PPC_EV_DHCP_REQUEST) && !has(o, PPC_EVF_REPLY_HERE)) {
                unseen = 1;
            }
        }
    }
    if (unseen) {
        line(buf, buflen, "A DHCP server answers the device that asked, not the network. Another device's OFFER and "
                          "ACK are not seen here: not seeing them says nothing about whether they were sent.");
    }
    if (b->lost || f->lost) {
        line(buf, buflen, "Frames were lost while this was captured (%s before, %s after). What is listed was seen; "
                          "a request or a reply may be among what was lost.",
             psk_fmt_count(b->lost, a, sizeof(a)), psk_fmt_count(f->lost, c, sizeof(c)));
    }
    /* The counters of lost frames, where they could not be read. */
    {
        int stopped = !(f->flags & PPC_EVW_OPEN) && ((f->flags & PPC_EVW_CUT_STOP) || f->span_ms == 0);
        char t1[24];
        char t2[24];
        uint32_t from;

        if (ev->head.edge == PPC_EV_EDGE_COUNTED && ev->head.edge_lost) {
            line(buf, buflen, "%s frame%s lost between %s and %s, while the counters could not be read: whether before "
                              "or after the mark is not known. Counted once, here, in neither window.",
                 psk_fmt_count(ev->head.edge_lost, a, sizeof(a)), ev->head.edge_lost == 1 ? " was" : "s were",
                 psk_fmt_elapsed(ev->head.edge_from_ms, t1, sizeof(t1)), psk_fmt_elapsed(ev->head.edge_to_ms, t2, sizeof(t2)));
        } else if (ev->head.edge == PPC_EV_EDGE_UNKNOWN) {
            line(buf, buflen, "%s %s the counters of lost frames could not be read%s: what was lost %s is not known, "
                              "and is counted in neither window.",
                 stopped ? "From" : "Since", since_text(ev->head.edge_from_ms, t1, sizeof(t1)),
                 stopped ? " until the capture stopped" : "", stopped ? "then" : "from then on");
        } else if (f->unknown) {
            from = f->unknown_from_ms;
            line(buf, buflen, "%s %s the counters of lost frames could not be read%s: what the window after the mark "
                              "lost %s is not known.",
                 stopped ? "From" : "Since", since_text(from, t1, sizeof(t1)),
                 stopped ? " until the capture stopped" : "", stopped ? "then" : "from then on");
        }
        if (b->lost_near) {
            line(buf, buflen, "%s frame%s lost while the counters could not be read, in a spell that began before the "
                              "window before the mark: they may be from before it, so they are told apart.",
                 psk_fmt_count(b->lost_near, a, sizeof(a)), b->lost_near == 1 ? " was" : "s were");
        }
        if (f->lost_near) {
            /* A reading across the end, after a spell or not. */
            line(buf, buflen, "%s frame%s lost between the last good reading of the counters inside the window after "
                              "the mark and the first after its end: they may be from after it, so they are told apart.",
                 psk_fmt_count(f->lost_near, a, sizeof(a)), f->lost_near == 1 ? " was" : "s were");
        }
        if ((b->flags | f->flags) & PPC_EVW_LOSSES_DROPPED) {
            line(buf, buflen, "The counters could not be read more often than the record of such spells holds: the "
                              "count of frames lost before the mark may be short.");
        }
    }
    if (b->held_s || f->held_s) {
        line(buf, buflen, "The capture was on HOLD for part of this time. Nothing was looked at then, so nothing "
                          "from those seconds is listed.");
    }
    if (b->shed_s || f->shed_s) {
        line(buf, buflen, "The capture fell behind for part of this time and only counted frames.");
    }
    if (b->flags & PPC_EVW_HISTORY) {
        line(buf, buflen, "More was observed before the mark than the history holds. The oldest of it is not "
                          "counted.");
    }
    if ((b->flags & PPC_EVW_CUT_START) && b->span_ms) {
        line(buf, buflen, "The capture started %s before the mark: there is no earlier evidence.",
             span_lower(b->span_ms, a, sizeof(a)));
    }
    if (f->flags & PPC_EVW_OPEN) {
        line(buf, buflen, "The window after the mark is still filling: what follows will be added.");
    } else if ((f->flags & PPC_EVW_CUT_STOP) && f->span_ms) {
        line(buf, buflen, "The capture stopped %s after the mark: nothing later was seen.",
             span_lower(f->span_ms, a, sizeof(a)));
    } else if (f->span_ms == 0) {
        line(buf, buflen, "The mark was set on a capture that had stopped: nothing follows it.");
    }
    if (ev->head.first > 1) {
        line(buf, buflen, "A capture keeps its eight newest marks. Earlier ones were replaced.");
    }
    return buf;
}

const char *psk_ev_nothing(const struct psk_evidence *ev, char *buf, size_t buflen)
{
    const struct ppc_evt_ev_win *b = &ev->win[0];
    const struct ppc_evt_ev_win *f = &ev->win[1];
    int limited = b->held_s || f->held_s || b->shed_s || f->shed_s || loss_limited(ev, 0) || loss_limited(ev, 1);
    /* Only what was lost is not known: nothing says part was unwatched. */
    int unknown = !(b->held_s || f->held_s || b->shed_s || f->shed_s || b->lost || f->lost || b->lost_near ||
                    f->lost_near || (ev->head.edge == PPC_EV_EDGE_COUNTED && ev->head.edge_lost));
    uint8_t excluded = excluded_of(ev);
    uint8_t unanalyzed = unanalyzed_of(ev);
    uint8_t blind = blind_of(ev);
    uint8_t visible = (uint8_t)(PPC_EV_CLASSES & ~blind);
    const char *filter = psk_cfilter_label((enum ppc_cfilter)ev->head.cfilter);
    char c[40];
    char d[40];

    buf[0] = '\0';
    if (total(b) + total(f) != 0) {
        return buf;
    }
    if (!visible) {
        /* No conclusion at all: the limits say why. */
        snprintf(buf, buflen, "Nothing of DHCP, ARP or DNS could be observed in this capture.");
        return buf;
    }
    snprintf(buf, buflen, "No %s message was seen in either window.", class_list(visible, "or", c, sizeof(c)));
    if (b->span_ms == 0 && f->span_ms == 0) {
        more(buf, buflen, " Neither window has any length yet.");
        return buf;
    }
    if (limited) {
        more(buf, buflen, " %s, so this is not proof that none was sent.",
             unknown ? "Whether frames were lost for part of the time is not known"
                     : "Part of the time was not fully watched");
        return buf;
    }
    /* Quiet, of what the capture could see and of what its filter let in. */
    if (ev->head.cfilter != PPC_CFILTER_ALL) {
        more(buf, buflen, " The capture was watching throughout: for %s, the traffic its filter (%s) let in was quiet.",
             blind ? class_list(visible, "and", c, sizeof(c)) : "these protocols", filter);
    } else {
        more(buf, buflen, " The capture was watching throughout: for %s the segment was quiet.",
             blind ? class_list(visible, "and", c, sizeof(c)) : "these protocols");
    }
    if (excluded) {
        more(buf, buflen, " %s %s kept out by the filter.", class_list(excluded, "and", d, sizeof(d)),
             (excluded & (excluded - 1u)) ? "were" : "was");
    }
    if (unanalyzed) {
        more(buf, buflen, " %s %s not analyzed.", class_list(unanalyzed, "and", d, sizeof(d)),
             (unanalyzed & (unanalyzed - 1u)) ? "were" : "was");
    }
    more(buf, buflen, " %s", blind ? "Nothing is said of what could not be seen, nor of what other devices sent to "
                                     "each other."
                                   : "It says nothing of what other devices sent to each other.");
    return buf;
}

/* ---- LIVE --------------------------------------------------------------------------------- */

uint32_t psk_ev_held(const struct psk_model *m)
{
    if (m->nmarks == 0) {
        return 0;
    }
    return m->first_mark && m->nmarks >= m->first_mark ? m->nmarks - m->first_mark + 1u : 1u;
}
