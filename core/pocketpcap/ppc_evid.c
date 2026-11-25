/*
 * pocketpcap evidence history (N1.0). See ppc_evid.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_evid.h"

#include "ppc_net.h"
#include "ppc_record.h"

#include <stdlib.h>
#include <string.h>

#define HIST_MASK (PPC_EV_HISTORY - 1u)
#define SEC_MASK (PPC_EV_SECONDS - 1u)

uint32_t ppc_evid_bytes(void)
{
    return (uint32_t)(PPC_EV_HISTORY * sizeof(struct ppc_ev_obs) + PPC_EV_MARKS * sizeof(struct ppc_ev_mark));
}

int ppc_evid_init(struct ppc_evid *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->mem = calloc(1, PPC_EV_HISTORY * sizeof(struct ppc_ev_obs) + PPC_EV_MARKS * sizeof(struct ppc_ev_mark));
    if (!ev->mem) {
        return -1;
    }
    ev->hist = ev->mem;
    ev->marks = (struct ppc_ev_mark *)(void *)(ev->hist + PPC_EV_HISTORY);
    ev->next_id = 1;
    return 0;
}

void ppc_evid_free(struct ppc_evid *ev)
{
    free(ev->mem);
    memset(ev, 0, sizeof(*ev));
}

/* ---- a window's rows -------------------------------------------------------------------- */

static int plain(const struct ppc_ev_obs *o)
{
    return (o->flags & PPC_EVF_PLAIN) != 0;
}

/* Count an observation in a side and keep it as a row when there is room:
 * routine ARP takes at most PPC_EV_PLAIN_KEEP rows, so a busy segment cannot
 * push a DHCP message out. Rows stay in the order they are given. */
static void side_add(struct ppc_ev_side *s, const struct ppc_ev_obs *o)
{
    if (s->count[o->kind] < 0xffffffffu) {
        s->count[o->kind]++;
    }
    if (s->nkept < PPC_EV_KEEP && (!plain(o) || s->nplain < PPC_EV_PLAIN_KEEP)) {
        s->kept[s->nkept++] = *o;
        if (plain(o)) {
            s->nplain++;
        }
    } else {
        s->more++;
    }
}

/* ---- visibility -------------------------------------------------------------------------- */

/* What the seconds that touch [from_ms, to_ms) were, as far as the ring
 * still holds them: a window that is not on a second's edge counts the
 * second it begins in, so a HOLD inside a short window is still told. At
 * most PPC_EV_SECONDS turns. Frames lost are not counted here: lost_in. */
static void side_seen(const struct ppc_evid *ev, struct ppc_ev_side *s, uint32_t from_ms, uint32_t to_ms)
{
    uint32_t first = from_ms / 1000u;
    uint32_t i;

    s->watched_s = 0;
    s->held_s = 0;
    s->shed_s = 0;
    if (to_ms <= from_ms) {
        return; /* a window of no length touches no second */
    }
    for (i = 0; i < PPC_EV_SECONDS; i++) {
        uint32_t sec = first + i;
        const struct ppc_ev_sec *e = &ev->secs[sec & SEC_MASK];

        if ((uint64_t)sec * 1000u >= to_ms) {
            break;
        }
        if (!e->used || e->sec != sec) {
            continue;
        }
        s->watched_s += (e->flags & PPC_EVS_WATCHED) != 0;
        s->held_s += (e->flags & PPC_EVS_HELD) != 0;
        s->shed_s += (e->flags & PPC_EVS_SHED) != 0;
    }
}

/* Frames reported lost in the seconds first..last, as far as the ring
 * still holds them. A second's losses are those read at its ticks. */
static uint64_t lost_in(const struct ppc_evid *ev, uint32_t first, uint32_t last)
{
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < PPC_EV_SECONDS && first + i <= last && first + i >= first; i++) {
        uint32_t sec = first + i;
        const struct ppc_ev_sec *e = &ev->secs[sec & SEC_MASK];

        if (e->used && e->sec == sec) {
            n += e->lost;
        }
    }
    return n;
}

static uint32_t lost_of_sec(const struct ppc_evid *ev, uint32_t sec)
{
    const struct ppc_ev_sec *e = &ev->secs[sec & SEC_MASK];

    return e->used && e->sec == sec ? e->lost : 0;
}

