/*
 * Non-maximum suppression (pocketvision.h): of overlapping boxes of one
 * class, the most confident survives.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_NMS_H
#define POCKETOS_VISION_NMS_H

#include "pocketvision.h"

/* Sort `n` candidates by confidence (the array is reordered) and copy the
 * survivors into `out`, at most `max`, most confident first: a candidate is
 * dropped when it overlaps a kept box of the same class by more than
 * iou_max (per-mille). Returns how many were kept. */
int vision_nms(struct vision_det *cands, int n, uint32_t iou_max, struct vision_det *out, int max);

#endif
