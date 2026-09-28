/*
 * PG Solitaire table widget: the felt, the thirteen piles, the selection and
 * the keyboard cursor, drawn in one draw callback from the game, the
 * interaction state and the view model's geometry; and a tap turned into a
 * hit on a pile or a card.
 *
 * It decides nothing. Its geometry is sol_view_table() of its own size, so a
 * different layout tomorrow changes the size it is given and nothing here.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_TABLE_WIDGET_H
#define PGSOL_TABLE_WIDGET_H

#include "lvgl.h"
#include "sol_view.h"

lv_obj_t *sol_table_create(lv_obj_t *parent);
void sol_table_bind(lv_obj_t *table, const struct sol_game *g, const struct sol_ui *ui);
void sol_table_on_tap(lv_obj_t *table, void (*cb)(void *user, struct sol_hit hit), void *user);
/* The geometry the table last drew with, for tests. */
void sol_table_geometry(lv_obj_t *table, struct sol_table *out);

#endif
