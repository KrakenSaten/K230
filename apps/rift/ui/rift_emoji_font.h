/*
 * RIFT colour emoji: an LVGL font that draws Noto Color Emoji images.
 *
 * Image glyphs (LV_FONT_GLYPH_FORMAT_IMAGE) are drawn by LVGL's software
 * renderer as images, in their own colours; that path is part of the core,
 * not of the imgfont module the device's LVGL is built without. The images
 * are compiled in as RGB565A8 (rift_emoji_img.h), so nothing is decoded and
 * the image cache, which is off, is never needed.
 *
 * Only RIFT uses it. rift_emoji_font() gives a copy of a Plex font
 * descriptor whose fallback is the colour font: Plex draws every character it
 * has, exactly as everywhere else, and LVGL asks the colour font only for
 * what Plex lacks. The Plex fonts themselves are not touched. The copy keeps
 * the base font's own fallback behind the colour font.
 *
 * An image sits centred in the line of the font it falls back from, with a
 * pixel of space on each side.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_EMOJI_FONT_H
#define RIFT_EMOJI_FONT_H

#include "lvgl.h"

#include <stdbool.h>

/* Copies the faces can hold: RIFT's body, caption and title sizes. */
#define RIFT_EMOJI_FACES 16

/* base with the colour emoji behind it; base itself if it already is such a
 * copy, or if every copy is taken (then emoji draw as base draws them). */
const lv_font_t *rift_emoji_font(const lv_font_t *base);

/* Whether font is one of rift_emoji_font()'s copies. */
bool rift_emoji_font_is(const lv_font_t *font);

/* The image for key (a code point, or a folded sequence's private code
 * point: rift_emoji.h), or NULL. */
const lv_image_dsc_t *rift_emoji_image(uint32_t key);

#endif
