/*
 * Two marks RIFT draws rather than types: the warning sign over ACTIVITY's
 * graph (TROPO, rift_tropo.h) and the favourite star on a CONTACTS row.
 *
 * Drawn, for the reason the link glyph is (rift_widgets.c): the shell's
 * fonts carry neither a warning triangle nor a star, and a symbol font would
 * put a second baseline into a row whose height is fixed. Each reads its
 * colour from the theme's tokens in its draw callback - the sanctioned way
 * (pos_styles.h) - so RIFT still owns no colour. A theme change repaints
 * them with the rest of RIFT's frame (rift_app.c on_theme_changed), as it
 * does the link glyphs; they are not registered with pos_theme_watch, whose
 * 32 places a pool of starred rows would use up.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_MARKS_H
#define RIFT_MARKS_H

#include "lvgl.h"

/* The warning sign: a triangle with an exclamation mark in it. */
enum rift_warn_tone {
    RIFT_WARN_NEUTRAL = 0, /* an outline in text_muted: nothing to warn of */
    RIFT_WARN_ERROR,       /* filled in status_error */
    RIFT_WARN_WARN,        /* filled in status_warn */
    RIFT_WARN_MUTED,       /* filled in text_muted: something that was */
};
#define RIFT_WARN_W 34
#define RIFT_WARN_H 30
lv_obj_t *rift_warn_create(lv_obj_t *parent);
void rift_warn_set(lv_obj_t *sign, enum rift_warn_tone tone);
enum rift_warn_tone rift_warn_get(lv_obj_t *sign);

/* The favourite star: filled in accent_primary when on, an outline in
 * text_secondary when off (the star filled, and its inside cut out in the
 * colour of whatever it is drawn on). The object is the size given; the star is drawn
 * centred in it at RIFT_STAR_PX, so a larger object is a larger target
 * around the same star. */
#define RIFT_STAR_PX 22
lv_obj_t *rift_star_create(lv_obj_t *parent, int32_t w, int32_t h);
void rift_star_set(lv_obj_t *star, int on);
int rift_star_get(lv_obj_t *star);

#endif
