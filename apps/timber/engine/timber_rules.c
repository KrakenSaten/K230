/*
 * PocketTimber run state and rules. See timber_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_rules.h"

#include <string.h>

/* ---- events ----------------------------------------------------------- */

void timber_run_clear_events(struct timber_run *run)
{
    if (!run) {
        return;
    }
    run->event_count = 0;
    run->events_dropped = 0;
}

/* Record something that happened. The newest event is the one dropped when
 * the queue is full: an event lost at the head would be a collapse the UI
 * never learned about, which is worse than a missed effect. */
static void emit(struct timber_run *run, enum timber_event_type type, int id, int32_t value)
{
    struct timber_event *e;
    const struct timber_block *b = timber_tower_block(&run->tower, id);

    if (run->event_count >= TIMBER_EVENTS_MAX) {
        if (run->events_dropped < 0xFFFFu) {
            run->events_dropped++;
        }
        return;
    }
    e = &run->events[run->event_count++];
    e->type = (uint8_t)type;
    e->block = b ? (uint8_t)id : (uint8_t)TIMBER_NO_BLOCK;
    e->layer = b ? b->layer : 0u;
    e->slot = b ? b->slot : 0u;
    e->value = value;
}

int timber_run_take_event(struct timber_run *run, struct timber_event *out)
{
    int i;

    if (!run || !out || run->event_count == 0) {
        return 0;
    }
    *out = run->events[0];
    for (i = 1; i < run->event_count; i++) {
        run->events[i - 1] = run->events[i];
    }
    run->event_count--;
    return 1;
}

/* ---- run --------------------------------------------------------------- */

static void reset_grip(struct timber_run *run)
{
    run->grip = 0;
    run->grip_broken = 0;
    run->travel_index = 0;
    memset(run->travel, 0, sizeof(run->travel));
}

void timber_run_new(struct timber_run *run, uint32_t seed)
{
    if (!run) {
        return;
    }
    memset(run, 0, sizeof(*run));
    run->seed = seed;
    timber_rng_seed(&run->rng, seed);
    timber_tower_generate(&run->tower, &run->rng);
    run->state = TIMBER_RUN_READY;
    run->turn = TIMBER_TURN_SELECT;
    run->selected = TIMBER_NO_BLOCK;
    run->held = TIMBER_NO_BLOCK;
    run->tests_left = TIMBER_TESTS_PER_TURN;
    run->disturb_sign = 1;
}

int timber_run_start(struct timber_run *run)
{
    if (!run || run->state != TIMBER_RUN_READY) {
        return -1;
    }
    run->state = TIMBER_RUN_ACTIVE;
    return 0;
}

void timber_run_tick(struct timber_run *run)
{
    if (!run) {
        return;
    }
    if (run->state != TIMBER_RUN_ACTIVE && run->state != TIMBER_RUN_COLLAPSING) {
        return;
    }
    run->ticks++;
}

int timber_run_is_over(const struct timber_run *run)
{
    return run && run->state == TIMBER_RUN_OVER;
}

/* ---- blocks ------------------------------------------------------------ */

int timber_run_load(const struct timber_run *run, int id)
{
    return run ? timber_tower_load(&run->tower, id) : 0;
}

int timber_run_class(const struct timber_run *run, int id)
{
    const struct timber_block *b = run ? timber_tower_block(&run->tower, id) : NULL;

    if (!b || !b->present) {
        return -1;
    }
    return timber_pull_class(b->seat, timber_tower_load(&run->tower, id));
}

int timber_run_held(const struct timber_run *run)
{
    if (!run || run->held == TIMBER_NO_BLOCK) {
        return -1;
    }
    return run->held;
}

/* ---- selection --------------------------------------------------------- */

int timber_run_select(struct timber_run *run, int id)
{
    if (!run || run->state != TIMBER_RUN_ACTIVE) {
        return -1;
    }
    if (!timber_tower_pullable(&run->tower, id)) {
        return -1;
    }
    if (run->selected == (uint8_t)id) {
        return 0;
    }
    /* A block part way out keeps the selection; a block in hand leaves
     * nothing to select. */
    if (run->turn != TIMBER_TURN_SELECT) {
        return -1;
    }
    if (run->selected != TIMBER_NO_BLOCK) {
        emit(run, TIMBER_EVENT_DESELECT, run->selected, 0);
    }
    run->selected = (uint8_t)id;
    reset_grip(run);
    emit(run, TIMBER_EVENT_SELECT, id, 0);
    return 0;
}

int timber_run_deselect(struct timber_run *run)
{
    if (!run || run->state != TIMBER_RUN_ACTIVE || run->turn == TIMBER_TURN_PULLING) {
        return -1;
    }
    if (run->selected != TIMBER_NO_BLOCK) {
        emit(run, TIMBER_EVENT_DESELECT, run->selected, 0);
        run->selected = TIMBER_NO_BLOCK;
        reset_grip(run);
    }
    return 0;
}

int timber_run_selected(const struct timber_run *run)
{
    if (!run || run->selected == TIMBER_NO_BLOCK) {
        return -1;
    }
    return run->selected;
}

/* ---- what play does to the tower ---------------------------------------- */

static void disturb_add(struct timber_run *run, int32_t amount, int axis, int sign)
{
    int32_t d = (int32_t)run->disturb + amount;

    if (d > TIMBER_DISTURB_MAX) {
        d = TIMBER_DISTURB_MAX;
    }
    if (d < 0) {
        d = 0;
    }
    run->disturb = (uint16_t)d;
    run->disturb_axis = (uint8_t)axis;
    run->disturb_sign = (int8_t)(sign < 0 ? -1 : 1);
}