/* The window after a mark as it stands at now_ms: still filling, full, or
 * cut where the capture stopped. What it saw closes with the clock; what it
 * lost only with a reading at or past its end, or with STOP: a tick past
 * the end comes before the reading that tells the end's losses. */
static void after_refresh(const struct ppc_evid *ev, struct ppc_ev_mark *m, uint32_t now_ms)
{
    struct ppc_ev_side *s = &m->side[1];
    uint32_t end = m->at_ms + PPC_EV_WINDOW_MS;
    uint32_t to = now_ms;

    if (!(s->flags & PPC_EVW_OPEN) && m->loss_done) {
        return;
    }
    if (ev->stopped && ev->stop_ms < to) {
        to = ev->stop_ms;
    }
    if (to < m->at_ms) {
        to = m->at_ms;
    }
    if (to > end) {
        to = end;
    }
    if (s->flags & PPC_EVW_OPEN) {
        if (to >= end) {
            s->flags &= (uint8_t)~PPC_EVW_OPEN;
        } else if (ev->stopped) {
            s->flags &= (uint8_t)~PPC_EVW_OPEN;
            s->flags |= PPC_EVW_CUT_STOP;
        }
        s->span_ms = to - m->at_ms;
        side_seen(ev, s, m->at_ms, to);
    }
    if (m->loss_done) {
        return;
    }
    /* Losses read from the mark's second to the second of `to` (its
     * readings up to `to` told what was lost before it), less what the
     * mark's second had before the mark (the window before's); and those of
     * the spells closed inside the window. */
    s->lost = s->lost_spells;
    if (to > m->at_ms) {
        uint64_t all = lost_in(ev, m->at_ms / 1000u, to / 1000u);

        s->lost += all > m->lost_split ? all - m->lost_split : 0;
    }
    /* A spell still open that reaches into the window: its losses are not
     * known. One that began before the mark is the mark's own (m->edge). */
    s->unknown = 0;
    if (ev->failing) {
        const struct ppc_ev_spell *sp = &ev->spells[(ev->nspells - 1u) % PPC_EV_SPELLS];

        if (sp->from_ms < to) {
            s->unknown = 1;
            s->unknown_from_ms = sp->from_ms;
        }
    }
    /* Final once a good reading at or past the end is told (one past it is
     * set apart before it is booked: ppc_evid_counters), or at STOP. */
    if (ev->stopped || ev->read_ms >= end) {
        m->loss_done = 1;
    }
}

static void refresh_all(struct ppc_evid *ev, uint32_t now_ms)
{
    unsigned i;

    for (i = 0; i < PPC_EV_MARKS; i++) {
        if (ev->marks[i].id) {
            after_refresh(ev, &ev->marks[i], now_ms);
        }
    }
}

/* ---- the clock --------------------------------------------------------------------------- */

static struct ppc_ev_sec *second(struct ppc_evid *ev, uint32_t sec)
{
    struct ppc_ev_sec *e = &ev->secs[sec & SEC_MASK];

    if (!e->used || e->sec != sec) {
        memset(e, 0, sizeof(*e));
        e->sec = sec;
        e->used = 1;
    }
    return e;
}

void ppc_evid_tick(struct ppc_evid *ev, uint32_t now_ms, unsigned flags)
{
    uint32_t sec = now_ms / 1000u;
    struct ppc_ev_sec *e;
    unsigned i;

    if (!ev->mem || ev->stopped) {
        return;
    }
    if (!ev->ticking) {
        ev->ticking = 1;
        ev->cur_sec = sec;
    } else if (sec < ev->cur_sec) {
        return; /* a clock that went back: nothing is rewritten */
    } else {
        uint32_t gap = sec - ev->cur_sec;

        /* The seconds passed over since the last tick were what the capture
         * was doing then. A gap longer than the ring keeps its newest end. */
        if (gap > PPC_EV_SECONDS) {
            gap = PPC_EV_SECONDS;
        }
        for (i = gap; i > 1; i--) {
            second(ev, sec - i + 1u)->flags |= ev->cur_flags;
        }
        ev->cur_sec = sec;
    }
    e = second(ev, sec);
    e->flags |= (uint8_t)flags;
    ev->cur_flags = (uint8_t)flags;
    refresh_all(ev, now_ms);
}

