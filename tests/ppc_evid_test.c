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

/* A tick, and a successful reading of the drop counters at it, as the
 * helper makes one every 250 ms. */
static void tick_read(struct ppc_evid *ev, uint32_t t_ms, unsigned flags, uint64_t lost)
{
    ppc_evid_tick(ev, t_ms, flags);
    ppc_evid_counters(ev, t_ms, 1, lost);
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
            tick_read(&p->ev, next, p->state, p->lost);
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
            tick_read(&p->ev, s->t_ms, p->state, p->lost);
            p->state = s->op == FX_HOLD_ON ? PPC_EVS_HELD : PPC_EVS_WATCHED;
            tick_read(&p->ev, s->t_ms, p->state, p->lost);
            break;
        case FX_MARK:
            tick_read(&p->ev, s->t_ms, p->state, p->lost);
            if (p->nmarks < 4) {
                p->mark_id[p->nmarks++] =
                    ppc_evid_mark(&p->ev, s->t_ms, p->state == PPC_EVS_HELD ? PPC_STATE_HOLD : PPC_STATE_CAPTURING, p->seq, 0)
                        ->id;
            }
            break;
        case FX_STOP:
            tick_read(&p->ev, s->t_ms, p->state, p->lost);
            ppc_evid_stop(&p->ev, s->t_ms, 0);
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
    tick_read(&ev, 0, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 1500, PPC_STATE_CAPTURING, 0, 0);
    check("a mark 1.5 s after START: the window before is 1.5 s, cut at START, empty, no packet yet",
          m && m->id == 1 && m->side[0].span_ms == 1500 && (m->side[0].flags & PPC_EVW_CUT_START) && m->seq == 0 &&
              total_of(&m->side[0]) == 0 && (m->side[1].flags & PPC_EVW_OPEN) && m->side[1].span_ms == 0);
    /* The window after fills as the clock is told. */
    note_at(&ev, 2000, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 1);
    tick_read(&ev, 2500, PPC_EVS_WATCHED, 0);
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
        tick_read(&ev, t, PPC_EVS_WATCHED, 0);
    }
    for (t = 11000; t <= 20000; t += 1000) {
        tick_read(&ev, t, PPC_EVS_HELD, 0);
    }
    m = ppc_evid_mark(&ev, 20500, PPC_STATE_HOLD, 2, 0);
    check("a mark on HOLD records the state and the 10 s held before it",
          m && m->id == 2 && m->state == PPC_STATE_HOLD && m->side[0].held_s == 10 && m->side[0].watched_s >= 10);
    for (t = 21000; t <= 25000; t += 1000) {
        tick_read(&ev, t, PPC_EVS_HELD, 0);
    }
    for (t = 26000; t <= 30000; t += 1000) {
        tick_read(&ev, t, PPC_EVS_SHED, 40);
    }
    m = ppc_evid_get(&ev, 2);
    /* 20.5 s to 30 s touches seconds 20 to 29: six held, four shed. */
    check("the window after tells its own held seconds, the seconds shed, and the frames lost",
          m->side[1].held_s == 6 && m->side[1].shed_s == 4 && m->side[1].lost == 40 && m->side[1].span_ms == 9500);

    /* STOP: open windows close where the capture stopped; a mark after STOP
     * is set at the stop. */
    tick_read(&ev, 31000, PPC_EVS_WATCHED, 40);
    ppc_evid_stop(&ev, 31200, 0);
    m = ppc_evid_get(&ev, 2);
    check("STOP closes the window after: cut, 10.7 s",
          !(m->side[1].flags & PPC_EVW_OPEN) && (m->side[1].flags & PPC_EVW_CUT_STOP) && m->side[1].span_ms == 10700);
    note_at(&ev, 31300, PPC_EV_DHCP_ACK, 0, 3);
    check("nothing is counted into a closed window", m->side[1].count[PPC_EV_DHCP_ACK] == 0);
    m = ppc_evid_mark(&ev, 40000, PPC_STATE_STOPPED, 3, 0);
    check("a mark after STOP is set where the capture stopped, its window after empty and cut",
          m && m->id == 3 && m->at_ms == 31200 && m->state == PPC_STATE_STOPPED && m->side[1].span_ms == 0 &&
              (m->side[1].flags & PPC_EVW_CUT_STOP) && !(m->side[1].flags & PPC_EVW_OPEN));
    ppc_evid_free(&ev);

    /* The history's bound. */
    ppc_evid_init(&ev);
    tick_read(&ev, 0, PPC_EVS_WATCHED, 0);
    for (i = 0; i < PPC_EV_HISTORY + 500u; i++) {
        note_at(&ev, 100000u + i * 10u, PPC_EV_ARP_REQUEST, PPC_EVF_PLAIN | PPC_EVF_BCAST, i + 1u);
    }
    note_at(&ev, 126000, PPC_EV_DHCP_NAK, PPC_EVF_BCAST, 9001);
    tick_read(&ev, 126000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 126100, PPC_STATE_CAPTURING, 9001, 0);
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
        ppc_evid_mark(&ev, 127000u + i * 100u, PPC_STATE_CAPTURING, 9300, 0);
    }
    check("the ninth mark replaces the first: eight are held, 2 to 9",
          ppc_evid_ids(&ev, &first, &last) == PPC_EV_MARKS && first == 2 && last == 9 && ppc_evid_get(&ev, 1) == NULL &&
              ppc_evid_get(&ev, 2) && ppc_evid_get(&ev, 9)->id == 9 && ppc_evid_get(&ev, 0)->id == 9 &&
              ppc_evid_get(&ev, 10) == NULL);
    ppc_evid_free(&ev);
    check("freed: nothing is held and nothing is taken", ev.mem == NULL && ppc_evid_mark(&ev, 0, 0, 0, 0) == NULL &&
                                                             ppc_evid_get(&ev, 0) == NULL);
}

