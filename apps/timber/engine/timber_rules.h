/*
 * PocketTimber run state and rules: the lifecycle of a run, the turn
 * state machine, selection, and the event stream the UI reacts to.
 *
 * A turn is SELECT -> (TEST)* -> PULL -> PLACE, and each step is a separate
 * thing the player knows. Selecting a block does nothing to the tower. A
 * TEST reveals a block's class and costs one of the turn's tests and a
 * little disturbance. The pull is a sequence of per-tick travel actions,
 * during which the selection is locked to the block that is part way out.
 * When the block slips free it is in hand, and placing it on top ends the
 * turn.
 *
 * Determinism. A run is reproduced by (seed, the ordered list of player
 * actions at their ticks). The generator is consumed only when the tower is
 * built and when a collapse begins; nothing the player does moves it.
 *
 * Milestone P1 carries the lifecycle, the tower, selection and events.
 * Testing, the pull, the stability model, placement, scoring and the
 * collapse arrive in the milestones after it and are documented as they
 * land (docs/apps/POCKETTIMBER.md).
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_RULES_H
#define POCKETTIMBER_RULES_H

#include "timber_rng.h"
#include "timber_tower.h"
#include "timber_types.h"

/* Tests a turn allows. Two is a real allocation problem; unlimited testing
 * would make the correct play a slow one. */
#define TIMBER_TESTS_PER_TURN 2

/* One tick can end a pull, place a block and start a collapse, so the queue
 * holds more than that. A caller that drains it every tick can never
 * overflow it; one that does not loses the newest events and is told so by
 * events_dropped. */
#define TIMBER_EVENTS_MAX 24

/* What happened, for the UI to react to and for tests to assert on. block
 * is the block involved or TIMBER_NO_BLOCK; layer and slot are where it was
 * when the event happened. value carries the number the event is about
 * (enum timber_event_type). */
struct timber_event {
    uint8_t type;           /* enum timber_event_type */
    uint8_t block;
    uint8_t layer;
    uint8_t slot;
    int32_t value;
};

struct timber_run {
    uint32_t seed;
    struct timber_rng rng;
    uint8_t state;          /* enum timber_run_state */
    uint8_t turn;           /* enum timber_turn */
    uint8_t selected;       /* block id or TIMBER_NO_BLOCK */
    uint8_t held;           /* the block in hand, or TIMBER_NO_BLOCK */
    uint8_t tests_left;     /* this turn's remaining TESTs */
    uint16_t turns;         /* completed turns, one per placement */
    uint32_t ticks;         /* ticks since the run started */
    struct timber_tower tower;
    struct timber_event events[TIMBER_EVENTS_MAX];
    uint8_t event_count;
    uint16_t events_dropped;
};

/* ---- run ------------------------------------------------------------- */

/* Build a run from a seed. It is TIMBER_RUN_READY: no tick counts until it
 * is started. */
void timber_run_new(struct timber_run *run, uint32_t seed);
/* Begin. Returns 0, or -1 when the run is not TIMBER_RUN_READY. */
int timber_run_start(struct timber_run *run);
/* One step of TIMBER_TICK_MS. Does nothing unless the run is active or
 * collapsing, so a caller that keeps ticking a finished run cannot corrupt
 * it. */
void timber_run_tick(struct timber_run *run);
int timber_run_is_over(const struct timber_run *run);

/* ---- selection ------------------------------------------------------- */

/* Select a pullable block. Selecting the block already selected changes
 * nothing. While a block is part way out the selection is locked to it and
 * any other choice is refused; while a block is in hand nothing can be
 * selected. Returns 0, or -1 when refused. */
int timber_run_select(struct timber_run *run, int id);
/* Drop the selection. Refused (-1) while a block is part way out. */
int timber_run_deselect(struct timber_run *run);
/* The selected block's id, or -1. */
int timber_run_selected(const struct timber_run *run);

/* ---- events ---------------------------------------------------------- */

/* Take the oldest pending event. Returns 1 and fills out, or 0 when the
 * queue is empty. */
int timber_run_take_event(struct timber_run *run, struct timber_event *out);
void timber_run_clear_events(struct timber_run *run);

#endif
