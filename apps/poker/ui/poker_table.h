/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DOORS_POKER_TABLE_H
#define DOORS_POKER_TABLE_H
#include "lvgl.h"
#include "poker.h"

/* The game is owned by the app, never copied or freed by the widget. */
lv_obj_t *poker_table_create(lv_obj_t *parent, const struct poker_game *game);
#endif
