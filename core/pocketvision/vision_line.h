/*
 * One virtual line, and the crossings counted on it (pocketvision.h).
 *
 * The line is a segment (x0, y0) to (x1, y1) in the coordinates the tracks
 * are in. It divides the plane into side A (to the left of the direction
 * from the first point to the second - for a line drawn left to right,
 * above it; for one drawn top to bottom, to its right... see
 * vision_line_side) and side B. Every confirmed track remembers the side
 * its centre was last clearly on; when the centre is clearly on the other
 * side, that is one crossing, counted in the direction it went. "Clearly"
 * is a dead band of `dead_px` either side of the line, so a centre wobbling
 * on the line itself counts nothing until it has committed.
 *
 * A track is never counted twice for one crossing, and counts again only
 * when it has really gone back: its remembered side changes only on a clear
 * sighting of the other side.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_LINE_H
#define POCKETOS_VISION_LINE_H

#include "vision_track.h"

#define VISION_LINE_DEAD_PX 4

struct vision_line {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    bool enabled;
};

struct vision_counts {
    uint32_t ab; /* crossings from side A to side B */
    uint32_t ba;
};

/* Which side of the line (px, py) is clearly on: -1 for A, +1 for B, 0
 * within dead_px of the line or on a line with no length. Side B is where
 * (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0) is positive: for a line
 * drawn left to right (x1 > x0, y1 == y0) that is below it, for one drawn
 * top to bottom (y1 > y0, x1 == x0) that is to its left. */
int vision_line_side(const struct vision_line *l, int32_t px, int32_t py, int32_t dead_px);

/* Look at every track after a tracker update and count what crossed.
 * Returns how many crossings this call added. Tracks that are not
 * confirmed only learn their side. */
int vision_line_count(const struct vision_line *l, struct vision_tracker *tr,
                      struct vision_counts *c);

#endif
