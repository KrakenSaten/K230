/*
 * peerd replay window (peer_replay.h). Pure.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_replay.h"

enum pr_verdict pr_check(const struct pr_window *w, uint64_t epoch, uint64_t seq)
{
    uint64_t behind;

    if (epoch == 0 || seq == 0) {
        return PR_INVALID;
    }
    if (epoch > w->epoch) {
        return PR_NEWER;
    }
    if (epoch < w->epoch) {
        return PR_REPLAY;
    }
    if (seq > w->top) {
        return PR_NEWER;
    }
    behind = w->top - seq;
    if (behind >= PR_WINDOW) {
        return PR_REPLAY;
    }
    return (w->map >> behind) & 1u ? PR_REPLAY : PR_WITHIN;
}

void pr_commit(struct pr_window *w, uint64_t epoch, uint64_t seq)
{
    switch (pr_check(w, epoch, seq)) {
    case PR_NEWER:
        if (epoch > w->epoch) {
            w->epoch = epoch;
            w->top = seq;
            w->map = 1;
        } else {
            uint64_t ahead = seq - w->top;

            /* A shift by the type's width is undefined: clear instead. */
            w->map = ahead >= PR_WINDOW ? 0 : w->map << ahead;
            w->map |= 1;
            w->top = seq;
        }
        break;
    case PR_WITHIN:
        w->map |= (uint64_t)1 << (w->top - seq);
        break;
    default:
        break;
    }
}

void pr_restore(struct pr_window *w, uint64_t epoch, uint64_t mark)
{
    w->epoch = epoch;
    w->top = mark;
    w->map = epoch && mark ? ~(uint64_t)0 : 0;
}
