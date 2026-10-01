/*
 * The optional Vision modes that look at pixels, not at the detector's
 * output (pocketvision.h, docs/apps/VISION.md "Optional modes"): each one
 * a pass over the preview picture the helper has already drawn (RGB565,
 * at most VISION_PIXELS_MAX_W wide), in place, integer only, with a small
 * result the screen can say.
 *
 *   COLOR   every pixel within `tol` of a target colour (the sum of the
 *           three channel differences, 8-bit) is painted the highlight;
 *           the count and the centroid of the matches come back. The
 *           target is sampled from the picture itself (a 5 x 5 mean
 *           around a point) or given.
 *   EDGE    Sobel on the luma: the picture becomes its edge magnitude in
 *           grey, or black and white above a threshold; the share of
 *           strong edges comes back.
 *   TRACE   the dominant dark (or light) line: per row the centroid of
 *           the dark pixels when there is a plausible run of them, a
 *           least-squares line through those centroids, the offset of the
 *           bottom band's centroid from the picture's centre, and the
 *           slope; the centroids are marked on the picture.
 *
 * Nothing here allocates: the working rows are static and bounded by the
 * picture width the helper can ever ask for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_PIXELS_H
#define POCKETOS_VISION_PIXELS_H

#include <stdbool.h>
#include <stdint.h>

#define VISION_PIXELS_MAX_W 1024
#define VISION_COLOR_TOL_MAX 765
#define VISION_EDGE_STRONG 64      /* the grey above which an edge counts as one */
#define VISION_TRACE_DARK 64       /* luma below this is a dark line */
#define VISION_TRACE_LIGHT 192     /* luma above this is a light line */

struct vision_rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct vision_color_result {
    uint32_t matched;   /* pixels painted */
    uint32_t total;
    int32_t cx;         /* the matches' centroid, or -1 with none */
    int32_t cy;
};

struct vision_edge_result {
    uint32_t strong;    /* pixels at or above VISION_EDGE_STRONG */
    uint32_t total;
};

struct vision_trace_result {
    bool found;
    int32_t offset_pm;  /* the bottom band's centroid from the centre: -1000 (left edge) .. 1000 */
    int32_t slope_pm;   /* the line's dx per 1000 rows going down: positive leans right at the bottom */
    uint32_t rows;      /* rows with a plausible line run */
};

/* The 8-bit colour of an RGB565 pixel, and back. */
struct vision_rgb vision_rgb565_to_rgb(uint16_t v);
uint16_t vision_rgb_to_rgb565(struct vision_rgb c);
/* The luma of a colour, 0..255. */
uint8_t vision_luma(struct vision_rgb c);

/* The mean colour of the 5 x 5 pixels around (x, y), clamped to the
 * picture. 0, or -EINVAL for a picture this cannot read. */
int vision_pixels_sample(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, int32_t x,
                         int32_t y, struct vision_rgb *out);

/* Paint every pixel within tol of target with the highlight (green, or
 * magenta when the target is greenish) and report. 0 or -EINVAL. */
int vision_pixels_color(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, struct vision_rgb target,
                        uint32_t tol, struct vision_color_result *out);

/* Replace the picture by its Sobel edge magnitude: grey when threshold is
 * 0, else white at or above it and black below. 0 or -EINVAL. */
int vision_pixels_edge(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t threshold,
                       struct vision_edge_result *out);

/* Find the dominant dark (or light) line and mark its centroids. 0 or
 * -EINVAL; `found` says whether there was one. */
int vision_pixels_trace(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, bool dark,
                        struct vision_trace_result *out);

#endif
