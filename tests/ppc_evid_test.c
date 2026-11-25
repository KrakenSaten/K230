/*
 * Host tests for pocketpcap's evidence history (core/pocketpcap/ppc_evid.h,
 * N1.0): what a mark holds of the minute before and the minute after it.
 *
 * The five fixtures of tests/ppc_evid_fixtures.h are played through the
 * real decoder, the DNS diagnosis and the network intelligence, as the
 * helper does, on a clock the test owns. Checked:
 *   - each fixture's observations, counted and kept, with their packets;
 *   - that a request whose reply this port cannot see is never marked as
 *     one whose reply would pass here, and that nothing here turns a reply
 *     not seen into an observation;
 *   - that what was observed while frames were lost is kept, with the loss;
 *   - a silent minute after a mark: closed by the clock, watched, empty;
 *   - replay consistency: a fixture played twice, and with the clock told
 *     four times as often, gives the same marks byte for byte;
 *   - marks near START, on HOLD, near STOP and after it; a stamp before the
 *     mark handed over after it; the history's bound and what a mark says
 *     when observations had left it; routine ARP never crowding out the
 *     rest; the ninth mark replacing the first.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_evid.h"
#include "pocketpcap/ppc_net.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_signals.h"
#include "ppc_evid_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int verbose;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

/* ---- the player ---------------------------------------------------------------------------- */

struct play {
    struct ppc_signals sig;
    struct ppc_net net;
    struct ppc_evid ev;
    uint32_t lead_ms;
    int have_lead;
    uint32_t seq;
    uint8_t self[6];
    int have_self;
    unsigned state;  /* PPC_EVS_* */
    uint64_t lost;
    uint32_t mark_id[4];
    unsigned nmarks;
};

static void on_said(void *ctx, const struct ppc_net_said *s)
{
    struct play *p = ctx;
    struct ppc_ev_obs o;

    if (ppc_evid_from_net(s, p->lead_ms, p->have_self ? p->self : NULL, &o)) {
        ppc_evid_note(&p->ev, &o);
    }
}

static void play_frame(struct play *p, const struct fx_step *s)
{
    struct ppc_frame f;
    struct ppc_record r;
    struct ppc_ev_obs o;
    uint32_t pay;
    uint32_t seq;

    if (p->state == PPC_EVS_HELD) {
        return; /* on HOLD a frame is drained and counted, never decoded */
    }
    memset(&f, 0, sizeof(f));
    f.data = s->frame;
    f.cap_len = s->len;
    f.wire_len = s->len;
    f.pkttype = s->dir;
    pay = ppc_decode(&f, &r);
    if (!p->have_lead) {
        p->have_lead = 1;
        p->lead_ms = s->t_ms;
    }
    r.rel_ns = (uint64_t)(s->t_ms - p->lead_ms) * 1000000u;
    seq = ++p->seq;
    if (s->dir == PPC_PKT_OUTGOING && !p->have_self) {
        memcpy(p->self, s->frame + 6, 6);
        p->have_self = 1;
    }
    r.signal = ppc_signals_frame(&p->sig, &r, 0, seq);
    if (ppc_evid_from_dns(&r, ppc_sig_diag(r.signal), p->sig.dns_last_us, s->dir == PPC_PKT_HOST, p->lead_ms, seq, &o)) {
        ppc_evid_note(&p->ev, &o);
    }
    ppc_net_frame(&p->net, &r, s->frame, s->len, pay, seq);
}

