/*
 * The detector's raw output into boxes (pocketvision.h).
 *
 * YOLOv8 detection heads, as nncase exports them for the K230 (the vendor's
 * yolov8.cc reads the same layout): one float tensor [1][4 + classes][rows],
 * rows = sum over the strides 8, 16 and 32 of (in_w / s) * (in_h / s), each
 * row a box centre and size in model-input pixels followed by one score per
 * class (already sigmoid'd, 0..1 - but the kmodel is quantized, and on unit B
 * a dark frame gave scores from -0.0091 to 0.0187: every row a hair below
 * zero. Scores within VISION_SCORE_SLACK of [0, 1] are quantization, clamped;
 * only what no sigmoid can give is refused). The picture went in letterboxed: scaled by
 * ratio = min(in_w / frame_w, in_h / frame_h) into the top-left corner and
 * padded on the right and bottom, so a coordinate comes back by dividing by
 * the ratio and nothing else.
 *
 * MALFORMED OUTPUT. A tensor whose shape is not this layout for the model's
 * declared input and classes is refused (-EPROTO) before a value is read.
 * Values are then read one row at a time: a row with a score that is not a
 * finite number, or a box that is not finite, empty, or far outside the
 * input, is skipped and counted in *bad; the caller decides what to do with a
 * frame where most rows are bad (the helper reports it and carries on). No
 * value ever reaches a coordinate larger than VISION_MAX_COORD, whatever the
 * tensor says.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_DECODE_H
#define POCKETOS_VISION_DECODE_H

#include "pocketvision.h"

/* How far outside [0, 1] a class score may be and still be a quantized
 * sigmoid. Measured noise on unit B: 0.0091 below zero; this is ten times it,
 * and still twenty times smaller than the smallest threshold anyone would
 * use. */
#define VISION_SCORE_SLACK 0.1f

struct vision_decode_params {
    uint32_t in_w;      /* the model's input */
    uint32_t in_h;
    uint32_t classes;
    uint32_t frame_w;   /* the frame that was letterboxed in */
    uint32_t frame_h;
    uint16_t conf_min;  /* per-mille; a row below it is not a candidate */
};

/* The rows a YOLOv8 head has for this input, or 0 when the input is not a
 * multiple of 32 (a head needs whole cells). */
uint32_t vision_decode_rows(uint32_t in_w, uint32_t in_h);

/* Decode `out`, a tensor of shape [dims[0]][dims[1]][dims[2]] (rank must
 * be 3) holding `count` floats, into at most `max` candidates in frame
 * pixels, the best by score kept when more pass. Returns how many, or
 * -EPROTO for a tensor of the wrong shape or size, -EINVAL for bad params.
 * *bad (may be NULL) gets the rows skipped as not-a-number or nonsense. */
int vision_decode(const float *out, size_t count, const uint32_t dims[3],
                  const struct vision_decode_params *p, struct vision_det *dets, int max,
                  uint32_t *bad);

#endif
