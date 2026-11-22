/*
 * PG Blackjack table widget: the dealer's and the player's hands with a
 * label over each, the stake as chips and a settled round's result, drawn in
 * one draw callback from the game and the view model's geometry, over the
 * scene's cloth. It decides nothing and takes no input.
 *
 * Motion (cg_motion.h): the app takes a picture before a command
 * (bj_table_before) and plays the difference after it (bj_table_after):
 * cards come from the shoe one by one, the hole card turns, the dealer draws
 * at a dealer's pace, the stake slides to whoever won it and the result
 * appears. The game has already settled; only the picture is late. Under
 * reduced motion the app does not call bj_table_after and the end state is
 * drawn at once.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGBJ_TABLE_WIDGET_H
#define PGBJ_TABLE_WIDGET_H

#include "bj_view.h"
#include "cg_motion.h"
#include "lvgl.h"

lv_obj_t *bj_table_create(lv_obj_t *parent);
void bj_table_bind(lv_obj_t *table, const struct bj_game *g);
void bj_table_geometry(lv_obj_t *table, struct bj_table *out);

void bj_table_before(lv_obj_t *table);
/* `new_round`: the hands were dealt anew, so every card is a new one;
 * `doubled`: the command doubled the stake. */
void bj_table_after(lv_obj_t *table, int new_round, int doubled);
int bj_table_moving(lv_obj_t *table);
/* Everything arrives now. */
void bj_table_finish(lv_obj_t *table);
const struct cg_motion *bj_table_motion(lv_obj_t *table);

#endif
