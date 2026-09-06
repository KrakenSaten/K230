/*
 * PocketTimber rules test: the run lifecycle, selection and its lock, the
 * event queue, and the digest that every later milestone uses to say that
 * a seed and a list of actions reproduce a run exactly.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_rules.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* A digest over the named fields of a run, so struct padding never affects
 * a comparison and a divergence is caught wherever it happens. */
static uint32_t fnv1a(uint32_t h, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

static uint32_t digest(const struct timber_run *run)
{
    uint32_t h = 2166136261u;
    int id;
    int layer;
    int slot;

    h = fnv1a(h, run->seed);
    h = fnv1a(h, run->rng.state);
    h = fnv1a(h, run->state);
    h = fnv1a(h, run->turn);
    h = fnv1a(h, run->selected);
    h = fnv1a(h, run->held);
    h = fnv1a(h, run->tests_left);
    h = fnv1a(h, run->turns);
    h = fnv1a(h, run->ticks);
    h = fnv1a(h, run->tower.layers);
    for (layer = 0; layer < TIMBER_LAYERS_MAX; layer++) {
        for (slot = 0; slot < TIMBER_SLOTS; slot++) {
            h = fnv1a(h, run->tower.grid[layer][slot]);
        }
    }
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &run->tower.blocks[id];

        h = fnv1a(h, b->present);
        h = fnv1a(h, b->layer);
        h = fnv1a(h, b->slot);
        h = fnv1a(h, b->seat);
        h = fnv1a(h, b->tell);
        h = fnv1a(h, b->tested);
        h = fnv1a(h, b->variant);
        h = fnv1a(h, b->moves);
        h = fnv1a(h, (uint32_t)(int32_t)b->extraction);
        h = fnv1a(h, (uint32_t)(int32_t)b->offset);
        h = fnv1a(h, b->mass);
    }
    return h;
}

static void start(struct timber_run *run, uint32_t seed)
{
    timber_run_new(run, seed);
    timber_run_start(run);
}

static void test_lifecycle(void)
{
    struct timber_run run;

    timber_run_new(&run, 7u);
    check("a new run is ready, on its first turn, with nothing selected or held",
          run.state == TIMBER_RUN_READY && run.turn == TIMBER_TURN_SELECT &&
          run.selected == TIMBER_NO_BLOCK && run.held == TIMBER_NO_BLOCK &&
          run.tests_left == TIMBER_TESTS_PER_TURN && run.turns == 0 && run.ticks == 0);
    check("a new run carries the canonical tower",
          timber_tower_validate(&run.tower) == TIMBER_VALID &&
          timber_tower_present_count(&run.tower) == TIMBER_BLOCKS);
    check("the seed is kept", run.seed == 7u);
    timber_run_tick(&run);
    check("nothing counts before the start", run.ticks == 0);
    check("selection before the start is refused", timber_run_select(&run, 0) == -1);
    check("starting works once", timber_run_start(&run) == 0 && run.state == TIMBER_RUN_ACTIVE);
    check("starting twice is refused", timber_run_start(&run) == -1);
    timber_run_tick(&run);
    timber_run_tick(&run);
    check("ticks count while active", run.ticks == 2);
    check("an active run is not over", !timber_run_is_over(&run));
    run.state = TIMBER_RUN_OVER;
    timber_run_tick(&run);
    check("a finished run stops counting and reports over",
          run.ticks == 2 && timber_run_is_over(&run));

    timber_run_new(NULL, 1u);
    timber_run_tick(NULL);
    check("NULL run is safe",
          timber_run_start(NULL) == -1 && !timber_run_is_over(NULL) &&
          timber_run_select(NULL, 0) == -1 && timber_run_deselect(NULL) == -1 &&
          timber_run_selected(NULL) == -1);
}

