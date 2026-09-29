/*
 * PG 2048 board widget: sixteen cells and the tiles on them, drawn in one
 * draw callback from the game state and the view model's geometry.
 *
 * The widget holds no rules and makes no decisions. It is told the board to
 * show, or a move to animate from the turn the engine reported, and it asks
 * g2048_view for every rectangle, fill and curve. Its size is whatever the
 * app gives it; the cells follow from that size, so a different layout
 * tomorrow needs no change here.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PG2048_BOARD_H
#define PG2048_BOARD_H

#include "g2048_rules.h"
#include "lvgl.h"

lv_obj_t *g2048_board_create(lv_obj_t *parent);
/* Show this board as it stands, ending any animation. */
void g2048_board_show(lv_obj_t *board, const struct g2048_game *g);
/* Animate the move the engine just made; g is the game after it. With
 * reduced motion the end state is shown at once (DS 12). */
void g2048_board_animate(lv_obj_t *board, const struct g2048_game *g, const struct g2048_turn *turn,
                         int reduced_motion);
/* Jump an animation to its end. */
void g2048_board_finish(lv_obj_t *board);
int g2048_board_animating(lv_obj_t *board);

#endif
