/*
 * PocketTimber view test: the projection, framing, the shape of a block
 * with its lean, sway and tell, the drawing order, touch picking, the sense
 * of the pull track and the sides of the slots. Headless: the view model
 * is free of LVGL.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../apps/timber/ui/timber_view.h"

#include <stdio.h>
#include <stdlib.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_projection(void)
{
    struct timber_view v;
    int sx;
    int sy;

    timber_view_init(&v, 528, 700, 1);
    check("the origin is centred with the base clear of the bottom",
          v.ox == 264 && v.oy == 700 - TIMBER_VIEW_MARGIN - 3 * TIMBER_VIEW_SCALE);
    timber_view_project(&v, 0, 0, 0, &sx, &sy);
    check("the world origin projects to the view origin", sx == v.ox && sy == v.oy);
    timber_view_project(&v, TIMBER_UNIT, 0, 0, &sx, &sy);
    check("+x goes right and down", sx == v.ox + TIMBER_VIEW_SCALE && sy == v.oy + TIMBER_VIEW_SCALE / 2);
    timber_view_project(&v, 0, TIMBER_UNIT, 0, &sx, &sy);
    check("+y goes left and down", sx == v.ox - TIMBER_VIEW_SCALE && sy == v.oy + TIMBER_VIEW_SCALE / 2);
    timber_view_project(&v, 0, 0, TIMBER_BLOCK_HEIGHT, &sx, &sy);
    check("+z goes straight up one layer", sx == v.ox && sy == v.oy - TIMBER_VIEW_LAYER_PX);
    timber_view_project(&v, 3 * TIMBER_UNIT, 3 * TIMBER_UNIT, 0, &sx, &sy);
    check("the near corner of the base is three scale below the origin",
          sx == v.ox && sy == v.oy + 3 * TIMBER_VIEW_SCALE);

    timber_view_frame(&v, 40);
    check("a tower too tall for the view keeps its top in view and drops its base out",
          v.oy - 40 * TIMBER_VIEW_LAYER_PX == TIMBER_VIEW_MARGIN && v.oy > 700);
    timber_view_frame(&v, 0);
    check("framing nothing frames one layer", v.oy - TIMBER_VIEW_LAYER_PX >= TIMBER_VIEW_MARGIN);
    timber_view_init(NULL, 1, 1, 1);
    timber_view_frame(NULL, 1);
    timber_view_project(NULL, 0, 0, 0, &sx, &sy);
    timber_view_project(&v, 0, 0, 0, NULL, NULL);
}

static void test_block_shape(void)
{
    struct timber_view v;
    struct timber_run run;
    struct timber_shape s;
    struct timber_shape t;
    int sx;
    int sy;

    timber_view_init(&v, 528, 700, 1);
    timber_run_new(&run, 7u);
    run.tower.blocks[0].tell = 0;
    check("a block not in the tower has no shape",
          (timber_tower_remove(&run.tower, 53), timber_view_block(&v, &run, 53, &s) == -1) &&
          timber_view_block(&v, &run, -1, &s) == -1 && timber_view_block(NULL, &run, 0, &s) == -1);
    check("block 0 has a shape", timber_view_block(&v, &run, 0, &s) == 0);
    timber_view_project(&v, 0, 0, TIMBER_BLOCK_HEIGHT, &sx, &sy);
    check("its top face starts at its far corner, one layer up", s.top.x[0] == sx && s.top.y[0] == sy);
    check("its sprite anchor is its far-bottom corner, and it runs along x",
          s.origin_x == v.ox && s.origin_y == v.oy && s.along_x);
    timber_view_project(&v, TIMBER_BLOCK_LENGTH, TIMBER_UNIT / 2, TIMBER_BLOCK_HEIGHT / 2, &sx, &sy);
    check("an x block is pulled by its +x end, which is the right face",
          s.end_is_right && s.end_x == sx && s.end_y == sy);
    check("the +x face hangs below the top's right edge",
          s.right.x[0] == s.top.x[1] && s.right.y[0] == s.top.y[1] && s.right.y[3] > s.right.y[0]);
    check("the +y face hangs below the top's left edge",
          s.left.x[1] == s.top.x[3] && s.left.y[1] == s.top.y[3] && s.left.y[2] > s.left.y[1]);
    check("block 3, in a y layer, is pulled by its +y end, the left face",
          timber_view_block(&v, &run, 3, &t) == 0 && !t.end_is_right);
    check("a standing block has no tilt", s.tilt == 0);

    /* A tell shows as a nudge along the axis. */
    run.tower.blocks[0].tell = 1;
    timber_view_block(&v, &run, 0, &t);
    check("a tell nudges the block along its axis by two pixels",
          t.top.x[0] == s.top.x[0] + TIMBER_VIEW_TELL_PX && t.top.y[0] == s.top.y[0] + TIMBER_VIEW_TELL_PX / 2);
    run.tower.blocks[0].tell = 0;

    /* Extraction moves the block along its axis in the same sense. */
    run.tower.blocks[0].extraction = TIMBER_UNIT;
    timber_view_block(&v, &run, 0, &t);
    check("a block drawn one width toward the player moves right and down by one scale",
          t.end_x == s.end_x + TIMBER_VIEW_SCALE && t.end_y == s.end_y + TIMBER_VIEW_SCALE / 2);
    run.tower.blocks[0].extraction = 0;

    /* Lean displaces a layer by lean times its height; the base not at all.
     * 26/256 widths a layer is a whole width ten layers up. */
    run.lean_x = 26 * 256;
    timber_view_block(&v, &run, 0, &t);
    check("the base does not lean", t.end_x == s.end_x && t.end_y == s.end_y);
    timber_view_block(&v, &run, 30, &s);
    run.lean_x = 0;
    timber_view_block(&v, &run, 30, &t);
    check("a block ten layers up leans a whole width over",
          s.end_x == t.end_x + TIMBER_VIEW_SCALE && s.end_y == t.end_y + TIMBER_VIEW_SCALE / 2);

    /* Sway displaces by the square of the height fraction, and not at all
     * with reduced motion. */
    timber_view_block(&v, &run, 51, &t);
    run.disturb_axis = TIMBER_AXIS_Y;
    run.sway = TIMBER_UNIT;
    timber_view_block(&v, &run, 51, &s);
    check("the top layer sways by the whole sway, along y",
          s.end_x == t.end_x - TIMBER_VIEW_SCALE && s.end_y == t.end_y + TIMBER_VIEW_SCALE / 2);
    /* Layer 9 of 17 is a little over half way: 81/289 of the sway, which
     * is 71/256 of a width, eleven pixels give or take the rounding. */
    timber_view_block(&v, &run, 27, &s);
    run.sway = 0;
    timber_view_block(&v, &run, 27, &t);
    check("a block half way up sways about a quarter of it",
          abs(s.end_x - (t.end_x - 11)) <= 1 && abs(s.end_y - (t.end_y + 5)) <= 1);
    run.sway = TIMBER_UNIT;
    timber_view_block(&v, &run, 0, &s);
    run.sway = 0;
    timber_view_block(&v, &run, 0, &t);
    check("the base does not sway", s.end_x == t.end_x && s.end_y == t.end_y);
    run.sway = TIMBER_UNIT;
    v.motion = 0;
    timber_view_block(&v, &run, 51, &s);
    run.sway = 0;
    timber_view_block(&v, &run, 51, &t);
    check("reduced motion draws no sway", s.end_x == t.end_x && s.end_y == t.end_y);
    v.motion = 1;
}

