/*
 * PocketTimber rules test: the run lifecycle, selection and its lock, the
 * TEST budget, the pull with its stiction, speed limit, partial extraction
 * and slip, the event queue, and the digest that says a seed and a list of
 * actions reproduce a run exactly.
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
    int i;

    h = fnv1a(h, run->seed);
    h = fnv1a(h, run->rng.state);
    h = fnv1a(h, run->state);
    h = fnv1a(h, run->turn);
    h = fnv1a(h, run->selected);
    h = fnv1a(h, run->held);
    h = fnv1a(h, run->tests_left);
    h = fnv1a(h, run->turns);
    h = fnv1a(h, run->ticks);
    h = fnv1a(h, run->grip_broken);
    h = fnv1a(h, run->pull_jolted);
    h = fnv1a(h, run->travel_index);
    h = fnv1a(h, (uint32_t)(int32_t)run->grip);
    for (i = 0; i < TIMBER_TRAVEL_WINDOW; i++) {
        h = fnv1a(h, (uint32_t)(int32_t)run->travel[i]);
    }
    h = fnv1a(h, run->disturb);
    h = fnv1a(h, run->disturb_axis);
    h = fnv1a(h, (uint32_t)(int32_t)run->disturb_sign);
    h = fnv1a(h, (uint32_t)run->lean_x);
    h = fnv1a(h, (uint32_t)run->lean_y);
    h = fnv1a(h, run->sway_phase);
    h = fnv1a(h, (uint32_t)(int32_t)run->sway);
    h = fnv1a(h, run->hinge);
    h = fnv1a(h, run->hinge_axis);
    h = fnv1a(h, (uint32_t)(int32_t)run->hinge_sign);
    h = fnv1a(h, run->creaking);
    h = fnv1a(h, (uint32_t)run->margin_static);
    h = fnv1a(h, (uint32_t)run->margin_eff);
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

/* Pull n ticks of the same travel, ticking the clock between them, and
 * count the events of one type that came out. */
static int drag(struct timber_run *run, int32_t travel, int n, enum timber_event_type type)
{
    struct timber_event e;
    int count = 0;
    int i;

    for (i = 0; i < n; i++) {
        timber_run_tick(run);
        timber_run_pull(run, travel);
        while (timber_run_take_event(run, &e)) {
            count += e.type == type;
        }
    }
    return count;
}

/* Make a bottom block STUCK and a low block FREE by seat, so the pull tests
 * do not depend on what a seed happened to draw. */
static void rig(struct timber_run *run, int stuck_id, int free_id)
{
    run->tower.blocks[stuck_id].seat = 0;
    run->tower.blocks[free_id].seat = 255;
}

static void test_lifecycle(void)
{
    struct timber_run run;

    timber_run_new(&run, 7u);
    check("a new run is ready, on its first turn, with nothing selected or held",
          run.state == TIMBER_RUN_READY && run.turn == TIMBER_TURN_SELECT &&
          run.selected == TIMBER_NO_BLOCK && run.held == TIMBER_NO_BLOCK &&
          run.tests_left == TIMBER_TESTS_PER_TURN && run.turns == 0 && run.ticks == 0);
    check("a new run carries a generated, valid tower",
          timber_tower_validate(&run.tower) == TIMBER_VALID &&
          timber_tower_present_count(&run.tower) == TIMBER_BLOCKS);
    check("a new run is undisturbed and straight",
          run.disturb == 0 && run.lean_x == 0 && run.lean_y == 0 && run.grip == 0);
    check("the seed is kept", run.seed == 7u);
    timber_run_tick(&run);
    check("nothing counts before the start", run.ticks == 0);
    check("selection, testing and pulling before the start are refused",
          timber_run_select(&run, 0) == -1 && timber_run_test(&run) == -1 &&
          timber_run_pull(&run, 10) == -1);
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
          timber_run_selected(NULL) == -1 && timber_run_test(NULL) == -1 &&
          timber_run_pull(NULL, 1) == -1 && timber_run_held(NULL) == -1 &&
          timber_run_class(NULL, 0) == -1 && timber_run_load(NULL, 0) == 0);
}

