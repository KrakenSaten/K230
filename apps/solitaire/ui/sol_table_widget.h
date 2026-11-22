/*
 * PG Solitaire table widget: the felt, the thirteen piles, the selection and
 * the keyboard cursor, drawn in one draw callback from the game, the
 * interaction state and the view model's geometry; and a tap turned into a
 * hit on a pile or a card.
 *
 * It decides nothing. Its geometry is sol_view_table() of its own size, so a
 * different layout tomorrow changes the size it is given and nothing here.
 * When a column is too long for the table even at its tightest fan, the
 * object scrolls vertically by sol_view_scroll_max() and the tableau alone
 * moves, clipped under the top row; the view model places, hits and
 * highlights cards at the same scroll.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGSOL_TABLE_WIDGET_H
#define PGSOL_TABLE_WIDGET_H

#include "lvgl.h"
#include "sol_view.h"

lv_obj_t *sol_table_create(lv_obj_t *parent);
void sol_table_bind(lv_obj_t *table, const struct sol_game *g, const struct sol_ui *ui);
void sol_table_on_tap(lv_obj_t *table, void (*cb)(void *user, struct sol_hit hit), void *user);
/* Redraw after the game changed, the tableau's scroll held to the range the
 * game now gives (sol_view_scroll_max). */
void sol_table_refresh(lv_obj_t *table);
/* Scroll the tableau, as little as it takes, so the keyboard cursor's card
 * (or empty column) is in view. A drag scrolls it too; nothing else does. */
void sol_table_follow_cursor(lv_obj_t *table);
/* The geometry the table last drew with, for tests. */
void sol_table_geometry(lv_obj_t *table, struct sol_table *out);

/* Motion (cg_motion.h): a picture before a command, and the difference
 * played after it - a card moved travels, a card drawn travels to the waste
 * turning over, a card uncovered in a column turns over. A whole new deal or
 * the waste turned back (more than the runner holds) simply appears. When
 * `won` the four kings then rise and settle one after another. The app does
 * not call sol_table_after under reduced motion. */
void sol_table_before(lv_obj_t *table);
void sol_table_after(lv_obj_t *table, int won);
int sol_table_moving(lv_obj_t *table);
void sol_table_finish(lv_obj_t *table);

#endif