static void test_ghost_and_order(void)
{
    struct timber_view v;
    struct timber_run run;
    struct timber_shape s;
    int ids[TIMBER_BLOCKS];
    int n;
    int i;
    int ascending = 1;

    timber_view_init(&v, 528, 700, 1);
    timber_run_new(&run, 8u);
    timber_run_start(&run);
    check("no ghost with nothing in hand", timber_view_ghost(&v, &run, 1, &s) == -1);
    run.tower.blocks[4].seat = 255;
    timber_run_select(&run, 4);
    for (i = 0; i < 7; i++) {
        timber_run_tick(&run);
        timber_run_pull(&run, 100);
    }
    check("the block is in hand", timber_run_held(&run) == 4);
    /* Above the top layer's top face, allowing for the lean the pull's
     * shift left behind. */
    check("the ghost sits on the new layer",
          timber_view_ghost(&v, &run, 1, &s) == 0 && s.end_is_right &&
          s.top.y[0] < v.oy - (TIMBER_LAYERS_BASE - 1) * TIMBER_VIEW_LAYER_PX);
    check("a ghost in a closed slot is refused",
          timber_view_ghost(&v, &run, 3, &s) == -1 && timber_view_ghost(&v, &run, -1, &s) == -1);

    n = timber_view_order(&run, ids);
    check("every present block is drawn", n == TIMBER_BLOCKS - 1);
    for (i = 1; i < n; i++) {
        const struct timber_block *a = &run.tower.blocks[ids[i - 1]];
        const struct timber_block *b = &run.tower.blocks[ids[i]];

        /* Within a layer nearer slots come later; a higher layer never
         * comes before a lower one at the same footprint. */
        if (a->layer == b->layer) {
            ascending &= a->slot <= b->slot;
        }
    }
    check("blocks are drawn back to front", ascending && ids[0] == 0);
    check("NULL is safe", timber_view_order(NULL, ids) == 0 && timber_view_order(&run, NULL) == 0);
}

