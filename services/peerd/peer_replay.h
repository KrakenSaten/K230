/*
 * peerd replay protection (docs/decisions/ADR-017, section 6). Pure.
 *
 * There is no clock that survives a power cut, so freshness is the sender's
 * own counters: a persistent epoch, raised once per run, and a sequence from
 * 1 within it. A receiver keeps, per node, the highest epoch accepted, the
 * highest sequence in it (`top`) and a 64-bit map of top-63 .. top.
 *
 *   a higher epoch          accepted; the window restarts
 *   the same epoch:
 *     above top             accepted; top advances (a jump of 64 or more
 *                           clears the map)
 *     top-63 .. top, unseen accepted
 *     seen, or below top-63 dropped
 *   a lower epoch           dropped
 *
 * Checking and committing are separate: a frame is checked before its tag or
 * signature (cheap, and read-only), and committed only after it verified.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_REPLAY_H
#define PEERD_REPLAY_H

#include <stdint.h>

#define PR_WINDOW 64

struct pr_window {
    uint64_t epoch;     /* 0: nothing accepted yet */
    uint64_t top;
    uint64_t map;       /* bit i: sequence top - i was accepted */
};

enum pr_verdict {
    PR_NEWER = 1,       /* a higher epoch, or above top: advances the window */
    PR_WITHIN = 2,      /* inside the window and unseen: accepted, advances nothing */
    PR_REPLAY = -1,     /* seen, below the window, or an older epoch */
    PR_INVALID = -2,    /* epoch or sequence zero */
};

/* Read-only. */
enum pr_verdict pr_check(const struct pr_window *w, uint64_t epoch, uint64_t seq);
/* Record an accepted frame. Call only after pr_check() said PR_NEWER or
 * PR_WITHIN for the same values; anything else is ignored. */
void pr_commit(struct pr_window *w, uint64_t epoch, uint64_t seq);
/* After a restart: everything at or below (epoch, mark) counts as seen. */
void pr_restore(struct pr_window *w, uint64_t epoch, uint64_t mark);

#endif
