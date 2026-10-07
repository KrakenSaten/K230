/*
 * Faces: a RetinaFace detector's nine outputs into face boxes
 * (pocketvision.h, docs/apps/VISION.md "FACE").
 *
 * The model (the vendor's face_detection_320, RetinaFace on a 0.25
 * MobileNet) looks at an in_w x in_h picture from three strides, 8, 16 and
 * 32, with two square anchors per cell: 16 and 32 pixels, 64 and 128, 256
 * and 512. Its outputs, in this order, each [1, C, H, W] with the anchor's
 * values channel-major (anchor k's value c of cell i at
 * (k * C/2 + c) * H * W + i):
 *
 *   0-2   loc     C = 8   per anchor dx, dy, dw, dh
 *   3-5   conf    C = 4   per anchor background and face logits
 *   6-8   landms  C = 20  per anchor five points (eyes, nose, mouth corners)
 *
 * decoded as RetinaFace's box coder does (variances 0.1 and 0.2), the face
 * score the softmax of the two logits, then the surest kept and the rest
 * suppressed by overlap. The vendor's demo (ai_demo/face_detection) is the
 * reference for every constant here.
 *
 * Bounded: VISION_FACE_CANDIDATES scored anchors, VISION_FACE_MAX faces, no
 * allocation. Reads the tensors' floats; NaN and infinities are skipped and
 * counted (tests/vision_face_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_FACE_H
#define POCKETOS_VISION_FACE_H

#include <stddef.h>

#include "pocketvision.h"

#define VISION_FACE_OUTPUTS 9
#define VISION_FACE_MAX 16            /* faces kept per picture, the surest */
#define VISION_FACE_CANDIDATES 128    /* anchors over the threshold considered, the surest */
#define VISION_FACE_CONF_PM 600       /* the vendor demo's threshold */
#define VISION_FACE_NMS_PM 200        /* and its overlap */
#define VISION_FACE_POINTS 5

struct vision_face {
    struct vision_box box;            /* in the model input's pixels */
    uint16_t conf;                    /* per-mille */
    int32_t pt[VISION_FACE_POINTS][2];
};

/* One anchor, normalised to the input as the vendor's table has them. */
struct vision_face_anchor {
    float cx;
    float cy;
    float w;
    float h;
};

/* How many anchors an in_w x in_h model has (4200 for 320 x 320); 0 when
 * the size is not a multiple of 32. */
int vision_face_anchors(uint32_t in_w, uint32_t in_h);
/* Anchor idx, in the order the outputs are read. 0, or -1. */
int vision_face_anchor(uint32_t in_w, uint32_t in_h, int idx, struct vision_face_anchor *a);

/* Whether nine outputs of these shapes (rank 4, [1, C, H, W]) are this
 * model's for an in_w x in_h input. 0, or -1. */
int vision_face_check(int outputs, const uint32_t *rank, const uint32_t (*dims)[4], uint32_t in_w, uint32_t in_h);

/* The faces in the nine outputs (out[i] holding count[i] floats), surest
 * first, at most max (<= VISION_FACE_MAX). *bad (may be NULL) gets the
 * anchors skipped for a value that is not a number. Returns how many, or
 * -1 for outputs of the wrong size. */
int vision_face_decode(const float *const *out, const size_t *count, uint32_t in_w, uint32_t in_h,
                       uint16_t conf_pm, uint16_t nms_pm, struct vision_face *faces, int max, uint32_t *bad);

#endif
