/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DOORS_POKER_APP_H
#define DOORS_POKER_APP_H
#include "lvgl.h"
#include "poker_view.h"

struct poker_game *poker_app_game(void *priv);
lv_obj_t *poker_app_root(void *priv);
lv_obj_t *poker_app_button(void *priv, enum poker_control control);
lv_obj_t *poker_app_caption(void *priv);
void poker_app_refresh(void *priv);
#endif
