/*
 * Host tests for the words PacketShark makes of a mark's evidence (N1.0,
 * docs/apps/PACKETSHARK.md §4d): apps/packetshark/psk_ev.c over the model.
 *
 * The first half takes the helper's own answers - the lines committed in
 * tests/data/packetshark/evid-<name>.expected, which
 * tests/pos_capture_evid_test.sh holds the real helper to - through the
 * parser and the model, and checks what the evidence page would say of
 * each fixture. The second half builds the answers no replay can give
 * (frames lost, HOLD, a window still open, one cut by START or STOP, a
 * quiet minute) and checks the same.
 *
 * What is held throughout: an observation is said as what was seen; a reply
 * that could not pass this interface is never called missing or
 * unanswered; a window says how much of it was watched; nothing is scored,
 * and no sentence is cut by its buffer.
 *
 * Usage: psk_ev_test <tests/data/packetshark>
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L /* setenv, tzset */

#include "psk_ev.h"
#include "psk_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOT " \xc2\xb7 "

static int failures;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static struct psk_model M;
static const char *data_dir;

/* A line through the parser and into the model, as the session does. */
static unsigned feed(const char *line)
{
    struct ppc_evt e;

    if (ppc_evt_parse(line, &e) == PPC_EVT_BAD) {
        return 0x80000000u;
    }
    return psk_model_apply(&M, &e, 1000);
}

/* A fixture's committed answer into a fresh model. Returns what changed. */
static unsigned golden(const char *name)
{
    char path[512];
    char line[PPC_LINE_MAX];
    unsigned changed = 0;
    FILE *f;

    psk_model_init(&M);
    M.want_ev = 1; /* the request the committed answer was made for */
    snprintf(path, sizeof(path), "%s/evid-%s.expected", data_dir, name);
    f = fopen(path, "r");
    if (!f) {
        printf("     cannot read %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n) {
            changed |= feed(line);
        }
    }
    fclose(f);
    return changed;
}

/* ---- what is asked of every answer ------------------------------------------------------- */

/* A word the evidence must never use. */
static const char *verdict_in(const char *text)
{
    static const char *const never[] = { "MISSING",   "UNANSWERED", "NOT ANSWERED", "NO REPLY",   "NO ANSWER", "FAULT",
                                         "BROKEN",    "FAILED TO",  "SCORE",        "CONFIDENCE", "HEALTH",    "%",
                                         "LIKELY",    "PROBABLY",   "DIAGNOS" };
    char up[1200];
    size_t k;
    unsigned i;

    for (k = 0; text[k] && k + 1 < sizeof(up); k++) {
        up[k] = (char)(text[k] >= 'a' && text[k] <= 'z' ? text[k] - 'a' + 'A' : text[k]);
    }
    up[k] = '\0';
    for (i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
        if (strstr(up, never[i])) {
            return never[i];
        }
    }
    return NULL;
}

/* A sentence is whole: it was not cut by its buffer, and ends as one does. */
static int whole(const char *s, size_t cap)
{
    size_t n = strlen(s);

    return n > 0 && n + 1 < cap && (s[n - 1] == '.' || s[n - 1] == ')');
}

/* Everything the page would say of the evidence in the model: no verdict
 * word, no cut sentence, nothing empty that should say something. Prints
 * what breaks it. */
static int sound(const struct psk_evidence *ev)
{
    struct psk_ev_row rows[PSK_EV_ROWS];
    struct psk_ev_count counts[PSK_EV_COUNTS];
    char b[1024];
    char c[64];
    char d[96];
    const char *v;
    unsigned n;
    unsigned i;
    unsigned s;
    int ok = 1;

    psk_ev_limits(ev, b, sizeof(b));
    if ((v = verdict_in(b)) != NULL || strlen(b) + 1 >= sizeof(b) || b[strlen(b) - 1] != '.') {
        printf("     limits: %s / '%s'\n", v ? v : "cut", b);
        ok = 0;
    }
    psk_ev_nothing(ev, b, sizeof(b));
    if (b[0] && ((v = verdict_in(b)) != NULL || !whole(b, sizeof(b)))) {
        printf("     nothing: %s / '%s'\n", v ? v : "cut", b);
        ok = 0;
    }
    for (s = 0; s < 2; s++) {
        psk_ev_window(ev, s, b, 64, c, sizeof(c), d, sizeof(d));
        if (!b[0] || strlen(b) + 1 >= 64 || strlen(c) + 1 >= sizeof(c) || strlen(d) + 1 >= sizeof(d) || verdict_in(b) ||
            verdict_in(c) || verdict_in(d)) {
            printf("     window %u: '%s' / '%s' / '%s'\n", s, b, c, d);
            ok = 0;
        }
        psk_ev_side(ev, s, b, 64);
        if (!b[0] || strlen(b) + 1 >= 64 || verdict_in(b)) {
            printf("     side %u: '%s'\n", s, b);
            ok = 0;
        }
    }
    n = psk_ev_counts(ev, counts, PSK_EV_COUNTS);
    for (i = 0; i < n; i++) {
        if (!counts[i].name[0] || !counts[i].before[0] || !counts[i].after[0] || verdict_in(counts[i].name)) {
            printf("     count %u: '%s'\n", i, counts[i].name);
            ok = 0;
        }
    }
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    for (i = 0; i < n; i++) {
        const struct psk_ev_row *r = &rows[i];

        if (!r->what[0] || strlen(r->what) + 1 >= sizeof(r->what) || !whole(r->sub, sizeof(r->sub)) || !r->when[0] ||
            strlen(r->when) + 1 >= sizeof(r->when) || strlen(r->held) + 1 >= sizeof(r->held) || verdict_in(r->what) ||
            verdict_in(r->sub) || verdict_in(r->held)) {
            printf("     row %u: '%s' / '%s' / '%s' / '%s'\n", i, r->when, r->what, r->sub, r->held);
            ok = 0;
        }
    }
    return ok;
}