/* Play a fixture with the clock told every cadence_ms. */
static void play(struct play *p, const struct fx *f, uint32_t cadence_ms)
{
    uint32_t next = 0;
    uint32_t i;

    memset(p, 0, sizeof(*p));
    if (ppc_signals_init(&p->sig, 64) != 0 || ppc_net_init(&p->net) != 0 || ppc_evid_init(&p->ev) != 0) {
        printf("FAIL out of memory\n");
        exit(2);
    }
    ppc_net_listen(&p->net, on_said, p);
    p->state = PPC_EVS_WATCHED;
    for (i = 0; i < f->nsteps; i++) {
        const struct fx_step *s = &f->step[i];

        while (next <= s->t_ms) {
            ppc_evid_tick(&p->ev, next, p->state, p->lost);
            next += cadence_ms;
        }
        switch (s->op) {
        case FX_FRAME:
            play_frame(p, s);
            break;
        case FX_LOST:
            p->lost += s->n;
            break;
        case FX_HOLD_ON:
        case FX_HOLD_OFF:
            ppc_evid_tick(&p->ev, s->t_ms, p->state, p->lost);
            p->state = s->op == FX_HOLD_ON ? PPC_EVS_HELD : PPC_EVS_WATCHED;
            ppc_evid_tick(&p->ev, s->t_ms, p->state, p->lost);
            break;
        case FX_MARK:
            ppc_evid_tick(&p->ev, s->t_ms, p->state, p->lost);
            if (p->nmarks < 4) {
                p->mark_id[p->nmarks++] =
                    ppc_evid_mark(&p->ev, s->t_ms, p->state == PPC_EVS_HELD ? PPC_STATE_HOLD : PPC_STATE_CAPTURING, p->seq)
                        ->id;
            }
            break;
        case FX_STOP:
            ppc_evid_tick(&p->ev, s->t_ms, p->state, p->lost);
            ppc_evid_stop(&p->ev, s->t_ms);
            break;
        default:
            break;
        }
    }
}

static void play_end(struct play *p)
{
    ppc_evid_free(&p->ev);
    ppc_net_free(&p->net);
    ppc_signals_free(&p->sig);
}

static const char *kind_name(unsigned k)
{
    static const char *const n[PPC_EV_KINDS] = { "-",        "DISCOVER", "OFFER",   "REQUEST",  "ACK",
                                                 "NAK",      "DECLINE",  "RELEASE", "INFORM",   "ARP-REQ",
                                                 "ARP-REPLY", "ARP-PROBE", "ARP-ANN", "ARP-CHANGED", "ARP-TWO-MACS",
                                                 "DNS-QUERY", "DNS-RETRY", "DNS-ANSWER", "DNS-FAILURE" };

    return k < PPC_EV_KINDS ? n[k] : "?";
}

static void dump(const struct ppc_ev_mark *m)
{
    unsigned w;
    unsigned i;

    if (!verbose || !m) {
        return;
    }
    printf("     mark %u at %u ms, seq %u, state %u\n", m->id, m->at_ms, m->seq, m->state);
    for (w = 0; w < 2; w++) {
        const struct ppc_ev_side *s = &m->side[w];

        printf("     %s span %u ms flags %02x watched %u held %u shed %u lost %llu kept %u more %u\n",
               w ? "after " : "before", s->span_ms, s->flags, s->watched_s, s->held_s, s->shed_s,
               (unsigned long long)s->lost, s->nkept, s->more);
        for (i = 1; i < PPC_EV_KINDS; i++) {
            if (s->count[i]) {
                printf("       %-13s %u\n", kind_name(i), s->count[i]);
            }
        }
        for (i = 0; i < s->nkept; i++) {
            const struct ppc_ev_obs *o = &s->kept[i];

            printf("       %7u ms #%-3u %-13s fl %02x mac ..%02x ip .%u ip2 .%u a %08x b %u\n", o->t_ms, o->seq,
                   kind_name(o->kind), o->flags, o->mac[5], o->ip[3], o->ip2[3], o->a, o->b);
        }
    }
}

static const struct ppc_ev_obs *kept_of(const struct ppc_ev_side *s, unsigned kind, unsigned nth)
{
    unsigned i;

    for (i = 0; i < s->nkept; i++) {
        if (s->kept[i].kind == kind && nth-- == 0) {
            return &s->kept[i];
        }
    }
    return NULL;
}

static unsigned total_of(const struct ppc_ev_side *s)
{
    unsigned n = 0;
    unsigned i;

    for (i = 1; i < PPC_EV_KINDS; i++) {
        n += s->count[i];
    }
    return n;
}

/* Rows are oldest first, and every row's packet number is one a frame got. */
static int rows_sound(const struct ppc_ev_side *s, uint32_t last_seq)
{
    unsigned i;

    for (i = 0; i < s->nkept; i++) {
        if (s->kept[i].seq == 0 || s->kept[i].seq > last_seq || (i && s->kept[i].t_ms < s->kept[i - 1].t_ms)) {
            return 0;
        }
    }
    return s->nkept + s->more == total_of(s);
}

/* ---- the fixtures -------------------------------------------------------------------------- */

