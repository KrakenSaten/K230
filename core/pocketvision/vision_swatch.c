/*
 * COLOR's reading. See vision_swatch.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_swatch.h"

#include <errno.h>
#include <string.h>

int32_t vision_swatch_half(uint32_t w, uint32_t h)
{
    int32_t half = (int32_t)((w < h ? w : h) / 16);

    if (half < VISION_SWATCH_HALF_MIN) {
        half = VISION_SWATCH_HALF_MIN;
    }
    return half > VISION_SWATCH_HALF_MAX ? VISION_SWATCH_HALF_MAX : half;
}

enum vision_swatch_cat vision_swatch_classify(struct vision_rgb c)
{
    int32_t mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int32_t mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    int32_t ch = mx - mn;
    int32_t s = mx ? (ch * 255) / mx : 0;
    int32_t hue;

    if (mx < VISION_SWATCH_DARK_V) {
        return VISION_SWATCH_DARK;
    }
    if (s < VISION_SWATCH_GREY_S) {
        return mx >= VISION_SWATCH_WHITE_V ? VISION_SWATCH_WHITE : VISION_SWATCH_GREY;
    }
    /* Hue in degrees, 0..359. */
    if (mx == c.r) {
        hue = (60 * ((int32_t)c.g - (int32_t)c.b)) / ch;
    } else if (mx == c.g) {
        hue = 120 + (60 * ((int32_t)c.b - (int32_t)c.r)) / ch;
    } else {
        hue = 240 + (60 * ((int32_t)c.r - (int32_t)c.g)) / ch;
    }
    if (hue < 0) {
        hue += 360;
    }
    if (hue < 15 || hue >= 345) {
        /* A pale red is pink. */
        return s < 150 && mx >= 170 ? VISION_SWATCH_PINK : VISION_SWATCH_RED;
    }
    if (hue < 50 && mx < 160) {
        /* A dark orange or a dark yellow-orange is brown. */
        return VISION_SWATCH_BROWN;
    }
    if (hue < 40) {
        return VISION_SWATCH_ORANGE;
    }
    if (hue < 70) {
        return VISION_SWATCH_YELLOW;
    }
    if (hue < 160) {
        return VISION_SWATCH_GREEN;
    }
    if (hue < 200) {
        return VISION_SWATCH_CYAN;
    }
    if (hue < 260) {
        return VISION_SWATCH_BLUE;
    }
    if (hue < 290) {
        return VISION_SWATCH_PURPLE;
    }
    return VISION_SWATCH_PINK;
}

int vision_swatch_read(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, int32_t x, int32_t y,
                       int32_t half, struct vision_swatch_read *out)
{
    uint32_t count[VISION_SWATCH_CATS];
    uint32_t sum[VISION_SWATCH_CATS][3];
    uint64_t value = 0;
    uint32_t n = 0;
    int32_t i;
    int32_t j;
    int first = 0;
    int second = -1;
    int k;

    if (!px || !out || w < 3 || h < 3 || w > VISION_PIXELS_MAX_W || stride < w || half < 1 ||
        2 * half + 1 > (int32_t)w || 2 * half + 1 > (int32_t)h) {
        return -EINVAL;
    }
    /* The square moves inside the picture rather than being cut by it. */
    if (x < half) {
        x = half;
    }
    if (y < half) {
        y = half;
    }
    if (x > (int32_t)w - 1 - half) {
        x = (int32_t)w - 1 - half;
    }
    if (y > (int32_t)h - 1 - half) {
        y = (int32_t)h - 1 - half;
    }
    memset(count, 0, sizeof(count));
    memset(sum, 0, sizeof(sum));
    for (j = y - half; j <= y + half; j++) {
        const uint16_t *row = px + (size_t)j * stride;

        for (i = x - half; i <= x + half; i++) {
            struct vision_rgb c = vision_rgb565_to_rgb(row[i]);
            enum vision_swatch_cat cat = vision_swatch_classify(c);
            uint8_t mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);

            count[cat]++;
            sum[cat][0] += c.r;
            sum[cat][1] += c.g;
            sum[cat][2] += c.b;
            value += mx;
            n++;
        }
    }
    for (k = 1; k < VISION_SWATCH_CATS; k++) {
        if (count[k] > count[first]) {
            first = k;
        }
    }
    for (k = 0; k < VISION_SWATCH_CATS; k++) {
        if (k != first && count[k] > 0 && (second < 0 || count[k] > count[second])) {
            second = k;
        }
    }
    memset(out, 0, sizeof(*out));
    out->first = (enum vision_swatch_cat)first;
    out->second = (enum vision_swatch_cat)(second < 0 ? first : second);
    out->first_pm = (count[first] * 1000) / n;
    out->second_pm = second < 0 ? 0 : (count[second] * 1000) / n;
    out->cat = out->first_pm >= VISION_SWATCH_MIXED_PM ? out->first : VISION_SWATCH_MIXED;
    out->mean.r = (uint8_t)(sum[first][0] / count[first]);
    out->mean.g = (uint8_t)(sum[first][1] / count[first]);
    out->mean.b = (uint8_t)(sum[first][2] / count[first]);
    out->value = (uint8_t)(value / n);
    return 0;
}

void vision_swatch_filter_reset(struct vision_swatch_filter *f)
{
    memset(f, 0, sizeof(*f));
}

static void shown_mean(struct vision_swatch_filter *f)
{
    uint32_t s[3] = { 0, 0, 0 };
    uint32_t i;

    for (i = 0; i < f->n; i++) {
        s[0] += f->ring[i].r;
        s[1] += f->ring[i].g;
        s[2] += f->ring[i].b;
    }
    f->rgb.r = (uint8_t)(s[0] / f->n);
    f->rgb.g = (uint8_t)(s[1] / f->n);
    f->rgb.b = (uint8_t)(s[2] / f->n);
}

static void take(struct vision_swatch_filter *f, const struct vision_swatch_read *r)
{
    f->shown = r->cat;
    f->shown_first = r->first;
    f->shown_second = r->second;
    f->shown_pm = r->first_pm;
    f->n = 0;
    f->next = 0;
    f->steady = 0;
    f->cand_n = 0;
    f->have = true;
}

bool vision_swatch_filter_add(struct vision_swatch_filter *f, const struct vision_swatch_read *r)
{
    bool changed = false;

    if (!f->have) {
        take(f, r);
        changed = true;
    } else if (r->cat != f->shown) {
        /* A new name has to hold for VISION_SWATCH_CONFIRM readings. */
        if (r->cat == f->cand && f->cand_n > 0) {
            f->cand_n++;
        } else {
            f->cand = r->cat;
            f->cand_n = 1;
        }
        if (f->cand_n < VISION_SWATCH_CONFIRM) {
            f->steady++;
            return false;
        }
        take(f, r);
        changed = true;
    } else {
        f->cand_n = 0;
        if (r->cat == VISION_SWATCH_MIXED && r->first != f->shown_first) {
            /* Still mixed, but another colour is the largest now: its mean
             * starts the window again, so red and blue never average into
             * purple. */
            f->n = 0;
            f->next = 0;
        }
        f->shown_first = r->first;
        f->shown_second = r->second;
        f->shown_pm = r->first_pm;
    }
    /* The colour of the name shown, averaged over a bounded window. */
    f->ring[f->next] = r->mean;
    f->next = (f->next + 1) % VISION_SWATCH_WINDOW;
    if (f->n < VISION_SWATCH_WINDOW) {
        f->n++;
    }
    shown_mean(f);
    if (!changed) {
        f->steady++;
    }
    return changed;
}
