/*
 * Trails: where each tracked object has been on the picture, for TRACK and
 * TRAFFIC to draw behind its box (docs/apps/VISION.md).
 *
 * Kept by the screen from the det lines it already gets (the boxes' centres
 * in view pixels, by track id): nothing new from the helper. Fixed storage -
 * VISION_TRAIL_TRACKS trails of VISION_TRAIL_POINTS points - so a busy scene
 * costs what an empty one does. A point is added only when the centre has
 * moved VISION_TRAIL_STEP_PX since the last, so a parked car leaves no
 * smear. A trail whose track is not on a det line for VISION_TRAIL_KEEP
 * lines in a row is let go; a new id never inherits an old trail.
 *
 * Pure C, no LVGL (tests/vision_model_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_TRAILS_H
#define VISION_TRAILS_H

#include "vision_session.h"

#include <stdint.h>

#define VISION_TRAIL_TRACKS VISION_MAX_SHOWN
#define VISION_TRAIL_POINTS 8
#define VISION_TRAIL_STEP_PX 6
#define VISION_TRAIL_KEEP 8

struct vision_trail {
    uint32_t id;       /* 0: free */
    uint8_t n;         /* points held */
    uint8_t head;      /* where the next goes */
    uint8_t missing;   /* det lines in a row without this track */
    int32_t x[VISION_TRAIL_POINTS];
    int32_t y[VISION_TRAIL_POINTS];
};

struct vision_trails {
    struct vision_trail t[VISION_TRAIL_TRACKS];
};

void vision_trails_clear(struct vision_trails *tr);
/* One det line: each confirmed box's centre joins its trail. */
void vision_trails_update(struct vision_trails *tr, const struct vision_shown *s, int n);
/* Trail i's points, oldest first, into xs/ys (up to max). Returns how many:
 * 0 for a free slot, and under 2 is nothing to draw. */
int vision_trails_points(const struct vision_trails *tr, int i, int32_t *xs, int32_t *ys, int max);

#endif