/* ---- the review of PR #188 (2026-11-25) ---------------------------------------------------- *
 *
 * Five findings, each with the case that would have shown it: what the
 * capture could not see; a count-only burst between two ticks; frames lost
 * either side of a mark in its own second; a DNS answer whose question the
 * tracker no longer holds; a mark set minutes after STOP. */

#define BIT(c) (1u << (c))

static void late_none(void *ctx, uint32_t seq, uint16_t bit)
{
    (void)ctx;
    (void)seq;
    (void)bit;
}

/* P1: what the capture can observe. */
static void coverage(void)
{
    struct ppc_evid ev;
    uint32_t i;

    check("coverage: no DNS table is DNS unanalysed; no network tables, DHCP and ARP; both missing, all three",
          ppc_evid_unanalyzed(0, 1) == BIT(PPC_CLASS_DNS) &&
              ppc_evid_unanalyzed(1, 0) == (BIT(PPC_CLASS_DHCP) | BIT(PPC_CLASS_ARP)) &&
              ppc_evid_unanalyzed(0, 0) == PPC_EV_CLASSES && ppc_evid_unanalyzed(1, 1) == 0);
    check("coverage: the DNS AND DHCP preset keeps ARP out, and only ARP; IP AND ARP, NOT MY SSH and ALL keep none",
          ppc_cfilter_excludes(PPC_CFILTER_NAMES) == BIT(PPC_CLASS_ARP) && ppc_cfilter_excludes(PPC_CFILTER_IP) == 0 &&
              ppc_cfilter_excludes(PPC_CFILTER_NOSELF) == 0 && ppc_cfilter_excludes(PPC_CFILTER_ALL) == 0);
    {
        /* The table above is read from the presets' own expressions. */
        char names[160];
        char ip[160];

        ppc_cfilter_expr(PPC_CFILTER_NAMES, NULL, names, sizeof(names));
        ppc_cfilter_expr(PPC_CFILTER_IP, NULL, ip, sizeof(ip));
        check("coverage: as the expressions say: NAMES has ports 53, 67 and 68 and no arp; IP names arp",
              strstr(names, "port 53") && strstr(names, "port 67") && strstr(names, "port 68") && !strstr(names, "arp") &&
                  strstr(ip, "arp") != NULL);
    }
    /* A capture filtered to DNS and DHCP: an ARP observation cannot be
     * there (a replay applies no filter, so one could come): not taken. */
    ppc_evid_init(&ev);
    ppc_evid_coverage(&ev, PPC_CFILTER_NAMES, 0);
    tick_read(&ev, 0, PPC_EVS_WATCHED, 0);
    note_at(&ev, 100, PPC_EV_ARP_REQUEST, 0, 1);
    note_at(&ev, 200, PPC_EV_DHCP_DISCOVER, PPC_EVF_BCAST, 2);
    note_at(&ev, 300, PPC_EV_DNS_QUERY, PPC_EVF_OWN, 3);
    check("coverage: under DNS AND DHCP an ARP observation is not taken; DHCP and DNS are",
          ev.count == 2 && ev.hist[0].kind == PPC_EV_DHCP_DISCOVER && ev.hist[1].kind == PPC_EV_DNS_QUERY);
    ppc_evid_free(&ev);
    ppc_evid_init(&ev);
    ppc_evid_coverage(&ev, PPC_CFILTER_ALL, BIT(PPC_CLASS_DNS));
    note_at(&ev, 100, PPC_EV_DNS_ANSWER, 0, 1);
    note_at(&ev, 200, PPC_EV_ARP_REPLY, 0, 2);
    check("coverage: with DNS unanalysed a DNS observation is not taken; ARP is", ev.count == 1 &&
                                                                               ev.hist[0].kind == PPC_EV_ARP_REPLY &&
                                                                               ev.unanalyzed == BIT(PPC_CLASS_DNS));
    ppc_evid_coverage(&ev, 99, 0xff);
    check("coverage: a filter out of range is ALL, a mask is cut to DHCP, ARP and DNS",
          ev.cfilter == PPC_CFILTER_ALL && ev.unanalyzed == PPC_EV_CLASSES);
    ppc_evid_free(&ev);
    /* Every kind belongs to one of the three. */
    ppc_evid_init(&ev);
    ppc_evid_coverage(&ev, PPC_CFILTER_ALL, PPC_EV_CLASSES);
    for (i = 1; i < PPC_EV_KINDS; i++) {
        note_at(&ev, 100 + i, i, 0, i);
    }
    check("coverage: with all three unanalysed no observation of any kind is taken", ev.count == 0);
    ppc_evid_free(&ev);
}