/* ---- the lost-frame counters ------------------------------------------------------------- */

/* Where a closed spell's losses go for one mark: the window it lies in;
 * around the mark when it spans the mark; apart when it reaches past a
 * window's far end; nowhere when it lies outside both. */
static void spell_to_mark(const struct ppc_evid *ev, struct ppc_ev_mark *m, const struct ppc_ev_spell *sp)
{
    uint32_t from = m->at_ms > PPC_EV_WINDOW_MS ? m->at_ms - PPC_EV_WINDOW_MS : 0;
    uint32_t end = m->at_ms + PPC_EV_WINDOW_MS;

    if (ev->stopped && ev->stop_ms < end) {
        end = ev->stop_ms;
    }
    /* A window closed while this spell was open said its losses were not
     * known: they are now, and placed below. */
    if (m->side[1].unknown && m->side[1].unknown_from_ms == sp->from_ms) {
        m->side[1].unknown = 0;
    }
    if (sp->from_ms < m->at_ms && sp->to_ms > m->at_ms) {
        /* A mark has one such spell at most: the one it was set in. */
        m->edge = PPC_EV_EDGE_COUNTED;
        m->edge_from_ms = sp->from_ms;
        m->edge_to_ms = sp->to_ms;
        m->edge_lost = sp->lost;
    } else if (sp->to_ms <= m->at_ms) {
        if (sp->from_ms >= from) {
            m->side[0].lost_spells += sp->lost;
            m->side[0].lost += sp->lost;
        } else if (sp->to_ms > from) {
            m->side[0].lost_near += sp->lost;
        }
    } else if (sp->from_ms < end) {
        if (sp->to_ms <= end) {
            m->side[1].lost_spells += sp->lost; /* counted by the refresh that follows */
        } else {
            m->side[1].lost_near += sp->lost;
        }
    }
}

void ppc_evid_counters(struct ppc_evid *ev, uint32_t now_ms, int ok, uint64_t lost_total)
{
    struct ppc_ev_spell *sp;
    uint64_t lost;
    unsigned i;

    if (!ev->mem || ev->stopped) {
        return;
    }
    if (now_ms < ev->read_ms) {
        now_ms = ev->read_ms; /* a clock that went back: readings keep their order */
    }
    if (!ok) {
        if (!ev->failing) {
            /* A spell opens at the last good reading. One that pushes an
             * old spell out of the ring says so to marks set later. */
            sp = &ev->spells[ev->nspells % PPC_EV_SPELLS];
            if (sp->used && !sp->open) {
                ev->dropped = 1;
                if (sp->to_ms > ev->dropped_to_ms) {
                    ev->dropped_to_ms = sp->to_ms;
                }
            }
            memset(sp, 0, sizeof(*sp));
            sp->used = 1;
            sp->open = 1;
            sp->from_ms = ev->read_ms;
            ev->nspells++;
            ev->failing = 1;
            /* A mark set since the last good reading without one of its own
             * is inside this spell. */
            for (i = 0; i < PPC_EV_MARKS; i++) {
                struct ppc_ev_mark *m = &ev->marks[i];

                if (m->id && m->edge == PPC_EV_EDGE_NONE && m->at_ms > sp->from_ms) {
                    m->edge = PPC_EV_EDGE_UNKNOWN;
                    m->edge_from_ms = sp->from_ms;
                }
            }
        }
        refresh_all(ev, now_ms);
        return;
    }
    lost = lost_total > ev->lost_total ? lost_total - ev->lost_total : 0;
    ev->lost_total = lost_total;
    if (ev->failing) {
        /* The reading that ends a spell: how many, not when. */
        sp = &ev->spells[(ev->nspells - 1u) % PPC_EV_SPELLS];
        sp->open = 0;
        sp->to_ms = now_ms;
        sp->lost = lost;
        ev->failing = 0;
        for (i = 0; i < PPC_EV_MARKS; i++) {
            if (ev->marks[i].id) {
                spell_to_mark(ev, &ev->marks[i], sp);
            }
        }
    } else {
        /* A reading past the end of a window after whose count is still
         * open ends that count before it is booked. What it tells was lost
         * since the reading before, inside the window: from either side of
         * the end, so it is told apart. Nothing read later is the window's. */
        for (i = 0; i < PPC_EV_MARKS; i++) {
            struct ppc_ev_mark *m = &ev->marks[i];
            uint32_t end = m->at_ms + PPC_EV_WINDOW_MS;

            if (m->id && !m->loss_done && now_ms > end) {
                after_refresh(ev, m, end);
                m->loss_done = 1;
                if (ev->read_ms < end) {
                    m->side[1].lost_near += lost;
                }
            }
        }
        if (lost) {
            /* A reading a period after the last: lost in its second. */
            struct ppc_ev_sec *e = second(ev, now_ms / 1000u);

            e->lost = lost > 0xffffffffu - e->lost ? 0xffffffffu : e->lost + (uint32_t)lost;
        }
    }
    ev->read_ms = now_ms;
    refresh_all(ev, now_ms);
}

