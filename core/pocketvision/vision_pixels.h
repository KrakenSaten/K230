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
 *   TRACE   one dark (or light) line, followed up from the bottom of the
 *           picture. Per row, the runs of pixels clearly darker (lighter)
 *           than that row's own mean - a threshold that follows the light,
 *           not a fixed grey - and of a plausible width are the
 *           candidates. One is picked in the lower half (the one nearest
 *           where the line was last seen, else the longest chain) and
 *           followed upwards, row by row, to the run nearest where it was
 *           heading, across short gaps. A least-squares line through that
 *           chain gives the offset of its bottom from the picture's centre
 *           and its lean. With a state (struct vision_trace_state) both
 *           are smoothed over consecutive pictures, and a picture without
 *           the line says so at once: nothing stale is reported as found.
 *           The candidates are marked in cyan, the line followed in yellow.
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
#define VISION_TRACE_CONTRAST 28   /* the least a line pixel differs from its row's mean luma */
#define VISION_TRACE_RUNS 8        /* candidate runs a row may hold; more is texture, not a line */
#define VISION_TRACE_REACQUIRE 20  /* pictures a lost line is looked for where it was last seen */
#define VISION_TRACE_START_ROWS 32 /* rows (holding candidates) of the lower half a line may start on */

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
    bool found;          /* the line is in this picture */
    int32_t offset_pm;   /* its bottom from the centre: -1000 (left edge) .. 1000 */
    int32_t slope_pm;    /* its dx per 1000 rows going down: positive leans right at the bottom */
    uint32_t rows;       /* rows on the line followed */
    uint32_t candidates; /* rows with any candidate run, on the line or not */
    int32_t x0;          /* the fitted line, from its top row (x0, y0) to its bottom row (x1, y1) */
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t lost;       /* pictures since the line was last found; 0 when found */
    bool ever;           /* a line has been found since the state was reset */
};

/* What TRACE carries from picture to picture: where the line was, its
 * smoothed offset and lean, how long it has been gone. */
struct vision_trace_state {
    bool ever;
    uint32_t lost;       /* pictures since it was last found */
    uint32_t streak;     /* pictures found in a row */
    int32_t last_x;      /* its bottom when last found */
    int32_t s_x16;       /* the smoothed bottom x, x16 */
    int32_t s_slope_pm;  /* the smoothed lean */
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

/* Find the dark (or light) line, and mark it and the other candidates. st
 * carries the line from picture to picture (NULL: this picture alone). 0
 * or -EINVAL; `found` says whether the line is in this picture. */
void vision_trace_reset(struct vision_trace_state *st);
int vision_pixels_trace(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, bool dark,
                        struct vision_trace_state *st, struct vision_trace_result *out);

#endif
