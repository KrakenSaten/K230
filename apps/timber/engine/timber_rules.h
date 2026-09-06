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
 * The tower answers. Disturbance decays every tick and sways the tower
 * while it lasts; the stability model (timber_stability.c) turns the
 * tower, the lean and the sway into a margin per layer, and the hinge is
 * the layer with the least of it. A block that lets go of the stack while
 * it is being drawn out shifts the stack onto the blocks left, which is a
 * disturbance of its own. The tower creaks when the hinge's static margin
 * falls below the creak line.
 *
 * Placing the block in hand on top ends the turn: the block is reseated
 * loose, the tower takes a small knock, and a block placed off centre on an
 * incomplete layer nudges the lean toward that side, so placing against
 * the lean is the correct play. Completing a layer unlocks the one that
 * was below it and pays a bonus.
 *
 * The trigger. After every tick and every act the run measures the tower;
 * when the hinge's effective margin is below zero the tower falls there,
 * and the cause is read from what the player did last: a placement within
 * TIMBER_CAUSE_PLACE_TICKS, else a jolt within TIMBER_CAUSE_JOLT_TICKS,
 * else TIP when the static margin itself is gone (a support pulled out),
 * else SWAY (the tower was standing; the sway from a test or a shift
 * tipped it). The run is then COLLAPSING until every block rests or
 * TIMBER_COLLAPSE_TICKS_MAX have passed, and then OVER. The score is kept.
 *
 * The only other way a run ends is the summit: a placement that completes
 * the top of a tower already at TIMBER_LAYERS_MAX. The next block pulled
 * would have nowhere to go, so the run is OVER standing, cause NONE.
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

#include "timber_collapse.h"
#include "timber_pull.h"
#include "timber_rng.h"
#include "timber_score.h"
#include "timber_stability.h"
#include "timber_tower.h"
#include "timber_types.h"

/* Tests a turn allows. Two is a real allocation problem; unlimited testing
 * would make the correct play a slow one. */
#define TIMBER_TESTS_PER_TURN 2

/* What placing a block does to the tower: a knock of 0.25, and, off
 * centre on an incomplete layer, a lean of 0.005 widths per layer toward
 * that side (Q16.16). The nudge is a design decision, not physics: it is
 * what makes placing against the lean the correct play. */
#define TIMBER_PLACE_IMPULSE 64
#define TIMBER_PLACE_LEAN 328

/* How long after a placement or a jolt the collapse is blamed on it. The
 * sway peaks a quarter period after a knock, five ticks. */
#define TIMBER_CAUSE_PLACE_TICKS 6
#define TIMBER_CAUSE_JOLT_TICKS 10

/* "It never happened", for the tick of the last jolt or placement. */
#define TIMBER_NEVER 0xFFFFFFFFu

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

    /* What play has done to the tower. */
    uint16_t disturb;       /* Q8.8, 0 to TIMBER_DISTURB_MAX; decays every tick */
    uint8_t disturb_axis;   /* enum timber_axis of the last disturbance */
    int8_t disturb_sign;    /* its direction along that axis */
    uint16_t sway_phase;    /* 1/65536 turn; restarts at every disturbance */
    int16_t sway;           /* displacement of the top along disturb_axis, Q8.8 */
    int32_t lean_x;         /* Q16.16 widths per layer */
    int32_t lean_y;

    /* What the tower says back, refreshed every tick and after every act. */
    uint8_t hinge;          /* the layer with the least margin, or TIMBER_NO_LAYER */
    uint8_t hinge_axis;     /* enum timber_axis the hinge would tip along */
    int8_t hinge_sign;      /* and which way */
    uint8_t creaking;       /* the static margin is under the creak line */
    int32_t margin_static;  /* Q8.8 at the hinge */
    int32_t margin_eff;     /* the same with the sway */

    /* For blaming a collapse on the right thing. */
    uint32_t last_jolt_tick;    /* or TIMBER_NEVER */
    uint32_t last_place_tick;
    uint8_t cause;              /* enum timber_cause, once collapsing */
    uint16_t collapse_ticks;    /* ticks since the collapse began */
    struct timber_collapse collapse;    /* the choreography, once collapsing */

    struct timber_score score;

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
/* What pulling this block clean, right now, would be worth: the number
 * the piece card shows. 0 for a block not in the tower. */
int32_t timber_run_worth(const struct timber_run *run, int id);
/* Why the tower fell, TIMBER_CAUSE_NONE while it stands. */
int timber_run_cause(const struct timber_run *run);
/* Where a block is while and after the tower falls, or NULL while it
 * stands: the view draws a falling block from here and a standing one from
 * its cell. */
const struct timber_fall *timber_run_fall(const struct timber_run *run, int id);

/* ---- the tower's state ----------------------------------------------- */

/* The layer with the least effective margin, or -1 when no layer carries
 * anything. */
int timber_run_hinge(const struct timber_run *run);
/* The effective margin at the hinge, Q8.8 widths; TIMBER_MARGIN_FULL when
 * there is no hinge. */
int32_t timber_run_margin(const struct timber_run *run);
/* The stability meter: the effective margin as a share of a full one,
 * 0 to 1000. */
int timber_run_stability_permille(const struct timber_run *run);

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
/* Put the block in hand on top, in this slot of the placement layer. Ends
 * the turn: two fresh tests, nothing selected. Returns 0, or -1 when the
 * run is not active, nothing is in hand, or the slot is taken or outside
 * the layer. The placement may be the last straw. */
int timber_run_place(struct timber_run *run, int slot);

/* ---- events ---------------------------------------------------------- */

/* Take the oldest pending event. Returns 1 and fills out, or 0 when the
 * queue is empty. */
int timber_run_take_event(struct timber_run *run, struct timber_event *out);
void timber_run_clear_events(struct timber_run *run);

#endif
