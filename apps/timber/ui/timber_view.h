/*
 * PocketTimber view model: where everything is on the screen.
 *
 * The camera is a fixed dimetric one, slightly elevated, looking at the
 * corner of the tower where the +x and +y faces meet, so every layer shows
 * three block ends on one face and one long side on the other and every
 * block is reachable without ever turning the tower:
 *
 *     sx = ox + (x - y) * scale
 *     sy = oy + (x + y) * scale / 2 - z * layer_px
 *
 * with x and y in Q8.8 widths and z in Q8.8 with one layer TIMBER_BLOCK_HEIGHT
 * tall. The lean displaces a layer by lean * layer, and the sway displaces
 * it by the top's sway times the square of its height fraction (the mode
 * shape the stability model uses); reduced motion draws no sway.
 *
 * This file knows nothing about LVGL, so the projection, the drawing order,
 * touch picking and the sense of the pull track are tested headless
 * (tests/timber_view_test.c) and tests/timber_lint.sh keeps it that way.
 *
 * P7 placeholder geometry: the numbers below are for the simulator. The
 * real ones wait on the sprite-storm redraw budget and the sway readability
 * gate (HARDWARE VALIDATION REQUIRED).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_VIEW_H
#define POCKETTIMBER_VIEW_H

#include "../engine/timber_rules.h"

/* The canonical projection (docs/apps/POCKETTIMBER_ART.md): a 2:1 dimetric
 * camera, azimuth 45 degrees, elevation 30, orthographic. One block width
 * is 36 px along a diagonal and its top face rises 18 px per width; a
 * layer, 0.6 widths tall, is 0.6 * sqrt(2) * cos(30) * 36 = 26.46 px in
 * the render and stacks at 26, the half-pixel hidden under the layer
 * above. The sprites are rendered to exactly these numbers. */
#define TIMBER_VIEW_SCALE 36
#define TIMBER_VIEW_LAYER_PX 26
/* Clearance kept between the tower and the viewport's edges. */
#define TIMBER_VIEW_MARGIN 16
/* How near a tap has to land to a block end, in pixels. */
#define TIMBER_VIEW_PICK_PX 44
/* A tell is drawn as this much misalignment along the block's axis. */
#define TIMBER_VIEW_TELL_PX 2

struct timber_quad {
    int x[4];
    int y[4];
};

/* A block's three visible faces as screen polygons, and the centre of the
 * end the player pulls it by. */
struct timber_shape {
    struct timber_quad top;
    struct timber_quad right;   /* the +x face */
    struct timber_quad left;    /* the +y face */
    int end_x;
    int end_y;
    int end_is_right;           /* which face is the pulling end */
    int tilt;                   /* 0 to 2, a falling or fallen block's pose */
    int origin_x;               /* the far-bottom corner: where a sprite's anchor goes */
    int origin_y;
    int along_x;                /* the orientation a sprite is chosen by */
};

struct timber_view {
    int width;
    int height;
    int scale;
    int layer_px;
    int ox;
    int oy;
    uint8_t motion;
};

void timber_view_init(struct timber_view *v, int width, int height, int motion);
/* Place the origin so a tower this many layers tall is framed: the near
 * corner clears the bottom, and when the tower is too tall for the viewport
 * the top stays in view and the base goes out of it. */
void timber_view_frame(struct timber_view *v, int layers);
void timber_view_project(const struct timber_view *v, int32_t x, int32_t y, int32_t z, int *sx,
                         int *sy);
/* The shape of a present block: standing in its cell with lean, sway and
 * tell, or, while the tower falls, wherever the choreography has it.
 * Returns 0, or -1 for a block not in the tower. */
int timber_view_block(const struct timber_view *v, const struct timber_run *run, int id,
                      struct timber_shape *out);
/* The shape of the block in hand were it placed in this slot. Returns 0, or
 * -1 when nothing is in hand or the slot is not open. */
int timber_view_ghost(const struct timber_view *v, const struct timber_run *run, int slot,
                      struct timber_shape *out);
/* Every present block, back to front. ids holds TIMBER_BLOCKS. Returns the
 * count. */
int timber_view_order(const struct timber_run *run, int *ids);
/* The pullable block whose end is nearest a tap, within TIMBER_VIEW_PICK_PX,
 * or -1. */
int timber_view_pick(const struct timber_view *v, const struct timber_run *run, int sx, int sy);
/* +1 when a block in this layer moves right on screen as it is drawn
 * toward the player, -1 when it moves left: the sense of the pull track. */
int timber_view_track_sign(int layer);
/* Finger travel on the track, in pixels, to engine travel in Q8.8 widths
 * with the sign the block's axis needs. */
int32_t timber_view_travel(const struct timber_view *v, int layer, int dx_px);
/* Which side of the screen a slot of a layer is on: -1 left, 0 centre,
 * +1 right, and the inverse. */
int timber_view_slot_side(int layer, int slot);
int timber_view_slot_for_side(int layer, int side);

#endif
