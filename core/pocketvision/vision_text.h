/*
 * Text: where the text is on a picture, and what a line of it says
 * (pocketvision.h, docs/apps/VISION.md "Read").
 *
 * Two models do the work (a DB text detector and a CTC line recogniser, as
 * PaddleOCR's PP-OCR has them); this is what comes after each:
 *
 *   regions   the detector's probability map - one float per model pixel,
 *             how sure it is that the pixel is text - into boxes: the map
 *             is looked at on a grid of at most VISION_TEXT_GRID cells a
 *             side (a cell is text when any pixel in it is over the
 *             threshold), text cells joined into regions (4-connected),
 *             each region's score the mean probability of its cells, weak
 *             and tiny regions dropped, and each box grown back by DB's
 *             unclip distance (area x 1.5 / perimeter: the model marks the
 *             middle of a line, not its edges);
 *   ctc       the recogniser's per-step class scores for one line into the
 *             classes it read: the best class of every step, repeats merged,
 *             the blank class dropped; with the mean score of what was kept.
 *
 * What a class means (which character) is the dictionary's, the caller's.
 * Everything is bounded: VISION_TEXT_MAX regions, a fixed grid and a fixed
 * work stack, no allocation. Reads the tensors' floats; integer otherwise
 * (tests/vision_text_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_TEXT_H
#define POCKETOS_VISION_TEXT_H

#include "pocketvision.h"

#define VISION_TEXT_MAX 12          /* regions kept per picture, the surest */
#define VISION_TEXT_GRID 256        /* the map is looked at on at most this many cells a side */
#define VISION_TEXT_MIN_CELLS 6     /* a region of fewer cells is noise */
#define VISION_TEXT_THRESHOLD 300   /* per-mille: a pixel over this is text */
#define VISION_TEXT_BOX_MIN 500     /* per-mille: a region's mean score under this is dropped */
#define VISION_TEXT_CHARS 64        /* classes read from one line, at most */

struct vision_text_box {
    struct vision_box box;          /* in the map's pixels */
    uint16_t score;                 /* per-mille */
};

/* Regions of a mw x mh probability map whose pixel (x, y) is
 * map[(y * mw + x) * step] (step: the channels between pixels). The
 * surest `max` (at most VISION_TEXT_MAX) are kept, in reading order: by
 * their top, then their left. Returns how many, or -1 for a map that makes
 * no sense. */
int vision_text_regions(const float *map, uint32_t mw, uint32_t mh, uint32_t step, uint16_t threshold_pm,
                        uint16_t box_min_pm, struct vision_text_box *out, int max);

/* The recogniser's scores for one line, steps x classes, into at most max
 * class ids (blank dropped, repeats merged). *conf_pm (may be NULL) gets the
 * mean of the kept steps' best scores, per-mille: the scores are taken as
 * probabilities when every step's sum is within 5 % of 1, otherwise as
 * logits and turned into probabilities first. Returns how many, or -1. */
int vision_text_ctc(const float *scores, uint32_t steps, uint32_t classes, uint32_t blank, uint32_t *out, int max,
                    uint16_t *conf_pm);
/* The same, with the step each class was read at into pos (may be NULL). */
int vision_text_ctc_pos(const float *scores, uint32_t steps, uint32_t classes, uint32_t blank, uint32_t *out,
                        uint16_t *pos, int max, uint16_t *conf_pm);

#endif
