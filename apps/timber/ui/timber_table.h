/*
 * PocketTimber table: one custom-drawn object for the whole tower rather
 * than a widget per block.
 *
 * P7 placeholder: every block is three flat faces in Design System tokens,
 * drawn through the view model's projection; the felt, the wood and the
 * light are the D1 art, which does not exist yet and is not drawn here. The
 * table owns no timer and no game state: the app ticks the run and
 * invalidates the table, so there is one clock in the app.
 *
 * Colour comes from pos_theme_color() inside the draw callback, which is the
 * sanctioned way for custom drawing to reach the tokens, and the object
 * registers with pos_theme_watch() so it repaints when the theme changes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETTIMBER_TABLE_H
#define POCKETTIMBER_TABLE_H

#include "timber_view.h"

#include "lvgl.h"

lv_obj_t *timber_table_create(lv_obj_t *parent, int width, int height);
/* The run to paint and the view to paint it through; the table only reads
 * them. */
void timber_table_bind(lv_obj_t *table, const struct timber_run *run, const struct timber_view *view);
/* The slot the block in hand would go to, drawn as a ghost; -1 for none. */
void timber_table_set_ghost(lv_obj_t *table, int slot);
/* Called with the block a tap picked, or -1 for the felt. */
void timber_table_set_tap(lv_obj_t *table, void (*cb)(void *user, int id), void *user);

#endif
