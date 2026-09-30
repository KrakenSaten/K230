/*
 * Trails. See vision_trails.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_trails.h"

#include <string.h>

void vision_trails_clear(struct vision_trails *tr)
{
    memset(tr, 0, sizeof(*tr));
}

static void push(struct vision_trail *t, int32_t x, int32_t y)
{
    if (t->n > 0) {
        int last = (t->head + VISION_TRAIL_POINTS - 1) % VISION_TRAIL_POINTS;
        int32_t dx = x - t->x[last];
        int32_t dy = y - t->y[last];

        if (dx * dx + dy * dy < VISION_TRAIL_STEP_PX * VISION_TRAIL_STEP_PX) {
            return;
        }
    }
    t->x[t->head] = x;
    t->y[t->head] = y;
    t->head = (uint8_t)((t->head + 1) % VISION_TRAIL_POINTS);
    if (t->n < VISION_TRAIL_POINTS) {
        t->n++;
    }
}

void vision_trails_update(struct vision_trails *tr, const struct vision_shown *s, int n)
{
    bool seen[VISION_TRAIL_TRACKS];
    int i;
    int j;

    memset(seen, 0, sizeof(seen));
    for (i = 0; i < n; i++) {
        int slot = -1;
        int free_slot = -1;

        if (s[i].id == 0) {
            continue; /* not confirmed: no trail yet */
        }
        for (j = 0; j < VISION_TRAIL_TRACKS; j++) {
            if (tr->t[j].id == s[i].id) {
                slot = j;
                break;
            }
            if (free_slot < 0 && tr->t[j].id == 0) {
                free_slot = j;
            }
        }
        if (slot < 0) {
            if (free_slot < 0) {
                continue; /* every slot is a live trail: this one waits */
            }
            slot = free_slot;
            memset(&tr->t[slot], 0, sizeof(tr->t[slot]));
            tr->t[slot].id = s[i].id;
        }
        seen[slot] = true;
        tr->t[slot].missing = 0;
        push(&tr->t[slot], s[i].x + s[i].w / 2, s[i].y + s[i].h / 2);
    }
    for (j = 0; j < VISION_TRAIL_TRACKS; j++) {
        if (tr->t[j].id == 0 || seen[j]) {
            continue;
        }
        if (++tr->t[j].missing >= VISION_TRAIL_KEEP) {
            memset(&tr->t[j], 0, sizeof(tr->t[j]));
        }
    }
}

int vision_trails_points(const struct vision_trails *tr, int i, int32_t *xs, int32_t *ys, int max)
{
    const struct vision_trail *t;
    int k;
    int start;

    if (i < 0 || i >= VISION_TRAIL_TRACKS || tr->t[i].id == 0) {
        return 0;
    }
    t = &tr->t[i];
    start = (t->head + VISION_TRAIL_POINTS - t->n) % VISION_TRAIL_POINTS;
    for (k = 0; k < t->n && k < max; k++) {
        xs[k] = t->x[(start + k) % VISION_TRAIL_POINTS];
        ys[k] = t->y[(start + k) % VISION_TRAIL_POINTS];
    }
    return k;
}