void ppc_evid_stop(struct ppc_evid *ev, uint32_t now_ms, uint64_t wall_s)
{
    if (!ev->mem || ev->stopped) {
        return;
    }
    ev->stopped = 1;
    ev->stop_ms = now_ms;
    ev->stop_wall_s = wall_s;
    refresh_all(ev, now_ms);
}

/* ---- what the capture can observe -------------------------------------------------------- */

void ppc_evid_coverage(struct ppc_evid *ev, unsigned cfilter, unsigned unanalyzed)
{
    ev->cfilter = (uint8_t)(cfilter < PPC_CFILTER_COUNT ? cfilter : PPC_CFILTER_ALL);
    ev->unanalyzed = (uint8_t)(unanalyzed & PPC_EV_CLASSES);
}

uint8_t ppc_evid_unanalyzed(int dns_ready, int net_ready)
{
    return (uint8_t)((dns_ready ? 0u : 1u << PPC_CLASS_DNS) |
                     (net_ready ? 0u : (1u << PPC_CLASS_DHCP | 1u << PPC_CLASS_ARP)));
}

static unsigned class_of_kind(unsigned kind)
{
    if (kind >= PPC_EV_DHCP_DISCOVER && kind <= PPC_EV_DHCP_INFORM) {
        return 1u << PPC_CLASS_DHCP;
    }
    if (kind >= PPC_EV_ARP_REQUEST && kind <= PPC_EV_ARP_TWO_MACS) {
        return 1u << PPC_CLASS_ARP;
    }
    if (kind >= PPC_EV_DNS_QUERY && kind <= PPC_EV_DNS_FAILURE) {
        return 1u << PPC_CLASS_DNS;
    }
    return 0;
}

void ppc_evid_bypass(struct ppc_evid *ev, uint32_t t_ms)
{
    uint32_t sec = t_ms / 1000u;
    struct ppc_ev_sec *e;
    unsigned i;

    if (!ev->mem || ev->stopped || (ev->bypass_any && sec == ev->bypass_sec)) {
        return;
    }
    /* Older than the ring, or ahead of the clock: no second to write. */
    if (ev->ticking && (sec + PPC_EV_SECONDS <= ev->cur_sec || sec > ev->cur_sec + 1u)) {
        return;
    }
    ev->bypass_any = 1;
    ev->bypass_sec = sec;
    e = second(ev, sec);
    if (e->flags & PPC_EVS_SHED) {
        return;
    }
    e->flags |= PPC_EVS_SHED;
    /* A window before a mark already set learns it as a late observation
     * would (ppc_evid_note); a window after reads it at its next refresh. */
    for (i = 0; i < PPC_EV_MARKS; i++) {
        struct ppc_ev_mark *m = &ev->marks[i];
        uint32_t from;

        if (!m->id || !(m->side[1].flags & PPC_EVW_OPEN)) {
            continue;
        }
        from = m->at_ms > PPC_EV_WINDOW_MS ? m->at_ms - PPC_EV_WINDOW_MS : 0;
        if ((uint64_t)sec * 1000u < m->at_ms && (uint64_t)sec * 1000u + 1000u > from) {
            m->side[0].shed_s++;
        }
    }
}

/* ---- observations ------------------------------------------------------------------------ */

