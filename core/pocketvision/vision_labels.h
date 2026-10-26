/*
 * The class names the detector was trained on: COCO's eighty, in the order
 * the vendor's coco_labels.txt lists them (which is the order the kmodel's
 * scores come in). Compiled in, so a missing file is never a missing name.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_LABELS_H
#define POCKETOS_VISION_LABELS_H

#include <stdint.h>

#define VISION_COCO_CLASSES 80

/* The name of class `cls`, or "?" for one the list does not have. Never
 * NULL, never longer than 20 characters, no spaces (protocol-safe). */
const char *vision_label(uint32_t cls);

/* COCO's twelve supercategories, which hold the eighty classes in runs of
 * consecutive indices: group g is classes vision_label_group_first(g) up to
 * vision_label_group_first(g + 1) - 1. */
#define VISION_COCO_GROUPS 12
uint32_t vision_label_group_first(int g);
/* The group's name on the screen ("VEHICLES"), or "?". */
const char *vision_label_group_name(int g);

/* The six classes R0 was trained on, by COCO index (docs/apps/VISION.md). */
#define VISION_R0_CLASSES 6
uint32_t vision_label_r0_class(int i);

#endif
