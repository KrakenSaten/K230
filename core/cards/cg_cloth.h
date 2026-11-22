/*
 * Card games: the table cloth under one lamp, rendered once into an RGB565
 * buffer (docs/design/card-games, direction A "Lamplight").
 *
 * The light falls from `lit` at the lamp's centre to `deep` at its radius and
 * on to black beyond it. An ordered dither and a little grain keep the dark
 * gradient from banding in 16-bit colour.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_CLOTH_H
#define CG_CLOTH_H

#include <stdint.h>

struct cg_cloth {
    uint32_t lit;       /* 0xRRGGBB at the lamp's centre */
    uint32_t deep;      /* 0xRRGGBB at the lamp's radius */
    int cx, cy;         /* the lamp's centre, pixels */
    int rx, ry;         /* its radii across and down, pixels (> 0) */
    uint8_t grain;      /* 0 none .. 8 coarse: the +- range added per pixel */
};

/* Fill w x h pixels of native-endian RGB565; `stride` is in pixels (>= w).
 * The same arguments always give the same buffer. */
void cg_cloth_render(uint16_t *px, int w, int h, int stride, const struct cg_cloth *c);

#endif
