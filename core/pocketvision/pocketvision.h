/*
 * pocketvision: what the Vision app's helper does with a camera frame after
 * the KPU has looked at it (docs/apps/VISION.md).
 *
 *   decode   the detector's raw output tensor into candidate boxes
 *            (vision_decode.h): YOLOv8's [1][4 + classes][rows] layout, the
 *            letterbox undone, thresholded, bounded, and refused when the
 *            tensor is not the shape it claims or is not numbers;
 *   nms      non-maximum suppression, per class (vision_nms.h);
 *   track    persistent identities across frames, a bounded list, expiry
 *            (vision_track.h);
 *   line     one virtual line and the crossings counted on it, with a
 *            direction (vision_line.h);
 *   geom     where a sensor-frame box lands on the picture the screen shows
 *            (vision_geom.h), the same turn, mirror and cover-fit
 *            pocketcam_convert.c draws with.
 *
 * All of it is plain C with fixed-size storage: no allocation, no floating
 * point outside the decoder (the tensor is float), no clock, no I/O. Every
 * count is bounded by a constant here, so a busy scene costs the same as an
 * empty one and nothing grows with time.
 *
 * Coordinates are integer pixels. Confidences are per-mille (0..1000).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_POCKETVISION_H
#define POCKETOS_POCKETVISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest class count a model may declare (COCO has 80). */
#define VISION_MAX_CLASSES 128
/* Candidates the decoder keeps: the best-scoring ones when more pass. */
#define VISION_MAX_CANDIDATES 256
/* Detections after suppression, per frame. */
#define VISION_MAX_DETECTIONS 32
/* Tracks alive at once. A detection with no room is dropped, not a track. */
#define VISION_MAX_TRACKS 32
#define VISION_CONF_SCALE 1000
/* The largest coordinate a box may carry (POCKETCAM_MAX_DIM, so a corrupt
 * tensor cannot make an overflow out of an area). */
#define VISION_MAX_COORD 4096

struct vision_box {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct vision_det {
    struct vision_box box;
    uint16_t cls;
    uint16_t conf; /* per-mille */
};

/* Intersection over union, per-mille. 0 for boxes that do not overlap or
 * have no area. */
uint32_t vision_iou_permille(const struct vision_box *a, const struct vision_box *b);

#endif