/* P1: a burst that only counted, shorter than the tick. */
static void bursts(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;
    uint32_t t;

    /* Ticks every 250 ms say WATCHED; frames between 1100 and 1180 ms were
     * only counted. Told as frames (ppc_evid_bypass), the second knows. */
    ppc_evid_init(&ev);
    for (t = 0; t <= 2500; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 0);
        if (t == 1000) {
            uint32_t f;

            for (f = 1100; f <= 1180; f += 2) {
                ppc_evid_bypass(&ev, f);
            }
        }
    }
    m = ppc_evid_mark(&ev, 2500, PPC_STATE_CAPTURING, 9, 0);
    check("burst: 80 ms counted between two ticks: the window before has a second counted only",
          m && m->side[0].shed_s == 1 && m->side[0].watched_s == 3);
    ppc_evid_free(&ev);
    /* The same burst told at its edges, as the helper does when it starts
     * and stops shedding, and no frame told: the second knows too. */
    ppc_evid_init(&ev);
    for (t = 0; t <= 2500; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 0);
        if (t == 1000) {
            tick_read(&ev, 1100, PPC_EVS_WATCHED, 0);
            tick_read(&ev, 1100, PPC_EVS_SHED, 0);
            tick_read(&ev, 1180, PPC_EVS_SHED, 0);
            tick_read(&ev, 1180, PPC_EVS_WATCHED, 0);
        }
    }
    m = ppc_evid_mark(&ev, 2500, PPC_STATE_CAPTURING, 9, 0);
    check("burst: told at its two edges alone, the second is counted only as well", m && m->side[0].shed_s == 1);
    ppc_evid_free(&ev);
    /* Neither: what the 250 ms tick alone would say. The helper may not
     * leave it at that (tests/packetshark_lint.sh). */
    ppc_evid_init(&ev);
    for (t = 0; t <= 2500; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 0);
    }
    m = ppc_evid_mark(&ev, 2500, PPC_STATE_CAPTURING, 9, 0);
    check("burst: the tick alone cannot see it (why the helper tells the frames and the edges)",
          m && m->side[0].shed_s == 0);
    /* A frame of the window before, counted only and handed over after the
     * mark (a backlog): the window before learns it. */
    ppc_evid_bypass(&ev, 1900);
    check("burst: a frame counted only after the mark, stamped before it, reaches the window before",
          m->side[0].shed_s == 1);
    ppc_evid_bypass(&ev, 1950);
    check("burst: the same second is written once", m->side[0].shed_s == 1);
    tick_read(&ev, 3000, PPC_EVS_WATCHED, 0);
    ppc_evid_bypass(&ev, 3100);
    tick_read(&ev, 3250, PPC_EVS_WATCHED, 0);
    check("burst: one after the mark is in the window after", m->side[1].shed_s == 1);
    tick_read(&ev, 200000, PPC_EVS_WATCHED, 0);
    ppc_evid_bypass(&ev, 1000);
    ppc_evid_bypass(&ev, 500000);
    check("burst: a stamp older than the ring or ahead of the clock writes nothing",
          ev.secs[1 & (PPC_EV_SECONDS - 1)].sec != 1 && ev.secs[500 & (PPC_EV_SECONDS - 1)].sec != 500);
    ppc_evid_free(&ev);
}

/* P2: frames lost either side of a mark in its own second, all read. */
static void boundary_losses(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;

    ppc_evid_init(&ev);
    tick_read(&ev, 9000, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10100, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10300, PPC_EVS_WATCHED, 100); /* 100 read before the mark */
    tick_read(&ev, 10500, PPC_EVS_WATCHED, 150); /* 50 more, read at the mark */
    m = ppc_evid_mark(&ev, 10500, PPC_STATE_CAPTURING, 5, 0);
    tick_read(&ev, 10700, PPC_EVS_WATCHED, 230); /* 80 read just after it, the same second */
    tick_read(&ev, 11200, PPC_EVS_WATCHED, 230);
    tick_read(&ev, 12000, PPC_EVS_WATCHED, 260); /* 30 well after it */
    check("losses, counters read at the mark: 150 before it, 110 after it, none around it, 260 in all",
          m->side[0].lost == 150 && m->side[1].lost == 110 && m->edge == PPC_EV_EDGE_NONE &&
              m->side[0].lost + m->side[1].lost == 260);
    ppc_evid_free(&ev);
    /* Before the fix of the first review the window after counted the
     * mark's second whole. */
    ppc_evid_init(&ev);
    tick_read(&ev, 10000, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10400, PPC_EVS_WATCHED, 70);
    m = ppc_evid_mark(&ev, 10400, PPC_STATE_CAPTURING, 5, 0);
    tick_read(&ev, 10600, PPC_EVS_WATCHED, 70);
    tick_read(&ev, 11000, PPC_EVS_WATCHED, 70);
    check("losses: 70 lost before a mark in its own second are counted before it and not after it as well",
          m->side[0].lost == 70 && m->side[1].lost == 0);
    ppc_evid_free(&ev);
    /* A mark on a second's edge: what was read at it is still before it. */
    ppc_evid_init(&ev);
    tick_read(&ev, 10500, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 11000, PPC_EVS_WATCHED, 40);
    m = ppc_evid_mark(&ev, 11000, PPC_STATE_CAPTURING, 5, 0);
    tick_read(&ev, 11300, PPC_EVS_WATCHED, 55);
    check("losses: a mark on a second's edge keeps what was read at it before it (40) and what came after after (15)",
          m->side[0].lost == 40 && m->side[1].lost == 15 && m->edge == PPC_EV_EDGE_NONE);
    ppc_evid_free(&ev);
    /* After STOP the counters were read at STOP: a later mark has them all. */
    ppc_evid_init(&ev);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 2200, PPC_EVS_WATCHED, 12);
    ppc_evid_stop(&ev, 2200, 0);
    m = ppc_evid_mark(&ev, 90000, PPC_STATE_STOPPED, 7, 0);
    check("losses: a mark after STOP, the counters read at STOP, has all 12 before it, none after, none around it",
          m->side[0].lost == 12 && m->side[1].lost == 0 && m->edge == PPC_EV_EDGE_NONE);
    ppc_evid_free(&ev);
}

