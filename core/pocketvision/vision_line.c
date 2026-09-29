/*
 * The virtual lines. See vision_line.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_line.h"

#include <string.h>

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

int32_t vision_line_dead_px(const struct vision_line *l, const struct vision_box *b)
{
    int32_t side = b->w < b->h ? b->w : b->h;
    int32_t dead = (int32_t)(((int64_t)side * VISION_LINE_DEAD_PM) / 1000);

    if (l->dead_max > 0 && dead > l->dead_max) {
        dead = l->dead_max;
    }
    return dead < VISION_LINE_DEAD_PX ? VISION_LINE_DEAD_PX : dead;
}

void vision_line_forget(struct vision_tracker *tr, int idx)
{
    int i;

    if (idx < 0 || idx >= VISION_LINES) {
        return;
    }
    for (i = 0; i < tr->count; i++) {
        memset(&tr->t[i].ls[idx], 0, sizeof(tr->t[i].ls[idx]));
    }
}

int vision_line_count(const struct vision_line *l, int idx, struct vision_tracker *tr,
                      struct vision_counts *c, struct vision_crossing *out, int max)
{
    int i;
    int added = 0;

    if (!l->enabled || idx < 0 || idx >= VISION_LINES) {
        return 0;
    }
    for (i = 0; i < tr->count; i++) {
        struct vision_track *t = &tr->t[i];
        struct vision_line_state *ls = &t->ls[idx];
        int32_t cx;
        int32_t cy;
        int32_t dead;
        int side;
        bool far;

        /* Only a real sighting moves a centre; a coasting track keeps its
         * side, so a prediction never crosses a line by itself. */
        if (!t->seen) {
            continue;
        }
        vision_box_centre(&t->box, &cx, &cy);
        dead = vision_line_dead_px(l, &t->box);
        side = vision_line_side(l, cx, cy, dead);
        /* Well beyond the band - VISION_LINE_FAR_PM of it - one sighting
         * settles: the object that walks out of the frame right after
         * crossing (unit B, 2026-09-29: one clear sighting 41 px past the
         * line, then gone) is a crossing all the same. */
        far = side != 0 && vision_line_side(l, cx, cy, (int32_t)(((int64_t)dead * VISION_LINE_FAR_PM) / 1000)) == side;
        if (side == 0) {
            /* On the line: whatever was pending has to start over. */
            ls->pending = 0;
            ls->run = 0;
            continue;
        }
        if (ls->side == 0) {
            ls->side = (int8_t)side;
            continue;
        }
        if (side == ls->side) {
            ls->pending = 0;
            ls->run = 0;
            continue;
        }
        if (ls->pending == side) {
            if (ls->run < 0xff) {
                ls->run++;
            }
        } else {
            ls->pending = (int8_t)side;
            ls->run = 1;
        }
        if (ls->run < VISION_LINE_SETTLE && !far) {
            continue;
        }
        /* Settled on the other side: one crossing, in the direction it
         * went. A track not yet confirmed only changes its mind. */
        if (t->confirmed) {
            if (side > 0) {
                c->ab++;
            } else {
                c->ba++;
            }
            if (out && added < max) {
                out[added].id = t->id;
                out[added].cls = t->cls;
                out[added].dir = (int8_t)side;
                out[added].index = i;
            }
            added++;
        }
        ls->side = (int8_t)side;
        ls->pending = 0;
        ls->run = 0;
    }
    return added;
}
