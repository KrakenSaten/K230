/*
 * RIFT colour emoji: the generated images (tools/design/gen_rift_emoji.js).
 *
 * rift_emoji_px holds every image back to back as LV_COLOR_FORMAT_RGB565A8 -
 * the RGB565 plane (w * 2 bytes a row), then the A8 plane (w bytes a row) -
 * which LVGL's software renderer blends straight onto the 16-bit panel with
 * no conversion and no cache. It is embedded by rift_emoji_px.S. The index is
 * sorted by key: the code point of a single emoji, or RIFT_EMOJI_PUA + i for
 * sequence i of rift_emoji_seqs[] (rift_emoji.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_EMOJI_IMG_H
#define RIFT_EMOJI_IMG_H

#include <stdint.h>

struct rift_emoji_img {
    uint32_t key;
    uint32_t off; /* into rift_emoji_px, 4-byte aligned */
    uint8_t w;
    uint8_t h;
};

extern const struct rift_emoji_img rift_emoji_imgs[];
extern const unsigned rift_emoji_img_count;
extern const unsigned rift_emoji_px_size;
extern const uint8_t rift_emoji_px[];

#endif
