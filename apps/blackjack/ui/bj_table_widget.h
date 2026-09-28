/*
 * PG Blackjack table widget: the felt, the dealer's and the player's hands
 * with a label over each, drawn in one draw callback from the game and the
 * view model's geometry. It decides nothing and takes no input.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_TABLE_WIDGET_H
#define PGBJ_TABLE_WIDGET_H

#include "bj_view.h"
#include "lvgl.h"

lv_obj_t *bj_table_create(lv_obj_t *parent);
void bj_table_bind(lv_obj_t *table, const struct bj_game *g);
void bj_table_geometry(lv_obj_t *table, struct bj_table *out);

#endif