void ppc_evid_note(struct ppc_evid *ev, const struct ppc_ev_obs *o)
{
    unsigned i;

    if (!ev->mem || o->kind == PPC_EV_NONE || o->kind >= PPC_EV_KINDS) {
        return;
    }
    if (class_of_kind(o->kind) & (ppc_cfilter_excludes((enum ppc_cfilter)ev->cfilter) | ev->unanalyzed)) {
        return; /* a class this capture cannot see (a replay applies no filter) */
    }
    if (ev->count == PPC_EV_HISTORY) {
        ev->gone = 1;
        ev->gone_ms = ev->hist[ev->head].t_ms;
    } else {
        ev->count++;
    }
    ev->hist[ev->head] = *o;
    ev->head = (ev->head + 1u) & HIST_MASK;
    ev->total++;
    for (i = 0; i < PPC_EV_MARKS; i++) {
        struct ppc_ev_mark *m = &ev->marks[i];

        if (!m->id) {
            continue;
        }
        if (o->t_ms < m->at_ms) {
            /* Stamped before the mark and handed over after it: it belongs
             * to the window before, while that window can still be told. */
            if (m->at_ms - o->t_ms <= PPC_EV_WINDOW_MS && (m->side[1].flags & PPC_EVW_OPEN)) {
                side_add(&m->side[0], o);
            }
        } else if (m->side[1].flags & PPC_EVW_OPEN) {
            if (o->t_ms - m->at_ms >= PPC_EV_WINDOW_MS) {
                after_refresh(ev, m, o->t_ms); /* the window is over: it closes */
            } else {
                side_add(&m->side[1], o);
            }
        }
    }
}

/* ---- marks ------------------------------------------------------------------------------- */

/* The window before a mark, out of the history: everything in it counted,
 * and the rows nearest the mark kept, routine ARP last. */
static void before_fill(const struct ppc_evid *ev, struct ppc_ev_mark *m)
{
    struct ppc_ev_side *s = &m->side[0];
    uint32_t from = m->at_ms > PPC_EV_WINDOW_MS ? m->at_ms - PPC_EV_WINDOW_MS : 0;
    const struct ppc_ev_obs *pick[PPC_EV_KEEP];
    const struct ppc_ev_obs *routine[PPC_EV_PLAIN_KEEP];
    uint32_t npick = 0;
    uint32_t nroutine = 0;
    uint32_t total = 0;
    uint32_t i;
    uint32_t k;

    s->span_ms = m->at_ms - from;
    if (m->at_ms < PPC_EV_WINDOW_MS) {
        s->flags |= PPC_EVW_CUT_START;
    }
    if (ev->gone && ev->gone_ms >= from) {
        s->flags |= PPC_EVW_HISTORY;
    }
    /* Newest first, the whole ring: its order is the packets', and a stamp
     * out of place by a little must not end the walk early. */
    for (i = 0; i < ev->count; i++) {
        const struct ppc_ev_obs *o = &ev->hist[(ev->head - 1u - i) & HIST_MASK];

        if (o->t_ms < from || o->t_ms > m->at_ms) {
            continue;
        }
        total++;
        if (s->count[o->kind] < 0xffffffffu) {
            s->count[o->kind]++;
        }
        if (plain(o)) {
            if (nroutine < PPC_EV_PLAIN_KEEP) {
                routine[nroutine++] = o;
            }
        } else if (npick < PPC_EV_KEEP) {
            pick[npick++] = o;
        }
    }
    /* Both lists are newest first. The rows: the picked ones, then routine
     * ones while there is room; written oldest first, by time and packet. */
    for (i = 0; i < nroutine && npick < PPC_EV_KEEP; i++) {
        pick[npick++] = routine[i];
        s->nplain++;
    }
    for (i = 0; i < npick; i++) {
        const struct ppc_ev_obs *o = pick[i];

        for (k = s->nkept; k > 0; k--) {
            const struct ppc_ev_obs *p = &s->kept[k - 1u];

            /* Two observations of one packet keep the order they were made
             * in: the walk is newest first, so the later one is in place. */
            if (p->t_ms < o->t_ms || (p->t_ms == o->t_ms && p->seq < o->seq)) {
                break;
            }
            s->kept[k] = s->kept[k - 1u];
        }
        s->kept[k] = *o;
        s->nkept++;
    }
    s->more = total - s->nkept;
    side_seen(ev, s, from, m->at_ms);
    /* Losses of the seconds before the mark's, and of the mark's own up to
     * the mark: they were read before it, so they happened before it. Then
     * the spells closed before it, by where they lie. */
    s->lost = (m->at_ms / 1000u > from / 1000u ? lost_in(ev, from / 1000u, m->at_ms / 1000u - 1u) : 0) +
              m->lost_split;
    for (i = 0; i < PPC_EV_SPELLS; i++) {
        const struct ppc_ev_spell *sp = &ev->spells[i];

        if (sp->used && !sp->open && sp->to_ms <= m->at_ms) {
            spell_to_mark(ev, m, sp);
        }
    }
    if (ev->dropped && ev->dropped_to_ms > from) {
        s->flags |= PPC_EVW_LOSSES_DROPPED;
    }
}

