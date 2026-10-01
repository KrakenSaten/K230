/*
 * RIFT's DM sound policy. See rift_notify.h.
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
    n->seen = m ? m->dm_arrivals : 0;
}

void rift_notify_set_enabled(struct rift_notify *n, int enabled)
{
    if (n) {
        n->enabled = enabled ? 1 : 0;
    }
}

int rift_notify_poll(struct rift_notify *n, const struct rift_model *m, int64_t now_ms,
                     int can_sound)
{
    unsigned fresh;

    if (!n || !m) {
        return 0;
    }
    /* Everything since the last look is consumed here, whatever becomes of
     * it: an arrival is news once. dm_arrivals only grows, so a smaller
     * number is a model this listener was not started on, and there is
     * nothing to say about it. */
    fresh = m->dm_arrivals >= n->seen ? m->dm_arrivals - n->seen : 0;
    n->seen = m->dm_arrivals;
    if (fresh == 0) {
        return 0;
    }
    if (!n->enabled) {
        n->off += fresh;
        return 0;
    }
    if (!can_sound) {
        n->silent += fresh;
        return 0;
    }
    /* One sound per gap, measured from the last sound rather than from the
     * last arrival, so a steady trickle is still heard now and then instead
     * of being held silent for as long as it keeps coming. A clock that went
     * backwards is not a reason to stay quiet for ever. */
    if (n->have_last && now_ms >= n->last_ms && now_ms - n->last_ms < RIFT_NOTIFY_GAP_MS) {
        n->coalesced += fresh;
        return 0;
    }
    n->have_last = 1;
    n->last_ms = now_ms;
    n->played++;
    n->coalesced += fresh - 1;
    return 1;
}