/* ---- the follow-up review (2026-11-25): readings that fail -------------------------------- */

static void read_fail(struct ppc_evid *ev, uint32_t t_ms, unsigned flags)
{
    ppc_evid_tick(ev, t_ms, flags);
    ppc_evid_counters(ev, t_ms, 0, 0);
}

/* The reading at the mark fails, and every one after it for `fail_s`
 * seconds; the first good one then tells 130 more. */
static void failed_at_mark(uint32_t fail_s)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;
    uint32_t back = 10500u + fail_s * 1000u;
    uint32_t t;
    char what[200];

    ppc_evid_init(&ev);
    tick_read(&ev, 9000, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10100, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10300, PPC_EVS_WATCHED, 100);
    read_fail(&ev, 10500, PPC_EVS_WATCHED); /* the reading at the mark */
    m = ppc_evid_mark(&ev, 10500, PPC_STATE_CAPTURING, 5, 0);
    snprintf(what, sizeof(what), "reading at the mark failed, %u s to recover: the mark knows it at once: around it "
             "from the last good reading (10.3 s), not known yet, both windows unknown from then", (unsigned)fail_s);
    check(what, m->edge == PPC_EV_EDGE_UNKNOWN && m->edge_from_ms == 10300 && m->side[0].lost == 100 &&
                    m->side[1].unknown && m->side[1].unknown_from_ms == 10300);
    for (t = 10750; t < back; t += 250) {
        read_fail(&ev, t, PPC_EVS_WATCHED);
    }
    snprintf(what, sizeof(what), "%u s of failed readings later it is still not known, and nothing was counted for "
             "it in either window", (unsigned)fail_s);
    check(what, m->edge == PPC_EV_EDGE_UNKNOWN && m->side[1].lost == 0 && m->side[1].unknown);
    tick_read(&ev, back, PPC_EVS_WATCHED, 230); /* the first good reading: 130 since 10.3 s */
    tick_read(&ev, back + 500u, PPC_EVS_WATCHED, 260); /* 30 more, well after */
    snprintf(what, sizeof(what), "recovered after %u s: the 130 are around the mark, from 10.3 s to the reading "
             "that told them, in neither window; 100 before, 30 after, 260 in all", (unsigned)fail_s);
    check(what, m->edge == PPC_EV_EDGE_COUNTED && m->edge_lost == 130 && m->edge_from_ms == 10300 &&
                    m->edge_to_ms == back && m->side[0].lost == 100 && m->side[1].lost == 30 && !m->side[1].unknown &&
                    m->side[0].lost + m->side[1].lost + m->edge_lost == 260);
    ppc_evid_free(&ev);
}

