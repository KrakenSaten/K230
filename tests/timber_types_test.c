/*
 * PocketTimber vocabulary test: the name tables, layer axes, block
 * footprints under extraction, and the rectangle arithmetic the stability
 * model is built from.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_types.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_names(void)
{
    int i;
    int named = 1;

    for (i = 0; i < TIMBER_CLASS_COUNT; i++) {
        named &= strcmp(timber_class_name((enum timber_class)i), "?") != 0;
    }
    for (i = 0; i < TIMBER_RUN_STATE_COUNT; i++) {
        named &= strcmp(timber_run_state_name((enum timber_run_state)i), "?") != 0;
    }
    for (i = 0; i < TIMBER_TURN_COUNT; i++) {
        named &= strcmp(timber_turn_name((enum timber_turn)i), "?") != 0;
    }
    for (i = 0; i < TIMBER_CAUSE_COUNT; i++) {
        named &= strcmp(timber_cause_name((enum timber_cause)i), "?") != 0;
    }
    for (i = 0; i < TIMBER_EVENT_COUNT; i++) {
        named &= strcmp(timber_event_name((enum timber_event_type)i), "?") != 0;
    }
    check("every enum value has a name", named);
    check("class names read as the player sees them",
          strcmp(timber_class_name(TIMBER_CLASS_FREE), "FREE") == 0 &&
          strcmp(timber_class_name(TIMBER_CLASS_STUCK), "STUCK") == 0);
    check("out-of-range values are '?'",
          strcmp(timber_class_name(TIMBER_CLASS_COUNT), "?") == 0 &&
          strcmp(timber_run_state_name((enum timber_run_state)99), "?") == 0 &&
          strcmp(timber_turn_name((enum timber_turn)-1), "?") == 0 &&
          strcmp(timber_cause_name(TIMBER_CAUSE_COUNT), "?") == 0 &&
          strcmp(timber_event_name(TIMBER_EVENT_COUNT), "?") == 0);
}

static void test_constants(void)
{
    check("a block is three widths long", TIMBER_BLOCK_LENGTH == 3 * TIMBER_UNIT);
    check("the base tower holds every block", TIMBER_BLOCKS == TIMBER_LAYERS_BASE * TIMBER_SLOTS);
    check("the layer bound covers every block restacked", TIMBER_LAYERS_MAX == 36);
    check("no block id collides with the empty cell marker", TIMBER_BLOCKS < TIMBER_NO_BLOCK);
    check("the tick is a whole number of milliseconds under 100",
          TIMBER_TICK_MS > 0 && TIMBER_TICK_MS < 100);
}

static void test_axes(void)
{
    int alternates = 1;
    int layer;

    for (layer = 0; layer < TIMBER_LAYERS_MAX; layer++) {
        alternates &= timber_layer_axis(layer) == ((layer & 1) ? TIMBER_AXIS_Y : TIMBER_AXIS_X);
    }
    check("layers alternate axis from x at the felt",
          alternates && timber_layer_axis(0) == TIMBER_AXIS_X);
}

static void test_footprints(void)
{
    struct timber_rect r;

    check("a seated x-layer block spans three widths in x and one in y",
          timber_block_rect(0, 0, 0, &r) == 0 &&
          r.x0 == 0 && r.x1 == TIMBER_BLOCK_LENGTH && r.y0 == 0 && r.y1 == TIMBER_UNIT);
    check("slot picks the across position",
          timber_block_rect(0, 2, 0, &r) == 0 && r.y0 == 2 * TIMBER_UNIT && r.y1 == 3 * TIMBER_UNIT);
    check("a y-layer block is the same shape turned",
          timber_block_rect(1, 2, 0, &r) == 0 &&
          r.x0 == 2 * TIMBER_UNIT && r.x1 == 3 * TIMBER_UNIT && r.y0 == 0 &&
          r.y1 == TIMBER_BLOCK_LENGTH);
    check("extraction moves an x block along x only",
          timber_block_rect(0, 1, 100, &r) == 0 &&
          r.x0 == 100 && r.x1 == 100 + TIMBER_BLOCK_LENGTH && r.y0 == TIMBER_UNIT &&
          r.y1 == 2 * TIMBER_UNIT);
    check("a negative extraction moves it the other way",
          timber_block_rect(0, 1, -100, &r) == 0 && r.x0 == -100 &&
          r.x1 == TIMBER_BLOCK_LENGTH - 100);
    check("extraction moves a y block along y only",
          timber_block_rect(3, 0, 200, &r) == 0 &&
          r.y0 == 200 && r.y1 == 200 + TIMBER_BLOCK_LENGTH && r.x0 == 0 && r.x1 == TIMBER_UNIT);
    check("a full extraction is accepted",
          timber_block_rect(0, 0, TIMBER_EXTRACTION_MAX, &r) == 0 && r.x0 == TIMBER_BLOCK_LENGTH);
    check("bad arguments are refused",
          timber_block_rect(-1, 0, 0, &r) == -1 &&
          timber_block_rect(TIMBER_LAYERS_MAX, 0, 0, &r) == -1 &&
          timber_block_rect(0, 3, 0, &r) == -1 &&
          timber_block_rect(0, 0, TIMBER_EXTRACTION_MAX + 1, &r) == -1 &&
          timber_block_rect(0, 0, -TIMBER_EXTRACTION_MAX - 1, &r) == -1 &&
          timber_block_rect(0, 0, 0, NULL) == -1);
}

static void test_rect_arithmetic(void)
{
    struct timber_rect below;
    struct timber_rect above;
    struct timber_rect cut;
    int32_t cx;
    int32_t cy;
    int slot;
    int each = 1;

    timber_block_rect(0, 0, 0, &below);
    check("a block's footprint area is three square widths",
          timber_rect_area(&below) == (int64_t)3 * TIMBER_UNIT * TIMBER_UNIT);

    /* A seated block touches each of the three blocks crossing it above
     * over one square width. */
    for (slot = 0; slot < TIMBER_SLOTS; slot++) {
        timber_block_rect(1, slot, 0, &above);
        each &= timber_rect_overlap(&below, &above) == (int64_t)TIMBER_UNIT * TIMBER_UNIT;
        each &= timber_rect_overlap(&above, &below) == (int64_t)TIMBER_UNIT * TIMBER_UNIT;
    }
    check("a seated block meets each crossing block over one square width", each);

    /* Pulled two widths out, it still meets the block over its last width
     * and none of the others. */
    timber_block_rect(0, 0, 2 * TIMBER_UNIT, &below);
    timber_block_rect(1, 0, 0, &above);
    check("two widths out, the first crossing block is no longer met",
          timber_rect_overlap(&below, &above) == 0);
    timber_block_rect(1, 2, 0, &above);
    check("two widths out, the last crossing block is still fully met",
          timber_rect_overlap(&below, &above) == (int64_t)TIMBER_UNIT * TIMBER_UNIT);
    check("the intersection is the shared square",
          timber_rect_intersect(&below, &above, &cut) == 1 &&
          cut.x0 == 2 * TIMBER_UNIT && cut.x1 == 3 * TIMBER_UNIT && cut.y0 == 0 &&
          cut.y1 == TIMBER_UNIT);

    timber_block_rect(0, 0, TIMBER_EXTRACTION_MAX, &below);
    check("fully out, nothing is met",
          timber_rect_overlap(&below, &above) == 0 &&
          timber_rect_intersect(&below, &above, &cut) == 0);

    /* Half a width out meets the edge block over half a square. */
    timber_block_rect(0, 0, TIMBER_UNIT / 2, &below);
    timber_block_rect(1, 0, 0, &above);
    check("half a width out halves the contact with the edge block",
          timber_rect_overlap(&below, &above) == (int64_t)TIMBER_UNIT * TIMBER_UNIT / 2);

    cut.x0 = 10;
    cut.x1 = 5;
    cut.y0 = 0;
    cut.y1 = 5;
    check("an inverted rectangle has no area", timber_rect_area(&cut) == 0);

    timber_block_rect(0, 1, -TIMBER_UNIT, &below);
    timber_rect_centre(&below, &cx, &cy);
    check("the centre follows extraction and slot",
          cx == TIMBER_UNIT / 2 && cy == TIMBER_UNIT + TIMBER_UNIT / 2);

    check("NULL rectangles are safe",
          timber_rect_area(NULL) == 0 && timber_rect_overlap(NULL, &above) == 0 &&
          timber_rect_intersect(&above, NULL, &cut) == 0);
    timber_rect_centre(NULL, &cx, &cy);
    timber_rect_centre(&above, NULL, NULL);
}

int main(void)
{
    test_names();
    test_constants();
    test_axes();
    test_footprints();
    test_rect_arithmetic();

    printf("timber_types_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
