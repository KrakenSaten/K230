/*
 * RIFT's message sound policy. See rift_notify.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_notify.h"

#include <string.h>

void rift_notify_init(struct rift_notify *n, const struct rift_model *m, int enabled)
{
    if (!n) {
        return;
    }
    memset(n, 0, sizeof(*n));
    n->enabled = enabled ? 1 : 0;
    n->ch_enabled = 1;
    n->seen = m ? m->dm_arrivals : 0;
    n->ch_seen = m ? m->ch_arrivals : 0;
}

void rift_notify_set_enabled(struct rift_notify *n, int enabled)
{
    if (n) {
        n->enabled = enabled ? 1 : 0;
    }
}

void rift_notify_set_channel(struct rift_notify *n, int enabled, rift_notify_muted_fn muted,
                             void *user)
{
    if (n) {
        n->ch_enabled = enabled ? 1 : 0;
        n->muted = muted;
        n->muted_user = user;
    }
}

/* How many arrived since the last look, consumed whatever becomes of them:
 * an arrival is news once. The counters only grow, so a smaller number is a
 * model this listener was not started on, and there is nothing to say. */
static unsigned take(unsigned now, unsigned *seen)
{
    unsigned fresh = now >= *seen ? now - *seen : 0;

    *seen = now;
    return fresh;
}

/* Of the channel arrivals from first to last (1-based), how many may sound:
 * those whose channel is not muted. One older than the model's ring cannot be
 * told apart, and is counted as muted rather than guessed at - only a burst
 * larger than the ring within one poll reaches that, and it is one sound at
 * most either way. */
static unsigned channel_audible(struct rift_notify *n, const struct rift_model *m,
                                unsigned first, unsigned last)
{
    unsigned audible = 0;
    unsigned i;

    for (i = first; i <= last; i++) {
        const char *conv = rift_model_ch_arrival_conv(m, i);

        if (!conv || (n->muted && n->muted(conv, n->muted_user))) {
            n->ch_muted++;
        } else {
            audible++;
        }
    }
    return audible;
}

int rift_notify_poll(struct rift_notify *n, const struct rift_model *m, int64_t now_ms,
                     int can_sound)
{
    unsigned first_ch;
    unsigned dm;
    unsigned ch;
    int want;

    if (!n || !m) {
        return RIFT_NOTIFY_NONE;
    }
    first_ch = n->ch_seen + 1;
    dm = take(m->dm_arrivals, &n->seen);
    ch = take(m->ch_arrivals, &n->ch_seen);
    if (dm && !n->enabled) {
        n->off += dm;
        dm = 0;
    }
    if (ch && !n->ch_enabled) {
        n->ch_off += ch;
        ch = 0;
    } else if (ch) {
        ch = channel_audible(n, m, first_ch, m->ch_arrivals);
    }
    if (dm + ch == 0) {
        return RIFT_NOTIFY_NONE;
    }
    if (!can_sound) {
        n->silent += dm + ch;
        return RIFT_NOTIFY_NONE;
    }
    /* One sound per gap, measured from the last sound rather than from the
     * last arrival, so a steady trickle is still heard now and then instead
     * of being held silent for as long as it keeps coming. A clock that went
     * backwards is not a reason to stay quiet for ever. */
    if (n->have_last && now_ms >= n->last_ms && now_ms - n->last_ms < RIFT_NOTIFY_GAP_MS) {
        n->coalesced += dm + ch;
        return RIFT_NOTIFY_NONE;
    }
    n->have_last = 1;
    n->last_ms = now_ms;
    n->coalesced += dm + ch - 1;
    /* A direct message is the more personal of the two: it wins a tie. */
    want = dm ? RIFT_NOTIFY_DM : RIFT_NOTIFY_CHANNEL;
    if (want == RIFT_NOTIFY_DM) {
        n->played++;
    } else {
        n->ch_played++;
    }
    return want;
}