static void fixture_dhcp_repeat(void)
{
    static struct fx f;
    static struct play p;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_side *b;
    const struct ppc_ev_side *a;
    const struct ppc_ev_obs *o;
    unsigned i;
    int ok;

    fx_dhcp_repeat(&f);
    play(&p, &f, 1000);
    m = ppc_evid_get(&p.ev, 0);
    dump(m);
    b = &m->side[0];
    a = &m->side[1];
    check("dhcp-repeat: the mark is set at 50 s, on the newest packet, while capturing",
          m && m->id == 1 && m->at_ms == 50000 && m->seq == 5 && m->state == PPC_STATE_CAPTURING);
    check("before: four DISCOVER and the router's one ARP request, nothing else",
          b->count[PPC_EV_DHCP_DISCOVER] == 4 && b->count[PPC_EV_ARP_REQUEST] == 1 && total_of(b) == 5);
    check("after: the fifth DISCOVER and one ARP request", a->count[PPC_EV_DHCP_DISCOVER] == 1 &&
                                                             a->count[PPC_EV_ARP_REQUEST] == 1 && total_of(a) == 2);
    ok = 1;
    for (i = 0; i < 4; i++) {
        o = kept_of(b, PPC_EV_DHCP_DISCOVER, i);
        ok = ok && o && o->seq == i + 2u && o->a == 0x5e210001u && o->mac[5] == 0x21 && (o->flags & PPC_EVF_BCAST) &&
             !(o->flags & (PPC_EVF_OWN | PPC_EVF_REPLY_HERE | PPC_EVF_PLAIN));
    }
    check("each DISCOVER is a row with its packet, its client and its transaction id", ok);
    check("and none is marked as a request whose reply would pass this port (its OFFER is unicast)", ok);
    check("no OFFER, ACK or NAK is counted on either side: none was seen, and none is made up",
          !b->count[PPC_EV_DHCP_OFFER] && !b->count[PPC_EV_DHCP_ACK] && !b->count[PPC_EV_DHCP_NAK] &&
              !a->count[PPC_EV_DHCP_OFFER] && !a->count[PPC_EV_DHCP_ACK] && !a->count[PPC_EV_DHCP_NAK]);
    check("the window before is cut where the capture started: 50 s of the 60",
          b->span_ms == 50000 && (b->flags & PPC_EVW_CUT_START) && !(b->flags & (PPC_EVW_HISTORY | PPC_EVW_OPEN)));
    check("the window after is whole and closed: 60 s, every second watched, nothing lost",
          a->span_ms == 60000 && !(a->flags & (PPC_EVW_OPEN | PPC_EVW_CUT_STOP)) && a->watched_s >= 60 &&
              a->held_s == 0 && a->shed_s == 0 && a->lost == 0);
    check("rows are in time order and add up with what was counted",
          rows_sound(b, p.seq) && rows_sound(a, p.seq));
    check("the network intelligence called nothing unanswered either (4 requests not judged)",
          p.net.st.dhcp_no_offer == 0 && p.net.st.dhcp_unseen == 1);
    play_end(&p);
}

static void fixture_dns_local(void)
{
    static struct fx f;
    static struct play p;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_side *b;
    const struct ppc_ev_side *a;
    const struct ppc_ev_obs *q;
    const struct ppc_ev_obs *r;
    const struct ppc_ev_obs *arp;

    fx_dns_local(&f);
    play(&p, &f, 1000);
    m = ppc_evid_get(&p.ev, 0);
    dump(m);
    b = &m->side[0];
    a = &m->side[1];
    q = kept_of(b, PPC_EV_DNS_QUERY, 0);
    r = kept_of(b, PPC_EV_DNS_ANSWER, 0);
    arp = kept_of(b, PPC_EV_ARP_REQUEST, 0);
    check("dns-local: one query and one answer before the mark",
          b->count[PPC_EV_DNS_QUERY] == 1 && b->count[PPC_EV_DNS_ANSWER] == 1 && !b->count[PPC_EV_DNS_RETRY] &&
              !b->count[PPC_EV_DNS_FAILURE]);
    check("the query is this device's own, and its reply would pass this port",
          q && q->seq == 3 && (q->flags & PPC_EVF_OWN) && (q->flags & PPC_EVF_REPLY_HERE) && q->ip[3] == 53 &&
              q->ip2[3] == 0x10 && (q->a & 0xffffu) == 0x1234);
    check("the answer is paired with it: to this device, NOERROR, 31 ms after the query",
          r && r->seq == 4 && (r->flags & PPC_EVF_OWN) && !(r->flags & PPC_EVF_UNPAIRED) && (r->a >> 16) == 0 &&
              r->b == 31000 && r->ip[3] == 53);
    check("this device's own ARP request is a row, not routine, its reply expected here",
          arp && arp->seq == 1 && (arp->flags & PPC_EVF_OWN) && (arp->flags & PPC_EVF_REPLY_HERE) &&
              !(arp->flags & PPC_EVF_PLAIN) && arp->ip2[3] == 53 && b->count[PPC_EV_ARP_REPLY] == 1);
    check("the window after is cut where the capture stopped, 5 s after the mark, and empty",
          a->span_ms == 5000 && (a->flags & PPC_EVW_CUT_STOP) && !(a->flags & PPC_EVW_OPEN) && total_of(a) == 0 &&
              a->nkept == 0);
    check("rows are in time order and add up", rows_sound(b, p.seq) && rows_sound(a, p.seq));
    play_end(&p);
}

