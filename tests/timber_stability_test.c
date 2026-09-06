/*
 * PocketTimber stability test: contacts and their bounding box, the stack
 * above a layer with its lean, the margins the design review worked out by
 * hand, the support threshold, the sway share and the sine, and the hinge.
 *
 * The margins are the numbers the game is built on: a complete layer under
 * a centred stack has 1.5 widths, a layer with only its centre block 0.5,
 * a layer with one side block none. They are asserted here exactly.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_stability.h"

#include <stdio.h>
#include <stdlib.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static int near(int32_t a, int32_t b, int32_t tolerance)
{
    return abs((int)(a - b)) <= tolerance;
}

static void test_canonical(void)
{
    struct timber_tower t;
    struct timber_contact c;
    struct timber_stack s;
    struct timber_margin m;
    int layer;
    int full = 1;
    int stacks = 1;
    int margins = 1;

    timber_tower_build(&t);
    for (layer = 0; layer < TIMBER_LAYERS_BASE - 1; layer++) {
        full &= timber_stability_contact(&t, layer, &c) == 0 && c.supported == 3 && c.above == 3 &&
                c.box.x0 == 0 && c.box.y0 == 0 && c.box.x1 == TIMBER_BLOCK_LENGTH &&
                c.box.y1 == TIMBER_BLOCK_LENGTH;
        stacks &= timber_stability_stack(&t, layer, 0, 0, &s) == 0 &&
                  s.blocks == TIMBER_BLOCKS - 3 * (layer + 1) &&
                  s.mass == (TIMBER_BLOCKS - 3 * (layer + 1)) * TIMBER_MASS_ONE &&
                  s.cx == 3 * TIMBER_UNIT / 2 && s.cy == 3 * TIMBER_UNIT / 2;
        margins &= timber_stability_margin(&t, layer, 0, 0, 0, 0, &m) == 0 &&
                   m.stat == TIMBER_MARGIN_FULL && m.eff == TIMBER_MARGIN_FULL;
    }
    check("every complete layer meets the layer above over the whole footprint", full);
    check("the stack above each layer is everything above it, centred", stacks);
    check("a complete layer under a centred stack has a margin of one and a half widths",
          margins && TIMBER_MARGIN_FULL == 384);
    check("the top layer has nothing above it",
          timber_stability_contact(&t, TIMBER_LAYERS_BASE - 1, &c) == -1 &&
          timber_stability_stack(&t, TIMBER_LAYERS_BASE - 1, 0, 0, &s) == -1 &&
          timber_stability_margin(&t, TIMBER_LAYERS_BASE - 1, 0, 0, 0, 0, &m) == -1);
    check("every block below the top carries load, no block in the top does",
          timber_stability_block_supports(&t, 0) && timber_stability_block_supports(&t, 50) &&
          !timber_stability_block_supports(&t, 51) && !timber_stability_block_supports(&t, 53));
    check("the hinge of a perfect tower is its lowest layer, on a tie",
          timber_stability_hinge(&t, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_MARGIN_FULL);
}

static void test_thinned_layers(void)
{
    struct timber_tower t;
    struct timber_contact c;
    struct timber_margin m;

    /* Layer 5 is a y layer: its slots are x positions. */
    timber_tower_build(&t);
    timber_tower_remove(&t, 16);
    check("without its centre block a layer still spans the footprint",
          timber_stability_contact(&t, 5, &c) == 0 && c.supported == 2 &&
          c.box.x0 == 0 && c.box.x1 == TIMBER_BLOCK_LENGTH &&
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_MARGIN_FULL);

    timber_tower_build(&t);
    timber_tower_remove(&t, 15);
    check("without one side block the margin is half a width toward that side",
          timber_stability_contact(&t, 5, &c) == 0 && c.supported == 2 &&
          c.box.x0 == TIMBER_UNIT && c.box.x1 == TIMBER_BLOCK_LENGTH &&
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2 &&
          m.axis == TIMBER_AXIS_X && m.sign == -1);

    timber_tower_build(&t);
    timber_tower_remove(&t, 15);
    timber_tower_remove(&t, 17);
    check("with only its centre block a layer has half a width either way",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2 &&
          c.above == 3);

    timber_tower_build(&t);
    timber_tower_remove(&t, 15);
    timber_tower_remove(&t, 16);
    check("with only one side block the stack has tipped",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == -TIMBER_UNIT / 2 &&
          m.axis == TIMBER_AXIS_X && m.sign == -1 && m.supported == 1);
    check("and that layer is the hinge",
          timber_stability_hinge(&t, 0, 0, 0, 0, &m) == 5 && m.stat == -TIMBER_UNIT / 2);

    /* A thinned layer and the layer under it share a margin: the stack
     * above either rests on the same single block. The thinned layer is
     * the hinge, because fewer of its blocks carry the stack. */
    timber_tower_build(&t);
    timber_tower_remove(&t, 15);
    timber_tower_remove(&t, 17);
    check("the layer under a thinned layer has the same margin",
          timber_stability_margin(&t, 4, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2 &&
          m.supported == 3);
    check("a tie goes to the layer with fewer blocks carrying the stack",
          timber_stability_hinge(&t, 0, 0, 0, 0, &m) == 5 && m.supported == 1);

    timber_tower_build(&t);
    timber_tower_remove(&t, 15);
    timber_tower_remove(&t, 16);
    timber_tower_remove(&t, 17);
    check("with nothing left in a layer the stack has nothing to stand on",
          timber_stability_contact(&t, 5, &c) == 0 && c.supported == 0 &&
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_MARGIN_NONE &&
          timber_stability_hinge(&t, 0, 0, 0, 0, &m) == 5);

    /* The other orientation: layer 4 is an x layer, slots are y. */
    timber_tower_build(&t);
    timber_tower_remove(&t, 14);
    check("the same rule holds along y",
          timber_stability_margin(&t, 4, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2 &&
          m.axis == TIMBER_AXIS_Y && m.sign == 1);
}

static void test_extraction(void)
{
    struct timber_tower t;
    struct timber_margin m;

    /* Block 16 is the centre of layer 5 (a y layer, so it runs along y).
     * Drawn part way out along +y, its contact with the layer above shrinks
     * from the low-y end. With the side blocks present nothing changes. */
    timber_tower_build(&t);
    t.blocks[16].extraction = 500;
    check("a block part way out changes nothing while its neighbours span the footprint",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_MARGIN_FULL &&
          timber_stability_block_supports(&t, 16));

    timber_tower_remove(&t, 15);
    timber_tower_remove(&t, 17);
    t.blocks[16].extraction = 0;
    check("alone and seated, the centre block gives half a width",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2);
    t.blocks[16].extraction = 200;
    check("alone and part way out, the across margin still rules",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_UNIT / 2 &&
          m.axis == TIMBER_AXIS_X);
    t.blocks[16].extraction = 300;
    check("further out, the along margin takes over: 384 - 300",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == 84 &&
          m.axis == TIMBER_AXIS_Y && m.sign == -1);
    t.blocks[16].extraction = 500;
    check("and the stack tips before the block is out",
          timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat < 0);

    /* The support threshold: a quarter of the footprint. */
    timber_tower_build(&t);
    t.blocks[16].extraction = 576;
    check("at three quarters out a block still just carries load",
          timber_stability_block_supports(&t, 16));
    t.blocks[16].extraction = 577;
    check("past three quarters it has let go",
          !timber_stability_block_supports(&t, 16));
    check("a block that has let go is not counted in the contact",
          (timber_tower_remove(&t, 15), timber_tower_remove(&t, 17),
           timber_stability_margin(&t, 5, 0, 0, 0, 0, &m) == 0 && m.stat == TIMBER_MARGIN_NONE));
    t.blocks[16].extraction = -577;
    check("the threshold holds the other way too", !timber_stability_block_supports(&t, 16));
    check("a block in hand carries nothing",
          (timber_tower_remove(&t, 16), !timber_stability_block_supports(&t, 16)));
}

static void test_lean(void)
{
    struct timber_tower t;
    struct timber_stack s;
    struct timber_margin m;
    int32_t lean = 3277; /* 0.05 widths per layer, Q16.16 */

    timber_tower_build(&t);
    /* The stack above layer 0 is layers 1..17, mean 9 layers up: 0.05 * 9
     * = 0.45 widths = 115 over. */
    check("lean moves the centre of mass of the stack above a layer",
          timber_stability_stack(&t, 0, lean, 0, &s) == 0 && near(s.cx, 384 + 115, 2) && s.cy == 384);
    check("and eats the margin on the side it leans toward",
          timber_stability_margin(&t, 0, lean, 0, 0, 0, &m) == 0 && near(m.stat, 384 - 115, 2) &&
          m.axis == TIMBER_AXIS_X && m.sign == 1);
    check("a lean the other way eats the other side",
          timber_stability_margin(&t, 0, -lean, 0, 0, 0, &m) == 0 && near(m.stat, 384 - 115, 2) &&
          m.sign == -1);
    check("lean along y works the same",
          timber_stability_margin(&t, 0, 0, lean, 0, 0, &m) == 0 && near(m.stat, 384 - 115, 2) &&
          m.axis == TIMBER_AXIS_Y);
    /* Higher up there is less stack to lean: above layer 16 only layer 17,
     * one layer up, 0.05 widths = 13. */
    check("a high layer feels little of the lean",
          timber_stability_margin(&t, 16, lean, 0, 0, 0, &m) == 0 && near(m.stat, 384 - 13, 2));
    check("the hinge of a leaning tower is its base",
          timber_stability_hinge(&t, lean, 0, 0, 0, &m) == 0);
}

static void test_offsets(void)
{
    struct timber_tower t;
    struct timber_stack s;

    timber_tower_build(&t);
    /* Block 3 is in layer 1, a y layer: its offset is across, along x. */
    t.blocks[3].offset = 15;
    check("a micro-offset moves a block across its axis",
          timber_stability_stack(&t, 0, 0, 0, &s) == 0 && s.cx == 384 + 15 / (TIMBER_BLOCKS - 3) &&
          s.cy == 384);
    t.blocks[3].offset = 0;
    t.blocks[0].offset = -15;
    check("a block's own offset is not part of the stack above its layer",
          timber_stability_stack(&t, 0, 0, 0, &s) == 0 && s.cx == 384 && s.cy == 384);
    t.blocks[0].mass = 0;
    check("a heavier block pulls the centre of mass toward itself",
          (t.blocks[0].mass = TIMBER_MASS_ONE, t.blocks[6].mass = 4 * TIMBER_MASS_ONE,
           timber_stability_stack(&t, 1, 0, 0, &s) == 0 && s.cy < 384 && s.cx == 384));
}

static void test_sway(void)
{
    struct timber_margin m;
    struct timber_tower t;
    int positive = 1;
    int layer;
    int peak = 0;
    int32_t peak_share = 0;

    check("the sine table is a sine",
          timber_stability_sin(0) == 0 && timber_stability_sin(16384) == 32767 &&
          timber_stability_sin(32768) == 0 && timber_stability_sin(49152) == -32767 &&
          near(timber_stability_sin(8192), 23170, 2) && near(timber_stability_sin(57344), -23170, 2));
    check("the share at the base is about a third", near(timber_stability_sway_share(0, 17), 93, 1));
    check("the share just under the top is small", near(timber_stability_sway_share(16, 17), 29, 1));
    check("nothing above means no share",
          timber_stability_sway_share(17, 17) == 0 && timber_stability_sway_share(0, 0) == 0 &&
          timber_stability_sway_share(5, 3) == 0);
    /* The share is the mean displacement of the stack above a layer, so it
     * rises a little at first (the barely-moving low layers drop out of the
     * mean), peaks in the lower middle, and falls toward the top. */
    for (layer = 0; layer < 17; layer++) {
        int32_t share = timber_stability_sway_share(layer, 17);

        positive &= share > 0;
        if (share > peak_share) {
            peak_share = share;
            peak = layer;
        }
    }
    check("every carrying layer feels some of the sway", positive);
    check("the share peaks in the lower middle of the tower",
          peak >= 3 && peak <= 6 && peak_share > 93 && peak_share < 110);
    check("and falls from there to the top",
          timber_stability_sway_share(16, 17) < timber_stability_sway_share(12, 17) &&
          timber_stability_sway_share(12, 17) < timber_stability_sway_share(8, 17) &&
          timber_stability_sway_share(8, 17) < peak_share);

    timber_tower_build(&t);
    check("a sway at the top displaces the base's stack by its share: 100 * 93 / 256",
          timber_stability_margin(&t, 0, 0, 0, 100, 0, &m) == 0 && m.stat == TIMBER_MARGIN_FULL &&
          near(m.eff, TIMBER_MARGIN_FULL - 36, 1) && m.axis == TIMBER_AXIS_X && m.sign == 1);
    check("the sway is signed",
          timber_stability_margin(&t, 0, 0, 0, -100, 0, &m) == 0 && near(m.eff, TIMBER_MARGIN_FULL - 36, 1) &&
          m.sign == -1);
    check("the sway and the lean add up",
          timber_stability_margin(&t, 0, 3277, 0, 100, 0, &m) == 0 && near(m.stat, 384 - 115, 2) &&
          near(m.eff, 384 - 115 - 36, 3));
    check("a sway against the lean gives margin back",
          timber_stability_margin(&t, 0, 3277, 0, -100, 0, &m) == 0 && near(m.eff, 384 - 115 + 36, 3));
    check("the hinge under a sway is the layer that feels most of it",
          timber_stability_hinge(&t, 0, 0, 0, 100, &m) == peak &&
          near(m.eff, TIMBER_MARGIN_FULL - 100 * peak_share / 256, 1) && m.stat == TIMBER_MARGIN_FULL);
}

static void test_null_is_safe(void)
{
    struct timber_tower t;
    struct timber_contact c;
    struct timber_stack s;
    struct timber_margin m;

    timber_tower_build(&t);
    check("NULL and bad layers are safe",
          !timber_stability_block_supports(NULL, 0) && !timber_stability_block_supports(&t, -1) &&
          timber_stability_contact(NULL, 0, &c) == -1 && timber_stability_contact(&t, -1, &c) == -1 &&
          timber_stability_contact(&t, 0, NULL) == -1 &&
          timber_stability_stack(NULL, 0, 0, 0, &s) == -1 && timber_stability_stack(&t, 99, 0, 0, &s) == -1 &&
          timber_stability_margin(NULL, 0, 0, 0, 0, 0, &m) == -1 &&
          timber_stability_margin(&t, 0, 0, 0, 0, 0, NULL) == -1 &&
          timber_stability_hinge(NULL, 0, 0, 0, 0, &m) == -1);
    timber_tower_remove(&t, 51);
    timber_tower_remove(&t, 52);
    timber_tower_remove(&t, 53);
    check("a hinge exists as long as some layer carries another",
          timber_stability_hinge(&t, 0, 0, 0, 0, &m) == 0);
    t.layers = 1;
    check("a single layer has no hinge", timber_stability_hinge(&t, 0, 0, 0, 0, &m) == -1);
}

int main(void)
{
    test_canonical();
    test_thinned_layers();
    test_extraction();
    test_lean();
    test_offsets();
    test_sway();
    test_null_is_safe();

    printf("timber_stability_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