static void test_generation_in_a_run(void)
{
    struct timber_run a;
    struct timber_run b;
    int id;
    int classes_seen[TIMBER_CLASS_COUNT] = { 0, 0, 0, 0 };

    timber_run_new(&a, 100u);
    timber_run_new(&b, 100u);
    check("the same seed builds the same run", digest(&a) == digest(&b));
    timber_run_new(&b, 101u);
    check("a different seed builds a different tower", digest(&a) != digest(&b));
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        int cls = timber_run_class(&a, id);

        if (cls >= 0 && cls < TIMBER_CLASS_COUNT) {
            classes_seen[cls]++;
        }
    }
    check("a tower has blocks of more than one class",
          (classes_seen[0] > 0) + (classes_seen[1] > 0) + (classes_seen[2] > 0) +
          (classes_seen[3] > 0) >= 2);
    check("class of an id outside the run is -1",
          timber_run_class(&a, -1) == -1 && timber_run_class(&a, TIMBER_BLOCKS) == -1);
    check("a bottom block carries the load the class table expects",
          timber_run_load(&a, 0) == TIMBER_LOAD_MAX &&
          timber_run_class(&a, 0) == (int)timber_pull_class(a.tower.blocks[0].seat, TIMBER_LOAD_MAX));
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
    run.state = TIMBER_RUN_OVER;
    check("an inactive run refuses selection", timber_run_select(&run, 9) == -1);
}

static void test_testing(void)
{
    struct timber_run run;
    struct timber_event e;
    int cls;

    start(&run, 5u);
    rig(&run, 1, 4);
    check("TEST with nothing selected is refused", timber_run_test(&run) == -1);
    timber_run_select(&run, 4);
    timber_run_clear_events(&run);
    cls = timber_run_test(&run);
    check("TEST reveals the class, marks the block, spends a test and nudges the tower",
          cls == TIMBER_CLASS_FREE && run.tower.blocks[4].tested && run.tests_left == 1 &&
          run.disturb == TIMBER_TEST_IMPULSE && run.disturb_axis == timber_layer_axis(1));
    check("TEST emits its class",
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_TEST && e.block == 4 &&
          e.value == TIMBER_CLASS_FREE && !timber_run_take_event(&run, &e));
    check("a tested block answers again for free and silently",
          timber_run_test(&run) == TIMBER_CLASS_FREE && run.tests_left == 1 &&
          run.event_count == 0 && run.disturb == TIMBER_TEST_IMPULSE);
    timber_run_select(&run, 1);
    check("the second test of the turn spends the last one",
          timber_run_test(&run) == TIMBER_CLASS_STUCK && run.tests_left == 0 &&
          run.disturb == 2 * TIMBER_TEST_IMPULSE);
    timber_run_select(&run, 7);
    check("a third test is refused and reveals nothing",
          timber_run_test(&run) == -1 && !run.tower.blocks[7].tested);
    check("the tested blocks are still known", timber_run_select(&run, 4) == 0 &&
          timber_run_test(&run) == TIMBER_CLASS_FREE);
    timber_run_pull(&run, 50);
    check("TEST is refused while a block is part way out",
          run.turn == TIMBER_TURN_PULLING && timber_run_test(&run) == -1);
}

