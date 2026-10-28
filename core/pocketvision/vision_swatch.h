/*
 * COLOR's reading (docs/apps/VISION.md, "COLOR"): what colour is in a small
 * square of the preview picture, by name, steadily.
 *
 *   READ    every pixel of the square is put in one colour category by
 *           its hue, saturation and value (integer HSV): DARK (too little
 *           light to say, or black), GREY, WHITE, or one of nine hues. The
 *           category most pixels share wins, with its share; when no
 *           category holds VISION_SWATCH_MIXED_PM of the square the reading
 *           is MIXED, and names the two largest. The colour shown is the
 *           mean of the winning category's pixels only, so a red patch
 *           beside a white margin is shown red, not pink.
 *   FILTER  a bounded temporal filter over the readings: the name shown
 *           changes only after VISION_SWATCH_CONFIRM readings in a row
 *           agree on a new one (one odd frame never flips it), and the
 *           colour is a running mean over at most VISION_SWATCH_WINDOW
 *           readings of the name shown, started again when the name
 *           changes - so the swatch never blends two colours into a third
 *           while it changes.
 *
 * It names colours as the camera sees them under the light there is: no
 * calibration, no white balance of its own, no claim of a measurement.
 * Integer only, no allocation, no clock (the caller counts readings).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_SWATCH_H
#define POCKETOS_VISION_SWATCH_H

#include "vision_pixels.h"

#include <stdbool.h>
#include <stdint.h>

enum vision_swatch_cat {
    VISION_SWATCH_DARK = 0, /* black, or too little light to tell */
    VISION_SWATCH_GREY,
    VISION_SWATCH_WHITE,
    VISION_SWATCH_RED,
    VISION_SWATCH_ORANGE,
    VISION_SWATCH_BROWN,
    VISION_SWATCH_YELLOW,
    VISION_SWATCH_GREEN,
    VISION_SWATCH_CYAN,
    VISION_SWATCH_BLUE,
    VISION_SWATCH_PURPLE,
    VISION_SWATCH_PINK,
    VISION_SWATCH_CATS,
    VISION_SWATCH_MIXED = VISION_SWATCH_CATS, /* no category holds enough of the square */
    VISION_SWATCH_NAMES
};

/* The square's half side: 1/16 of the picture's smaller side, within
 * these bounds. */
#define VISION_SWATCH_HALF_MIN 6
#define VISION_SWATCH_HALF_MAX 24
/* A category needs this share of the square (per-mille) to name it. */
#define VISION_SWATCH_MIXED_PM 550
/* Readings in a row that must agree before the name shown changes. */
#define VISION_SWATCH_CONFIRM 3
/* The most readings the shown colour is a mean of. */
#define VISION_SWATCH_WINDOW 8
/* The thresholds of the categories (8-bit value and saturation). */
#define VISION_SWATCH_DARK_V 48     /* value below this: DARK */
#define VISION_SWATCH_GREY_S 48     /* saturation below this: GREY or WHITE (or DARK) */
#define VISION_SWATCH_WHITE_V 200   /* a low-saturation value at or above this: WHITE */

struct vision_swatch_read {
    enum vision_swatch_cat cat;     /* the winner, or MIXED */
    enum vision_swatch_cat first;   /* the largest category (also when MIXED) */
    enum vision_swatch_cat second;  /* the next largest, or first when there is none */
    uint32_t first_pm;              /* their shares of the square, per-mille */
    uint32_t second_pm;
    struct vision_rgb mean;         /* the mean of the first category's pixels */
    uint8_t value;                  /* the square's mean value (max of r, g, b), 0..255 */
};

struct vision_swatch_filter {
    bool have;                      /* something is shown */
    enum vision_swatch_cat shown;
    enum vision_swatch_cat shown_first; /* the largest category (shown unless MIXED) */
    enum vision_swatch_cat shown_second;
    uint32_t shown_pm;
    enum vision_swatch_cat cand;    /* a different name seen in a row */
    uint32_t cand_n;
    uint32_t n;                     /* readings in the mean, at most VISION_SWATCH_WINDOW */
    uint32_t next;                  /* where the next one goes in ring */
    struct vision_rgb ring[VISION_SWATCH_WINDOW];
    struct vision_rgb rgb;          /* the colour shown: the mean of ring */
    uint32_t steady;                /* readings since the name shown last changed */
};

/* The square's half side for a picture. */
int32_t vision_swatch_half(uint32_t w, uint32_t h);
/* The category of one colour. */
enum vision_swatch_cat vision_swatch_classify(struct vision_rgb c);
/* The category's name on the screen, lower case ("red", "mixed"). Here, so
 * the screen can name a category without linking the pixel code. */
static inline const char *vision_swatch_name(int cat)
{
    static const char *const names[VISION_SWATCH_NAMES] = {
        "dark", "grey", "white", "red", "orange", "brown", "yellow", "green", "cyan", "blue", "purple", "pink", "mixed",
    };

    return cat >= 0 && cat < VISION_SWATCH_NAMES ? names[cat] : "?";
}

/* Read the square of half side `half` around (x, y), moved inside the
 * picture. 0, or -EINVAL for a picture this cannot read. */
int vision_swatch_read(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, int32_t x, int32_t y,
                       int32_t half, struct vision_swatch_read *out);

void vision_swatch_filter_reset(struct vision_swatch_filter *f);
/* One reading into the filter; returns true when the name shown changed. */
bool vision_swatch_filter_add(struct vision_swatch_filter *f, const struct vision_swatch_read *r);

#endif
