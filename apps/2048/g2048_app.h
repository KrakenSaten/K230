/*
 * PG 2048 app, the parts a test may look at.
 *
 * The app itself is `app_2048` (app.h). A test hosting it the way the shell
 * does can read the game it holds and whether it is asking to start again,
 * so what a key or a finger did is checked against the rules rather than by
 * reading pixels. Nothing here changes the game.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PG2048_APP_H
#define PG2048_APP_H

#include "engine/g2048_rules.h"
#include "lvgl.h"

const struct g2048_game *g2048_app_game(void *priv);
int g2048_app_confirming(void *priv);
/* The board widget, the caption over the buttons, and the two buttons (the
 * accent one first); a hidden button is still returned. */
lv_obj_t *g2048_app_board(void *priv);
lv_obj_t *g2048_app_caption(void *priv);
lv_obj_t *g2048_app_button(void *priv, int index);

#endif