const struct ppc_ev_mark *ppc_evid_mark(struct ppc_evid *ev, uint32_t now_ms, unsigned state, uint32_t last_seq,
                                        uint64_t wall_s)
{
    struct ppc_ev_mark *m;

    if (!ev->mem) {
        return NULL;
    }
    if (ev->stopped) {
        /* Nothing is observed after the capture stopped: a mark set later
         * stands where it stopped, on the session's clock and the wall's. */
        if (now_ms > ev->stop_ms) {
            now_ms = ev->stop_ms;
        }
        wall_s = ev->stop_wall_s;
    }
    m = &ev->marks[(ev->next_id - 1u) % PPC_EV_MARKS];
    memset(m, 0, sizeof(*m));
    m->id = ev->next_id++;
    m->at_ms = now_ms;
    m->seq = last_seq;
    m->state = (uint8_t)state;
    m->wall_s = wall_s;
    m->lost_split = lost_of_sec(ev, now_ms / 1000u);
    /* Not read since before the mark (the reading at it failed, or STOP came
     * while none could be made): what was lost from the last good reading on
     * is around the mark, until a reading tells how many. */
    if (ev->failing) {
        const struct ppc_ev_spell *sp = &ev->spells[(ev->nspells - 1u) % PPC_EV_SPELLS];

        if (sp->from_ms < now_ms || (sp->from_ms == now_ms && ev->stopped)) {
            m->edge = PPC_EV_EDGE_UNKNOWN;
            m->edge_from_ms = sp->from_ms;
        }
    }
    before_fill(ev, m);
    m->side[1].flags = PPC_EVW_OPEN;
    after_refresh(ev, m, now_ms);
    return m;
}

const struct ppc_ev_mark *ppc_evid_get(const struct ppc_evid *ev, uint32_t id)
{
    const struct ppc_ev_mark *m;

    if (!ev->mem || ev->next_id < 2u) {
        return NULL;
    }
    if (id == 0) {
        id = ev->next_id - 1u;
    }
    m = &ev->marks[(id - 1u) % PPC_EV_MARKS];
    return m->id == id ? m : NULL;
}

unsigned ppc_evid_ids(const struct ppc_evid *ev, uint32_t *first, uint32_t *last)
{
    uint32_t n = ev->mem ? ev->next_id - 1u : 0;

    *last = n;
    *first = n > PPC_EV_MARKS ? n - PPC_EV_MARKS + 1u : (n ? 1u : 0u);
    return n > PPC_EV_MARKS ? PPC_EV_MARKS : n;
}

/* ---- from the frames --------------------------------------------------------------------- */

static uint32_t t_of(uint64_t rel_ns, uint32_t lead_ms)
{
    return lead_ms + (uint32_t)(rel_ns / 1000000u);
}

