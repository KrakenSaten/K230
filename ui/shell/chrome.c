/*
 * Status chrome policy (DS §30, §36): the resolver and the geometry that
 * follows from it. See chrome.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chrome.h"

enum pocketos_chrome chrome_resolve(enum pocketos_chrome declared, bool landscape, bool home)
{
    (void)landscape;
    /* DS §36.2: the shell's own screens always show the cluster; an app
     * shows it unless it declares NONE (§30.8, the fullscreen apps). This is
     * the whole of the rule, held by one group of checks in
     * tests/chrome_test.c. */
    if (!home && declared == POCKETOS_CHROME_NONE) {
        return POCKETOS_CHROME_NONE;
    }
    return POCKETOS_CHROME_CLUSTER;
}

int32_t chrome_height(enum pocketos_chrome effective)
{
    (void)effective;
    return 0;
}

const char *chrome_name(enum pocketos_chrome c)
{
    switch (c) {
    case POCKETOS_CHROME_CLUSTER:
        return "cluster";
    case POCKETOS_CHROME_NONE:
        return "none";
    case POCKETOS_CHROME_DEFAULT:
    default:
        return "default";
    }
}

struct chrome_box chrome_content_box(enum pocketos_chrome effective, int32_t display_h,
                                     int32_t reserve)
{
    struct chrome_box b;

    if (reserve < 0) {
        reserve = 0;
    }
    b.y = chrome_height(effective);
    b.height = display_h - b.y - reserve;
    if (b.height < 0) {
        b.height = 0;
    }
    return b;
}

/* ---- the status cluster ------------------------------------------------- */

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

int32_t chrome_cluster_width(int32_t chip_w, int32_t clock_w)
{
    chip_w = max32(chip_w, 0);
    if (clock_w <= 0) {
        /* The chip alone, with the same air on both sides of it. */
        return POCKETOS_CHROME_CLUSTER_PAD_L + chip_w + POCKETOS_CHROME_CLUSTER_PAD_L;
    }
    return POCKETOS_CHROME_CLUSTER_PAD_L + chip_w + POCKETOS_CHROME_CLUSTER_GAP + clock_w +
           POCKETOS_CHROME_CLUSTER_PAD_R;
}

struct chrome_rect chrome_cluster_box(int32_t display_w, int32_t inset_right, int32_t width)
{
    struct chrome_rect r;
    int32_t margin = max32(POCKETOS_CHROME_EDGE_MIN, inset_right);
    int32_t room = display_w - 2 * margin;

    room = max32(room, 0);
    width = max32(width, 0);
    if (width > room) {
        width = room;
    }
    r.w = width;
    r.x = display_w - margin - width;
    r.y = POCKETOS_CHROME_CLUSTER_Y;
    r.h = POCKETOS_CHROME_CLUSTER_H;
    return r;
}

int32_t chrome_row_reserve(enum pocketos_chrome effective, int32_t display_w,
                           const struct chrome_rect *reserve)
{
    if (effective == POCKETOS_CHROME_NONE || !reserve) {
        return 0;
    }
    return max32(display_w - reserve->x + POCKETOS_CHROME_CLUSTER_CLEAR, 0);
}

bool chrome_rects_overlap(const struct chrome_rect *a, const struct chrome_rect *b)
{
    return a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0 && a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

struct chrome_chip chrome_chip_box(int32_t line_h)
{
    struct chrome_chip c;
    int32_t room = POCKETOS_CHROME_CLUSTER_H - 2 * POCKETOS_CHROME_CHIP_AIR;
    int32_t spare;

    if (line_h < 0) {
        line_h = 0;
    }
    c.height = POCKETOS_CHROME_CHIP_H;
    if (c.height < line_h + 2 * POCKETOS_CHROME_CHIP_MIN_PAD) {
        c.height = line_h + 2 * POCKETOS_CHROME_CHIP_MIN_PAD;
    }
    if (c.height > room) {
        c.height = room;
    }
    spare = c.height - line_h;
    if (spare < 0) {
        spare = 0;
    }
    c.pad_top = spare / 2;
    c.pad_bottom = spare - c.pad_top;
    return c;
}