static void test_selection(void)
{
    struct timber_run run;
    struct timber_event e;
    int top = (TIMBER_LAYERS_BASE - 1) * TIMBER_SLOTS;

    start(&run, 11u);
    check("nothing is selected at first", timber_run_selected(&run) == -1);
    check("a pullable block can be selected",
          timber_run_select(&run, 4) == 0 && timber_run_selected(&run) == 4);
    check("selecting it emits one SELECT naming its cell",
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_SELECT && e.block == 4 &&
          e.layer == 1 && e.slot == 1 && !timber_run_take_event(&run, &e));
    check("selecting it again changes nothing and says nothing",
          timber_run_select(&run, 4) == 0 && timber_run_selected(&run) == 4 &&
          run.event_count == 0);
    check("a block in the locked top layer is refused and the selection stands",
          timber_run_select(&run, top) == -1 && timber_run_selected(&run) == 4);
    check("an id outside the run is refused",
          timber_run_select(&run, -1) == -1 && timber_run_select(&run, TIMBER_BLOCKS) == -1);
    check("choosing another block deselects the first",
          timber_run_select(&run, 9) == 0 && timber_run_selected(&run) == 9 &&
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_DESELECT && e.block == 4 &&
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_SELECT && e.block == 9);
    check("deselecting clears it and says so",
          timber_run_deselect(&run) == 0 && timber_run_selected(&run) == -1 &&
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_DESELECT && e.block == 9);
    check("deselecting nothing is fine and silent",
          timber_run_deselect(&run) == 0 && run.event_count == 0);

    /* The selection lock: a block part way out keeps the selection. The
     * pull itself arrives in P2; here the state is set by hand. */
    timber_run_select(&run, 4);
    run.turn = TIMBER_TURN_PULLING;
    run.tower.blocks[4].extraction = 100;
    check("while a block is part way out no other block can be selected",
          timber_run_select(&run, 9) == -1 && timber_run_selected(&run) == 4);
    check("re-selecting the block that is out is fine", timber_run_select(&run, 4) == 0);
    check("deselecting while a block is out is refused",
          timber_run_deselect(&run) == -1 && timber_run_selected(&run) == 4);
    run.turn = TIMBER_TURN_PLACING;
    run.tower.blocks[4].extraction = 0;
    run.selected = TIMBER_NO_BLOCK;
    check("while a block is in hand nothing can be selected", timber_run_select(&run, 9) == -1);
    run.turn = TIMBER_TURN_SELECT;
    run.state = TIMBER_RUN_OVER;
    check("an inactive run refuses selection", timber_run_select(&run, 9) == -1);
}

static void test_events(void)
{
    struct timber_run run;
    struct timber_event e;
    int i;
    int order = 1;

    start(&run, 3u);
    /* Alternate two blocks: each switch is a DESELECT and a SELECT. */
    for (i = 0; i < TIMBER_EVENTS_MAX; i++) {
        timber_run_select(&run, i & 1);
    }
    check("the queue fills and counts what it dropped",
          run.event_count == TIMBER_EVENTS_MAX && run.events_dropped > 0);
    for (i = 0; timber_run_take_event(&run, &e); i++) {
        int want = i == 0 ? TIMBER_EVENT_SELECT
                          : ((i & 1) ? TIMBER_EVENT_DESELECT : TIMBER_EVENT_SELECT);

        order &= e.type == want;
    }
    check("events come out oldest first", order && i == TIMBER_EVENTS_MAX);
    timber_run_select(&run, 5);
    timber_run_clear_events(&run);
    check("clearing empties the queue and the drop count",
          run.event_count == 0 && run.events_dropped == 0 && !timber_run_take_event(&run, &e));
    check("NULL is safe", !timber_run_take_event(NULL, &e) && !timber_run_take_event(&run, NULL));
    timber_run_clear_events(NULL);
}

static void test_determinism(void)
{
    struct timber_run a;
    struct timber_run b;
    int i;

    start(&a, 2026u);
    start(&b, 2026u);
    for (i = 0; i < 50; i++) {
        timber_run_tick(&a);
        timber_run_tick(&b);
        if (i % 7 == 0) {
            timber_run_select(&a, i % 40);
            timber_run_select(&b, i % 40);
        }
    }
    check("the same seed and actions reproduce the run exactly", digest(&a) == digest(&b));
    check("the tower is still valid after them",
          timber_tower_validate(&a.tower) == TIMBER_VALID && timber_tower_gap(&a.tower) == -1);
    timber_run_select(&b, 3);
    check("a different action diverges", digest(&a) != digest(&b));
}

int main(void)
{
    test_lifecycle();
    test_selection();
    test_events();
    test_determinism();

    printf("timber_rules_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
