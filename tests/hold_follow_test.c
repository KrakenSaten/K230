/*
 * Following the hold while a camera screen is open (ui/shell/hold_follow.h):
 * a new hold is laid out once; a pass that could not run (the layout's hold
 * stays the old one, as when the frame has no area) is not retried on every
 * tick; another hold, or the hold coming back first, allows a new try.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hold_follow.h"

#include <stdio.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

int main(void)
{
    int tried = -1;
    int lay = 0;
    int passes = 0;
    int tick;

    check("the hold laid out for: nothing to do", !hold_follow(0, lay, &tried) && tried == -1);

    /* A new hold whose pass runs: one pass, and the layout takes the hold. */
    if (hold_follow(270, lay, &tried)) {
        passes++;
        lay = 270;
    }
    check("a new hold: one pass", passes == 1 && lay == 270);
    check("then nothing more once laid out", !hold_follow(270, lay, &tried) && tried == -1);

    /* A new hold whose pass cannot run (lay stays 270): 100 ticks, one try. */
    passes = 0;
    for (tick = 0; tick < 100; tick++) {
        if (hold_follow(0, lay, &tried)) {
            passes++; /* the pass runs into a frame with no area: lay unchanged */
        }
    }
    check("a pass that cannot run is tried once, not every tick", passes == 1 && lay == 270);

    /* Another hold while that one is pending: tried, once. */
    passes = 0;
    for (tick = 0; tick < 10; tick++) {
        passes += hold_follow(90, lay, &tried);
    }
    check("another hold is tried, once", passes == 1);

    /* Back to the hold laid out for, then the failed one again: tried anew. */
    check("back to the layout's hold: nothing, and the try is forgotten", !hold_follow(270, lay, &tried) && tried == -1);
    check("the failed hold again after that: tried again", hold_follow(0, lay, &tried) && tried == 0);

    printf("hold_follow_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
