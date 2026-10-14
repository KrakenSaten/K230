/*
 * Following the unit's hold while a camera screen is open (app.h
 * hold_rotation): on an external display the shell does not restart when
 * the hold changes, so Camera and Vision lay themselves out again. This is
 * the one decision they share, kept apart so it can be tested: lay out for
 * a new hold once, and not again every tick when that pass could not run
 * (a frame with no area); try again only for another hold, or after the
 * hold came back to the one laid out for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_HOLD_FOLLOW_H
#define DOORS_HOLD_FOLLOW_H

#include <stdbool.h>

/* hold: the hold now; lay_hold: the one the layout was made for; *tried:
 * the last hold a pass was started for, -1 for none. True when a layout
 * pass should be started now (and *tried is then the hold). */
static inline bool hold_follow(int hold, int lay_hold, int *tried)
{
    if (hold == lay_hold) {
        *tried = -1;
        return false;
    }
    if (hold == *tried) {
        return false;
    }
    *tried = hold;
    return true;
}

#endif
