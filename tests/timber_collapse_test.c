/*
 * PocketTimber collapse test: the stump stays, the stack above the hinge
 * tips and then breaks, every block lands on the felt or the stump and
 * rests, the same state falls the same way twice, another seed falls
 * differently, and the ceiling puts down whatever is still moving.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_collapse.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void begin(struct timber_collapse *c, struct timber_tower *t, int hinge, int axis, int sign,
                  uint32_t seed)
{
    struct timber_rng rng;

    timber_tower_build(t);
    timber_rng_seed(&rng, seed);
    timber_collapse_begin(c, t, hinge, axis, sign, &rng);
}

/* Run a collapse to its end; returns the ticks it took, or -1 if it did
 * not end under the ceiling. */
static int play(struct timber_collapse *c)
{
    int ticks = 0;

    while (!timber_collapse_done(c) && ticks < TIMBER_COLLAPSE_TICKS_MAX + 5) {
        timber_collapse_tick(c);
        ticks++;
    }
    return timber_collapse_done(c) ? ticks : -1;
}

static void test_begin(void)
{
    struct timber_collapse c;
    struct timber_tower t;
    int id;
    int stump_stays = 1;
    int stack_falls = 1;
    int from_cells = 1;
    int drawn = 1;

    begin(&c, &t, 5, TIMBER_AXIS_X, 1, 1u);
    check("the collapse is on, at the hinge, along its axis",
          c.active && c.hinge == 5 && c.axis == TIMBER_AXIS_X && c.sign == 1 && c.ticks == 0);
    check("everything above the hinge falls and the stump does not",
          c.falling == TIMBER_BLOCKS - 3 * 6 && c.fell == c.falling);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_fall *f = timber_collapse_block(&c, id);
        struct timber_rect r;
        int32_t cx;
        int32_t cy;

        timber_block_rect(t.blocks[id].layer, t.blocks[id].slot, 0, &r);
        timber_rect_centre(&r, &cx, &cy);
        from_cells &= f && f->x == cx && f->y == cy &&
                      f->z == t.blocks[id].layer * TIMBER_BLOCK_HEIGHT &&
                      f->rest_tick == TIMBER_REST_NEVER;
        if (t.blocks[id].layer <= 5) {
            stump_stays &= !f->falling && f->vx == 0 && f->vy == 0 && f->vz == 0;
        } else {
            stack_falls &= f->falling && f->vx > 0 && f->vz == 0;
            drawn &= f->tumble >= TIMBER_TUMBLE_MIN && f->tumble <= TIMBER_TUMBLE_MAX &&
                     f->rest_pose < TIMBER_POSES;
        }
    }
    check("every block starts from its cell", from_cells);
    check("stump blocks have no motion", stump_stays);
    check("falling blocks are sent the way the hinge gave", stack_falls);
    check("each falling block has a tumble and a rest pose drawn", drawn);
    check("a block outside the run is no block",
          timber_collapse_block(&c, -1) == NULL && timber_collapse_block(&c, TIMBER_BLOCKS) == NULL);
    check("the floor is the stump over the footprint and the felt beyond it",
          timber_collapse_floor(&c, 384, 384) == 6 * TIMBER_BLOCK_HEIGHT &&
          timber_collapse_floor(&c, -1, 384) == 0 && timber_collapse_floor(&c, 384, 900) == 0);

    /* A block in hand does not fall, and a block part way out does. */
    timber_tower_build(&t);
    timber_tower_remove(&t, 40);
    t.blocks[31].extraction = 300;
    {
        struct timber_rng rng;

        timber_rng_seed(&rng, 2u);
        timber_collapse_begin(&c, &t, 5, TIMBER_AXIS_Y, -1, &rng);
    }
    check("a block in hand is not part of the fall",
          c.falling == TIMBER_BLOCKS - 18 - 1 && !timber_collapse_block(&c, 40)->falling);
    /* Block 31 is the centre of layer 10, an x layer: it runs along x. */
    check("a block part way out falls from where it was",
          timber_collapse_block(&c, 31)->falling && timber_collapse_block(&c, 31)->x == 384 + 300 &&
          timber_collapse_block(&c, 31)->y == 384 && timber_collapse_block(&c, 31)->layer == 10);
    check("the other way sends blocks the other way", timber_collapse_block(&c, 31)->vy < 0);
}

