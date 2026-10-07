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

#endif
