/*
 * PocketTimber scoring: what a pull is worth, what a completed layer adds,
 * and the lifetime record a run is measured against.
 *
 * The model is small enough to hold in the head while playing:
 *
 *   points = base * depth * clean * untested * streak
 *
 * base comes from the block's class at the moment it slipped free. depth
 * adds TIMBER_SCORE_DEPTH_PCT percent per layer above the block, so a
 * block from the bottom of an eighteen-layer tower is worth two thirds as
 * much again as one from just under the top. clean is the bonus for a pull
 * with no jolt in it, untested the bonus for a block the player never
 * tested: the gamble. The streak multiplier rises TIMBER_SCORE_STREAK_STEP
 * percent for each consecutive clean pull already standing and stops at
 * TIMBER_SCORE_STREAK_CAP steps, so it tops out at 2.0 and a good run
 * cannot run away from an average one by an order of magnitude. The
 * factors are applied in that order, each truncating, so a replay scores
 * identically.
 *
 * Completing a layer on top is worth a flat bonus. A collapse ends the run
 * and takes nothing away: the risk of a hard pull is the run it might end,
 * not points already earned. A safe pull is always available and always
 * worth less.
 *
 * Integer arithmetic throughout.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_SCORE_H
#define POCKETTIMBER_SCORE_H

#include "timber_types.h"

#define TIMBER_SCORE_FREE 60
#define TIMBER_SCORE_EASY 100
#define TIMBER_SCORE_FIRM 180
#define TIMBER_SCORE_STUCK 300
/* Percent of base added per layer above the block. */
#define TIMBER_SCORE_DEPTH_PCT 4
/* Percent for a pull with no jolt, and for a block never tested. */
#define TIMBER_SCORE_CLEAN_PCT 150
#define TIMBER_SCORE_UNTESTED_PCT 125
/* Percent added per consecutive clean pull, and the number of steps before
 * it stops: 100, 120, 140, 160, 180, 200. */
#define TIMBER_SCORE_STREAK_STEP 20
#define TIMBER_SCORE_STREAK_CAP 5
/* A completed layer on top. */
#define TIMBER_SCORE_LAYER 250

struct timber_score {
    int32_t points;
    uint16_t streak;        /* consecutive clean pulls standing */
    uint16_t best_streak;   /* the longest reached in this run */
    uint16_t pulls;         /* blocks pulled free */
    uint16_t clean;         /* of which without a jolt */
    uint16_t layers_built;  /* layers completed on top */
    uint8_t height;         /* the most layers the tower reached */
};

/* What a run leaves behind. Small on purpose: a best score and enough
 * counters to say whether this run was a good one, and nothing that would
 * turn into a progression system. */
struct timber_record {
    uint32_t best_score;
    uint16_t best_height;
    uint16_t best_streak;
    uint32_t runs;
    uint32_t pulls;         /* lifetime blocks pulled free */
};

void timber_score_init(struct timber_score *score);

/* Base points for a class, 0 for an out-of-range class. */
int32_t timber_score_base(enum timber_class cls);
/* What pulling a block of this class from under layers_above layers is
 * worth, clean or not, tested or not, with a streak already standing:
 * the number the UI shows beside the piece before it is pulled. */
int32_t timber_score_value(enum timber_class cls, int layers_above, int clean, int tested, int streak);

/* Record a block pulled free and return what it was worth. A clean pull
 * extends the streak; a jolted one ends it. */
int32_t timber_score_pull(struct timber_score *score, enum timber_class cls, int layers_above,
                          int clean, int tested);
/* Record a layer completed on top and return what it was worth. */
int32_t timber_score_layer(struct timber_score *score);
/* Note the tower's height in layers; the record keeps the tallest. */
void timber_score_height(struct timber_score *score, int layers);

void timber_record_init(struct timber_record *record);
/* Fold a finished run into the record. Returns 1 when it set a new best
 * score, which is the one thing the app announces. */
int timber_record_note_run(struct timber_record *record, const struct timber_score *score);

#endif