static void readings(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;
    const struct ppc_ev_mark *late;
    uint32_t t;
    unsigned i;
    int ok;

    /* The first review's fixed second is gone: a spell is placed by its two
     * readings, however long it lasts. */
    failed_at_mark(3);
    failed_at_mark(20);

    /* Repeated failures inside the window before, recovered before the
     * mark: the spell lies in the window, and its losses are its own. */
    ppc_evid_init(&ev);
    tick_read(&ev, 5000, PPC_EVS_WATCHED, 0);
    for (t = 5250; t < 8000; t += 250) {
        read_fail(&ev, t, PPC_EVS_WATCHED);
    }
    tick_read(&ev, 8000, PPC_EVS_WATCHED, 25);
    for (t = 8250; t <= 20000; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 25);
    }
    m = ppc_evid_mark(&ev, 20000, PPC_STATE_CAPTURING, 9, 0);
    check("repeated failures recovered before the mark: the 25 of the spell are the window before's; nothing around "
          "the mark, nothing unknown",
          m->side[0].lost == 25 && m->side[0].lost_spells == 25 && m->edge == PPC_EV_EDGE_NONE && !m->side[1].unknown &&
              m->side[0].lost_near == 0);
    ppc_evid_free(&ev);

    /* A spell reaching past the start of the window before: apart. */
    ppc_evid_init(&ev);
    tick_read(&ev, 2000, PPC_EVS_WATCHED, 0);
    for (t = 2250; t < 6000; t += 250) {
        read_fail(&ev, t, PPC_EVS_WATCHED);
    }
    tick_read(&ev, 6000, PPC_EVS_WATCHED, 40);
    for (t = 6250; t <= 64000; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 40);
    }
    m = ppc_evid_mark(&ev, 64000, PPC_STATE_CAPTURING, 9, 0);
    check("a spell from 2 s to 6 s, the window before from 4 s: its 40 are apart, not in the window's count",
          m->side[0].lost_near == 40 && m->side[0].lost == 0);
    ppc_evid_free(&ev);

    /* A spell after the mark, past the end of the window after: apart. */
    ppc_evid_init(&ev);
    tick_read(&ev, 10000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 10000, PPC_STATE_CAPTURING, 9, 0);
    for (t = 10250; t <= 65000; t += 250) {
        tick_read(&ev, t, PPC_EVS_WATCHED, 0);
    }
    for (t = 65250; t < 75000; t += 250) {
        read_fail(&ev, t, PPC_EVS_WATCHED);
    }
    check("a spell open as the window after closes (70 s): the closed window says its losses are not known from 65 s",
          !(m->side[1].flags & PPC_EVW_OPEN) && m->side[1].unknown && m->side[1].unknown_from_ms == 65000);
    tick_read(&ev, 75000, PPC_EVS_WATCHED, 30);
    check("a spell from 65 s to 75 s, the window after to 70 s: its 30 are apart, not in the window's count, and the "
          "window is no longer unknown",
          m->side[1].lost_near == 30 && m->side[1].lost == 0 && m->edge == PPC_EV_EDGE_NONE && !m->side[1].unknown);
    ppc_evid_free(&ev);

    /* A spell after the mark that is still open while its window is. */
    ppc_evid_init(&ev);
    tick_read(&ev, 10000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 10000, PPC_STATE_CAPTURING, 9, 0);
    tick_read(&ev, 12000, PPC_EVS_WATCHED, 0);
    read_fail(&ev, 12250, PPC_EVS_WATCHED);
    read_fail(&ev, 12500, PPC_EVS_WATCHED);
    check("a spell after the mark, open: the window after is unknown from the last good reading (12 s); the one "
          "before and the mark are not",
          m->side[1].unknown && m->side[1].unknown_from_ms == 12000 && m->edge == PPC_EV_EDGE_NONE);
    ppc_evid_free(&ev);

    /* STOP while no reading can be made: never recovered. */
    ppc_evid_init(&ev);
    tick_read(&ev, 9000, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 10000, PPC_EVS_WATCHED, 40);
    read_fail(&ev, 10250, PPC_EVS_WATCHED);
    read_fail(&ev, 10500, PPC_EVS_WATCHED);
    m = ppc_evid_mark(&ev, 10500, PPC_STATE_CAPTURING, 5, 0);
    read_fail(&ev, 11000, PPC_EVS_WATCHED);
    read_fail(&ev, 12000, PPC_EVS_WATCHED); /* the reading STOP makes, failed too */
    ppc_evid_stop(&ev, 12000, 0);
    ppc_evid_counters(&ev, 13000, 1, 99); /* nothing is read after STOP */
    check("STOP while the counters could not be read: the mark's losses since 10 s are never known; 40 before it "
          "stay; the window after is cut and unknown; nothing after STOP is taken",
          m->edge == PPC_EV_EDGE_UNKNOWN && m->edge_from_ms == 10000 && m->side[0].lost == 40 &&
              (m->side[1].flags & PPC_EVW_CUT_STOP) && m->side[1].unknown && m->side[1].lost == 0 &&
              m->edge_lost == 0);
    late = ppc_evid_mark(&ev, 90000, PPC_STATE_STOPPED, 7, 0);
    check("a mark after that STOP stands at STOP, its losses since 10 s not known either",
          late && late->at_ms == 12000 && late->edge == PPC_EV_EDGE_UNKNOWN && late->edge_from_ms == 10000 &&
              late->side[0].lost == 40);
    ppc_evid_free(&ev);

    /* The counters never read at all: unknown from START. */
    ppc_evid_init(&ev);
    for (t = 0; t <= 5000; t += 250) {
        read_fail(&ev, t, PPC_EVS_WATCHED);
    }
    m = ppc_evid_mark(&ev, 5000, PPC_STATE_CAPTURING, 3, 0);
    check("counters never read: around the mark from START (0), not known, nothing counted",
          m->edge == PPC_EV_EDGE_UNKNOWN && m->edge_from_ms == 0 && m->side[0].lost == 0);
    ppc_evid_free(&ev);

    /* A spell over the mark with nothing lost: known, and nothing to tell. */
    ppc_evid_init(&ev);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    read_fail(&ev, 1500, PPC_EVS_WATCHED);
    m = ppc_evid_mark(&ev, 1500, PPC_STATE_CAPTURING, 3, 0);
    tick_read(&ev, 2500, PPC_EVS_WATCHED, 0);
    check("a spell over the mark that lost nothing: counted, 0, both windows known",
          m->edge == PPC_EV_EDGE_COUNTED && m->edge_lost == 0 && !m->side[1].unknown);
    ppc_evid_free(&ev);

    /* A mark set without a reading of its own (the API alone): a spell that
     * opens after it from an earlier reading spans it. */
    ppc_evid_init(&ev);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    ppc_evid_tick(&ev, 1400, PPC_EVS_WATCHED);
    m = ppc_evid_mark(&ev, 1400, PPC_STATE_CAPTURING, 3, 0);
    read_fail(&ev, 1500, PPC_EVS_WATCHED);
    check("a spell from a reading before a mark that had none of its own is around that mark",
          m->edge == PPC_EV_EDGE_UNKNOWN && m->edge_from_ms == 1000);
    tick_read(&ev, 1750, PPC_EVS_WATCHED, 6);
    check("and its losses, when read, are around it, not after it", m->edge == PPC_EV_EDGE_COUNTED &&
                                                                     m->edge_lost == 6 && m->side[1].lost == 0);
    ppc_evid_free(&ev);

    /* More spells than the ring holds within a minute: a mark set then
     * says its record of them is incomplete. */
    ppc_evid_init(&ev);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    for (i = 0; i < PPC_EV_SPELLS + 4u; i++) {
        read_fail(&ev, 2000u + i * 1000u, PPC_EVS_WATCHED);
        tick_read(&ev, 2500u + i * 1000u, PPC_EVS_WATCHED, i + 1u);
    }
    m = ppc_evid_mark(&ev, 2500u + (PPC_EV_SPELLS + 4u) * 1000u, PPC_STATE_CAPTURING, 3, 0);
    check("20 spells in 20 s, a ring of 16: the window before says its record of them is incomplete",
          (m->side[0].flags & PPC_EVW_LOSSES_DROPPED) != 0);
    ppc_evid_free(&ev);
    ppc_evid_init(&ev);
    tick_read(&ev, 1000, PPC_EVS_WATCHED, 0);
    for (i = 0; i < 4; i++) {
        read_fail(&ev, 2000u + i * 1000u, PPC_EVS_WATCHED);
        tick_read(&ev, 2500u + i * 1000u, PPC_EVS_WATCHED, i + 1u);
    }
    m = ppc_evid_mark(&ev, 8000, PPC_STATE_CAPTURING, 3, 0);
    check("4 spells: every one kept, nothing said incomplete, their 4 in the window before",
          !(m->side[0].flags & PPC_EVW_LOSSES_DROPPED) && m->side[0].lost == 4);
    ppc_evid_free(&ev);

    /* Memory stays as it was told: the ring is part of the struct, not the
     * allocation, and has a fixed size. */
    ok = sizeof(ev.spells) == PPC_EV_SPELLS * sizeof(struct ppc_ev_spell) && PPC_EV_SPELLS <= 16u;
    check("the spells are a fixed ring of 16", ok);
}

