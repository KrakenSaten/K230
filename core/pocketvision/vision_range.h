/*
 * Detection range: what NEAR, NORMAL and FAR change in the pipeline
 * (pocketvision.h, docs/apps/VISION.md "Detection range").
 *
 * A range is a pipeline preset, not a distance: nothing here knows metres.
 * What limits how far the detector sees is how many of the model's pixels an
 * object covers. The camera's 640 x 360 picture goes into a 320 x 320 model
 * at half size, so a car 24 px long on the picture is 12 px to the model,
 * below what YOLOv8n finds reliably (docs/apps/VISION.md has the bench
 * figures). FAR adds a second pass over the picture's centre at the model's
 * own resolution - a window of the model's size, twice the pixels per object
 * - and merges what it finds with the full picture's pass. It does not lower
 * the confidence threshold: a quieter threshold finds more of everything,
 * the false boxes included. To keep what the extra pass adds honest, a FAR
 * track needs one more sighting before it is confirmed, and a far object -
 * few pixels, flickering in and out - is kept a little longer while unseen.
 *
 * NEAR is the other way round: only objects of some size count. A box whose
 * smaller side is under an eighth of the picture's smaller side (45 px on
 * the 360 px side) is dropped, so a camera watching a driveway ignores the
 * street beyond it, and a track is let go sooner. The threshold is NORMAL's:
 * a higher one (0.45 was tried) lost the nearest car of a real road picture
 * on the KPU, which scored it 0.44. NORMAL is the pipeline as it was: the
 * safe default.
 *
 *   range    confidence  smallest box      zoom pass  confirm  kept unseen
 *   NEAR     0.35        1/8 of the side   no         2        10 frames
 *   NORMAL   0.35        -                 no         2        15 frames
 *   FAR      0.35        -                 centre     3        20 frames
 *
 * Pure C, integer, no allocation (tests/vision_range_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_RANGE_H
#define POCKETOS_VISION_RANGE_H

#include "pocketvision.h"

enum vision_range {
    VISION_RANGE_NEAR = 0,
    VISION_RANGE_NORMAL,
    VISION_RANGE_FAR,
    VISION_RANGES
};

/* A zoom box this close to the window's edge (and not at the picture's) was
 * cut by the window: the full pass sees that object whole. */
#define VISION_RANGE_EDGE_PX 2
/* The two passes' boxes of one object: overlapping this much (per-mille
 * IoU), or one this much inside the other. */
#define VISION_RANGE_MERGE_IOU 500
#define VISION_RANGE_MERGE_INSIDE 850

struct vision_range_params {
    uint16_t conf_min;      /* per-mille: a candidate below it is not a detection */
    uint16_t min_side_pm;   /* a box whose smaller side is under this share (per-mille) of the picture's smaller side is dropped; 0: none */
    bool zoom;              /* the second pass over the centre */
    uint16_t min_hits;      /* sightings before a track is confirmed */
    uint16_t max_misses;    /* frames a track is kept unseen */
};

void vision_range_params(enum vision_range r, struct vision_range_params *p);
/* "near", "normal", "far"; the parse gives -1 for anything else. */
const char *vision_range_word(enum vision_range r);
int vision_range_parse(const char *word);

/* The zoom window on a pic_w x pic_h picture for a model of in_w x in_h: the
 * largest centred window of the model's shape that the model sees at its own
 * resolution or better (at most in_w x in_h, and inside the picture). 0, or
 * -1 when no window gives the model more pixels per object than the full
 * picture already does (a picture no larger than the model). */
int vision_range_zoom_window(uint32_t pic_w, uint32_t pic_h, uint32_t in_w, uint32_t in_h, struct vision_box *win);

/* The zoom pass's boxes (already in picture pixels) merged into the full
 * pass's: a zoom box cut by the window's edge is dropped; a zoom box that is
 * the same object as a full box (same class or same group: IoU at least
 * VISION_RANGE_MERGE_IOU, or either VISION_RANGE_MERGE_INSIDE inside the
 * other) replaces it when it is more confident and is dropped otherwise;
 * every other zoom box is added. `full` holds nfull boxes with room for max;
 * returns how many it holds after. `group` (may be NULL) is the tracker's
 * class-group table. */
int vision_range_merge(struct vision_det *full, int nfull, int max, const struct vision_det *zoom, int nzoom,
                       const struct vision_box *win, uint32_t pic_w, uint32_t pic_h, const uint8_t *group,
                       uint32_t group_classes);

/* Drop what the range does not take (below its confidence, smaller than its
 * smallest box). Compacts d in place, keeping the order; returns how many
 * remain. */
int vision_range_filter(struct vision_det *d, int n, const struct vision_range_params *p, uint32_t pic_w,
                        uint32_t pic_h);

#endif
