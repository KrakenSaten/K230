/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DOORS_POKER_TABLE_H
#define DOORS_POKER_TABLE_H
#include "lvgl.h"
#include "poker.h"

/* The game is owned by the app, never copied or freed by the widget. */
lv_obj_t *poker_table_create(lv_obj_t *parent, const struct poker_game *game);
/* Motion (cg_motion.h): a picture before an action, and the difference
 * played after it - hole cards dealt round the table from the deck, the
 * board's cards turned from it, the hands turned over at a showdown and the
 * pot slid to the winner. The app does not call poker_table_after under
 * reduced motion, and holds the opponents while the table moves. */
void poker_table_before(lv_obj_t *table);
void poker_table_after(lv_obj_t *table);
int poker_table_moving(lv_obj_t *table);
void poker_table_finish(lv_obj_t *table);
#endif
