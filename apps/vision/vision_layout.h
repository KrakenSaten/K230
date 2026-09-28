/*
 * Where Vision's parts go, for the two shapes (DS §38): the picture in the
 * frame's own shape, a status line, two counters, and two buttons. Pure
 * arithmetic on the body's size; it never asks which way the display is
 * turned (DS §21.2).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef VISION_LAYOUT_H
#define VISION_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define VISION_GAP 16
#define VISION_BTN_H 64
#define VISION_STATUS_H 28
#define VISION_COUNT_H 48
#define VISION_PICTURE_MAX 1024
#define VISION_SIDE_MIN 240
#define VISION_CONTROLS_MIN (VISION_STATUS_H + VISION_GAP + VISION_COUNT_H + VISION_GAP + VISION_BTN_H + VISION_GAP)

struct vision_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct vision_layout {
    bool wide;
    struct vision_rect picture;
    struct vision_rect status;
    struct vision_rect count_a;
    struct vision_rect count_b;
    struct vision_rect line_btn;
    struct vision_rect reset_btn;
};

/* w x h is the body, the insets the corner clearance; frame_w x frame_h the
 * picture's shape as the owner holds the unit (the preview turned upright).
 * 0, or -1 when the body cannot hold the screen. */
int vision_layout_compute(struct vision_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t frame_w, uint32_t frame_h);

#endif
