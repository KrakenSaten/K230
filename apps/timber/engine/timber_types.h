/*
 * PocketTimber vocabulary: the tower's units and geometry, block classes,
 * run and turn states, collapse causes and the event stream.
 *
 * PocketTimber is a game: a tower of wooden blocks on a table, taken apart
 * one block at a time and stacked back on top until it falls. Nothing in it
 * observes the physical world; every block is invented by timber_rng.c from
 * a recorded seed.
 *
 * Geometry. The tower stands on a square footprint three block widths on a
 * side. One block width is TIMBER_UNIT (256, so positions are Q8.8 fixed
 * point), a block is three units long and TIMBER_BLOCK_HEIGHT tall. Layer 0
 * rests on the felt; even layers run along x and odd layers along y, so each
 * layer crosses the one below it. Within a layer a block occupies slot 0, 1
 * or 2 across the layer's axis. A block's extraction is its displacement
 * along its own axis, signed: positive toward +x or +y, which is the face
 * the player looks at, negative toward the far face.
 *
 * Integers throughout: the engine does no floating-point arithmetic and
 * calls no libm, so a run replays bit-identically on the host and on the
 * K230, and tests/timber_lint.sh keeps it that way.
 *
 * Time. Everything in the engine is counted in ticks of TIMBER_TICK_MS
 * (timber_tuning.h). The engine never reads a clock; the caller decides
 * when a tick happens.
 *
 * Pure C, no LVGL and no I/O, so the whole engine is unit-tested natively.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_TYPES_H
#define POCKETTIMBER_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "timber_tuning.h"

/* One block width, and the fixed-point scale of every position. */
#define TIMBER_UNIT 256
#define TIMBER_BLOCK_LENGTH (3 * TIMBER_UNIT)
/* 0.6 widths, the proportion of the classic block. The engine only needs
 * it for the vertical axis of a collapse; the view has its own pixels. */
#define TIMBER_BLOCK_HEIGHT 154
/* A block is fully out when its extraction reaches its own length. */
#define TIMBER_EXTRACTION_MAX TIMBER_BLOCK_LENGTH

#define TIMBER_SLOTS 3
#define TIMBER_LAYERS_BASE 18
#define TIMBER_BLOCKS (TIMBER_LAYERS_BASE * TIMBER_SLOTS)
/* Blocks only ever move upward, so the tower can never be taller than the
 * base plus every block restacked on top. Array bound, not a rule. */
#define TIMBER_LAYERS_MAX (TIMBER_LAYERS_BASE + TIMBER_BLOCKS / TIMBER_SLOTS)

/* The most blocks that can ever sit above one: everything but its own
 * layer. Fixed for the run so a class means the same thing all game. */
#define TIMBER_LOAD_MAX (TIMBER_BLOCKS - TIMBER_SLOTS)

/* Block ids are 0 .. TIMBER_BLOCKS-1 and stable for the whole run; this is
 * the value of an empty grid cell and of "no block" everywhere a uint8_t
 * holds an id. */
#define TIMBER_NO_BLOCK 0xFFu

/* Mass is Q8.8 too. Every block is one unit in v0.1; the field exists so a
 * heavier wood is a table entry later, not a model change. */
#define TIMBER_MASS_ONE 256

enum timber_axis {
    TIMBER_AXIS_X = 0,
    TIMBER_AXIS_Y,
    TIMBER_AXIS_COUNT
};

/* How tightly a block sits, as the player learns it from a TEST. Derived
 * from the hidden seat and the load above the block (timber_pull.c), so a
 * block's class can change as the tower is rebuilt over it. */
enum timber_class {
    TIMBER_CLASS_FREE = 0,
    TIMBER_CLASS_EASY,
    TIMBER_CLASS_FIRM,
    TIMBER_CLASS_STUCK,
    TIMBER_CLASS_COUNT
};

enum timber_run_state {
    TIMBER_RUN_READY = 0,       /* built from a seed, not yet started */
    TIMBER_RUN_ACTIVE,
    TIMBER_RUN_COLLAPSING,      /* the choreography is playing out */
    TIMBER_RUN_OVER,
    TIMBER_RUN_STATE_COUNT
};

/* Where a turn is. SELECT: nothing is out; the player may pick, test and
 * begin a pull. PULLING: one block is part way out and the selection is
 * locked to it. PLACING: a block is in hand and must go on top. */
enum timber_turn {
    TIMBER_TURN_SELECT = 0,
    TIMBER_TURN_PULLING,
    TIMBER_TURN_PLACING,
    TIMBER_TURN_COUNT
};

/* Why the tower fell, for the result screen. Classified at the moment the
 * hinge layer's margin crosses zero (timber_rules.c). */
enum timber_cause {
    TIMBER_CAUSE_NONE = 0,
    TIMBER_CAUSE_TIP,           /* the support was pulled out from under the stack */
    TIMBER_CAUSE_JOLT,          /* a rough pull leaned it over */
    TIMBER_CAUSE_PLACEMENT,     /* the block placed on top was the last straw */
    TIMBER_CAUSE_SWAY,          /* it was standing still; the sway tipped it */
    TIMBER_CAUSE_COUNT
};

enum timber_event_type {
    TIMBER_EVENT_NONE = 0,
    TIMBER_EVENT_SELECT,
    TIMBER_EVENT_DESELECT,
    TIMBER_EVENT_TEST,          /* value: the class revealed */
    TIMBER_EVENT_STICK,         /* a tight block broke free */
    TIMBER_EVENT_JOLT,          /* value: the jolt's size */
    TIMBER_EVENT_SLIP,          /* the block came out; value: points */
    TIMBER_EVENT_PLACE,
    TIMBER_EVENT_LAYER,         /* a layer was completed; value: points */
    TIMBER_EVENT_CREAK,         /* a turn began with little margin left */
    TIMBER_EVENT_COLLAPSE,      /* value: the hinge layer */
    TIMBER_EVENT_LAND,          /* a falling block came to rest */
    TIMBER_EVENT_OVER,          /* value: the final score */
    TIMBER_EVENT_COUNT
};

/* An axis-aligned footprint in Q8.8 world units, half-open on both axes. */
struct timber_rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

/* ---- names ------------------------------------------------------------ */

/* Display names, or "?" for an out-of-range value. */
const char *timber_class_name(enum timber_class cls);
const char *timber_run_state_name(enum timber_run_state state);
const char *timber_turn_name(enum timber_turn turn);
const char *timber_cause_name(enum timber_cause cause);
const char *timber_event_name(enum timber_event_type type);

/* ---- geometry --------------------------------------------------------- */

/* The axis a layer's blocks run along: x for even layers, y for odd. */
int timber_layer_axis(int layer);
/* The footprint of a block in this layer and slot, displaced along its axis
 * by extraction. Returns 0, or -1 (out untouched) for a layer, slot or
 * extraction outside the tower. */
int timber_block_rect(int layer, int slot, int32_t extraction, struct timber_rect *out);
/* Area in Q8.8 squared, 0 for an empty or inverted rectangle. */
int64_t timber_rect_area(const struct timber_rect *r);
/* The intersection. Returns 1 and fills out when it is not empty, else 0
 * with out untouched. out may alias a or b. */
int timber_rect_intersect(const struct timber_rect *a, const struct timber_rect *b,
                          struct timber_rect *out);
/* Area of the intersection, 0 when the rectangles do not overlap. */
int64_t timber_rect_overlap(const struct timber_rect *a, const struct timber_rect *b);
/* The centre, rounded toward negative infinity. */
void timber_rect_centre(const struct timber_rect *r, int32_t *cx, int32_t *cy);

#endif