static void fixture_unseen_reply(void)
{
    static struct fx f;
    static struct play p;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_side *b;
    const struct ppc_ev_obs *d;
    const struct ppc_ev_obs *q;
    const struct ppc_ev_obs *pr;
    const struct ppc_ev_obs *an;

    fx_unseen_reply(&f);
    play(&p, &f, 1000);
    m = ppc_evid_get(&p.ev, 0);
    dump(m);
    b = &m->side[0];
    d = kept_of(b, PPC_EV_DHCP_DISCOVER, 0);
    q = kept_of(b, PPC_EV_DHCP_REQUEST, 0);
    pr = kept_of(b, PPC_EV_ARP_PROBE, 0);
    an = kept_of(b, PPC_EV_ARP_ANNOUNCE, 0);
    check("unseen-reply: DISCOVER, REQUEST, a probe and an announcement, in that order",
          d && q && pr && an && d->seq == 1 && q->seq == 2 && pr->seq == 3 && an->seq == 4 && total_of(b) == 4);
    check("the REQUEST names the server it chose and the address it asks for",
          q && q->ip[3] == 57 && q->ip2[3] == FX_ROUTER && q->mac[5] == 0x31);
    check("neither request is marked as one whose reply would pass this port",
          d && q && !(d->flags & PPC_EVF_REPLY_HERE) && !(q->flags & PPC_EVF_REPLY_HERE));
    check("no OFFER and no ACK is counted: the unicast replies were never here to see",
          !b->count[PPC_EV_DHCP_OFFER] && !b->count[PPC_EV_DHCP_ACK]);
    check("what the client did next is observed: it probed 192.0.2.57, then announced it",
          pr && an && pr->ip2[3] == 57 && pr->ip[3] == 0 && an->ip[3] == 57 && an->mac[5] == 0x31 &&
              !(pr->flags & PPC_EVF_PLAIN) && !(an->flags & PPC_EVF_PLAIN));
    check("the network intelligence judged neither request (2 not judged, 0 unanswered)",
          p.net.st.dhcp_unseen == 2 && p.net.st.dhcp_no_offer == 0 && p.net.st.dhcp_no_ack == 0);
    check("the window before is cut at START (30 s); the one after is whole",
          b->span_ms == 30000 && (b->flags & PPC_EVW_CUT_START) && m->side[1].span_ms == 60000 &&
              total_of(&m->side[1]) == 0);
    play_end(&p);
}

