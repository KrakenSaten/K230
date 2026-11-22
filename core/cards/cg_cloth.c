/*
 * Card games: the table cloth. See cg_cloth.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_cloth.h"

/* 4 x 4 ordered dither thresholds, 0..15. */
static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint32_t hash(uint32_t x, uint32_t y)
{
    uint32_t v = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA6Bu;

    v ^= v >> 15;
    v *= 0x2C1B3C6Du;
    v ^= v >> 12;
    return v;
}

/* 8-bit channel to `bits` bits, rounding up where the dither says so. */
static uint32_t quantise(int v8, int bits, int threshold)
{
    int top = (1 << bits) - 1;
    int scaled = v8 * top;      /* 0 .. 255 * top */
    int q = scaled / 255;
    int rem = scaled % 255;

    if (rem * 16 > threshold * 255 + 127 && q < top) {
        q++;
    }
    return (uint32_t)q;
}

void cg_cloth_render(uint16_t *px, int w, int h, int stride, const struct cg_cloth *c)
{
    int lit[3], deep[3];
    int x, y, k;

    if (!px || !c || w <= 0 || h <= 0 || stride < w || c->rx <= 0 || c->ry <= 0) {
        return;
    }
    for (k = 0; k < 3; k++) {
        lit[k] = (int)(c->lit >> (16 - 8 * k)) & 0xFF;
        deep[k] = (int)(c->deep >> (16 - 8 * k)) & 0xFF;
    }
    for (y = 0; y < h; y++) {
        /* Distances are kept as 1/1024ths of the radius, squared. */
        int64_t dy = ((int64_t)(y - c->cy) * 1024) / c->ry;
        uint16_t *row = px + (int64_t)y * stride;

        for (x = 0; x < w; x++) {
            int64_t dx = ((int64_t)(x - c->cx) * 1024) / c->rx;
            int64_t d2 = (dx * dx + dy * dy) >> 10;   /* 1024 at the radius */
            int t = d2 > 1024 ? 1024 : (int)d2;        /* lit -> deep */
            /* Beyond two thirds of the radius (d2 = 461) the cloth falls to
             * black, reaching it at about 1.16 radii (d2 = 1382). */
            int fall = d2 <= 461 ? 0 : d2 >= 1382 ? 1024 : (int)(((d2 - 461) * 1024) / (1382 - 461));
            int keep = 1024 - fall;
            int threshold = bayer[y & 3][x & 3];
            int noise = 0;
            int ch[3];

            keep = (keep * keep) >> 10;                 /* ease the edge */
            if (c->grain) {
                noise = (int)(hash((uint32_t)x, (uint32_t)y) % (2u * c->grain + 1u)) - (int)c->grain;
            }
            for (k = 0; k < 3; k++) {
                int v = lit[k] + ((deep[k] - lit[k]) * t) / 1024;

                v = (v * keep) >> 10;
                /* Grain scales with the light, so black stays black. */
                v += (noise * v) / 96;
                ch[k] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
            row[x] = (uint16_t)((quantise(ch[0], 5, threshold) << 11) | (quantise(ch[1], 6, threshold) << 5) |
                                quantise(ch[2], 5, threshold));
        }
    }
}
