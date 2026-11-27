/*
 * TROPO. See rift_tropo.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_tropo.h"

#include "rift_rxlog.h"

#include <string.h>

/* MeshCore's PAYLOAD_TYPE_TRACE: its path bytes are per-hop SNRs. */
#define TYPE_TRACE 9

void rift_tropo_init(struct rift_tropo *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

/* ---- the ordinary depth ------------------------------------------------------ */

/* Fixed quarter hours on the monotonic clock: this one and the one before.
 * The values are hops + 1, so 0 is "nothing heard" and a mesh whose floods
 * all arrive at zero hops still has a baseline. */
static int64_t quarter(int64_t ms)
{
    return ms < 0 ? 0 : ms - ms % RIFT_TROPO_WINDOW_MS;
}

static void baseline(struct rift_tropo *t, int64_t rx, int hops)
{
    int64_t w = quarter(rx);
    uint8_t v = (uint8_t)(hops + 1);

    if (t->base_now == 0 && t->base_before == 0) {
        t->base_window_ms = w;
    }
    if (w > t->base_window_ms) {
        t->base_before = (w - t->base_window_ms == RIFT_TROPO_WINDOW_MS) ? t->base_now : 0;
        t->base_now = 0;
        t->base_window_ms = w;
    } else if (w < t->base_window_ms) {
        /* A late report from the quarter before: it belongs there. */
        if (w == t->base_window_ms - RIFT_TROPO_WINDOW_MS && v > t->base_before) {
            t->base_before = v;
            t->revision++;
        }
        return;
    }
    if (v > t->base_now) {
        t->base_now = v;
        t->revision++;
    }
}

/* ---- repeats ------------------------------------------------------------------ */

static int seen(const struct rift_tropo *t, const uint8_t *hash)
{
    int i;

    for (i = 0; i < t->seen_count; i++) {
        if (memcmp(t->seen[i], hash, RIFT_TROPO_HASH_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static void remember(struct rift_tropo *t, const uint8_t *hash)
{
    memcpy(t->seen[t->seen_next], hash, RIFT_TROPO_HASH_LEN);
    t->seen_next = (t->seen_next + 1) % RIFT_TROPO_SEEN;
    if (t->seen_count < RIFT_TROPO_SEEN) {
        t->seen_count++;
    }
}

/* ---- the opening ---------------------------------------------------------------- */

static int active_at(const struct rift_tropo *t, int64_t ms)
{
    return t->have_opening && ms < t->last_ms + RIFT_TROPO_HOLD_MS;
}

/* A deep packet with no opening active when it was received: one more
 * candidate, in time order, and only the ones no further back than the
 * window from the newest kept. Five of them open an opening. */
static enum rift_tropo_verdict candidate(struct rift_tropo *t, int64_t rx, int hops)
{
    int at = t->cand_count;
    int i;

    while (at > 0 && t->cand[at - 1].mono_ms > rx) {
        at--;
    }
    memmove(&t->cand[at + 1], &t->cand[at], sizeof(t->cand[0]) * (size_t)(t->cand_count - at));
    t->cand[at].mono_ms = rx;
    t->cand[at].hops = (uint8_t)hops;
    t->cand_count++;
    while (t->cand_count > 1 &&
           t->cand[t->cand_count - 1].mono_ms - t->cand[0].mono_ms > RIFT_TROPO_WINDOW_MS) {
        memmove(&t->cand[0], &t->cand[1], sizeof(t->cand[0]) * (size_t)(t->cand_count - 1));
        t->cand_count--;
    }
    if (t->cand_count < RIFT_TROPO_NEEDED) {
        return RIFT_TROPO_CANDIDATE;
    }
    /* Five within the window: an opening, from the first of them to the
     * last, and the candidates are its evidence now, not a head start on
     * the next one. */
    t->have_opening = 1;
    t->first_ms = t->cand[0].mono_ms;
    t->last_ms = t->cand[t->cand_count - 1].mono_ms;
    t->detected_ms = rx > t->last_ms ? rx : t->last_ms;
    t->evidence = (unsigned)t->cand_count;
    t->peak_hops = 0;
    for (i = 0; i < t->cand_count; i++) {
        if (t->cand[i].hops > t->peak_hops) {
            t->peak_hops = t->cand[i].hops;
        }
    }
    t->openings++;
    t->cand_count = 0;
    return RIFT_TROPO_OPENED;
}

enum rift_tropo_verdict rift_tropo_step(struct rift_tropo *t, int64_t rx_mono, int hops, int flood,
                                        int handled, int own, const uint8_t *hash,
                                        int64_t now_mono)
{
    return rift_tropo_step_seen(t, rx_mono, hops, flood, handled, own, hash, 0, now_mono);
}

enum rift_tropo_verdict rift_tropo_step_seen(struct rift_tropo *t, int64_t rx_mono, int hops,
                                             int flood, int handled, int own, const uint8_t *hash,
                                             int service_repeat, int64_t now_mono)
{
    int64_t rx;

    if (!t) {
        return RIFT_TROPO_NOT_FRAME;
    }
    if (!handled) {
        return RIFT_TROPO_REJECTED;
    }
    if (hops < 0) {
        return RIFT_TROPO_NO_PATH;
    }
    if (!flood) {
        return RIFT_TROPO_NOT_FLOOD;
    }
    if (hops >= RIFT_TROPO_IMPLAUSIBLE) {
        return RIFT_TROPO_IMPLAUSIBLE_PATH;
    }
    if (own) {
        return RIFT_TROPO_OWN;
    }
    /* Received after now is a clock this one does not share: now. */
    rx = rx_mono > now_mono ? now_mono : rx_mono;
    if (hops < RIFT_TROPO_HOPS) {
        baseline(t, rx, hops);
        return RIFT_TROPO_SHALLOW;
    }
    t->deep_seen++;
    t->revision++;
    if (!hash) {
        return RIFT_TROPO_NO_HASH;
    }
    /* A repeat if either memory says so: this ring, or the service's (its
     * own log's count, MeshCore's seen table). The ring is bounded, and a
     * hash that has left it would otherwise be new evidence again. */
    if (service_repeat || seen(t, hash)) {
        t->repeats++;
        return RIFT_TROPO_REPEAT;
    }
    if (now_mono - rx > RIFT_TROPO_WINDOW_MS) {
        t->stale++;
        return RIFT_TROPO_STALE;
    }
    remember(t, hash);
    if (active_at(t, rx)) {
        /* Part of the opening that is under way: it holds it open longer
         * and may deepen it. Reported late, it can be older than the first
         * evidence; the span says what was received, in either direction. */
        if (rx > t->last_ms) {
            t->last_ms = rx;
        }
        if (rx < t->first_ms) {
            t->first_ms = rx;
        }
        t->evidence++;
        if (hops > t->peak_hops) {
            t->peak_hops = (uint8_t)hops;
        }
        return RIFT_TROPO_EXTENDED;
    }
    return candidate(t, rx, hops);
}

enum rift_tropo_verdict rift_tropo_observe(struct rift_tropo *t, const struct rift_rx_entry *e,
                                           int64_t now_mono)
{
    int flood;
    int own;

    if (!t || !e || e->kind != RIFT_RX_FRAME) {
        return RIFT_TROPO_NOT_FRAME;
    }
    if (e->state == RIFT_RXS_REJECTED || e->state == RIFT_RXS_UNRESOLVED) {
        return RIFT_TROPO_REJECTED;
    }
    if (!(e->flags & RIFT_RXF_PARSED) || (e->flags & RIFT_RXF_PATH_SNR) ||
        ((e->flags & RIFT_RXF_HAVE_HEADER) && e->type_code == TYPE_TRACE)) {
        return RIFT_TROPO_NO_PATH;
    }
    flood = (e->flags & RIFT_RXF_HAVE_HEADER) &&
            (e->route == RIFT_RXRT_FLOOD || e->route == RIFT_RXRT_TRANSPORT_FLOOD);
    own = (e->flags & RIFT_RXF_OWN) || e->state == RIFT_RXS_ECHO;
    return rift_tropo_step_seen(t, e->mono_ms, e->path_hops, flood, 1, own,
                                (e->flags & RIFT_RXF_HAVE_HASH) ? e->hash : NULL,
                                e->dup > 1 || (e->flags & RIFT_RXF_MC_DUPLICATE), now_mono);
}

int rift_tropo_active(const struct rift_tropo *t, int64_t now_mono)
{
    return t && active_at(t, now_mono);
}

void rift_tropo_read(const struct rift_tropo *t, int64_t now_mono, struct rift_tropo_view *out)
{
    int64_t nw;
    int i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!t) {
        return;
    }
    out->openings = t->openings;
    for (i = 0; i < t->cand_count; i++) {
        if (now_mono - t->cand[i].mono_ms <= RIFT_TROPO_WINDOW_MS) {
            out->candidates++;
        }
    }
    nw = quarter(now_mono);
    if (t->base_window_ms == nw) {
        uint8_t v = t->base_now > t->base_before ? t->base_now : t->base_before;

        out->have_base = v > 0;
        out->base_hops = v > 0 ? (uint8_t)(v - 1) : 0;
    } else if (t->base_window_ms == nw - RIFT_TROPO_WINDOW_MS && t->base_now > 0) {
        out->have_base = 1;
        out->base_hops = (uint8_t)(t->base_now - 1);
    }
    if (!t->have_opening) {
        out->level = RIFT_TROPO_NONE;
        return;
    }
    out->age_ms = now_mono - t->last_ms;
    if (out->age_ms < 0) {
        out->age_ms = 0;
    }
    out->active = out->age_ms < RIFT_TROPO_HOLD_MS;
    out->level = out->age_ms < RIFT_TROPO_DETECTED_MS ? RIFT_TROPO_DETECTED
                 : out->age_ms < RIFT_TROPO_RECENT_MS ? RIFT_TROPO_RECENT
                                                      : RIFT_TROPO_LAST;
    out->span_ms = t->last_ms - t->first_ms;
    out->since_ms = now_mono - t->first_ms;
    out->closes_in_ms = out->active ? RIFT_TROPO_HOLD_MS - out->age_ms : 0;
    out->evidence = t->evidence;
    out->peak_hops = t->peak_hops;
}

const char *rift_tropo_level_word(enum rift_tropo_level level)
{
    switch (level) {
    case RIFT_TROPO_DETECTED:
        return "TROPO DETECTED";
    case RIFT_TROPO_RECENT:
        return "RECENT TROPO";
    case RIFT_TROPO_LAST:
        return "LAST TROPO";
    case RIFT_TROPO_NONE:
    default:
        return "NO TROPO SEEN";
    }
}
