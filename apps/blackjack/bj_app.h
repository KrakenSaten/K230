/*
 * PG Blackjack app, the parts a test may look at, and one it may change: the
 * game, so a test can stack the shoe and set the bankroll before it acts
 * through keys and buttons exactly as a player would.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_APP_H
#define PGBJ_APP_H

#include "bj_view.h"
#include "lvgl.h"

struct bj_game *bj_app_game(void *priv);
lv_obj_t *bj_app_table(void *priv);
lv_obj_t *bj_app_caption(void *priv);
/* The HUD's value labels, 0 bankroll and 1 bet, and the words above them. */
lv_obj_t *bj_app_value(void *priv, int index);
lv_obj_t *bj_app_hud_label(void *priv, int index);
/* Buttons left to right, 0..2; a hidden button is still returned. */
lv_obj_t *bj_app_button(void *priv, int index);
/* Repaint after a test changed the game behind the app's back. */
void bj_app_refresh(void *priv);

#endif