int ppc_evid_from_net(const struct ppc_net_said *s, uint32_t lead_ms, const uint8_t *self, struct ppc_ev_obs *o)
{
    static const uint8_t dhcp_kind[9] = { 0,
                                          PPC_EV_DHCP_DISCOVER,
                                          PPC_EV_DHCP_OFFER,
                                          PPC_EV_DHCP_REQUEST,
                                          PPC_EV_DHCP_DECLINE,
                                          PPC_EV_DHCP_ACK,
                                          PPC_EV_DHCP_NAK,
                                          PPC_EV_DHCP_RELEASE,
                                          PPC_EV_DHCP_INFORM };
    int own = s->own;

    memset(o, 0, sizeof(*o));
    switch (s->what) {
    case PPC_SAID_DHCP:
        if (s->type < 1 || s->type > 8) {
            return 0;
        }
        o->kind = dhcp_kind[s->type];
        /* A server's message is this device's when it is for this device. */
        if (s->type == 2 || s->type == 5 || s->type == 6) {
            own = self && memcmp(s->mac, self, 6) == 0;
        }
        break;
    case PPC_SAID_ARP_REQUEST:
        o->kind = PPC_EV_ARP_REQUEST;
        break;
    case PPC_SAID_ARP_REPLY:
        o->kind = PPC_EV_ARP_REPLY;
        break;
    case PPC_SAID_ARP_PROBE:
        o->kind = PPC_EV_ARP_PROBE;
        break;
    case PPC_SAID_ARP_ANNOUNCE:
        o->kind = PPC_EV_ARP_ANNOUNCE;
        break;
    case PPC_SAID_EVENT:
        if (s->type == PPC_NK_ARP_CONFLICT || s->type == PPC_NK_ARP_DUPLICATE) {
            o->kind = PPC_EV_ARP_TWO_MACS;
        } else if (s->type == PPC_NK_ARP_MOVED || s->type == PPC_NK_ARP_ROUTER_MOVED) {
            o->kind = PPC_EV_ARP_CHANGED;
        } else {
            return 0; /* a judgement of the tables, not something a packet stated */
        }
        break;
    default:
        return 0;
    }
    o->t_ms = t_of(s->ns, lead_ms);
    o->seq = s->seq;
    o->flags = (uint8_t)((own ? PPC_EVF_OWN : 0u) | (s->bcast ? PPC_EVF_BCAST : 0u) |
                         (s->reply_here ? PPC_EVF_REPLY_HERE : 0u));
    /* Another station's ordinary ARP is the segment's background. */
    if ((o->kind == PPC_EV_ARP_REQUEST || o->kind == PPC_EV_ARP_REPLY) && !own) {
        o->flags |= PPC_EVF_PLAIN;
    }
    memcpy(o->mac, s->mac, 6);
    memcpy(o->ip, s->ip, 4);
    memcpy(o->ip2, s->ip2, 4);
    o->a = s->a;
    o->b = s->b;
    return 1;
}

int ppc_evid_from_dns(const struct ppc_record *r, unsigned diag, uint32_t rtt_us, int to_self, uint32_t lead_ms,
                      uint32_t seq, struct ppc_ev_obs *o)
{
    unsigned rcode = r->a & 0xfu;
    int answer = 0;

    memset(o, 0, sizeof(*o));
    if (r->info != PPC_INFO_DNS || r->app != PPC_APP_DNS || r->l4 != 17) {
        return 0; /* mDNS and LLMNR ask everyone: no answer is how they say no */
    }
    switch (diag) {
    case PPC_DIAG_DNS_QUERY:
        o->kind = PPC_EV_DNS_QUERY;
        break;
    case PPC_DIAG_DNS_RETRY:
        o->kind = PPC_EV_DNS_RETRY;
        break;
    case PPC_DIAG_DNS_ANSWER:
    case PPC_DIAG_DNS_LATE:
        answer = 1;
        break;
    case PPC_DIAG_DNS_ORPHAN:
        answer = 1;
        o->flags |= PPC_EVF_UNPAIRED;
        break;
    default:
        return 0;
    }
    if (answer) {
        o->kind = rcode ? PPC_EV_DNS_FAILURE : PPC_EV_DNS_ANSWER;
        o->b = (o->flags & PPC_EVF_UNPAIRED) ? 0 : rtt_us;
        if (to_self) {
            o->flags |= PPC_EVF_OWN;
        }
    } else if (r->flags & PPC_F_OUT) {
        /* This device asked: the answer comes back to this interface. */
        o->flags |= PPC_EVF_OWN | PPC_EVF_REPLY_HERE;
    }
    if (r->flags & (PPC_F_BROADCAST | PPC_F_MULTICAST)) {
        o->flags |= PPC_EVF_BCAST;
    }
    o->t_ms = t_of(r->rel_ns, lead_ms);
    o->seq = seq;
    o->a = (r->a >> 16) | (uint32_t)rcode << 16;
    if (r->l3 == PPC_L3_IPV4) {
        memcpy(o->ip, answer ? r->src : r->dst, 4);  /* the resolver */
        memcpy(o->ip2, answer ? r->dst : r->src, 4); /* the client */
    } else {
        o->flags |= PPC_EVF_V6;
    }
    return 1;
}
