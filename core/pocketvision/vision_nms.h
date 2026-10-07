/*
 * Non-maximum suppression (pocketvision.h): of overlapping boxes of one
 * class, the most confident survives.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_NMS_H
#define POCKETOS_VISION_NMS_H

#include "pocketvision.h"

/* Sort `n` candidates by confidence (the array is reordered) and copy the
 * survivors into `out`, at most `max`, most confident first: a candidate is
 * dropped when it overlaps a kept box of the same class by more than
 * iou_max (per-mille). Returns how many were kept. */
int vision_nms(struct vision_det *cands, int n, uint32_t iou_max, struct vision_det *out, int max);

/* After suppression: a box of which at least inside_pm (per-mille of its
 * own area) lies inside a larger box of the same class is the same object
 * seen twice - a detector's partial box beside its full one (unit B,
 * 2026-09-29: a person walking past gave two person boxes, both crossed
 * the line, and was counted twice) - and is dropped; the larger one stays.
 * Their overlap is too small for the IoU test to catch. Compacts `dets` in
 * place, keeping the order; returns how many remain. */
int vision_nms_nested(struct vision_det *dets, int n, uint32_t inside_pm);

#endif
