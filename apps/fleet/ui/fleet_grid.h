/*
 * PocketFleet tactical grid: one custom-drawn LVGL object for the whole
 * 10 x 10 board instead of a hundred widgets.
 *
 * It paints with pos_theme_color() only, which is the sanctioned way for a
 * draw callback to reach the Design System tokens (pos_styles.h), so the grid
 * follows the selected theme and mode and tests/style_lint.sh stays green.
 * It repaints itself when the theme changes.
 *
 * Cell language (DS §2: colour never carries meaning alone, so every state
 * also has a shape):
 *
 *   radio_tx  your fire            radio_rx  fire coming at you
 *   surface   water not yet fired at
 *   bg + dot  a shot that found nothing
 *   solid square  a hit            ring + 2 px hull outline  a ship sunk
 *   surface_raised + hairline  one of your ships, still afloat
 *
 * status_error is never used here; it stays reserved for error state.
 *
 * Touch follows the approved aim-then-confirm pattern for dense grids: a tap
 * reports a cell and nothing else. Committing a shot is the caller's job, on
 * a 64 px button.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_GRID_H
#define POCKETFLEET_GRID_H

#include "../engine/fleet_rules.h"

#include "lvgl.h"

#define FLEET_GRID_GAP 2
#define FLEET_GRID_GUTTER 24   /* room for the A-J and 1-10 captions */

enum fleet_grid_mode {
    FLEET_GRID_TARGET = 0,  /* enemy waters: only what you have learned */
    FLEET_GRID_OWN,         /* your waters: your ships and incoming fire */
    FLEET_GRID_DEPLOY       /* your waters while the fleet is being placed */
};

/* Create a grid of cell x cell squares. With labels, the A-J and 1-10
 * captions take a gutter on the top and left. */
lv_obj_t *fleet_grid_create(lv_obj_t *parent, enum fleet_grid_mode mode, int cell,
                            int labels);
/* The board to read. The grid keeps the pointer, so it must outlive it. */
void fleet_grid_bind(lv_obj_t *grid, const struct fleet_board *board);
/* Crosshair position, or row < 0 to clear it. */
void fleet_grid_set_cursor(lv_obj_t *grid, int row, int col);
/* 0 and fills row/col when a crosshair is set, -1 otherwise. */
int fleet_grid_get_cursor(lv_obj_t *grid, int *row, int *col);
/* Outline a candidate ship position while deploying. valid picks between the
 * accent outline and the warning outline. */
void fleet_grid_set_preview(lv_obj_t *grid, int row, int col, int length, int vertical,
                            int valid);
void fleet_grid_clear_preview(lv_obj_t *grid);
/* Called with the cell a tap landed on. It must not commit anything. */
void fleet_grid_set_tap_cb(lv_obj_t *grid, void (*cb)(void *user, int row, int col),
                           void *user);
void fleet_grid_refresh(lv_obj_t *grid);

/* Motion (DS §12). Off leaves the grid completely static, which is the
 * reduced-motion behaviour: no sweep, and resolutions appear instantly.
 * On a target grid, motion also runs the sonar sweep. */
void fleet_grid_set_motion(lv_obj_t *grid, int enabled);
/* Mark a cell as just resolved: a ring that widens and fades once. Ignored
 * when motion is off, where the new cell state is already the whole story. */
void fleet_grid_flash(lv_obj_t *grid, int row, int col);

#endif