static const struct psk_ev_count *count_of(const struct psk_ev_count *c, unsigned n, unsigned cls, const char *name)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (c[i].cls == cls && strcmp(c[i].name, name) == 0) {
            return &c[i];
        }
    }
    return NULL;
}

static int count_is(const struct psk_ev_count *c, unsigned n, unsigned cls, const char *name, const char *before,
                    const char *after)
{
    const struct psk_ev_count *k = count_of(c, n, cls, name);

    return k && strcmp(k->before, before) == 0 && strcmp(k->after, after) == 0;
}

/* ---- the fixtures, from the helper's committed answers ----------------------------------- */

static void fixtures(void)
{
    const struct psk_evidence *ev = &M.ev;
    struct psk_ev_row rows[PSK_EV_ROWS];
    struct psk_ev_count counts[PSK_EV_COUNTS];
    char b[1024];
    char c[64];
    char d[96];
    unsigned n;
    unsigned sev;

    /* Repeated DHCP discovery. */
    check("dhcp-repeat: the committed answer parses and reaches the model whole",
          (golden("dhcp-repeat") & (PSK_CHANGED_MARK | PSK_CHANGED_EVID)) == (PSK_CHANGED_MARK | PSK_CHANGED_EVID) &&
              M.have_ev && M.nmarks == 1 && M.last_mark.at_ms == 88000 && ev->head.id == 1 && ev->nobs[0] == 4 &&
              ev->nobs[1] == 0);
    check("dhcp-repeat: it is headed Mark, 01:28 into the capture, set on a stopped capture",
          strcmp(psk_ev_title(ev, b, sizeof(b)), "Mark") == 0 &&
              strcmp(psk_ev_when(ev, b, sizeof(b)), "01:28 INTO THE CAPTURE" DOT "CAPTURE STOPPED") == 0);
    sev = psk_ev_window(ev, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("dhcp-repeat: the window before is 60 S BEFORE, every frame seen", sev == PSK_SEV_OK &&
                                                                              strcmp(b, "60 S BEFORE") == 0 && !c[0] &&
                                                                              strcmp(d, "EVERY FRAME SEEN") == 0);
    sev = psk_ev_window(ev, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("dhcp-repeat: the window after is NOTHING AFTER, the capture had stopped", sev == PSK_SEV_NOTE &&
                                                                                    strcmp(b, "NOTHING AFTER") == 0 &&
                                                                                    strcmp(c, "THE CAPTURE HAD STOPPED") == 0 &&
                                                                                    !d[0]);
    n = psk_ev_counts(ev, counts, PSK_EV_COUNTS);
    check("dhcp-repeat: DISCOVER 3, and OFFER, REQUEST and ACK listed at none",
          count_is(counts, n, PPC_CLASS_DHCP, "DISCOVER", "3", "0") && count_is(counts, n, PPC_CLASS_DHCP, "OFFER", "0", "0") &&
              count_is(counts, n, PPC_CLASS_DHCP, "REQUEST", "0", "0") && count_is(counts, n, PPC_CLASS_DHCP, "ACK", "0", "0") &&
              !count_of(counts, n, PPC_CLASS_DHCP, "NAK"));
    check("dhcp-repeat: ARP REQUEST 1 with REPLY at none, and DNS as NONE SEEN on one line",
          count_is(counts, n, PPC_CLASS_ARP, "REQUEST", "1", "0") && count_is(counts, n, PPC_CLASS_ARP, "REPLY", "0", "0") &&
              count_is(counts, n, PPC_CLASS_DNS, "NONE SEEN", "0", "0") && n == 7 && counts[0].first && !counts[1].first &&
              counts[4].first && counts[6].first && counts[6].none);
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    check("dhcp-repeat: a row for each DISCOVER: when, who, its packet, and that its reply is not for this port",
          n == 4 && strcmp(rows[0].when, "-57.7 S") == 0 && strcmp(rows[0].what, "DISCOVER" DOT "02:00:5e:00:00:21") == 0 &&
              rows[0].seq == 4 && rows[0].cls == PPC_CLASS_DHCP && rows[0].side == 0 &&
              strcmp(rows[0].sub, "Its reply goes to that device alone: not seeing one here says nothing.") == 0 &&
              strcmp(rows[1].when, "-41.5 S") == 0 && rows[1].seq == 5 && strcmp(rows[2].when, "-25.3 S") == 0 &&
              rows[2].seq == 6 && !rows[0].held[0]);
    check("dhcp-repeat: the ARP request at the mark has no sign, and says where its reply goes",
          strcmp(rows[3].when, "0.0 S") == 0 && strcmp(rows[3].what, "REQUEST" DOT "WHO HAS 192.0.2.16") == 0 &&
              strcmp(rows[3].sub, "Asked by 02:00:5e:00:00:01 (192.0.2.1). The reply goes to the asker alone.") == 0);
    psk_ev_limits(ev, b, sizeof(b));
    check("dhcp-repeat: the limits say what one interface sees, that another device's OFFER and ACK are not seen "
          "here, and that nothing follows a mark on a stopped capture",
          strstr(b, "This is what one interface saw: this device's own traffic and what is sent to every station.") &&
              strstr(b, "Another device's OFFER and ACK are not seen here: not seeing them says nothing about "
                        "whether they were sent.") &&
              strstr(b, "The mark was set on a capture that had stopped: nothing follows it."));
    check("dhcp-repeat: the sides are headed BEFORE THE MARK · 4 SEEN and AFTER THE MARK · NONE SEEN",
          strcmp(psk_ev_side(ev, 0, b, 64), "BEFORE THE MARK" DOT "4 SEEN") == 0 &&
              strcmp(psk_ev_side(ev, 1, b, 64), "AFTER THE MARK" DOT "NONE SEEN") == 0);
    check("dhcp-repeat: there is something to say, so the nothing-seen sentence is empty", !psk_ev_nothing(ev, b, sizeof(b))[0]);
    check("dhcp-repeat: no verdict word, no cut sentence", sound(ev));

    /* A complete local DNS transaction. */
    check("dns-local: the committed answer reaches the model", (golden("dns-local") & PSK_CHANGED_EVID) && ev->nobs[0] == 4);
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    check("dns-local: this device's ARP request says its reply comes back to this port",
          n == 4 && strcmp(rows[0].what, "REQUEST" DOT "WHO HAS 192.0.2.53") == 0 &&
              strcmp(rows[0].sub, "Asked by this device. The reply comes back to this port.") == 0 &&
              strcmp(rows[1].what, "REPLY" DOT "192.0.2.53 IS AT 02:00:5e:00:00:35") == 0);
    check("dns-local: the question is this device's own, and its answer comes back here",
          strcmp(rows[2].what, "QUERY" DOT "THIS DEVICE ASKS 192.0.2.53") == 0 && rows[2].cls == PPC_CLASS_DNS &&
              strcmp(rows[2].sub, "Its answer comes back to this port.") == 0);
    check("dns-local: the answer carries the time between the two, as measured",
          strcmp(rows[3].what, "ANSWER" DOT "31 MS" DOT "FROM 192.0.2.53") == 0 &&
              strcmp(rows[3].sub, "Answered 31 ms after the question.") == 0);
    n = psk_ev_counts(ev, counts, PSK_EV_COUNTS);
    check("dns-local: QUERY 1 and ANSWER 1, DHCP as NONE SEEN",
          count_is(counts, n, PPC_CLASS_DNS, "QUERY", "1", "0") && count_is(counts, n, PPC_CLASS_DNS, "ANSWER", "1", "0") &&
              count_is(counts, n, PPC_CLASS_DHCP, "NONE SEEN", "0", "0") && !count_of(counts, n, PPC_CLASS_DNS, "FAILURE"));
    sev = psk_ev_window(ev, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("dns-local: a window shorter than a second says UNDER 1 S BEFORE, the capture started then",
          sev == PSK_SEV_OK && strcmp(b, "UNDER 1 S BEFORE") == 0 && strcmp(c, "THE CAPTURE STARTED THEN") == 0);
    psk_ev_limits(ev, b, sizeof(b));
    check("dns-local: and the limits say the capture started under a second before the mark, with no DHCP caveat",
          strstr(b, "The capture started under a second before the mark: there is no earlier evidence.") &&
              !strstr(b, "A DHCP server answers"));
    check("dns-local: no verdict word, no cut sentence", sound(ev));

    /* An invisible third-party reply. */
    check("unseen-reply: the committed answer reaches the model", (golden("unseen-reply") & PSK_CHANGED_EVID) && ev->nobs[0] == 4);
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    check("unseen-reply: the REQUEST names what it asks of whom, and that its reply is not for this port",
          n == 4 && strcmp(rows[1].what, "REQUEST" DOT "02:00:5e:00:00:31") == 0 &&
              strcmp(rows[1].sub, "Asks server 192.0.2.1 for 192.0.2.57. Its reply goes to that device alone: not "
                                  "seeing one here says nothing.") == 0);
    check("unseen-reply: the probe and the announcement that followed are observations of their own",
          strcmp(rows[2].what, "PROBE" DOT "FOR 192.0.2.57") == 0 &&
              strcmp(rows[2].sub, "02:00:5e:00:00:31 checks that nobody uses the address before taking it.") == 0 &&
              strcmp(rows[3].what, "ANNOUNCEMENT" DOT "192.0.2.57 IS AT 02:00:5e:00:00:31") == 0);
    n = psk_ev_counts(ev, counts, PSK_EV_COUNTS);
    check("unseen-reply: REQUEST 1 and ACK 0 are counts of what passed here, side by side",
          count_is(counts, n, PPC_CLASS_DHCP, "REQUEST", "1", "0") && count_is(counts, n, PPC_CLASS_DHCP, "ACK", "0", "0") &&
              count_is(counts, n, PPC_CLASS_ARP, "PROBE", "1", "0") && count_is(counts, n, PPC_CLASS_ARP, "ANNOUNCEMENT", "1", "0"));
    psk_ev_limits(ev, b, sizeof(b));
    check("unseen-reply: the limits say the ACK's absence says nothing about whether it was sent",
          strstr(b, "not seeing them says nothing about whether they were sent") != NULL);
    check("unseen-reply: no verdict word, no cut sentence", sound(ev));

    /* An address claimed by two MACs, and a NAK. */
    check("loss-event: the committed answer reaches the model", (golden("loss-event") & PSK_CHANGED_EVID) && ev->nobs[0] == 6);
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    check("loss-event: an address that changed MAC is said as seen, and what it may mean says may",
          n == 6 && strcmp(rows[2].what, "ADDRESS CHANGED MAC" DOT "192.0.2.77") == 0 &&
              strcmp(rows[2].sub, "02:00:5e:00:00:42 now claims an address another MAC answered for before. A device "
                                  "may have been replaced, or two may hold one address.") == 0 &&
              strcmp(rows[4].what, "TWO MACS, ONE ADDRESS" DOT "192.0.2.77") == 0 && strstr(rows[4].sub, " may hold the same address."));
    check("loss-event: two observations of one packet keep its order and its number",
          rows[1].seq == 2 && rows[2].seq == 2 && strcmp(rows[1].when, rows[2].when) == 0 &&
              strncmp(rows[1].what, "ANNOUNCEMENT", 12) == 0);
    check("loss-event: the NAK says whose it is and that it is routine for a device from another network",
          strcmp(rows[5].what, "NAK" DOT "TO 02:00:5e:00:00:44") == 0 &&
              strcmp(rows[5].sub, "Server 192.0.2.1 refuses the address that device asked for. Routine for a device "
                                  "arriving from another network.") == 0);
    check("loss-event: no verdict word, no cut sentence", sound(ev));

    /* The quiet fixture, as a replay gives it. */
    check("silence: the committed answer reaches the model", (golden("silence") & PSK_CHANGED_EVID) && ev->nobs[0] == 4);
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    check("silence: another station's DNS answer is said with its time",
          n == 4 && strcmp(rows[3].what, "ANSWER" DOT "18 MS" DOT "FROM 192.0.2.53") == 0 && strcmp(rows[0].when, "-15.0 S") == 0);
    check("silence: no verdict word, no cut sentence", sound(ev));
}

/* ---- the answers no replay gives ----------------------------------------------------------- */

static struct psk_evidence E;

static void fresh(void)
{
    memset(&E, 0, sizeof(E));
    E.head.id = 1;
    E.head.first = 1;
    E.head.last = 1;
    E.head.at_ms = 253000;
    E.head.state = PPC_STATE_CAPTURING;
    E.head.seq = 900;
    E.win[0].side = 0;
    E.win[0].span_ms = 60000;
    E.win[0].watched_s = 60;
    E.win[1].side = 1;
    E.win[1].span_ms = 60000;
    E.win[1].watched_s = 60;
}

static struct ppc_evt_ev_obs *add(unsigned side, unsigned kind, unsigned flags, uint32_t t_ms, uint32_t seq)
{
    struct ppc_evt_ev_obs *o = &E.obs[side][E.nobs[side]++];

    memset(o, 0, sizeof(*o));
    o->side = (uint8_t)side;
    o->kind = (uint8_t)kind;
    o->flags = (uint8_t)flags;
    o->alive = PPC_EV_WHOLE;
    o->t_ms = t_ms;
    o->seq = seq;
    snprintf(o->mac, sizeof(o->mac), "02:00:5e:00:00:21");
    snprintf(o->ip, sizeof(o->ip), "192.0.2.50");
    snprintf(o->ip2, sizeof(o->ip2), "192.0.2.1");
    if (kind < PPC_EV_KINDS) {
        E.win[side].count[kind]++;
    }
    E.win[side].kept++;
    return o;
}

static void built(void)
{
    struct psk_ev_row rows[PSK_EV_ROWS];
    struct psk_ev_count counts[PSK_EV_COUNTS];
    char b[1024];
    char c[64];
    char d[96];
    unsigned n;
    unsigned sev;
    unsigned k;
    int ok;

    /* An observed event during packet loss: what was seen stays, and the
     * loss is said with it. */
    fresh();
    E.win[0].lost = 312;
    add(0, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 240600, 880);
    add(0, PPC_EV_ARP_TWO_MACS, 0, 251000, 890);
    sev = psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("frames lost: the window says 312 FRAMES LOST, and that is more than a note", sev == PSK_SEV_WARN &&
                                                                                        strcmp(b, "60 S BEFORE") == 0 &&
                                                                                        strcmp(d, "312 FRAMES LOST") == 0);
    n = psk_ev_rows(&E, rows, PSK_EV_ROWS);
    check("frames lost: the observations made in that window are rows as ever", n == 2 &&
                                                                                 strcmp(rows[0].when, "-12.4 S") == 0 &&
                                                                                 strncmp(rows[1].what, "TWO MACS, ONE ADDRESS", 21) == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("frames lost: the limits say what is listed was seen, and that a request or a reply may be among the lost",
          strstr(b, "Frames were lost while this was captured (312 before, 0 after). What is listed was seen; a "
                    "request or a reply may be among what was lost.") != NULL);
    E.win[0].lost = 1;
    psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("frames lost: one is 1 FRAME LOST", strcmp(d, "1 FRAME LOST") == 0);
    check("frames lost: no verdict word, no cut sentence", sound(&E));

    /* Counting only, and HOLD. */
    fresh();
    E.win[0].shed_s = 5;
    E.win[1].held_s = 14;
    sev = psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("counting only: COUNTING ONLY 5 S, more than a note", sev == PSK_SEV_WARN && strcmp(d, "COUNTING ONLY 5 S") == 0);
    sev = psk_ev_window(&E, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("HOLD: ON HOLD 14 S, a note", sev == PSK_SEV_NOTE && strcmp(d, "ON HOLD 14 S") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("HOLD: the limits say nothing was looked at then, and that the capture fell behind",
          strstr(b, "The capture was on HOLD for part of this time. Nothing was looked at then, so nothing from "
                    "those seconds is listed.") &&
              strstr(b, "The capture fell behind for part of this time and only counted frames."));
    E.win[1].span_ms = 2300;
    E.win[1].held_s = 4;
    E.win[1].flags = PPC_EVW_OPEN;
    psk_ev_window(&E, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("HOLD: seconds on HOLD are never said as more than the window is long",
          strcmp(b, "2 S AFTER") == 0 && strcmp(c, "STILL OPEN") == 0 && strcmp(d, "ON HOLD 3 S") == 0);
    check("HOLD: no verdict word, no cut sentence", sound(&E));

    /* A window still open, one cut by STOP, one by START, one with no length. */
    fresh();
    E.win[1].span_ms = 19000;
    E.win[1].flags = PPC_EVW_OPEN;
    sev = psk_ev_window(&E, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("open: 19 S AFTER, STILL OPEN, every frame seen so far", sev == PSK_SEV_OK && strcmp(b, "19 S AFTER") == 0 &&
                                                                        strcmp(c, "STILL OPEN") == 0 &&
                                                                        strcmp(d, "EVERY FRAME SEEN") == 0);
    check("open: nothing seen in it yet is NONE SEEN YET", strcmp(psk_ev_side(&E, 1, b, 64), "AFTER THE MARK" DOT "NONE SEEN YET") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("open: the limits say it is still filling", strstr(b, "The window after the mark is still filling: what follows will be added.") != NULL);
    E.win[1].span_ms = 0;
    sev = psk_ev_window(&E, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("just set: 0 S AFTER, STILL OPEN, and nothing is claimed of it", sev == PSK_SEV_NOTE && strcmp(b, "0 S AFTER") == 0 &&
                                                                               strcmp(c, "STILL OPEN") == 0 && !d[0]);
    E.win[1].span_ms = 5200;
    E.win[1].flags = PPC_EVW_CUT_STOP;
    psk_ev_window(&E, 1, b, 64, c, sizeof(c), d, sizeof(d));
    check("near STOP: 5 S AFTER, THE CAPTURE STOPPED THEN", strcmp(b, "5 S AFTER") == 0 && strcmp(c, "THE CAPTURE STOPPED THEN") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("near STOP: the limits say nothing later was seen", strstr(b, "The capture stopped 5 s after the mark: nothing later was seen.") != NULL);
    E.win[0].span_ms = 8400;
    E.win[0].flags = PPC_EVW_CUT_START;
    psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("near START: 8 S BEFORE, THE CAPTURE STARTED THEN", strcmp(b, "8 S BEFORE") == 0 && strcmp(c, "THE CAPTURE STARTED THEN") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("near START: the limits say there is no earlier evidence", strstr(b, "The capture started 8 s before the mark: there is no earlier evidence.") != NULL);
    E.win[0].span_ms = 0;
    sev = psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("at START: NOTHING BEFORE, SET AS THE CAPTURE STARTED", sev == PSK_SEV_NOTE && strcmp(b, "NOTHING BEFORE") == 0 &&
                                                                      strcmp(c, "SET AS THE CAPTURE STARTED") == 0 && !d[0]);
    E.win[0].span_ms = 60000;
    E.win[0].flags = PPC_EVW_HISTORY;
    sev = psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    psk_ev_limits(&E, b, sizeof(b));
    check("a history that holds less than the window says OLDEST NOT KEPT, and the limits say why",
          sev == PSK_SEV_NOTE && strcmp(d, "OLDEST NOT KEPT") == 0 &&
              strstr(b, "More was observed before the mark than the history holds. The oldest of it is not counted."));
    check("the cut windows: no verdict word, no cut sentence", sound(&E));

    /* Everything that can limit a mark, at once: the page's buffer still
     * holds the limits whole. */
    fresh();
    E.head.id = 9;
    E.head.first = 2;
    E.head.last = 9;
    E.win[0].flags = PPC_EVW_CUT_START | PPC_EVW_HISTORY;
    E.win[0].span_ms = 59000;
    E.win[0].lost = 4000000000u;
    E.win[0].held_s = 59;
    E.win[0].shed_s = 59;
    E.win[1].flags = PPC_EVW_OPEN;
    E.win[1].span_ms = 59000;
    E.win[1].lost = 4000000000u;
    E.win[1].held_s = 59;
    E.win[1].shed_s = 59;
    add(0, PPC_EV_DHCP_REQUEST, PPC_EVF_BCAST, 250000, 1);
    psk_ev_limits(&E, b, sizeof(b));
    check("every limit at once: nine sentences, none cut, the last one whole",
          strlen(b) + 1 < sizeof(b) && b[strlen(b) - 1] == '.' && strstr(b, "A DHCP server answers") &&
              strstr(b, "Frames were lost") && strstr(b, "on HOLD") && strstr(b, "fell behind") &&
              strstr(b, "than the history holds") && strstr(b, "there is no earlier evidence") &&
              strstr(b, "still filling") && strstr(b, "Earlier ones were replaced."));
    sev = psk_ev_window(&E, 0, b, 64, c, sizeof(c), d, sizeof(d));
    check("and its window's line holds every part too", sev == PSK_SEV_WARN && strlen(d) + 1 < sizeof(d) &&
                                                            strstr(d, "FRAMES LOST") && strstr(d, "COUNTING ONLY 59 S") &&
                                                            strstr(d, "ON HOLD 59 S") && strstr(d, "OLDEST NOT KEPT"));

    /* A mark followed by silence. */
    fresh();
    n = psk_ev_counts(&E, counts, PSK_EV_COUNTS);
    check("silence: the counts are three lines of NONE SEEN", n == 3 && counts[0].none && counts[1].none && counts[2].none &&
                                                                  counts[0].cls == PPC_CLASS_DHCP && counts[2].cls == PPC_CLASS_DNS);
    check("silence: watched throughout, it says the segment was quiet for these protocols, and what that leaves out",
          strcmp(psk_ev_nothing(&E, b, sizeof(b)),
                 "No DHCP, ARP or DNS message was seen in either window. The capture was watching throughout: for "
                 "these protocols the segment was quiet. It says nothing of what other devices sent to each other.") == 0);
    check("silence: there are no rows, and both sides are headed NONE SEEN",
          psk_ev_rows(&E, rows, PSK_EV_ROWS) == 0 && strcmp(psk_ev_side(&E, 0, b, 64), "BEFORE THE MARK" DOT "NONE SEEN") == 0 &&
              strcmp(psk_ev_side(&E, 1, b, 64), "AFTER THE MARK" DOT "NONE SEEN") == 0);
    E.win[1].held_s = 20;
    check("silence with part of it on HOLD is not proof that nothing was sent, and says so",
          strcmp(psk_ev_nothing(&E, b, sizeof(b)),
                 "No DHCP, ARP or DNS message was seen in either window. Part of the time was not fully watched, so "
                 "this is not proof that none was sent.") == 0);
    E.win[1].held_s = 0;
    E.win[0].lost = 3;
    check("and the same with frames lost", strstr(psk_ev_nothing(&E, b, sizeof(b)), "this is not proof that none was sent") != NULL);
    E.win[0].lost = 0;
    E.win[0].span_ms = 0;
    E.win[1].span_ms = 0;
    E.win[1].flags = PPC_EVW_OPEN;
    check("a mark set as the capture starts says neither window has any length yet",
          strcmp(psk_ev_nothing(&E, b, sizeof(b)),
                 "No DHCP, ARP or DNS message was seen in either window. Neither window has any length yet.") == 0);
    check("silence: no verdict word, no cut sentence", sound(&E));

    /* Which mark of how many, when, and what the capture was doing. */
    fresh();
    E.head.id = 5;
    E.head.first = 4;
    E.head.last = 6;
    check("the title counts the marks held: Mark 2 of 3", strcmp(psk_ev_title(&E, b, sizeof(b)), "Mark 2 of 3") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("when earlier marks were replaced the limits say a capture keeps its eight newest",
          strstr(b, "A capture keeps its eight newest marks. Earlier ones were replaced.") != NULL);
    E.head.state = PPC_STATE_HOLD;
    E.head.wall_s = 1700000000u; /* 22:13:20 UTC */
    check("when: the capture's clock, the wall clock when it is set, and ON HOLD",
          strcmp(psk_ev_when(&E, b, sizeof(b)), "04:13 INTO THE CAPTURE" DOT "22:13:20" DOT "ON HOLD") == 0);
    E.head.state = PPC_STATE_CAPTURING;
    E.head.wall_s = 0;
    check("when: no wall clock, no time of day; capturing is not said", strcmp(psk_ev_when(&E, b, sizeof(b)), "04:13 INTO THE CAPTURE") == 0);

    /* More observations than rows, and the routine ones. */
    fresh();
    add(0, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 250000, 1);
    E.win[0].count[PPC_EV_ARP_REQUEST] = 42;
    E.win[0].more = 42;
    check("more observed than shown: BEFORE THE MARK · 1 OF 43 · NEAREST THE MARK",
          strcmp(psk_ev_side(&E, 0, b, 64), "BEFORE THE MARK" DOT "1 OF 43" DOT "NEAREST THE MARK") == 0);
    n = psk_ev_counts(&E, counts, PSK_EV_COUNTS);
    check("a count of thousands is grouped as the app's figures are", count_is(counts, n, PPC_CLASS_ARP, "REQUEST", "42", "0"));
    E.win[1].count[PPC_EV_DNS_QUERY] = 2089;
    n = psk_ev_counts(&E, counts, PSK_EV_COUNTS);
    check("and 2 089 is written 2 089", count_is(counts, n, PPC_CLASS_DNS, "QUERY", "0", "2 089"));

    /* What the buffer still holds of a row's packet. */
    fresh();
    add(0, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 250000, 7)->alive = PPC_EV_GONE;
    add(0, PPC_EV_DHCP_OFFER, PPC_EVF_BCAST, 250100, 8)->alive = PPC_EV_SUMMARY;
    add(0, PPC_EV_DHCP_ACK, PPC_EVF_BCAST, 250300, 9);
    n = psk_ev_rows(&E, rows, PSK_EV_ROWS);
    check("a packet that left the buffer is said so on its row; one whose bytes left, that its summary remains",
          n == 3 && rows[0].alive == PPC_EV_GONE && strcmp(rows[0].held, "ITS PACKET HAS LEFT THE BUFFER") == 0 &&
              rows[1].alive == PPC_EV_SUMMARY &&
              strcmp(rows[1].held, "ITS BYTES HAVE LEFT THE BUFFER" DOT "THE SUMMARY REMAINS") == 0 && !rows[2].held[0] &&
              rows[0].seq == 7);
    check("the observation itself is said as before: expiry takes the packet, not what was seen",
          strcmp(rows[0].what, "DISCOVER" DOT "02:00:5e:00:00:21") == 0 &&
              strcmp(rows[1].what, "OFFER" DOT "192.0.2.50 FOR 02:00:5e:00:00:21") == 0 &&
              strcmp(rows[1].sub, "Server 192.0.2.1 offers the address. Sent to every station.") == 0);

    /* Whose reply could be seen here. */
    fresh();
    add(0, PPC_EV_DHCP_DISCOVER, PPC_EVF_OWN | PPC_EVF_BCAST | PPC_EVF_REPLY_HERE, 250000, 1);
    add(0, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST | PPC_EVF_REPLY_HERE, 250100, 2);
    add(0, PPC_EV_DHCP_REQUEST, PPC_EVF_BCAST, 250200, 3);
    n = psk_ev_rows(&E, rows, PSK_EV_ROWS);
    check("this device's own DISCOVER says its reply comes back to this port",
          n == 3 && strcmp(rows[0].what, "DISCOVER" DOT "THIS DEVICE") == 0 &&
              strcmp(rows[0].sub, "Asks for 192.0.2.50. Its reply comes back to this port.") == 0);
    check("another device's with the broadcast flag says its reply would pass this port",
          strcmp(rows[1].sub, "Asks for 192.0.2.50. It asks for a broadcast reply, so the reply would pass this port.") == 0);
    psk_ev_limits(&E, b, sizeof(b));
    check("the DHCP caveat is on the page when any request's reply is not for this port",
          strstr(rows[2].sub, "not seeing one here says nothing") && strstr(b, "A DHCP server answers the device that asked"));
    E.nobs[0] = 2;
    psk_ev_limits(&E, b, sizeof(b));
    check("and not when every reply would be seen here", !strstr(b, "A DHCP server answers the device that asked"));

    /* DNS: failure codes, an answer without its question, IPv6. */
    fresh();
    add(1, PPC_EV_DNS_FAILURE, PPC_EVF_OWN, 253500, 20)->a = 3u << 16;
    add(1, PPC_EV_DNS_FAILURE, PPC_EVF_UNPAIRED, 253600, 21)->a = 2u << 16;
    add(1, PPC_EV_DNS_RETRY, 0, 253700, 22);
    add(1, PPC_EV_DNS_ANSWER, PPC_EVF_V6, 253800, 23)->b = 1500;
    n = psk_ev_rows(&E, rows, PSK_EV_ROWS);
    check("NXDOMAIN is said as the resolver's own word, after the mark",
          n == 4 && strcmp(rows[0].when, "+0.5 S") == 0 && rows[0].side == 1 &&
              strcmp(rows[0].what, "FAILURE" DOT "NXDOMAIN" DOT "FROM 192.0.2.50") == 0 &&
              strncmp(rows[0].sub, "The resolver says the name does not exist.", 42) == 0);
    check("an answer whose question was not seen has no response time, and says why",
          strcmp(rows[1].what, "FAILURE" DOT "SERVFAIL" DOT "FROM 192.0.2.50") == 0 &&
              strstr(rows[1].sub, "Its question was not seen in this capture, so it has no response time."));
    check("another device's repeated question says its answer may never pass this port",
          strcmp(rows[2].what, "QUERY AGAIN" DOT "TO 192.0.2.50") == 0 &&
              strcmp(rows[2].sub, "The same question again. Another device's question: its answer goes to that device "
                                  "and may never pass this port.") == 0);
    check("an answer over IPv6 says so", strcmp(rows[3].what, "ANSWER" DOT "1.5 MS" DOT "FROM 192.0.2.50") == 0 &&
                                             strstr(rows[3].sub, " Over IPv6."));

    /* Every kind, with and without its addresses: a row, a whole sentence,
     * no verdict. */
    ok = 1;
    for (k = 1; k < PPC_EV_KINDS; k++) {
        unsigned flags;

        for (flags = 0; flags <= PPC_EVF_ALL; flags += 7) {
            struct ppc_evt_ev_obs *o;

            fresh();
            o = add(k < PPC_EV_DNS_QUERY ? 0 : 1, k, flags & PPC_EVF_ALL, 253000 + k, k);
            o->a = (flags & 1u) ? 5u << 16 : 9u << 16;
            o->b = (flags & 2u) ? 86400 : 0;
            if (flags & 4u) {
                o->mac[0] = '\0';
                o->ip[0] = '\0';
                o->ip2[0] = '\0';
            }
            if (psk_ev_rows(&E, rows, PSK_EV_ROWS) != 1 || !sound(&E)) {
                printf("     kind %u flags %#x\n", k, flags & PPC_EVF_ALL);
                ok = 0;
            }
        }
    }
    check("every kind of observation, with any flags and with or without addresses, is a sound row", ok);
    fresh();
    add(0, PPC_EV_NONE, 0, 1, 1);
    add(0, PPC_EV_KINDS, 0, 1, 1);
    check("a kind this build does not know is not a row", psk_ev_rows(&E, rows, PSK_EV_ROWS) == 0);
}

/* ---- the model ----------------------------------------------------------------------------- */

static void model(void)
{
    psk_model_init(&M);
    check("a capture without a mark holds none", psk_ev_held(&M) == 0 && !M.have_ev);
    feed("mark\t1\t1\t4000\tcapturing\t120\t0");
    check("a mark line: one mark, when it was set, on the capture's clock",
          M.nmarks == 1 && M.first_mark == 1 && M.last_mark.at_ms == 4000 && M.last_mark.seq == 120 && psk_ev_held(&M) == 1);
    feed("mark\t9\t2\t64000\thold\t5000\t1700000000");
    check("the ninth mark: eight are held, from the second", M.nmarks == 9 && M.first_mark == 2 && psk_ev_held(&M) == 8 &&
                                                              M.last_mark.state == PPC_STATE_HOLD && M.last_mark.wall_s == 1700000000u);
    M.want_ev = 7;
    check("an answer to an older request is not taken",
          feed("ev.begin\t6\t9\t2\t9\t64000\thold\t5000\t0\t65000") == 0 && feed("ev.end\t6") == 0 && !M.have_ev);
    feed("ev.begin\t7\t9\t2\t9\t64000\thold\t5000\t0\t65000");
    check("an answer is not shown until its last line", !M.have_ev);
    check("the answer asked for is taken whole at its last line", (feed("ev.end\t7") & PSK_CHANGED_EVID) && M.have_ev &&
                                                                      M.ev.head.id == 9 && M.ev.head.first == 2);
    psk_model_clear_packets(&M);
    check("a new capture has no mark and no evidence", M.nmarks == 0 && !M.have_ev && psk_ev_held(&M) == 0 && M.want_ev == 0);
    check("the list of a mark's packets is DHCP, ARP and DNS",
          psk_ev_classes() == (uint8_t)(1u << PPC_CLASS_DHCP | 1u << PPC_CLASS_ARP | 1u << PPC_CLASS_DNS));
}

int main(int argc, char **argv)
{
    data_dir = argc > 1 ? argv[1] : "tests/data/packetshark";
    /* The wall clock is said in the device's own time zone: UTC here. */
    setenv("TZ", "UTC", 1);
    tzset();
    fixtures();
    built();
    model();
    printf("psk_ev_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