static void fixture_loss_event(void)
{
    static struct fx f;
    static struct play p;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_side *b;
    const struct ppc_ev_obs *nak;
    const struct ppc_ev_obs *two;

    fx_loss_event(&f);
    play(&p, &f, 1000);
    m = ppc_evid_get(&p.ev, 0);
    dump(m);
    b = &m->side[0];
    nak = kept_of(b, PPC_EV_DHCP_NAK, 0);
    two = kept_of(b, PPC_EV_ARP_TWO_MACS, 0);
    check("loss-event: the three announcements of 192.0.2.77 are all there",
          b->count[PPC_EV_ARP_ANNOUNCE] == 3 && kept_of(b, PPC_EV_ARP_ANNOUNCE, 2) &&
              kept_of(b, PPC_EV_ARP_ANNOUNCE, 0)->mac[5] == 0x41 && kept_of(b, PPC_EV_ARP_ANNOUNCE, 1)->mac[5] == 0x42);
    check("the address changing hands and two MACs claiming it are observed, with their packets",
          b->count[PPC_EV_ARP_CHANGED] == 1 && b->count[PPC_EV_ARP_TWO_MACS] == 1 && two && two->ip[3] == 77 &&
              two->seq == 3 && kept_of(b, PPC_EV_ARP_CHANGED, 0)->seq == 2);
    check("a packet's own statement comes before what it showed: the announcement, then the change",
          b->nkept == 6 && b->kept[1].kind == PPC_EV_ARP_ANNOUNCE && b->kept[2].kind == PPC_EV_ARP_CHANGED &&
              b->kept[3].kind == PPC_EV_ARP_ANNOUNCE && b->kept[4].kind == PPC_EV_ARP_TWO_MACS);
    check("the broadcast NAK is observed: its server, its client, its packet",
          b->count[PPC_EV_DHCP_NAK] == 1 && nak && nak->seq == 4 && nak->ip2[3] == FX_ROUTER && nak->mac[5] == 0x44 &&
              (nak->flags & PPC_EVF_BCAST));
    check("the 500 frames reported lost are told with the window, not hidden and not guessed around",
          b->lost == 500 && b->held_s == 0 && b->shed_s == 0);
    check("loss removes no observation: every row is kept, none is counted without its packet",
          b->more == 0 && rows_sound(b, p.seq) && b->nkept == total_of(b));
    check("the window after saw no loss", m->side[1].lost == 0 && m->side[1].span_ms == 60000);
    play_end(&p);
}

static void fixture_silence(void)
{
    static struct fx f;
    static struct play p;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_side *b;
    const struct ppc_ev_side *a;

    fx_silence(&f);
    play(&p, &f, 1000);
    m = ppc_evid_get(&p.ev, 0);
    dump(m);
    b = &m->side[0];
    a = &m->side[1];
    check("silence: before the mark, two routine ARP requests and this device's DNS exchange",
          b->count[PPC_EV_ARP_REQUEST] == 2 && b->count[PPC_EV_DNS_QUERY] == 1 && b->count[PPC_EV_DNS_ANSWER] == 1 &&
              total_of(b) == 4 && b->nplain == 2);
    check("after the mark nothing is counted and no row is kept", total_of(a) == 0 && a->nkept == 0 && a->more == 0);
    check("the silent window is closed by the clock, whole: 60 s", a->span_ms == 60000 && !(a->flags & PPC_EVW_OPEN) &&
                                                                       !(a->flags & PPC_EVW_CUT_STOP));
    check("and it was watched throughout: silence here is the capture seeing nothing, not being away",
          a->watched_s >= 60 && a->held_s == 0 && a->shed_s == 0 && a->lost == 0);
    play_end(&p);
}

/* ---- replay consistency -------------------------------------------------------------------- */

static void replay(void)
{
    static struct fx f;
    static struct play p1;
    static struct play p2;
    static struct play p3;
    unsigned i;
    int same = 1;
    int cadence = 1;

    for (i = 0; i < FX_COUNT; i++) {
        const struct ppc_ev_mark *a;
        const struct ppc_ev_mark *b;
        const struct ppc_ev_mark *c;

        fx_build(i, &f);
        play(&p1, &f, 1000);
        play(&p2, &f, 1000);
        play(&p3, &f, 250);
        a = ppc_evid_get(&p1.ev, 0);
        b = ppc_evid_get(&p2.ev, 0);
        c = ppc_evid_get(&p3.ev, 0);
        same = same && a && b && memcmp(a, b, sizeof(*a)) == 0 && p1.ev.total == p2.ev.total;
        cadence = cadence && a && c && memcmp(a, c, sizeof(*a)) == 0;
        if (!(a && c && memcmp(a, c, sizeof(*a)) == 0)) {
            printf("     %s differs with the clock told every 250 ms\n", f.name);
            verbose = 1;
            dump(a);
            dump(c);
            verbose = 0;
        }
        play_end(&p1);
        play_end(&p2);
        play_end(&p3);
    }
    check("replay: every fixture played twice gives the same mark, byte for byte", same);
    check("replay: and the same with the clock told four times as often", cadence);
}

