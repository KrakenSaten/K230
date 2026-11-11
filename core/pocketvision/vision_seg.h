/*
 * Segmentation after the KPU (pocketvision.h): SEGMENT's class mask and
 * INSTANCES' object masks, as byte maps the helper paints into the preview.
 *
 * SEMANTIC. The model (PP-LiteSeg-T, 19 Cityscapes classes) gives logits
 * [1][classes][h][w] at a fraction of its input: nncase's quantized Resize
 * does not honour half_pixel on the K230, so the last upsampling is done
 * here on the CPU, then an argmax per pixel. Both loops are written for the
 * compiler's vectoriser (Makefile: this file is built -O3).
 *
 * INSTANCE. A YOLOv8-seg head: [1][4 + classes + coeffs][rows] (a box, a
 * score per class, mask coefficients) and prototypes [1][coeffs][ph][pw].
 * An object's mask is its coefficients times the prototypes, positive
 * inside its box.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_SEG_H
#define POCKETOS_VISION_SEG_H

#include "pocketvision.h"

#define VISION_SEG_CLASSES 19
/* A map cell that belongs to nothing. */
#define VISION_SEG_NONE 255

/* Bilinear upsampling of c planes of h x w floats by the integer s, with
 * half_pixel coordinates and clamped edges, into dst (c planes of h*s x
 * w*s). 0, or -ENOMEM. */
int vision_seg_upsample(const float *src, int c, int h, int w, int s, float *dst);
/* The plane with the largest value at each of hw pixels, the first on a
 * tie (as numpy's argmax). c <= 255. */
void vision_seg_argmax(const float *v, int c, size_t hw, uint8_t *m);
/* A semantic class's name (never NULL, no spaces) and its colour, RGB565. */
const char *vision_seg_label(uint32_t cls);
uint16_t vision_seg_color(uint32_t cls);

#define VISION_INST_MAX 16
#define VISION_INST_COEFFS 32

struct vision_inst {
    struct vision_det det;   /* the box in model-input pixels */
    float coeff[VISION_INST_COEFFS];
};

/* Decode the head `out` ([ch][rows], ch = 4 + classes + VISION_INST_COEFFS)
 * into at most `max` objects, the most confident first, suppressed per
 * class. Returns how many, or -EINVAL. */
int vision_inst_decode(const float *out, uint32_t ch, uint32_t rows, uint32_t in_w, uint32_t in_h,
                       uint16_t conf_min, uint32_t iou_max, struct vision_inst *inst, int max);
/* The objects' masks as one map of 2*pw x 2*ph cells over the model's
 * input: each cell the index of the object it belongs to (the most
 * confident where two overlap) or VISION_SEG_NONE. tmp is pw*ph floats of
 * scratch. */
void vision_inst_map(const struct vision_inst *inst, int n, const float *proto, uint32_t pw, uint32_t ph,
                     uint32_t in_w, uint32_t in_h, float *tmp, uint8_t *map);
/* An object's colour by its class, RGB565. */
uint16_t vision_inst_color(uint32_t cls);

#endif