static void test_tip(void)
{
    struct timber_collapse c;
    struct timber_tower t;
    const struct timber_fall *low;
    const struct timber_fall *high;
    int i;

    int32_t low_x;
    int32_t low_y;
    int32_t high_x;
    int32_t high_y;

    begin(&c, &t, 5, TIMBER_AXIS_X, 1, 3u);
    low = timber_collapse_block(&c, 18);    /* layer 6, one above the hinge */
    high = timber_collapse_block(&c, 51);   /* layer 17, twelve above */
    low_x = low->x;
    low_y = low->y;
    high_x = high->x;
    high_y = high->y;
    for (i = 0; i < TIMBER_TIP_TICKS; i++) {
        check("nobody lands during the tip", timber_collapse_tick(&c) == 0);
    }
    check("during the tip each layer slides further than the one below, and drops a touch",
          low->x == low_x + TIMBER_TIP_SHEAR * 1 * TIMBER_TIP_TICKS &&
          high->x == high_x + TIMBER_TIP_SHEAR * 12 * TIMBER_TIP_TICKS &&
          low->y == low_y && high->y == high_y &&
          low->z == 6 * TIMBER_BLOCK_HEIGHT - TIMBER_TIP_DROP * TIMBER_TIP_TICKS &&
          high->z == 17 * TIMBER_BLOCK_HEIGHT - TIMBER_TIP_DROP * TIMBER_TIP_TICKS);
    check("the stump has not moved",
          timber_collapse_block(&c, 16)->x == 384 && timber_collapse_block(&c, 16)->y == 384 &&
          timber_collapse_block(&c, 0)->z == 0);
    timber_collapse_tick(&c);
    check("then the stack breaks and the blocks fly",
          high->x > high_x + TIMBER_TIP_SHEAR * 12 * TIMBER_TIP_TICKS && high->vz < 0);
}

static void test_landing(void)
{
    struct timber_collapse c;
    struct timber_tower t;
    int ticks;
    int id;
    int at_rest = 1;
    int on_a_floor = 1;
    int on_stump = 0;
    int on_felt = 0;
    int bounced = 0;
    int poses = 1;

    begin(&c, &t, 5, TIMBER_AXIS_X, 1, 4u);
    ticks = play(&c);
    printf("     hinge 5, +x: %d blocks rested in %d ticks\n", c.fell, ticks);
    check("every block comes to rest under the ceiling",
          ticks > TIMBER_TIP_TICKS && ticks < TIMBER_COLLAPSE_TICKS_MAX && c.falling == 0 &&
          timber_collapse_done(&c));
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_fall *f = timber_collapse_block(&c, id);

        if (t.blocks[id].layer <= 5) {
            continue;
        }
        at_rest &= !f->falling && f->vx == 0 && f->vy == 0 && f->vz == 0 &&
                   f->rest_tick != TIMBER_REST_NEVER && f->rest_tick <= c.ticks;
        on_a_floor &= f->z == timber_collapse_floor(&c, f->x, f->y);
        on_stump |= f->z == 6 * TIMBER_BLOCK_HEIGHT;
        on_felt |= f->z == 0;
        bounced |= f->bounced;
        poses &= f->pose == f->rest_pose && f->pose < TIMBER_POSES;
    }
    check("every fallen block is at rest with no speed left", at_rest);
    check("every fallen block sits on the felt or on the stump, never below", on_a_floor);
    check("some land on the stump and some on the felt", on_stump && on_felt);
    check("at least one bounced on the way", bounced);
    check("a rested block holds its rest pose", poses);
    check("the stump is where it was",
          timber_collapse_block(&c, 17)->x == 384 + 256 && timber_collapse_block(&c, 17)->z == 5 * TIMBER_BLOCK_HEIGHT);
    check("ticking a finished collapse changes nothing",
          timber_collapse_tick(&c) == 0 && c.ticks == (uint16_t)ticks);
}

