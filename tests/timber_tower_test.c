/*
 * PocketTimber tower test: the canonical build, the grid and the blocks
 * agreeing about where everything is, the locked-layers rule, removing and
 * placing blocks, and the validator refusing every impossible state the
 * rules could never produce.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_tower.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_build(void)
{
    struct timber_tower t;
    int id;
    int consistent = 1;
    int layer;
    int full = 1;

    timber_tower_build(&t);
    check("the canonical tower holds every block",
          timber_tower_present_count(&t) == TIMBER_BLOCKS);
    check("it is the base number of layers tall", timber_tower_layers(&t) == TIMBER_LAYERS_BASE);
    for (layer = 0; layer < TIMBER_LAYERS_BASE; layer++) {
        full &= timber_tower_layer_fill(&t, layer) == TIMBER_SLOTS;
    }
    check("every layer is complete", full);
    check("layers above the tower are empty",
          timber_tower_layer_fill(&t, TIMBER_LAYERS_BASE) == 0 &&
          timber_tower_layer_fill(&t, -1) == 0 &&
          timber_tower_layer_fill(&t, TIMBER_LAYERS_MAX) == 0);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = timber_tower_block(&t, id);

        consistent &= b && b->present && b->layer == id / TIMBER_SLOTS &&
                      b->slot == id % TIMBER_SLOTS;
        consistent &= timber_tower_at(&t, id / TIMBER_SLOTS, id % TIMBER_SLOTS) == id;
        consistent &= b->extraction == 0 && b->seat == TIMBER_SEAT_DEFAULT &&
                      b->mass == TIMBER_MASS_ONE;
        consistent &= b->tell == 0 && b->tested == 0 && b->moves == 0 && b->offset == 0;
    }
    check("block id is layer times slots plus slot, and both views agree", consistent);
    check("the top complete layer is the highest one",
          timber_tower_top_complete(&t) == TIMBER_LAYERS_BASE - 1);
    check("the canonical tower validates", timber_tower_validate(&t) == TIMBER_VALID);
    check("it has no gap", timber_tower_gap(&t) == -1);
    check("cells outside the tower are empty",
          timber_tower_at(&t, -1, 0) == -1 && timber_tower_at(&t, 0, 3) == -1 &&
          timber_tower_at(&t, TIMBER_LAYERS_MAX, 0) == -1);
    check("ids outside the run are no block",
          timber_tower_block(&t, -1) == NULL && timber_tower_block(&t, TIMBER_BLOCKS) == NULL);
}

static void test_pullable(void)
{
    struct timber_tower t;
    int id;
    int rule = 1;
    struct timber_rect r;

    timber_tower_build(&t);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        int want = id / TIMBER_SLOTS < TIMBER_LAYERS_BASE - 1;

        rule &= timber_tower_pullable(&t, id) == want;
    }
    check("every block below the top layer is pullable and the top layer is locked", rule);
    check("the bottom layer is pullable: what makes it dangerous is not a rule",
          timber_tower_pullable(&t, 0) && timber_tower_pullable(&t, 1) &&
          timber_tower_pullable(&t, 2));
    check("an id outside the run is not pullable",
          !timber_tower_pullable(&t, -1) && !timber_tower_pullable(&t, TIMBER_BLOCKS));

    check("a present block reports its footprint",
          timber_tower_rect(&t, 4, &r) == 0 && r.x0 == TIMBER_UNIT && r.x1 == 2 * TIMBER_UNIT &&
          r.y0 == 0 && r.y1 == TIMBER_BLOCK_LENGTH);
    t.blocks[4].extraction = 50;
    check("the footprint follows extraction",
          timber_tower_rect(&t, 4, &r) == 0 && r.y0 == 50 && r.y1 == TIMBER_BLOCK_LENGTH + 50);
    timber_tower_remove(&t, 4);
    check("an absent block has no footprint", timber_tower_rect(&t, 4, &r) == -1);
}

static void test_remove_and_place(void)
{
    struct timber_tower t;
    int id;

    timber_tower_build(&t);

    t.blocks[7].present = 0;
    check("removing an absent block is refused", timber_tower_remove(&t, 7) == -1);
    t.blocks[7].present = 1;
    check("removing an id outside the run is refused",
          timber_tower_remove(&t, -1) == -1 && timber_tower_remove(&t, TIMBER_BLOCKS) == -1);

    t.blocks[7].extraction = 600;
    check("a removed block leaves its cell, loses its extraction and stays valid",
          timber_tower_remove(&t, 7) == 0 && !t.blocks[7].present &&
          t.blocks[7].extraction == 0 && timber_tower_at(&t, 2, 1) == -1 &&
          timber_tower_present_count(&t) == TIMBER_BLOCKS - 1 &&
          timber_tower_validate(&t) == TIMBER_VALID);
    check("a removed block is not pullable", !timber_tower_pullable(&t, 7));
    check("a block in hand goes to a new layer on top",
          timber_tower_place_layer(&t) == TIMBER_LAYERS_BASE);
    check("every slot of the new layer is open",
          timber_tower_can_place(&t, 0) && timber_tower_can_place(&t, 1) &&
          timber_tower_can_place(&t, 2));
    check("slots outside the layer are not",
          !timber_tower_can_place(&t, -1) && !timber_tower_can_place(&t, TIMBER_SLOTS));
    check("placing a present block is refused", timber_tower_place(&t, 8, 1) == -1);
    check("placing on a closed slot is refused", timber_tower_place(&t, 7, 3) == -1);

    check("placing opens a new layer",
          timber_tower_place(&t, 7, 1) == 0 && timber_tower_layers(&t) == TIMBER_LAYERS_BASE + 1 &&
          timber_tower_at(&t, TIMBER_LAYERS_BASE, 1) == 7 && t.blocks[7].present &&
          t.blocks[7].layer == TIMBER_LAYERS_BASE && t.blocks[7].slot == 1 &&
          t.blocks[7].moves == 1 && timber_tower_validate(&t) == TIMBER_VALID);
    check("the placed slot is now closed",
          !timber_tower_can_place(&t, 1) && timber_tower_can_place(&t, 0));
    check("an incomplete top does not unlock the layer below it",
          timber_tower_top_complete(&t) == TIMBER_LAYERS_BASE - 1 &&
          !timber_tower_pullable(&t, (TIMBER_LAYERS_BASE - 1) * TIMBER_SLOTS));
    check("the new layer's block is locked too", !timber_tower_pullable(&t, 7));

    timber_tower_remove(&t, 10);
    timber_tower_place(&t, 10, 0);
    timber_tower_remove(&t, 13);
    check("the placement layer stays the incomplete top",
          timber_tower_place_layer(&t) == TIMBER_LAYERS_BASE && timber_tower_place(&t, 13, 2) == 0);
    check("completing the top unlocks the layer that was below it",
          timber_tower_top_complete(&t) == TIMBER_LAYERS_BASE &&
          timber_tower_pullable(&t, (TIMBER_LAYERS_BASE - 1) * TIMBER_SLOTS) &&
          !timber_tower_pullable(&t, 7) && timber_tower_validate(&t) == TIMBER_VALID);
    check("the next block in hand opens the layer above",
          timber_tower_place_layer(&t) == TIMBER_LAYERS_BASE + 1);

    /* Restack every pullable block once and confirm the invariants hold. */
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        if (timber_tower_place_layer(&t) < 0) {
            break;
        }
        if (timber_tower_pullable(&t, id)) {
            int slot;

            timber_tower_remove(&t, id);
            for (slot = 0; slot < TIMBER_SLOTS; slot++) {
                if (timber_tower_can_place(&t, slot)) {
                    timber_tower_place(&t, id, slot);
                    break;
                }
            }
        }
    }
    check("restacking never breaks an invariant", timber_tower_validate(&t) == TIMBER_VALID);
    check("the layer count never exceeds its bound",
          timber_tower_layers(&t) <= TIMBER_LAYERS_MAX);
}