/* P2: a DNS answer whose question the tracker no longer holds. */
static void dns_eviction(void)
{
    static struct fx f; /* one frame at a time */
    struct play p;
    struct fx empty;
    const struct ppc_ev_obs *o;
    uint64_t unjudged;
    uint32_t i;

    memset(&empty, 0, sizeof(empty));
    /* By capacity: a question, then 20,000 others; the table keeps 1,024,
     * and the oldest waiting one gives way. Its answer then has no question
     * to be timed by. */
    play(&p, &empty, 1000);
    p.have_lead = 1;
    p.lead_ms = 0;
    memset(&f, 0, sizeof(f));
    fx_dns(&f, 1000, 0, FX_SELF, 0x4242, 0, "a.example.net");
    play_frame(&p, &f.step[0]);
    for (i = 0; i < 20000; i++) {
        f.nsteps = 0;
        fx_dns(&f, 1001 + i / 100u, 0, 0x40u + i % 150u, 0x1000u + i, 0, "b.example.net");
        play_frame(&p, &f.step[0]);
    }
    unjudged = p.sig.st.dns_unjudged;
    f.nsteps = 0;
    fx_dns(&f, 1300, 1, FX_SELF, 0x4242, 0, "a.example.net");
    play_frame(&p, &f.step[0]);
    o = &p.ev.hist[(p.ev.head - 1u) & (PPC_EV_HISTORY - 1u)];
    check("dns eviction by capacity: questions gave way, and the answer to the first has no question to time it by",
          unjudged > 0 && o->kind == PPC_EV_DNS_ANSWER && (o->flags & PPC_EVF_UNPAIRED) && o->b == 0 &&
              (o->flags & PPC_EVF_OWN));
    play_end(&p);

    /* By age: a question unanswered for 5 s is called so, and forgotten at
     * 30 s; an answer after that has none. Before that it is late, timed. */
    play(&p, &empty, 1000);
    p.have_lead = 1;
    p.lead_ms = 0;
    f.nsteps = 0;
    fx_dns(&f, 1000, 0, FX_SELF, 0x5151, 0, "c.example.net");
    play_frame(&p, &f.step[0]);
    f.nsteps = 0;
    fx_dns(&f, 1000, 0, FX_SELF, 0x6161, 0, "d.example.net");
    play_frame(&p, &f.step[0]);
    ppc_signals_expire(&p.sig, 7000000000ull, late_none, NULL);
    f.nsteps = 0;
    fx_dns(&f, 20000, 1, FX_SELF, 0x6161, 0, "d.example.net");
    play_frame(&p, &f.step[0]);
    o = &p.ev.hist[(p.ev.head - 1u) & (PPC_EV_HISTORY - 1u)];
    check("dns, late but remembered: the answer at 20 s is timed by its question (19 s)",
          o->kind == PPC_EV_DNS_ANSWER && !(o->flags & PPC_EVF_UNPAIRED) && o->b == 19000000u);
    ppc_signals_expire(&p.sig, 32000000000ull, late_none, NULL);
    f.nsteps = 0;
    fx_dns(&f, 32500, 1, FX_SELF, 0x5151, 3, "c.example.net");
    play_frame(&p, &f.step[0]);
    o = &p.ev.hist[(p.ev.head - 1u) & (PPC_EV_HISTORY - 1u)];
    check("dns eviction by age: after 30 s the question is forgotten, and its answer is a failure with no response time",
          o->kind == PPC_EV_DNS_FAILURE && (o->flags & PPC_EVF_UNPAIRED) && o->b == 0 && (o->a >> 16) == 3);
    play_end(&p);
}

