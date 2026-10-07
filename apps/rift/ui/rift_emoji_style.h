/*
 * RIFT colour emoji on a label: one style per type role whose only property
 * is the role's current font with the colour emoji behind it
 * (rift_emoji_font.h). Added after the role's shared style, it wins for the
 * font and changes nothing else; Plex still draws every letter.
 *
 * The shared styles change font with the text size and the display mode, so
 * rift_emoji_style_refresh() follows them; the RIFT app calls it on the
 * theme-changed event, which comes after the shared styles are refilled.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_EMOJI_STYLE_H
#define RIFT_EMOJI_STYLE_H

#include "lvgl.h"
#include "pos_styles.h"

/* The colour-emoji style for role's font, made current. */
lv_style_t *rift_emoji_style(enum pos_style_role role);

/* Adds it to label (after the label's role style). */
void rift_emoji_style_add(lv_obj_t *label, enum pos_style_role role);

/* Every colour-emoji style made so far, back on its role's current font. */
void rift_emoji_style_refresh(void);

#endif
