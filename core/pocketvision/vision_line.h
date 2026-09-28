/*
 * Virtual lines, and the crossings counted on them (pocketvision.h).
 *
 * A line is a segment (x0, y0) to (x1, y1) in the coordinates the tracks
 * are in. It divides the plane into side A (to the left of the direction
 * from the first point to the second - for a line drawn left to right,
 * above it; for one drawn top to bottom, to its right... see
 * vision_line_side) and side B. Every confirmed track remembers the side
 * its centre was last clearly on; when the centre has settled clearly on
 * the other side, that is one crossing, counted in the direction it went.
 *
 * "Clearly" is a dead band either side of the line, sized to the object:
 * VISION_LINE_DEAD_PM of the box's smaller side, at least
 * VISION_LINE_DEAD_PX, so a centre wobbling on the line itself counts
 * nothing until it has committed. "Settled" is VISION_LINE_SETTLE clear
 * sightings on the new side in a row: one frame's jump across and back is
 * a jitter, not a crossing and a return.
 *
 * A track is never counted twice for one crossing, and counts again only
 * when it has really gone back: its remembered side changes only when the
 * other side has settled. A coasting (unseen) track keeps its side, so a
 * prediction never crosses a line by itself.
 *
 * A track keeps VISION_LINES such states, one per line it is watched
 * against (the count line and the two speed lines); `idx` says which.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_LINE_H
#define POCKETOS_VISION_LINE_H

#include "vision_track.h"

#define VISION_LINE_DEAD_PX 4
#define VISION_LINE_DEAD_PM 250  /* of the box's smaller side, either side of the line */
#define VISION_LINE_SETTLE 2     /* clear sightings on the other side before it counts */

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

/* One crossing, as vision_line_count reports it. */
struct vision_crossing {
    uint32_t id;
    uint16_t cls;
    int8_t dir;   /* +1: A to B, -1: B to A */
    int index;    /* the track's place in the tracker, this frame */
};

/* Which side of the line (px, py) is clearly on: -1 for A, +1 for B, 0
 * within dead_px of the line or on a line with no length. Side B is where
 * (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0) is positive: for a line
 * drawn left to right (x1 > x0, y1 == y0) that is below it, for one drawn
 * top to bottom (y1 > y0, x1 == x0) that is to its left. */
int vision_line_side(const struct vision_line *l, int32_t px, int32_t py, int32_t dead_px);

/* The dead band for a box of this size. */
int32_t vision_line_dead_px(const struct vision_box *b);

/* Look at every track after a tracker update and count what crossed line
 * `idx` (0 .. VISION_LINES - 1: the track state to use). Returns how many
 * crossings this call added; up to `max` of them are described in `out`
 * (may be NULL). Tracks that are not confirmed only learn their side. */
int vision_line_count(const struct vision_line *l, int idx, struct vision_tracker *tr,
                      struct vision_counts *c, struct vision_crossing *out, int max);

/* Every track learns its side of line idx afresh (a line that moved). */
void vision_line_forget(struct vision_tracker *tr, int idx);

#endif