/* P2: a mark set minutes after STOP. */
static void post_stop_wall(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;

    ppc_evid_init(&ev);
    tick_read(&ev, 0, PPC_EVS_WATCHED, 0);
    tick_read(&ev, 31000, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 30500, PPC_STATE_CAPTURING, 3, 1700000000u - 31u);
    check("wall: a mark while capturing has the wall clock it was given", m && m->wall_s == 1700000000u - 31u);
    ppc_evid_stop(&ev, 31200, 1700000000u);
    /* Five minutes later, on both clocks. */
    m = ppc_evid_mark(&ev, 31200u + 300000u, PPC_STATE_STOPPED, 9, 1700000000u + 300u);
    check("wall: a mark five minutes after STOP stands at STOP on both clocks (31.2 s, the wall clock then)",
          m && m->at_ms == 31200u && m->wall_s == 1700000000u && m->side[1].span_ms == 0);
    ppc_evid_stop(&ev, 999999, 1800000000u);
    m = ppc_evid_mark(&ev, 31200u + 600000u, PPC_STATE_STOPPED, 9, 1700000600u);
    check("wall: a second STOP changes nothing, and a later mark is the same", m && m->at_ms == 31200u &&
                                                                              m->wall_s == 1700000000u);
    ppc_evid_free(&ev);
    ppc_evid_init(&ev);
    tick_read(&ev, 0, PPC_EVS_WATCHED, 0);
    ppc_evid_stop(&ev, 5000, 0);
    m = ppc_evid_mark(&ev, 400000, PPC_STATE_STOPPED, 1, 1700000400u);
    check("wall: with the clock not set at STOP, a later mark has none (0), not the time it was set",
          m && m->wall_s == 0 && m->at_ms == 5000u);
    ppc_evid_free(&ev);
}

/* ---- the window after's end (the closure check of PR #188, 2026-11-25) --------------------
 * The helper reads the counters as `read_drops` does: a tick first, then the
 * reading at the same time. A tick past the end of the window after closes
 * what it saw; its count of lost frames must still wait for the reading. */

/* A reading, then the tick: the other order, which must give the same. */
static void read_tick(struct ppc_evid *ev, uint32_t t_ms, unsigned flags, uint64_t lost)
{
    ppc_evid_counters(ev, t_ms, 1, lost);
    ppc_evid_tick(ev, t_ms, flags);
}

/* Readings every 250 ms from 10.0 s, a mark at 10.3 s (window after to
 * 70.3 s), 20 lost read at 40.0 s, 3 more at 70.1 s: the last reading inside
 * the window. Returns the mark. */
static const struct ppc_ev_mark *to_the_end(struct ppc_evid *ev)
{
    const struct ppc_ev_mark *m;
    uint32_t t;

    ppc_evid_init(ev);
    for (t = 10000; t <= 10300; t += 100) {
        tick_read(ev, t, PPC_EVS_WATCHED, 0);
    }
    m = ppc_evid_mark(ev, 10300, PPC_STATE_CAPTURING, 7, 0);
    for (t = 10550; t < 70100; t += 250) {
        tick_read(ev, t, PPC_EVS_WATCHED, t >= 40000 ? 20 : 0);
    }
    tick_read(ev, 70100, PPC_EVS_WATCHED, 23);
    return m;
}

static int after_is(const struct ppc_ev_mark *m, uint64_t lost, uint64_t near, int unknown, uint32_t from)
{
    const struct ppc_ev_side *a = &m->side[1];
    int ok = a->lost == lost && a->lost_near == near && a->unknown == unknown && (!unknown || a->unknown_from_ms == from) &&
             !(a->flags & PPC_EVW_OPEN) && a->span_ms == PPC_EV_WINDOW_MS;

    if (!ok) {
        printf("     after: lost %llu near %llu unknown %u from %u flags %u span %u\n", (unsigned long long)a->lost,
               (unsigned long long)a->lost_near, (unsigned)a->unknown, (unsigned)a->unknown_from_ms, (unsigned)a->flags,
               (unsigned)a->span_ms);
    }
    return ok;
}

