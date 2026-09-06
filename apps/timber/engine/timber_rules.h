/*
 * PocketTimber run state and rules: the lifecycle of a run, the turn
 * state machine, selection, testing, the pull, and the event stream the
 * UI reacts to.
 *
 * A turn is SELECT -> (TEST)* -> PULL -> PLACE, and each step is a separate
 * thing the player knows. Selecting a block does nothing to the tower. A
 * TEST reveals a block's class, costs one of the turn's tests and a little
 * disturbance, and is sticky: a block tested once stays tested. The pull is
 * a sequence of per-tick travel actions. A tight block absorbs travel
 * before it moves, then lurches free; from the moment a block is part way
 * out the selection is locked to it, and pushing it fully back unlocks it
 * again. Travel faster than the block's class allows is a jolt, which
 * disturbs the tower and leans it along the pull. When four fifths of the
 * block is out it slips free into the player's hand, and placing it on top
 * ends the turn.
 *
 * What the tower does with disturbance and lean - decay, sway, margins and
 * the collapse - is milestone P3; here they only accumulate.
 *
 * Determinism. A run is reproduced by (seed, the ordered list of player
 * actions at their ticks). The generator is consumed only when the tower is
 * built and when a collapse begins; nothing the player does moves it.
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_RULES_H
#define POCKETTIMBER_RULES_H

#include "timber_pull.h"
#include "timber_rng.h"
#include "timber_tower.h"
#include "timber_types.h"

/* Tests a turn allows. Two is a real allocation problem; unlimited testing
 * would make the correct play a slow one. */
#define TIMBER_TESTS_PER_TURN 2

/* Lean is clamped at half a width per layer: far past anything that could
 * still be standing, and small enough that the arithmetic on it never
 * overflows. */
#define TIMBER_LEAN_MAX 32768

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
    uint8_t selected;       /* block id or TIMBER_NO_BLOCK; the held block while placing */
    uint8_t held;           /* the block in hand, or TIMBER_NO_BLOCK */
    uint8_t tests_left;     /* this turn's remaining TESTs */
    uint16_t turns;         /* completed turns, one per placement */
    uint32_t ticks;         /* ticks since the run started */
    struct timber_tower tower;

    /* The pull in progress. Reset whenever the selection changes. */
    uint8_t grip_broken;    /* the selected block has broken free */
    uint8_t pull_jolted;    /* a jolt happened during this pull */
    uint8_t travel_index;
    int16_t grip;           /* travel absorbed against stiction, Q8.8 */
    int16_t travel[TIMBER_TRAVEL_WINDOW];   /* recent per-tick travel magnitudes */

    /* What play has done to the tower. Accumulated here; decayed and
     * turned into consequences by the stability model (P3). */
    uint16_t disturb;       /* Q8.8, 0 to TIMBER_DISTURB_MAX */
    uint8_t disturb_axis;   /* enum timber_axis of the last disturbance */
    int8_t disturb_sign;    /* its direction along that axis */
    int32_t lean_x;         /* Q16.16 widths per layer */
    int32_t lean_y;

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

/* ---- blocks ---------------------------------------------------------- */

/* Blocks above this block's layer: the load it carries. */
int timber_run_load(const struct timber_run *run, int id);
/* The class the block has right now, or -1 for a block not in the tower.
 * Whether the player may see it is the UI's business (tested). */
int timber_run_class(const struct timber_run *run, int id);
/* The block in hand, or -1. */
int timber_run_held(const struct timber_run *run);

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

/* ---- testing and pulling --------------------------------------------- */

/* TEST the selected block: reveal its class. Costs one of the turn's tests
 * and nudges the tower. A block already tested answers again for free and
 * silently. Returns the class, or -1 when the run is not active, nothing
 * is selected, a block is part way out or in hand, or the turn's tests are
 * spent. */
int timber_run_test(struct timber_run *run);
/* One tick of finger travel on the selected block, Q8.8 block widths,
 * signed: positive draws the block toward the face the player looks at,
 * negative pushes it toward the far face. Returns 1 when the block moved,
 * 0 when it did not (stiction, or no travel), -1 when refused. A block that
 * reaches TIMBER_SLIP_AT either way slips free into the hand. */
int timber_run_pull(struct timber_run *run, int32_t travel);

/* ---- events ---------------------------------------------------------- */

/* Take the oldest pending event. Returns 1 and fills out, or 0 when the
 * queue is empty. */
int timber_run_take_event(struct timber_run *run, struct timber_event *out);
void timber_run_clear_events(struct timber_run *run);

#endif
