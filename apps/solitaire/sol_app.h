/*
 * PG Solitaire app, the parts a test may look at: the game and interaction
 * state it holds, and its objects. Nothing here changes anything.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_APP_H
#define PGSOL_APP_H

#include "lvgl.h"
#include "sol_view.h"

const struct sol_game *sol_app_game(void *priv);
const struct sol_ui *sol_app_ui(void *priv);
lv_obj_t *sol_app_table(void *priv);
lv_obj_t *sol_app_caption(void *priv);
/* 0: the accent button; 1: the other. A hidden button is still returned. */
lv_obj_t *sol_app_button(void *priv, int index);

#endif