static void closure(void)
{
    struct ppc_evid ev;
    const struct ppc_ev_mark *m;

    /* The tick of the helper's own 250 ms clock passes the end before the
     * next reading: 7 more read at 70.35 s, lost between 70.1 and 70.35 s. */
    m = to_the_end(&ev);
    ppc_evid_tick(&ev, 70310, PPC_EVS_WATCHED);
    check("closure: a tick past the end closes what the window saw", !(m->side[1].flags & PPC_EVW_OPEN) &&
                                                                     m->side[1].span_ms == PPC_EV_WINDOW_MS);
    tick_read(&ev, 70350, PPC_EVS_WATCHED, 30);
    check("closure: the reading after the end is not omitted: its 7, lost between a reading inside the window and one "
          "after it, are told apart (near its end), the window keeps its 23",
          after_is(m, 23, 7, 0, 0));
    check("closure: every frame lost by then is told once: 0 before + 23 after + 7 near its end = 30",
          m->side[0].lost + m->edge_lost + m->side[1].lost + m->side[1].lost_near == 30);
    /* Later readings, in the end's own second and after, and a spell begun
     * after the end: none of it is the window's. */
    tick_read(&ev, 70750, PPC_EVS_WATCHED, 41);
    tick_read(&ev, 71500, PPC_EVS_WATCHED, 54);
    read_fail(&ev, 71750, PPC_EVS_WATCHED);
    read_fail(&ev, 72000, PPC_EVS_WATCHED);
    check("closure: a spell begun after the end does not make the closed window's losses unknown",
          after_is(m, 23, 7, 0, 0));
    tick_read(&ev, 73000, PPC_EVS_WATCHED, 74);
    check("closure: a closed window takes nothing read later: 11 in the end's own second, 13, 20 after a spell",
          after_is(m, 23, 7, 0, 0));
    ppc_evid_free(&ev);

    /* The helper's order alone (read_drops: the tick, then the reading). */
    m = to_the_end(&ev);
    tick_read(&ev, 70350, PPC_EVS_WATCHED, 30);
    check("closure, the tick at the reading itself: the 7 are not omitted", after_is(m, 23, 7, 0, 0));
    ppc_evid_free(&ev);
    m = to_the_end(&ev);
    read_tick(&ev, 70350, PPC_EVS_WATCHED, 30);
    check("closure, the reading before the tick: the same", after_is(m, 23, 7, 0, 0));
    ppc_evid_free(&ev);

    /* A reading at the end itself reports what was lost inside the window. */
    m = to_the_end(&ev);
    tick_read(&ev, 70300, PPC_EVS_WATCHED, 28);
    check("closure: a reading at the end itself: its 5 are the window's", after_is(m, 28, 0, 0, 0));
    tick_read(&ev, 70550, PPC_EVS_WATCHED, 30);
    tick_read(&ev, 70800, PPC_EVS_WATCHED, 35);
    check("closure: and the readings after it, from it on, are not the window's, nor near it", after_is(m, 28, 0, 0, 0));
    ppc_evid_free(&ev);

    /* The reading at closure fails: what was lost after the last good one
     * is not known, however the window was closed. */
    m = to_the_end(&ev);
    read_fail(&ev, 70350, PPC_EVS_WATCHED);
    check("closure, the reading fails: the window's losses are not known since the last good reading (70.1 s)",
          after_is(m, 23, 0, 1, 70100));
    read_fail(&ev, 70600, PPC_EVS_WATCHED);
    read_fail(&ev, 71000, PPC_EVS_WATCHED);
    ppc_evid_tick(&ev, 71250, PPC_EVS_WATCHED);
    check("closure, failing on: still not known, nothing counted for it", after_is(m, 23, 0, 1, 70100));
    tick_read(&ev, 72000, PPC_EVS_WATCHED, 40);
    check("closure, recovered at 72 s: the 17 of the spell across the end are told apart, the window known again",
          after_is(m, 23, 17, 0, 0));
    tick_read(&ev, 72250, PPC_EVS_WATCHED, 45);
    check("closure, recovered: and nothing later is added", after_is(m, 23, 17, 0, 0));
    ppc_evid_free(&ev);
    /* The same with a tick past the end first, as the helper's clock may. */
    m = to_the_end(&ev);
    ppc_evid_tick(&ev, 70310, PPC_EVS_WATCHED);
    read_fail(&ev, 70350, PPC_EVS_WATCHED);
    check("closure, a tick past the end, then the reading fails: not known either", after_is(m, 23, 0, 1, 70100));
    ppc_evid_free(&ev);
    /* STOP before any reading succeeds: not known for good. */
    m = to_the_end(&ev);
    read_fail(&ev, 70350, PPC_EVS_WATCHED);
    read_fail(&ev, 70600, PPC_EVS_WATCHED);
    ppc_evid_stop(&ev, 70700, 0);
    ppc_evid_counters(&ev, 71000, 1, 99);
    ppc_evid_tick(&ev, 71000, PPC_EVS_WATCHED);
    check("closure, STOP while failing: the window's losses stay not known, nothing is counted after STOP",
          after_is(m, 23, 0, 1, 70100));
    ppc_evid_free(&ev);
    /* STOP inside the window: the reading at STOP is the window's. */
    m = to_the_end(&ev);
    ppc_evid_free(&ev);
    ppc_evid_init(&ev);
    tick_read(&ev, 10300, PPC_EVS_WATCHED, 0);
    m = ppc_evid_mark(&ev, 10300, PPC_STATE_CAPTURING, 7, 0);
    tick_read(&ev, 20000, PPC_EVS_WATCHED, 4);
    tick_read(&ev, 30100, PPC_EVS_WATCHED, 10);
    ppc_evid_stop(&ev, 30100, 0);
    check("closure, STOP inside the window: the reading at STOP is counted in it, cut there",
          m->side[1].lost == 10 && m->side[1].lost_near == 0 && (m->side[1].flags & PPC_EVW_CUT_STOP) &&
              m->side[1].span_ms == 19800);
    ppc_evid_free(&ev);
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
    coverage();
    bursts();
    boundary_losses();
    readings();
    closure();
    dns_eviction();
    post_stop_wall();
    printf("ppc_evid_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