static void lean_add(struct timber_run *run, int axis, int32_t delta)
{
    int32_t *lean = axis == TIMBER_AXIS_X ? &run->lean_x : &run->lean_y;
    int64_t v = (int64_t)*lean + delta;

    if (v > TIMBER_LEAN_MAX) {
        v = TIMBER_LEAN_MAX;
    }
    if (v < -TIMBER_LEAN_MAX) {
        v = -TIMBER_LEAN_MAX;
    }
    *lean = (int32_t)v;
}

/* ---- testing ------------------------------------------------------------ */

int timber_run_test(struct timber_run *run)
{
    struct timber_block *b;
    int cls;

    if (!run || run->state != TIMBER_RUN_ACTIVE || run->turn != TIMBER_TURN_SELECT ||
        run->selected == TIMBER_NO_BLOCK) {
        return -1;
    }
    b = &run->tower.blocks[run->selected];
    cls = timber_run_class(run, run->selected);
    if (b->tested) {
        return cls;
    }
    if (run->tests_left == 0) {
        return -1;
    }
    run->tests_left--;
    b->tested = 1;
    disturb_add(run, TIMBER_TEST_IMPULSE, timber_layer_axis(b->layer), 1);
    emit(run, TIMBER_EVENT_TEST, run->selected, cls);
    return cls;
}

/* ---- the pull ----------------------------------------------------------- */

/* The block came free. It leaves the tower for the hand and the turn moves
 * on to placing it. */
static void slip(struct timber_run *run)
{
    int id = run->selected;

    emit(run, TIMBER_EVENT_SLIP, id, 0);
    timber_tower_remove(&run->tower, id);
    run->held = (uint8_t)id;
    run->turn = TIMBER_TURN_PLACING;
    run->pull_jolted = 0;
    reset_grip(run);
}

static void move_block(struct timber_run *run, struct timber_block *b, int32_t delta)
{
    int32_t e = (int32_t)b->extraction + delta;

    if (e > TIMBER_EXTRACTION_MAX) {
        e = TIMBER_EXTRACTION_MAX;
    }
    if (e < -TIMBER_EXTRACTION_MAX) {
        e = -TIMBER_EXTRACTION_MAX;
    }
    b->extraction = (int16_t)e;
    /* The lock follows the block: part way out means PULLING, pushed all
     * the way back means the turn is open again. */
    run->turn = e != 0 ? TIMBER_TURN_PULLING : TIMBER_TURN_SELECT;
    if (e >= TIMBER_SLIP_AT || e <= -TIMBER_SLIP_AT) {
        slip(run);
    }
}

int timber_run_pull(struct timber_run *run, int32_t travel)
{
    struct timber_block *b;
    int cls;
    int32_t tightness;
    int32_t magnitude;
    int32_t sum = 0;
    int32_t excess;
    int axis;
    int sign;
    int i;

    if (!run || run->state != TIMBER_RUN_ACTIVE || run->turn == TIMBER_TURN_PLACING ||
        run->selected == TIMBER_NO_BLOCK) {
        return -1;
    }
    b = &run->tower.blocks[run->selected];
    if (!b->present) {
        return -1;
    }
    if (travel > TIMBER_EXTRACTION_MAX) {
        travel = TIMBER_EXTRACTION_MAX;
    }
    if (travel < -TIMBER_EXTRACTION_MAX) {
        travel = -TIMBER_EXTRACTION_MAX;
    }
    magnitude = travel < 0 ? -travel : travel;
    sign = travel < 0 ? -1 : 1;
    axis = timber_layer_axis(b->layer);
    cls = timber_pull_class(b->seat, timber_tower_load(&run->tower, run->selected));
    tightness = timber_pull_tightness(b->seat, timber_tower_load(&run->tower, run->selected));

    /* The finger's recent history, stiction or not: a block yanked against
     * its stiction breaks free into a jolt, which is what yanking does. */
    run->travel[run->travel_index] = (int16_t)magnitude;
    run->travel_index = (uint8_t)((run->travel_index + 1) % TIMBER_TRAVEL_WINDOW);

    if (magnitude == 0) {
        return 0;
    }

    if (!run->grip_broken) {
        int32_t stiction = timber_pull_stiction((enum timber_class)cls);

        if (stiction > 0) {
            int32_t grip = (int32_t)run->grip + magnitude;

            if (grip < stiction) {
                run->grip = (int16_t)grip;
                return 0;
            }
            /* It gives. The lurch is the whole of this tick's movement. */
            run->grip_broken = 1;
            disturb_add(run, TIMBER_LURCH_IMPULSE, axis, sign);
            emit(run, TIMBER_EVENT_STICK, run->selected, 0);
            move_block(run, b, sign * TIMBER_LURCH);
            return 1;
        }
        run->grip_broken = 1;
    }

    for (i = 0; i < TIMBER_TRAVEL_WINDOW; i++) {
        sum += run->travel[i];
    }
    excess = sum / TIMBER_TRAVEL_WINDOW - timber_pull_limit((enum timber_class)cls);
    if (excess > 0) {
        int32_t jolt = timber_pull_jolt(excess, tightness);

        if (jolt > 0) {
            disturb_add(run, (int32_t)(((int64_t)jolt * TIMBER_JOLT_DISTURB) >> 8), axis, sign);
            lean_add(run, axis, (int32_t)(((int64_t)sign * jolt * TIMBER_JOLT_LEAN) >> 8));
            run->pull_jolted = 1;
            emit(run, TIMBER_EVENT_JOLT, run->selected, jolt);
        }
    }
    move_block(run, b, travel);
    return 1;
}
