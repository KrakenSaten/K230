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
static void emit_at(struct timber_run *run, enum timber_event_type type, int id, int layer,
                    int slot, int32_t value)
{
    struct timber_event *e;

    if (run->event_count >= TIMBER_EVENTS_MAX) {
        if (run->events_dropped < 0xFFFFu) {
            run->events_dropped++;
        }
        return;
    }
    e = &run->events[run->event_count++];
    e->type = (uint8_t)type;
    e->block = id >= 0 && id < TIMBER_BLOCKS ? (uint8_t)id : (uint8_t)TIMBER_NO_BLOCK;
    e->layer = (uint8_t)layer;
    e->slot = (uint8_t)slot;
    e->value = value;
}

/* An event about a block, from the cell it is in. */
static void emit(struct timber_run *run, enum timber_event_type type, int id, int32_t value)
{
    const struct timber_block *b = timber_tower_block(&run->tower, id);

    emit_at(run, type, id, b ? b->layer : 0, b ? b->slot : 0, value);
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

/* ---- the tower's answer ------------------------------------------------- */

/* The tower falls at the hinge. Read the cause from what the player did
 * last, freeze the answer, and hand the run to the collapse. */
static void collapse(struct timber_run *run)
{
    enum timber_cause cause;

    if (run->last_place_tick != TIMBER_NEVER &&
        run->ticks - run->last_place_tick <= TIMBER_CAUSE_PLACE_TICKS) {
        cause = TIMBER_CAUSE_PLACEMENT;
    } else if (run->last_jolt_tick != TIMBER_NEVER &&
               run->ticks - run->last_jolt_tick <= TIMBER_CAUSE_JOLT_TICKS) {
        cause = TIMBER_CAUSE_JOLT;
    } else if (run->margin_static < 0) {
        cause = TIMBER_CAUSE_TIP;
    } else {
        cause = TIMBER_CAUSE_SWAY;
    }
    run->cause = (uint8_t)cause;
    run->state = TIMBER_RUN_COLLAPSING;
    run->collapse_ticks = 0;
    emit_at(run, TIMBER_EVENT_COLLAPSE, -1, run->hinge, 0, (int32_t)cause);
    /* The second and last time the generator is consumed in a run. */
    timber_collapse_begin(&run->collapse, &run->tower, run->hinge, run->hinge_axis,
                          run->hinge_sign, &run->rng);
}

/* The run is over: the last block has rested, or the ceiling passed. */
static void finish(struct timber_run *run)
{
    run->state = TIMBER_RUN_OVER;
    emit_at(run, TIMBER_EVENT_OVER, -1, run->hinge, 0, run->score.points);
}

/* Measure the tower: the hinge and its margins, whether it creaks, and
 * whether it is still standing. Called after every tick and every act, so
 * the run always carries a current answer. */
static void settle(struct timber_run *run)
{
    struct timber_margin m;
    int32_t sx = run->disturb_axis == TIMBER_AXIS_X ? run->sway : 0;
    int32_t sy = run->disturb_axis == TIMBER_AXIS_Y ? run->sway : 0;
    int hinge = timber_stability_hinge(&run->tower, run->lean_x, run->lean_y, sx, sy, &m);

    if (hinge < 0) {
        run->hinge = TIMBER_NO_LAYER;
        run->hinge_axis = TIMBER_AXIS_X;
        run->hinge_sign = 0;
        run->margin_static = TIMBER_MARGIN_FULL;
        run->margin_eff = TIMBER_MARGIN_FULL;
        run->creaking = 0;
        return;
    }
    run->hinge = (uint8_t)hinge;
    run->hinge_axis = m.axis;
    run->hinge_sign = m.sign;
    run->margin_static = m.stat;
    run->margin_eff = m.eff;
    if (run->state == TIMBER_RUN_ACTIVE && m.eff < 0) {
        collapse(run);
        return;
    }
    /* Edge-triggered: one creak when the margin goes under the line, and
     * not another until it has come back over it. */
    if (m.stat < TIMBER_CREAK_MARGIN) {
        if (!run->creaking) {
            run->creaking = 1;
            emit_at(run, TIMBER_EVENT_CREAK, -1, hinge, 0, m.stat);
        }
    } else {
        run->creaking = 0;
    }
}

/* The sway right now: the disturbance times the amplitude times the sine
 * of the phase, in the direction the disturbance came from. Computed on
 * the magnitude so the shift is never applied to a negative number. */
static int32_t sway_of(const struct timber_run *run)
{
    int32_t s = timber_stability_sin(run->sway_phase);
    int negative = s < 0;
    int64_t magnitude;

    if (negative) {
        s = -s;
    }
    /* Q8.8 * Q8.8 * Q15 -> Q8.8 */
    magnitude = ((int64_t)run->disturb * TIMBER_SWAY_AMP * s) >> 23;
    if (run->disturb_sign < 0) {
        negative = !negative;
    }
    return negative ? (int32_t)-magnitude : (int32_t)magnitude;
}

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
    /* The sway starts over in the direction of the new knock. */
    run->sway_phase = 0;
    run->sway = 0;
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
    run->hinge = TIMBER_NO_LAYER;
    run->last_jolt_tick = TIMBER_NEVER;
    run->last_place_tick = TIMBER_NEVER;
    run->cause = TIMBER_CAUSE_NONE;
    timber_score_init(&run->score);
    timber_score_height(&run->score, timber_tower_layers(&run->tower));
    settle(run);
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
    if (run->state == TIMBER_RUN_ACTIVE) {
        run->ticks++;
        run->disturb = (uint16_t)(((uint32_t)run->disturb * TIMBER_DISTURB_DECAY) >> 8);
        run->sway_phase = (uint16_t)(run->sway_phase + TIMBER_SWAY_STEP);
        run->sway = (int16_t)sway_of(run);
        settle(run);
    } else if (run->state == TIMBER_RUN_COLLAPSING) {
        run->ticks++;
        run->collapse_ticks++;
        if (timber_collapse_tick(&run->collapse) > 0) {
            int id;

            for (id = 0; id < TIMBER_BLOCKS; id++) {
                const struct timber_fall *f = &run->collapse.blocks[id];

                if (f->rest_tick == run->collapse.ticks) {
                    emit_at(run, TIMBER_EVENT_LAND, id, run->tower.blocks[id].layer,
                            run->tower.blocks[id].slot, f->z);
                }
            }
        }
        /* The choreography puts everything down by the ceiling; the
         * check on the count is the belt to its braces. */
        if (timber_collapse_done(&run->collapse) ||
            run->collapse_ticks >= TIMBER_COLLAPSE_TICKS_MAX) {
            finish(run);
        }
    }
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

/* Layers above a block's layer, for the depth factor. */
static int layers_above(const struct timber_run *run, const struct timber_block *b)
{
    return timber_tower_layers(&run->tower) - 1 - b->layer;
}

int32_t timber_run_worth(const struct timber_run *run, int id)
{
    const struct timber_block *b = run ? timber_tower_block(&run->tower, id) : NULL;

    if (!b || !b->present) {
        return 0;
    }
    return timber_score_value((enum timber_class)timber_run_class(run, id), layers_above(run, b), 1,
                              b->tested, run->score.streak);
}

int timber_run_cause(const struct timber_run *run)
{
    return run ? run->cause : TIMBER_CAUSE_NONE;
}

const struct timber_fall *timber_run_fall(const struct timber_run *run, int id)
{
    if (!run || (run->state != TIMBER_RUN_COLLAPSING && run->state != TIMBER_RUN_OVER)) {
        return NULL;
    }
    return timber_collapse_block(&run->collapse, id);
}

/* ---- the tower's state -------------------------------------------------- */

int timber_run_hinge(const struct timber_run *run)
{
    if (!run || run->hinge == TIMBER_NO_LAYER) {
        return -1;
    }
    return run->hinge;
}

int32_t timber_run_margin(const struct timber_run *run)
{
    return run ? run->margin_eff : TIMBER_MARGIN_FULL;
}

int timber_run_stability_permille(const struct timber_run *run)
{
    int32_t m = timber_run_margin(run);

    if (m < 0) {
        m = 0;
    }
    if (m > TIMBER_MARGIN_FULL) {
        m = TIMBER_MARGIN_FULL;
    }
    return (int)(m * 1000 / TIMBER_MARGIN_FULL);
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
    settle(run);
    return cls;
}

/* ---- the pull ----------------------------------------------------------- */

/* The block came free. It is scored as it was at that moment, leaves the
 * tower for the hand, and the turn moves on to placing it. */
static void slip(struct timber_run *run)
{
    int id = run->selected;
    const struct timber_block *b = &run->tower.blocks[id];
    int32_t points = timber_score_pull(&run->score, (enum timber_class)timber_run_class(run, id),
                                       layers_above(run, b), !run->pull_jolted, b->tested);

    emit(run, TIMBER_EVENT_SLIP, id, points);
    timber_tower_remove(&run->tower, id);
    run->held = (uint8_t)id;
    run->turn = TIMBER_TURN_PLACING;
    run->pull_jolted = 0;
    reset_grip(run);
}

static void move_block(struct timber_run *run, struct timber_block *b, int32_t delta)
{
    int id = run->selected;
    int carried = timber_stability_block_supports(&run->tower, id);
    int load = timber_tower_load(&run->tower, id);
    int axis = timber_layer_axis(b->layer);
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
    /* A block that was carrying the stack and has just let go of it, out
     * of the tower or merely far enough, drops the stack onto the blocks
     * that are left. The heavier the load, the harder it settles. */
    if (carried && !timber_stability_block_supports(&run->tower, id)) {
        int32_t impulse = TIMBER_SHIFT_IMPULSE_MIN +
                          (TIMBER_SHIFT_IMPULSE_MAX - TIMBER_SHIFT_IMPULSE_MIN) * load / TIMBER_LOAD_MAX;

        disturb_add(run, impulse, axis, e < 0 ? -1 : 1);
        emit(run, TIMBER_EVENT_SHIFT, id, impulse);
    }
    settle(run);
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
            run->last_jolt_tick = run->ticks;
            emit(run, TIMBER_EVENT_JOLT, run->selected, jolt);
        }
    }
    move_block(run, b, travel);
    return 1;
}