static void test_free_pull(void)
{
    struct timber_run run;
    struct timber_event e;
    int moved;
    int jolts;

    start(&run, 21u);
    rig(&run, 1, 4);
    check("a pull with nothing selected is refused", timber_run_pull(&run, 50) == -1);
    timber_run_select(&run, 4);
    timber_run_clear_events(&run);
    check("no travel moves nothing and is not a refusal",
          timber_run_pull(&run, 0) == 0 && run.tower.blocks[4].extraction == 0 &&
          run.turn == TIMBER_TURN_SELECT);
    moved = timber_run_pull(&run, 100);
    check("a free block moves with the finger at once",
          moved == 1 && run.tower.blocks[4].extraction == 100 && run.turn == TIMBER_TURN_PULLING);
    check("the tower stays valid with a block part way out",
          timber_tower_validate(&run.tower) == TIMBER_VALID);
    check("a block part way out locks the selection",
          timber_run_select(&run, 7) == -1 && timber_run_deselect(&run) == -1);
    jolts = drag(&run, 100, 3, TIMBER_EVENT_JOLT);
    check("under the limit there is no jolt and the tower is untouched",
          jolts == 0 && run.disturb == 0 && run.lean_y == 0 && !run.pull_jolted);
    check("travel adds up", run.tower.blocks[4].extraction == 400);
    timber_run_pull(&run, -400);
    check("pushed all the way back the block is seated and the turn is open again",
          run.tower.blocks[4].extraction == 0 && run.turn == TIMBER_TURN_SELECT &&
          timber_run_select(&run, 7) == 0);
    timber_run_select(&run, 4);
    timber_run_pull(&run, -100);
    check("a block can go out the far side instead",
          run.tower.blocks[4].extraction == -100 && run.turn == TIMBER_TURN_PULLING);
    drag(&run, -100, 5, TIMBER_EVENT_JOLT);
    check("just short of the slip point it is still in the tower",
          run.tower.blocks[4].extraction == -600 && run.turn == TIMBER_TURN_PULLING);
    timber_run_clear_events(&run);
    timber_run_pull(&run, -100);
    check("past the slip point the block leaves the tower into the hand",
          run.turn == TIMBER_TURN_PLACING && timber_run_held(&run) == 4 &&
          !run.tower.blocks[4].present && run.tower.blocks[4].extraction == 0 &&
          timber_tower_present_count(&run.tower) == TIMBER_BLOCKS - 1 &&
          timber_tower_validate(&run.tower) == TIMBER_VALID);
    check("the slip is announced from the cell it left",
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_SLIP && e.block == 4 &&
          e.layer == 1 && e.slot == 1);
    check("the piece card keeps the block in hand as the selection",
          timber_run_selected(&run) == 4);
    check("with a block in hand nothing can be pulled, tested or selected",
          timber_run_pull(&run, 10) == -1 && timber_run_test(&run) == -1 &&
          timber_run_select(&run, 7) == -1 && timber_run_select(&run, 4) == -1);
}

