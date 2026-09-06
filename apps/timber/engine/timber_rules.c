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

void timber_run_new(struct timber_run *run, uint32_t seed)
{
    if (!run) {
        return;
    }
    memset(run, 0, sizeof(*run));
    run->seed = seed;
    timber_rng_seed(&run->rng, seed);
    timber_tower_build(&run->tower);
    run->state = TIMBER_RUN_READY;
    run->turn = TIMBER_TURN_SELECT;
    run->selected = TIMBER_NO_BLOCK;
    run->held = TIMBER_NO_BLOCK;
    run->tests_left = TIMBER_TESTS_PER_TURN;
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