static void test_picking(void)
{
    struct timber_view v;
    struct timber_run run;
    struct timber_shape s;
    int id;
    int picked_all = 1;

    timber_view_init(&v, 528, 700, 1);
    timber_run_new(&run, 9u);
    timber_run_start(&run);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        if (!timber_tower_pullable(&run.tower, id)) {
            continue;
        }
        timber_view_block(&v, &run, id, &s);
        picked_all &= timber_view_pick(&v, &run, s.end_x, s.end_y) == id;
    }
    check("a tap on any pullable block's end picks that block", picked_all);
    timber_view_block(&v, &run, 51, &s);
    check("a tap on a locked block's end picks nothing",
          timber_view_pick(&v, &run, s.end_x, s.end_y) == -1);
    timber_view_block(&v, &run, 10, &s);
    check("a tap near an end still picks it",
          timber_view_pick(&v, &run, s.end_x + 12, s.end_y - 8) == 10);
    check("a tap on the felt picks nothing",
          timber_view_pick(&v, &run, 4, 4) == -1 && timber_view_pick(&v, &run, 520, 550) == -1);
    check("NULL is safe", timber_view_pick(NULL, &run, 0, 0) == -1 && timber_view_pick(&v, NULL, 0, 0) == -1);
}

static void test_track_and_sides(void)
{
    struct timber_view v;

    timber_view_init(&v, 528, 700, 1);
    check("an x block moves right when drawn toward the player, a y block left",
          timber_view_track_sign(0) == 1 && timber_view_track_sign(1) == -1);
    check("one scale of drag is one width of travel, in the block's sense",
          timber_view_travel(&v, 0, TIMBER_VIEW_SCALE) == TIMBER_UNIT &&
          timber_view_travel(&v, 1, TIMBER_VIEW_SCALE) == -TIMBER_UNIT &&
          timber_view_travel(&v, 0, -10) == -10 * 256 / TIMBER_VIEW_SCALE &&
          timber_view_travel(NULL, 0, 10) == 0);
    check("across an x layer slot 0 is the right side, across a y layer the left",
          timber_view_slot_side(0, 0) == 1 && timber_view_slot_side(0, 1) == 0 &&
          timber_view_slot_side(0, 2) == -1 && timber_view_slot_side(1, 0) == -1 &&
          timber_view_slot_side(1, 2) == 1 && timber_view_slot_side(0, 3) == 0);
    check("sides map back to slots",
          timber_view_slot_for_side(0, 1) == 0 && timber_view_slot_for_side(0, -1) == 2 &&
          timber_view_slot_for_side(1, -1) == 0 && timber_view_slot_for_side(1, 0) == 1 &&
          timber_view_slot_for_side(0, 2) == -1);
}

int main(void)
{
    test_projection();
    test_block_shape();
    test_ghost_and_order();
    test_picking();
    test_track_and_sides();

    printf("timber_view_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
