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
 * Touch follows the approved aim-then-confirm pattern for dense grids: a
 * press reports a cell and nothing else. Committing a shot is the caller's
 * job, on a 64 px button.
 *
 * The press reports, and so does every moment of a drag. A cell in the wide
 * shape is smaller than a thumb whatever the layout does - ten rows in a
 * 386 px body can never be more than 38 px each - so the player is not asked
 * to hit one. They land anywhere on the board and slide, and the square under
 * the finger is reported the whole way, to be read off the readout beside the
 * board. Nothing about that commits a shot.
 *
 * A cell need not be square. The wide shape is short of height and has width
 * to spare, so its cells are wider than they are tall; the two sizes are
 * stored separately and both the drawing and the hit test read them, so they
 * cannot disagree.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
/* The side of the whole grid - gutter, cells and gaps - for a cell of this
 * size, with or without labels. The one place the span is worked out, so a
 * layout can be measured before the grid it will hold exists. */
int fleet_grid_span_for(int cell, int labels);
/* Its inverse: the largest cell whose whole board fits in `span` px, or 0 when
 * none does. Board geometry lives here and nowhere else, so a layout asks for
 * a size rather than working one out from the gutter and the gaps itself. */
int fleet_grid_cell_for_span(int span, int labels);
/* The cell height the grid is drawing and hitting with now, or 0. */
int fleet_grid_cell(lv_obj_t *grid);
/* Its width, which is the same unless the layout has asked for wider ones. */
int fleet_grid_cell_across(lv_obj_t *grid);
/* Change the cell size in place: the object is resized and repainted, and
 * nothing is rebuilt. Drawing and the tap conversion both read these stored
 * sizes, so a cell can never be drawn at one size and hit at another. A size
 * that is not positive, or the size already in force, does nothing. */
void fleet_grid_set_cell(lv_obj_t *grid, int cell);
/* The same, for cells that are wider than they are tall. */
void fleet_grid_set_cell_size(lv_obj_t *grid, int cell_w, int cell_h);
/* Where the grid draws a cell, in screen coordinates. This is the same
 * arithmetic the drawing uses, exposed so that what is drawn can be compared
 * with what is hit instead of both being taken on trust. */
int fleet_grid_cell_rect(lv_obj_t *grid, int row, int col, lv_area_t *out);
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
/* Called with the cell under the finger, on the press and throughout a drag.
 * It must not commit anything. */
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
