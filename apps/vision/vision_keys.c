/*
 * Vision's keyboard moves. See vision_keys.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_keys.h"

#include <stdint.h>

/* Centres in doubled pixels, so an odd size needs no rounding. */
static int64_t cx2(const struct vision_rect *r)
{
    return 2 * (int64_t)r->x + r->w;
}

static int64_t cy2(const struct vision_rect *r)
{
    return 2 * (int64_t)r->y + r->h;
}

int vision_keys_move(const struct vision_rect *r, int n, int from, enum vision_key_move m)
{
    int64_t best_d = -1;
    int best = -1;
    int i;

    if (n <= 0) {
        return -1;
    }
    if (from < 0 || from >= n) {
        return 0;
    }
    switch (m) {
    case VISION_KEY_NEXT:
        return (from + 1) % n;
    case VISION_KEY_PREV:
        return (from + n - 1) % n;
    case VISION_KEY_LEFT:
    case VISION_KEY_RIGHT:
    case VISION_KEY_UP:
    case VISION_KEY_DOWN:
        break;
    case VISION_KEY_NONE:
    default:
        return from;
    }
    for (i = 0; i < n; i++) {
        int64_t dx = cx2(&r[i]) - cx2(&r[from]);
        int64_t dy = cy2(&r[i]) - cy2(&r[from]);
        int64_t along;
        int64_t across;
        int64_t d;

        if (i == from) {
            continue;
        }
        switch (m) {
        case VISION_KEY_LEFT: along = -dx; across = dy; break;
        case VISION_KEY_RIGHT: along = dx; across = dy; break;
        case VISION_KEY_UP: along = -dy; across = dx; break;
        default: along = dy; across = dx; break;
        }
        if (along <= 0) {
            continue;
        }
        d = along + 2 * (across < 0 ? -across : across);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best < 0 ? from : best;
}
