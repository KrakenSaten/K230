/*
 * PocketRadar sensor scope: one custom-drawn object for the whole circular
 * display rather than a widget per contact.
 *
 * It draws the rim, the range rings, the bearing ticks, the sweep and every
 * contact on the scope, and it converts a touch into the polar coordinates
 * the engine picks with. It owns no timer and no game state: the app ticks
 * the run and invalidates the scope, so there is one clock in the app and
 * not two that could disagree.
 *
 * Colour comes from pos_theme_color() inside the draw callback, which is the
 * sanctioned way for custom drawing to reach the Design System tokens, and
 * the object registers with pos_theme_watch() so it repaints when the theme
 * changes - a rewritten shared style does not invalidate custom drawing by
 * itself.
 *
 * Every contact state carries a shape as well as a colour (DS section 2):
 * an unidentified return is a hollow circle, a normal target a filled one, a
 * fast target a triangle, a decoy a crossed diamond and a high-value target
 * a square inside a ring. Selection adds corner brackets and acquisition
 * fills a ring around the marker, so the three interaction steps are legible
 * with no colour at all.
 *
 * Integer arithmetic only, through lv_trigo_sin/cos, lv_atan2 and lv_sqrt32:
 * no libm in PocketRadar at any layer.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETRADAR_SCOPE_H
#define POCKETRADAR_SCOPE_H

#include "../engine/radar_rules.h"

#include "lvgl.h"

/* What a brief effect marks. They are transient and drawn from a timestamp,
 * so nothing has to be cleaned up if the run ends mid-effect. */
enum radar_scope_flash {
    RADAR_FLASH_NONE = 0,
    RADAR_FLASH_HIT,      /* a target was destroyed: a burst that widens out */
    RADAR_FLASH_FOUL,     /* a decoy was engaged: the same burst, in warn */
    RADAR_FLASH_FADED     /* a track was lost: a ring that closes in */
};

/* A square object of the given side; the scope is inscribed in it. */
lv_obj_t *radar_scope_create(lv_obj_t *parent, int size);
/* The side the scope is drawn at now, or 0. */
int radar_scope_size(lv_obj_t *scope);
/* Change that side in place: the object is resized and repainted, and nothing
 * is rebuilt. The face, the contacts and the conversion between a tap and a
 * bearing and range all read the one stored geometry, so they cannot fall out
 * of step. A side that is not positive, or the one already in force, does
 * nothing. */
void radar_scope_set_size(lv_obj_t *scope, int size);
/* The run to paint. The scope only reads it. */
void radar_scope_bind(lv_obj_t *scope, const struct radar_run *run);
/* Decorative motion on or off (DS section 12). With it off there is no
 * rotating sweep and no burst; contacts still move, because their movement
 * is the game rather than an animation of it. */
void radar_scope_set_motion(lv_obj_t *scope, int enabled);
/* Called with the polar position of a tap, whatever it landed on; deciding
 * what that means is the screen's job, not the scope's. */
void radar_scope_set_tap(lv_obj_t *scope, void (*cb)(void *user, int bearing, int range),
                         void *user);
/* Start a brief effect at a position. Replaces any effect still running. */
void radar_scope_flash(lv_obj_t *scope, enum radar_scope_flash kind, int bearing,
                       int range);

/* Pixel offset from the centre of the scope to a bearing and range, for the
 * screen to place anything it wants to hang off a contact. Exposed mainly so
 * the round trip against the tap conversion can be checked. */
void radar_scope_point(const lv_obj_t *scope, int bearing, int range, int *dx, int *dy);
/* The inverse: a pixel offset from the centre back to bearing and range.
 * Returns 0, or -1 when the offset falls outside the scope. */
int radar_scope_polar(const lv_obj_t *scope, int dx, int dy, int *bearing, int *range);

#endif
