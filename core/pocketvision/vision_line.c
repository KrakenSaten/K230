/*
 * The virtual line. See vision_line.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_line.h"

int vision_line_side(const struct vision_line *l, int32_t px, int32_t py, int32_t dead_px)
{
    int64_t dx = (int64_t)l->x1 - l->x0;
    int64_t dy = (int64_t)l->y1 - l->y0;
    int64_t cross = dx * ((int64_t)py - l->y0) - dy * ((int64_t)px - l->x0);
    int64_t len2 = dx * dx + dy * dy;
    int64_t band;

    if (len2 == 0) {
        return 0;
    }
    /* |cross| / |len| is the distance from the line. Compare squares so no
     * root is needed: within the band when cross^2 <= dead^2 * len^2. */
    band = (int64_t)dead_px * dead_px * len2;
    if (cross * cross <= band) {
        return 0;
    }
    return cross > 0 ? 1 : -1;
}

int vision_line_count(const struct vision_line *l, struct vision_tracker *tr,
                      struct vision_counts *c)
{
    int i;
    int added = 0;

    if (!l->enabled) {
        return 0;
    }
    for (i = 0; i < tr->count; i++) {
        struct vision_track *t = &tr->t[i];
        int32_t cx;
        int32_t cy;
        int side;

        /* Only a real sighting moves a centre; a coasting track keeps its
         * side, so a prediction never crosses a line by itself. */
        if (!t->seen) {
            continue;
        }
        vision_box_centre(&t->box, &cx, &cy);
        side = vision_line_side(l, cx, cy, VISION_LINE_DEAD_PX);
        if (side == 0) {
            continue;
        }
        if (t->side == 0) {
            t->side = (int8_t)side;
            continue;
        }
        if (side != t->side) {
            if (t->confirmed) {
                if (side > 0) {
                    c->ab++;
                } else {
                    c->ba++;
                }
                added++;
            }
            t->side = (int8_t)side;
        }
    }
    return added;
}