static void test_gap(void)
{
    struct timber_tower t;

    timber_tower_build(&t);
    timber_tower_remove(&t, 9);
    timber_tower_remove(&t, 10);
    check("a thinned layer is not a gap", timber_tower_gap(&t) == -1);
    timber_tower_remove(&t, 11);
    check("an emptied layer below the top is a gap, reported by layer",
          timber_tower_gap(&t) == 3);
    check("a gap still validates structurally: the stability model forbids it, not the grid",
          timber_tower_validate(&t) == TIMBER_VALID);

    /* Emptying the top layer itself shortens the tower instead. */
    timber_tower_build(&t);
    timber_tower_remove(&t, 51);
    timber_tower_remove(&t, 52);
    timber_tower_remove(&t, 53);
    check("emptying the top layer shortens the tower",
          timber_tower_layers(&t) == TIMBER_LAYERS_BASE - 1 && timber_tower_gap(&t) == -1 &&
          timber_tower_validate(&t) == TIMBER_VALID);
}

static void test_validate(void)
{
    struct timber_tower t;

    timber_tower_build(&t);
    t.layers = TIMBER_LAYERS_BASE + 1;
    check("a layer count the grid does not support is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_LAYERS);
    t.layers = TIMBER_LAYERS_MAX + 1;
    check("a layer count over the bound is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_LAYERS);

    timber_tower_build(&t);
    t.grid[0][0] = 5;
    check("a cell naming a block that sits elsewhere is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_GRID);
    timber_tower_build(&t);
    t.grid[0][0] = TIMBER_BLOCKS;
    check("a cell naming an id outside the run is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_GRID);

    timber_tower_build(&t);
    t.grid[0][0] = TIMBER_NO_BLOCK;
    check("a present block whose cell is empty is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_BLOCK);
    timber_tower_build(&t);
    t.blocks[0].slot = 3;
    t.grid[0][0] = TIMBER_NO_BLOCK;
    check("a block outside the tower is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_SLOT);

    timber_tower_build(&t);
    t.blocks[0].extraction = TIMBER_EXTRACTION_MAX + 1;
    check("an extraction beyond a block length is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_EXTRACTION);
    timber_tower_build(&t);
    t.blocks[0].extraction = 100;
    t.blocks[1].extraction = 100;
    check("two blocks part way out at once is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_EXTRACTION);
    timber_tower_build(&t);
    t.blocks[TIMBER_BLOCKS - 1].extraction = 100;
    check("a block part way out of a locked layer is invalid",
          timber_tower_validate(&t) == TIMBER_INVALID_EXTRACTION);
    timber_tower_build(&t);
    timber_tower_remove(&t, 3);
    t.blocks[3].extraction = 100;
    check("a block in hand cannot be part way out",
          timber_tower_validate(&t) == TIMBER_INVALID_EXTRACTION);
    timber_tower_build(&t);
    t.blocks[0].extraction = 100;
    check("one block part way out of a pullable layer is fine",
          timber_tower_validate(&t) == TIMBER_VALID);

    timber_tower_build(&t);
    t.blocks[20].mass = 0;
    check("a weightless block is invalid", timber_tower_validate(&t) == TIMBER_INVALID_MASS);

    check("NULL is invalid", timber_tower_validate(NULL) == TIMBER_INVALID_LAYERS);
}

static void test_generate(void)
{
    struct timber_tower a;
    struct timber_tower b;
    struct timber_rng rng;
    uint32_t seed;
    int id;
    int layer;
    int guaranteed = 1;
    int tight_bottom = 1;
    int fields = 1;
    int tells_only_on_loose_enough = 1;
    int varied = 0;
    long tells[3] = { 0, 0, 0 };
    long blocks[3] = { 0, 0, 0 };

    timber_rng_seed(&rng, 2026u);
    timber_tower_generate(&a, &rng);
    timber_rng_seed(&rng, 2026u);
    timber_tower_generate(&b, &rng);
    check("a seed builds the same tower twice", memcmp(&a, &b, sizeof(a)) == 0);
    check("a generated tower validates", timber_tower_validate(&a) == TIMBER_VALID &&
          timber_tower_present_count(&a) == TIMBER_BLOCKS);
    timber_rng_seed(&rng, 2027u);
    timber_tower_generate(&b, &rng);
    check("another seed builds another tower", memcmp(&a, &b, sizeof(a)) != 0);
    timber_tower_generate(&b, NULL);
    timber_tower_build(&a);
    check("no generator means the canonical tower", memcmp(&a, &b, sizeof(a)) == 0);

    for (seed = 1; seed <= 400; seed++) {
        timber_rng_seed(&rng, seed);
        timber_tower_generate(&a, &rng);
        for (layer = 0; layer < TIMBER_LAYERS_BASE; layer++) {
            int loosest = 0;
            int slot;

            for (slot = 0; slot < TIMBER_SLOTS; slot++) {
                const struct timber_block *blk = &a.blocks[layer * TIMBER_SLOTS + slot];

                if (blk->seat > loosest) {
                    loosest = blk->seat;
                }
            }
            guaranteed &= loosest >= TIMBER_SEAT_GUARANTEE;
        }
        for (id = 0; id < TIMBER_BLOCKS; id++) {
            const struct timber_block *blk = &a.blocks[id];
            int magnitude = blk->offset < 0 ? -blk->offset : blk->offset;
            int band = blk->seat >= TIMBER_TELL_LOOSE_FROM ? 0
                       : blk->seat >= TIMBER_TELL_MEDIUM_FROM ? 1 : 2;

            fields &= blk->variant < TIMBER_VARIANTS && blk->tested == 0 && blk->moves == 0 &&
                      blk->extraction == 0 && blk->mass == TIMBER_MASS_ONE;
            fields &= magnitude >= TIMBER_OFFSET_MIN && magnitude <= TIMBER_OFFSET_MAX;
            varied |= blk->seat != a.blocks[0].seat;
            if (blk->layer < TIMBER_TIGHT_LAYERS && blk->seat < TIMBER_SEAT_GUARANTEE) {
                tight_bottom &= blk->seat < TIMBER_TIGHT_SEAT_MAX;
            }
            tells_only_on_loose_enough &= !(blk->tell && band == 2);
            tells[band] += blk->tell;
            blocks[band]++;
        }
    }
    check("every layer keeps one block at least as loose as the guarantee", guaranteed);
    check("the bottom layers draw their seats below the tight ceiling", tight_bottom);
    check("variants, offsets and the fresh flags are in range", fields);
    check("seats vary", varied);
    check("a tight block never shows a tell", tells_only_on_loose_enough);
    printf("     tells: loose %ld of %ld, medium %ld of %ld, tight %ld of %ld\n",
           tells[0], blocks[0], tells[1], blocks[1], tells[2], blocks[2]);
    check("loose blocks show a tell about six times in ten",
          tells[0] * 100 / blocks[0] >= 55 && tells[0] * 100 / blocks[0] <= 65);
    check("middling blocks show a tell about three times in ten",
          tells[1] * 100 / blocks[1] >= 25 && tells[1] * 100 / blocks[1] <= 35);

    /* Load: blocks above a block's layer. */
    timber_tower_build(&a);
    check("a bottom block carries everything above its layer",
          timber_tower_load(&a, 0) == TIMBER_LOAD_MAX);
    check("a top block carries nothing",
          timber_tower_load(&a, TIMBER_BLOCKS - 1) == 0);
    timber_tower_remove(&a, 30);
    check("load counts blocks, not layers",
          timber_tower_load(&a, 0) == TIMBER_LOAD_MAX - 1 && timber_tower_load(&a, 30) == 0 &&
          timber_tower_load(&a, 27) == TIMBER_LOAD_MAX - 27 - 1);
    check("load of an id outside the run is 0", timber_tower_load(&a, -1) == 0);
}

static void test_reseat(void)
{
    struct timber_tower a;
    struct timber_tower b;
    int seats_differ = 0;
    int in_range = 1;
    int salt;

    timber_tower_build(&a);
    a.blocks[5].tested = 1;
    timber_tower_reseat(&a, 5, 3u);
    b = a;
    timber_tower_reseat(&b, 5, 3u);
    check("the same block and turn reseat the same way",
          a.blocks[5].seat == b.blocks[5].seat && a.blocks[5].offset == b.blocks[5].offset &&
          a.blocks[5].tell == b.blocks[5].tell);
    check("a placed block sits loose and forgets what a TEST found",
          a.blocks[5].seat >= TIMBER_SEAT_PLACED_MIN && a.blocks[5].tested == 0);
    for (salt = 0; salt < 64; salt++) {
        int magnitude;

        timber_tower_reseat(&b, 5, (uint32_t)salt);
        seats_differ |= b.blocks[5].seat != a.blocks[5].seat;
        magnitude = b.blocks[5].offset < 0 ? -b.blocks[5].offset : b.blocks[5].offset;
        in_range &= b.blocks[5].seat >= TIMBER_SEAT_PLACED_MIN &&
                    magnitude >= TIMBER_OFFSET_MIN && magnitude <= TIMBER_OFFSET_MAX;
    }
    check("another turn reseats differently", seats_differ);
    check("every reseat stays in range", in_range);
    timber_tower_reseat(&a, -1, 0u);
    timber_tower_reseat(NULL, 0, 0u);
    check("reseating nothing is safe", timber_tower_validate(&a) == TIMBER_VALID);
}

static void test_null_is_safe(void)
{
    struct timber_rect r;

    timber_tower_build(NULL);
    timber_tower_generate(NULL, NULL);
    check("NULL tower is safe",
          timber_tower_layers(NULL) == 0 && timber_tower_layer_fill(NULL, 0) == 0 &&
          timber_tower_top_complete(NULL) == -1 && timber_tower_at(NULL, 0, 0) == -1 &&
          timber_tower_block(NULL, 0) == NULL && timber_tower_present_count(NULL) == 0 &&
          !timber_tower_pullable(NULL, 0) && timber_tower_rect(NULL, 0, &r) == -1 &&
          timber_tower_gap(NULL) == -1 && timber_tower_place_layer(NULL) == -1 &&
          !timber_tower_can_place(NULL, 0) && timber_tower_remove(NULL, 0) == -1 &&
          timber_tower_place(NULL, 0, 0) == -1);
}

int main(void)
{
    test_build();
    test_pullable();
    test_remove_and_place();
    test_gap();
    test_validate();
    test_generate();
    test_reseat();
    test_null_is_safe();

    printf("timber_tower_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