static void test_determinism_and_variation(void)
{
    struct timber_collapse a;
    struct timber_collapse b;
    struct timber_tower t;
    int ticks_a;
    int ticks_b;

    begin(&a, &t, 8, TIMBER_AXIS_Y, -1, 77u);
    begin(&b, &t, 8, TIMBER_AXIS_Y, -1, 77u);
    ticks_a = play(&a);
    ticks_b = play(&b);
    check("the same state falls the same way twice",
          ticks_a == ticks_b && ticks_a > 0 && memcmp(&a, &b, sizeof(a)) == 0);

    begin(&b, &t, 8, TIMBER_AXIS_Y, -1, 78u);
    play(&b);
    check("another seed scatters differently", memcmp(&a, &b, sizeof(a)) != 0);

    begin(&b, &t, 12, TIMBER_AXIS_Y, -1, 77u);
    play(&b);
    check("a higher hinge drops fewer blocks", b.fell < a.fell && b.fell == 3 * 5);

    begin(&a, &t, 8, TIMBER_AXIS_X, 1, 77u);
    begin(&b, &t, 8, TIMBER_AXIS_X, -1, 77u);
    play(&a);
    play(&b);
    check("the direction the hinge gave decides which way the blocks go",
          timber_collapse_block(&a, 53)->x > 384 && timber_collapse_block(&b, 53)->x < 384);

    begin(&a, &t, 16, TIMBER_AXIS_X, 1, 5u);
    ticks_a = play(&a);
    check("a top layer alone falls quickly", a.fell == 3 && ticks_a > TIMBER_TIP_TICKS && ticks_a < 30);
}

static void test_ceiling(void)
{
    struct timber_collapse c;
    struct timber_tower t;
    struct timber_fall *f;
    int ticks;

    begin(&c, &t, 5, TIMBER_AXIS_X, 1, 9u);
    /* A block launched upward hard enough would still be in the air past
     * the ceiling: the ceiling puts it down where it is. */
    f = &c.blocks[53];
    f->vz = 2000;
    ticks = play(&c);
    check("the ceiling ends a collapse that would not end on its own",
          ticks == TIMBER_COLLAPSE_TICKS_MAX && timber_collapse_done(&c) && !f->falling &&
          f->rest_tick == TIMBER_COLLAPSE_TICKS_MAX && f->z == timber_collapse_floor(&c, f->x, f->y));
}

static void test_null_is_safe(void)
{
    struct timber_collapse c;
    struct timber_tower t;

    timber_tower_build(&t);
    timber_collapse_begin(NULL, &t, 0, 0, 1, NULL);
    timber_collapse_begin(&c, NULL, 0, 0, 1, NULL);
    check("a collapse begun without a tower is inactive and done",
          !c.active && timber_collapse_done(&c) && timber_collapse_tick(&c) == 0 &&
          timber_collapse_block(&c, 0) == NULL && timber_collapse_floor(&c, 0, 0) == 0);
    timber_collapse_begin(&c, &t, TIMBER_LAYERS_MAX, 0, 1, NULL);
    check("a hinge outside the tower is refused", !c.active);
    check("NULL is done", timber_collapse_done(NULL) && timber_collapse_tick(NULL) == 0 &&
          timber_collapse_block(NULL, 0) == NULL && timber_collapse_floor(NULL, 0, 0) == 0);
    /* No generator: every draw is the low end of its range, so the scatter
     * is all one way and the tumble the quickest; it still falls. */
    timber_collapse_begin(&c, &t, 5, TIMBER_AXIS_X, 1, NULL);
    check("without a generator the fall is plain but still falls",
          c.active && c.falling == 36 && c.blocks[53].vy == -TIMBER_SCATTER &&
          c.blocks[53].vx == TIMBER_BREAK_SPEED + 12 * TIMBER_BREAK_SPEED_PER_LAYER - TIMBER_SCATTER &&
          c.blocks[53].tumble == TIMBER_TUMBLE_MIN && play(&c) > 0);
}

int main(void)
{
    test_begin();
    test_tip();
    test_landing();
    test_determinism_and_variation();
    test_ceiling();
    test_null_is_safe();

    printf("timber_collapse_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