/* ---- placing ------------------------------------------------------------ */

int timber_run_place(struct timber_run *run, int slot)
{
    int id;
    int layer;
    int axis;
    int side;

    if (!run || run->state != TIMBER_RUN_ACTIVE || run->turn != TIMBER_TURN_PLACING ||
        run->held == TIMBER_NO_BLOCK || !timber_tower_can_place(&run->tower, slot)) {
        return -1;
    }
    id = run->held;
    layer = timber_tower_place_layer(&run->tower);
    if (timber_tower_place(&run->tower, id, slot) != 0) {
        return -1;
    }
    timber_tower_reseat(&run->tower, id, run->turns);
    /* Across the new layer's axis, slot 0 is the low side and slot 2 the
     * high side. */
    axis = timber_layer_axis(layer) == TIMBER_AXIS_X ? TIMBER_AXIS_Y : TIMBER_AXIS_X;
    side = slot == 0 ? -1 : 1;
    emit(run, TIMBER_EVENT_PLACE, id, 0);
    if (timber_tower_layer_fill(&run->tower, layer) == TIMBER_SLOTS) {
        int32_t bonus = timber_score_layer(&run->score);

        emit_at(run, TIMBER_EVENT_LAYER, -1, layer, 0, bonus);
    } else if (slot != 1) {
        lean_add(run, axis, side * TIMBER_PLACE_LEAN);
    }
    timber_score_height(&run->score, timber_tower_layers(&run->tower));
    disturb_add(run, TIMBER_PLACE_IMPULSE, axis, side);

    run->held = TIMBER_NO_BLOCK;
    run->selected = TIMBER_NO_BLOCK;
    run->turn = TIMBER_TURN_SELECT;
    run->tests_left = TIMBER_TESTS_PER_TURN;
    run->turns++;
    run->last_place_tick = run->ticks;
    reset_grip(run);
    settle(run);
    return 0;
}