static void test_fast_pull_jolts(void)
{
    struct timber_run run;
    struct timber_event e;
    int i;

    start(&run, 22u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    timber_run_clear_events(&run);
    /* Block 4 is in layer 1, a y layer. The FREE limit is 110 a tick, and
     * the limit is judged on the average of the last three ticks. */
    timber_run_tick(&run);
    check("one fast sample on its own is not a jolt",
          timber_run_pull(&run, 300) == 1 && run.event_count == 0 && run.disturb == 0);
    timber_run_tick(&run);
    check("a second fast tick is, and the jolt names its size",
          timber_run_pull(&run, 300) == 1 && timber_run_take_event(&run, &e) &&
          e.type == TIMBER_EVENT_JOLT && e.block == 4 && e.value > 0 && run.pull_jolted);
    check("a jolt disturbs the tower along the block's axis in the pull's direction",
          run.disturb > 0 && run.disturb_axis == TIMBER_AXIS_Y && run.disturb_sign == 1);
    check("a jolt leans the tower along the block's axis in the pull's direction",
          run.lean_y > 0 && run.lean_x == 0);
    check("the block still moved with the finger", run.tower.blocks[4].extraction == 600);

    /* Pushing hard the other way leans it back. */
    start(&run, 22u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    drag(&run, -300, 2, TIMBER_EVENT_JOLT);
    check("a jolt the other way leans the other way",
          run.lean_y < 0 && run.disturb_sign == -1);

    /* Rattling a free block in and out keeps the tower shaking; the decay
     * every tick holds it short of the ceiling. */
    start(&run, 22u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    for (i = 0; i < 40; i++) {
        drag(&run, (i & 1) ? -300 : 300, 1, TIMBER_EVENT_JOLT);
    }
    check("a sustained rattle keeps the tower shaking short of the ceiling",
          run.disturb > 400 && run.disturb < TIMBER_DISTURB_MAX && run.turn == TIMBER_TURN_SELECT &&
          run.tower.blocks[4].extraction == 0);

    /* The ceiling: a wedged base block yanked straight out. */
    start(&run, 22u);
    rig(&run, 1, 4);
    timber_run_select(&run, 1);
    timber_run_pull(&run, 700);
    timber_run_pull(&run, 700);
    check("disturbance never exceeds its ceiling",
          run.disturb == TIMBER_DISTURB_MAX && run.turn == TIMBER_TURN_PLACING);
}

static void test_stuck_pull(void)
{
    struct timber_run run;
    struct timber_event e;
    int i;
    int held = 1;
    int sticks;
    int jolts;
    int ticks_to_slip = 0;

    start(&run, 23u);
    rig(&run, 1, 4);
    timber_run_select(&run, 1);
    check("the rigged bottom block is STUCK", timber_run_class(&run, 1) == TIMBER_CLASS_STUCK);
    timber_run_clear_events(&run);
    /* 128 of stiction at 10 a tick: twelve ticks hold, the thirteenth gives. */
    for (i = 0; i < 12; i++) {
        timber_run_tick(&run);
        held &= timber_run_pull(&run, 10) == 0;
        held &= run.tower.blocks[1].extraction == 0;
    }
    check("a stuck block absorbs travel without moving", held && run.grip == 120 &&
          run.turn == TIMBER_TURN_SELECT && run.event_count == 0);
    check("while it holds, the selection is still free",
          timber_run_select(&run, 4) == 0 && run.grip == 0);
    timber_run_select(&run, 1);
    check("changing the selection forgets the grip", run.grip == 0 && !run.grip_broken);
    for (i = 0; i < 12; i++) {
        timber_run_pull(&run, 10);
    }
    timber_run_clear_events(&run);
    check("the tick that breaks it moves it by the lurch and nothing more",
          timber_run_pull(&run, 10) == 1 && run.tower.blocks[1].extraction == TIMBER_LURCH &&
          run.grip_broken && run.turn == TIMBER_TURN_PULLING);
    check("breaking free is announced and shakes the tower a little",
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_STICK && e.block == 1 &&
          run.disturb == TIMBER_LURCH_IMPULSE);
    sticks = drag(&run, 10, 3, TIMBER_EVENT_STICK);
    check("it breaks only once", sticks == 0 && run.tower.blocks[1].extraction == TIMBER_LURCH + 30);

    /* Slow is safe: 10 a tick is under the STUCK limit of 22. */
    jolts = 0;
    while (run.turn == TIMBER_TURN_PULLING && ticks_to_slip < 200) {
        jolts += drag(&run, 10, 1, TIMBER_EVENT_JOLT);
        ticks_to_slip++;
    }
    check("a slow steady pull on a stuck block never jolts", jolts == 0 && !run.pull_jolted);
    check("and brings it out in about two and a half seconds",
          run.turn == TIMBER_TURN_PLACING && timber_run_held(&run) == 1 &&
          ticks_to_slip >= 55 && ticks_to_slip <= 65);

    /* Fast is not: 60 a tick on a stuck block. */
    start(&run, 23u);
    rig(&run, 1, 4);
    timber_run_select(&run, 1);
    for (i = 0; i < 3; i++) {
        timber_run_pull(&run, 60);
    }
    timber_run_clear_events(&run);
    jolts = drag(&run, 60, 3, TIMBER_EVENT_JOLT);
    check("a yanked stuck block jolts hard", jolts == 3 && run.disturb > 100 && run.lean_x > 0);
    check("a yank leans along x for an x-layer block", run.lean_y == 0);
}

static void test_decay_and_sway(void)
{
    struct timber_run run;
    int i;
    int falls = 1;
    int swung = 0;
    int forward = 1;
    uint16_t previous;

    start(&run, 31u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    timber_run_test(&run);
    check("a knock restarts the sway from rest",
          run.disturb == TIMBER_TEST_IMPULSE && run.sway_phase == 0 && run.sway == 0);
    previous = run.disturb;
    for (i = 0; i < 40; i++) {
        timber_run_tick(&run);
        falls &= run.disturb <= previous;
        previous = run.disturb;
        if (i < TIMBER_SWAY_PERIOD_TICKS / 2) {
            /* The first half period sways the way the knock went. */
            forward &= run.sway >= 0;
            swung |= run.sway > 0;
        }
    }
    check("disturbance only ever falls and is gone within two seconds",
          falls && run.disturb == 0);
    check("the tower sways the way it was knocked, then comes to rest",
          swung && forward && run.sway == 0);
    check("the sway phase advances a turn every period",
          run.sway_phase == (uint16_t)(40 * TIMBER_SWAY_STEP));

    /* A knock the other way sways the other way. */
    start(&run, 31u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    drag(&run, -300, 2, TIMBER_EVENT_JOLT);
    check("a knock the other way sways the other way",
          run.disturb_sign == -1 && run.sway == 0 &&
          (timber_run_tick(&run), timber_run_tick(&run), run.sway < 0));
}

static void test_margins_in_a_run(void)
{
    struct timber_run run;
    struct timber_event e;
    int creaks;
    int shifts;
    int i;

    timber_run_new(&run, 41u);
    check("a fresh tower is nearly perfectly balanced and the meter says so",
          timber_run_hinge(&run) >= 0 && run.margin_static > 350 &&
          run.margin_static <= TIMBER_MARGIN_FULL && timber_run_stability_permille(&run) > 900 &&
          timber_run_stability_permille(&run) <= 1000 && !run.creaking);
    timber_run_start(&run);

    /* Thin layer 5 to its centre block by hand: half a width of margin, so
     * the hinge is there and the meter is at a third. */
    timber_tower_remove(&run.tower, 15);
    timber_tower_remove(&run.tower, 17);
    timber_run_clear_events(&run);
    timber_run_tick(&run);
    /* The thinned layer and the one under it rest on the same block; the
     * micro-offsets decide which of the two reads a hair smaller. */
    check("a thinned layer becomes the hinge",
          (timber_run_hinge(&run) == 5 || timber_run_hinge(&run) == 4) &&
          run.margin_static > 100 && run.margin_static < 160 &&
          timber_run_stability_permille(&run) > 250 && timber_run_stability_permille(&run) < 420);
    check("half a width is not yet a creak", run.event_count == 0 && !run.creaking);

    /* Lean it until the margin is under a quarter of a width. */
    run.lean_x = 3000;
    timber_run_tick(&run);
    creaks = 0;
    while (timber_run_take_event(&run, &e)) {
        creaks += e.type == TIMBER_EVENT_CREAK && e.layer == run.hinge &&
                  e.block == TIMBER_NO_BLOCK && e.value == run.margin_static;
    }
    check("under the creak line the tower creaks once, naming the hinge",
          creaks == 1 && run.creaking && run.margin_static < TIMBER_CREAK_MARGIN &&
          (run.hinge == 4 || run.hinge == 5));
    for (i = 0; i < 5; i++) {
        timber_run_tick(&run);
    }
    check("it does not creak again while it stays there", run.event_count == 0 && run.creaking);
    run.lean_x = 0;
    timber_run_tick(&run);
    check("straightened, it stops creaking", !run.creaking && run.event_count == 0);
    run.lean_x = 3000;
    timber_run_tick(&run);
    check("and creaks again when it goes back under",
          timber_run_take_event(&run, &e) && e.type == TIMBER_EVENT_CREAK);
    run.lean_x = 6000;
    timber_run_tick(&run);
    check("leaned past its margin the hinge reads negative; the trigger is P4",
          run.margin_static < 0 && run.margin_eff < 0 && run.state == TIMBER_RUN_ACTIVE);

    /* The load shift: a free centre block drawn slowly lets go of the stack
     * at three quarters out, before it slips at four fifths. */
    start(&run, 42u);
    rig(&run, 1, 4);
    timber_run_select(&run, 4);
    shifts = drag(&run, 100, 5, TIMBER_EVENT_SHIFT);
    check("half out, the block still carries its share and nothing has shifted",
          shifts == 0 && run.disturb == 0 && run.tower.blocks[4].extraction == 500);
    timber_run_clear_events(&run);
    timber_run_pull(&run, 100);
    shifts = 0;
    while (timber_run_take_event(&run, &e)) {
        shifts += e.type == TIMBER_EVENT_SHIFT && e.block == 4 && e.value > 0;
    }
    check("past three quarters the stack settles onto the blocks left, with a knock",
          shifts == 1 && run.disturb > 0 && run.tower.blocks[4].extraction == 600 &&
          run.turn == TIMBER_TURN_PULLING);
    check("the knock grows with the load carried: a low block settles harder",
          run.disturb == TIMBER_SHIFT_IMPULSE_MIN +
                         (TIMBER_SHIFT_IMPULSE_MAX - TIMBER_SHIFT_IMPULSE_MIN) * 48 / TIMBER_LOAD_MAX);
    shifts = drag(&run, 100, 1, TIMBER_EVENT_SHIFT);
    check("the slip that follows is not a second shift",
          shifts == 0 && run.turn == TIMBER_TURN_PLACING);

    /* Pulling the base's centre block changes no margin; a side block of
     * the base leaves half a width. */
    start(&run, 43u);
    run.tower.blocks[1].seat = 255;
    timber_run_select(&run, 1);
    drag(&run, 100, 7, TIMBER_EVENT_JOLT);
    check("the base without its centre block stands as before",
          run.turn == TIMBER_TURN_PLACING && run.margin_static > 350);
    start(&run, 43u);
    run.tower.blocks[0].seat = 255;
    timber_run_select(&run, 0);
    drag(&run, 100, 7, TIMBER_EVENT_JOLT);
    check("the base without a side block has half a width left, at the base",
          run.turn == TIMBER_TURN_PLACING && timber_run_hinge(&run) == 0 &&
          run.margin_static > 100 && run.margin_static < 150);
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
    uint32_t rng_before;
    int i;

    start(&a, 2026u);
    start(&b, 2026u);
    rng_before = a.rng.state;
    for (i = 0; i < 120; i++) {
        int32_t travel = (i % 5) * 30 - 30;

        timber_run_tick(&a);
        timber_run_tick(&b);
        if (i % 17 == 0) {
            timber_run_select(&a, i % 40);
            timber_run_select(&b, i % 40);
            timber_run_test(&a);
            timber_run_test(&b);
        }
        timber_run_pull(&a, travel);
        timber_run_pull(&b, travel);
        timber_run_clear_events(&a);
        timber_run_clear_events(&b);
    }
    check("the same seed and actions reproduce the run exactly", digest(&a) == digest(&b));
    check("the tower is still valid after them",
          timber_tower_validate(&a.tower) == TIMBER_VALID && timber_tower_gap(&a.tower) == -1);
    check("nothing the player did moved the generator", a.rng.state == rng_before);

    start(&a, 2026u);
    start(&b, 2026u);
    rig(&a, 1, 4);
    rig(&b, 1, 4);
    timber_run_select(&a, 4);
    timber_run_select(&b, 4);
    timber_run_pull(&a, 100);
    timber_run_pull(&b, 101);
    check("a different action diverges", digest(&a) != digest(&b));
}

int main(void)
{
    test_lifecycle();
    test_generation_in_a_run();
    test_selection();
    test_testing();
    test_free_pull();
    test_fast_pull_jolts();
    test_stuck_pull();
    test_decay_and_sway();
    test_margins_in_a_run();
    test_events();
    test_determinism();

    printf("timber_rules_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