/* ---- the edges ----------------------------------------------------------------------------- */

static void note_at(struct ppc_evid *ev, uint32_t t_ms, unsigned kind, unsigned flags, uint32_t seq)
{
    struct ppc_ev_obs o;

    memset(&o, 0, sizeof(o));
    o.t_ms = t_ms;
    o.kind = (uint8_t)kind;
    o.flags = (uint8_t)flags;
    o.seq = seq;
    ppc_evid_note(ev, &o);
}

static void edges(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;
    uint32_t first;
    uint32_t last;
    uint32_t t;
    unsigned i;
    int ok;

    /* A mark before anything was told. */
    check("init: one allocation, sized as documented",
          ppc_evid_init(&ev) == 0 && ppc_evid_bytes() == PPC_EV_HISTORY * 32u + PPC_EV_MARKS * sizeof(struct ppc_ev_mark));
    check("no mark is held before one is set", ppc_evid_get(&ev, 0) == NULL && ppc_evid_ids(&ev, &first, &last) == 0);
    ppc_evid_tick(&ev, 0, PPC_EVS_WATCHED, 0);
    ppc_evid_tick(&ev, 1000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 1500, PPC_STATE_CAPTURING, 0);
    check("a mark 1.5 s after START: the window before is 1.5 s, cut at START, empty, no packet yet",
          m && m->id == 1 && m->side[0].span_ms == 1500 && (m->side[0].flags & PPC_EVW_CUT_START) && m->seq == 0 &&
              total_of(&m->side[0]) == 0 && (m->side[1].flags & PPC_EVW_OPEN) && m->side[1].span_ms == 0);
    /* The window after fills as the clock is told. */
    note_at(&ev, 2000, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 1);
    ppc_evid_tick(&ev, 2500, PPC_EVS_WATCHED, 0);
    m = ppc_evid_get(&ev, 1);
    check("the window after is open and grows: 1 s, one observation", m->side[1].span_ms == 1000 &&
                                                                         (m->side[1].flags & PPC_EVW_OPEN) &&
                                                                         m->side[1].count[PPC_EV_DHCP_DISCOVER] == 1);
    /* A frame stamped before the mark, handed over after it. */
    note_at(&ev, 1400, PPC_EV_ARP_PROBE, 0, 2);
    check("an observation stamped before the mark and handed over after it counts before",
          m->side[0].count[PPC_EV_ARP_PROBE] == 1 && m->side[0].nkept == 1 && m->side[1].count[PPC_EV_ARP_PROBE] == 0);

    /* HOLD: a mark on HOLD says so, and both windows tell the seconds held. */
    for (t = 3000; t <= 10000; t += 1000) {
        ppc_evid_tick(&ev, t, PPC_EVS_WATCHED, 0);
    }
    for (t = 11000; t <= 20000; t += 1000) {
        ppc_evid_tick(&ev, t, PPC_EVS_HELD, 0);
    }
    m = ppc_evid_mark(&ev, 20500, PPC_STATE_HOLD, 2);
    check("a mark on HOLD records the state and the 10 s held before it",
          m && m->id == 2 && m->state == PPC_STATE_HOLD && m->side[0].held_s == 10 && m->side[0].watched_s >= 10);
    for (t = 21000; t <= 25000; t += 1000) {
        ppc_evid_tick(&ev, t, PPC_EVS_HELD, 0);
    }
    for (t = 26000; t <= 30000; t += 1000) {
        ppc_evid_tick(&ev, t, PPC_EVS_SHED, 40);
    }
    m = ppc_evid_get(&ev, 2);
    /* 20.5 s to 30 s touches seconds 20 to 29: six held, four shed. */
    check("the window after tells its own held seconds, the seconds shed, and the frames lost",
          m->side[1].held_s == 6 && m->side[1].shed_s == 4 && m->side[1].lost == 40 && m->side[1].span_ms == 9500);

    /* STOP: open windows close where the capture stopped; a mark after STOP
     * is set at the stop. */
    ppc_evid_tick(&ev, 31000, PPC_EVS_WATCHED, 40);
    ppc_evid_stop(&ev, 31200);
    m = ppc_evid_get(&ev, 2);
    check("STOP closes the window after: cut, 10.7 s",
          !(m->side[1].flags & PPC_EVW_OPEN) && (m->side[1].flags & PPC_EVW_CUT_STOP) && m->side[1].span_ms == 10700);
    note_at(&ev, 31300, PPC_EV_DHCP_ACK, 0, 3);
    check("nothing is counted into a closed window", m->side[1].count[PPC_EV_DHCP_ACK] == 0);
    m = ppc_evid_mark(&ev, 40000, PPC_STATE_STOPPED, 3);
    check("a mark after STOP is set where the capture stopped, its window after empty and cut",
          m && m->id == 3 && m->at_ms == 31200 && m->state == PPC_STATE_STOPPED && m->side[1].span_ms == 0 &&
              (m->side[1].flags & PPC_EVW_CUT_STOP) && !(m->side[1].flags & PPC_EVW_OPEN));
    ppc_evid_free(&ev);

    /* The history's bound. */
    ppc_evid_init(&ev);
    ppc_evid_tick(&ev, 0, PPC_EVS_WATCHED, 0);
    for (i = 0; i < PPC_EV_HISTORY + 500u; i++) {
        note_at(&ev, 100000u + i * 10u, PPC_EV_ARP_REQUEST, PPC_EVF_PLAIN | PPC_EVF_BCAST, i + 1u);
    }
    note_at(&ev, 126000, PPC_EV_DHCP_NAK, PPC_EVF_BCAST, 9001);
    ppc_evid_tick(&ev, 126000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 126100, PPC_STATE_CAPTURING, 9001);
    check("more observations than the history holds: the mark says so, and counts what remained",
          m && (m->side[0].flags & PPC_EVW_HISTORY) && total_of(&m->side[0]) == PPC_EV_HISTORY &&
              m->side[0].count[PPC_EV_DHCP_NAK] == 1);
    ok = m->side[0].nkept == PPC_EV_PLAIN_KEEP + 1u && m->side[0].nplain == PPC_EV_PLAIN_KEEP &&
         kept_of(&m->side[0], PPC_EV_DHCP_NAK, 0) != NULL && m->side[0].more == PPC_EV_HISTORY - m->side[0].nkept;
    check("routine ARP takes three rows at most: the NAK among 2,047 requests is still a row", ok);
    /* The same after the mark. */
    for (i = 0; i < 40; i++) {
        note_at(&ev, 126200u + i, PPC_EV_ARP_REPLY, PPC_EVF_PLAIN, 9100u + i);
    }
    for (i = 0; i < 20; i++) {
        note_at(&ev, 126300u + i, PPC_EV_DNS_QUERY, PPC_EVF_OWN, 9200u + i);
    }
    m = ppc_evid_get(&ev, 0);
    check("after a mark: at most twelve rows, three of them routine, the rest counted as more",
          m->side[1].nkept == PPC_EV_KEEP && m->side[1].nplain == PPC_EV_PLAIN_KEEP && total_of(&m->side[1]) == 60 &&
              m->side[1].more == 48);
    /* An observation of no kind, or of one that does not exist, is not taken. */
    note_at(&ev, 126400, PPC_EV_NONE, 0, 1);
    note_at(&ev, 126400, PPC_EV_KINDS, 0, 1);
    check("an observation without a kind is not taken", total_of(&m->side[1]) == 60);

    /* Nine marks: the first is gone, ids are never reused. */
    for (i = 0; i < PPC_EV_MARKS; i++) {
        ppc_evid_mark(&ev, 127000u + i * 100u, PPC_STATE_CAPTURING, 9300);
    }
    check("the ninth mark replaces the first: eight are held, 2 to 9",
          ppc_evid_ids(&ev, &first, &last) == PPC_EV_MARKS && first == 2 && last == 9 && ppc_evid_get(&ev, 1) == NULL &&
              ppc_evid_get(&ev, 2) && ppc_evid_get(&ev, 9)->id == 9 && ppc_evid_get(&ev, 0)->id == 9 &&
              ppc_evid_get(&ev, 10) == NULL);
    ppc_evid_free(&ev);
    check("freed: nothing is held and nothing is taken", ev.mem == NULL && ppc_evid_mark(&ev, 0, 0, 0) == NULL &&
                                                             ppc_evid_get(&ev, 0) == NULL);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    fixture_dhcp_repeat();
    fixture_dns_local();
    fixture_unseen_reply();
    fixture_loss_event();
    fixture_silence();
    replay();
    edges();
    printf("ppc_evid_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
