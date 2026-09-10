/*
 * PocketTimber pull model: how tightly a block sits, how fast it may be
 * drawn, what it takes to break a tight one free, and what a rough pull
 * does to the tower.
 *
 * Tightness is the hidden seat scaled by the load above the block,
 *
 *     tightness = (255 - seat) / 255 * (0.5 + 0.5 * load / TIMBER_LOAD_MAX)
 *
 * in Q8.8, so a wedged block under the whole tower is 1.0 and a free block
 * is 0 whatever sits on it. The class the player is shown is tightness cut
 * at three thresholds. Load is the number of blocks above the block's
 * layer, so a block's class follows the tower: a TEST reveals the seat,
 * and the readout of a tested block is derived from it afresh each time it
 * is asked for.
 *
 * The pull is judged per tick. A FIRM or STUCK block absorbs a fixed travel
 * before it moves at all, then lurches; after that every block moves with
 * the finger. Travel averaged over TIMBER_TRAVEL_WINDOW ticks above the
 * class's limit is a jolt,
 *
 *     jolt = excess * (0.25 + tightness)
 *
 * which adds disturbance and leans the tower along the block's axis in the
 * direction of the pull (timber_rules.c). A free block dragged fast jolts
 * a little; a stuck block yanked jolts hard.
 *
 * The per-tick speeds and stiction distances live in timber_tuning.h
 * because they depend on the touch panel; everything here is game tuning.
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_PULL_H
#define POCKETTIMBER_PULL_H

#include "timber_types.h"

/* Tightness thresholds, Q8.8: below the first is FREE, then EASY, then
 * FIRM, then STUCK. 0.15, 0.35 and 0.65. */
#define TIMBER_TIGHT_EASY_FROM 38
#define TIMBER_TIGHT_FIRM_FROM 90
#define TIMBER_TIGHT_STUCK_FROM 166

/* Disturbance is Q8.8 and dimensionless: 256 is the sway a hard knock
 * leaves. What each act adds, and the ceiling. */
#define TIMBER_TEST_IMPULSE 38          /* 0.15 */
#define TIMBER_LURCH_IMPULSE 26         /* 0.10 */
#define TIMBER_JOLT_DISTURB 512         /* 2.0 per unit of jolt */
#define TIMBER_DISTURB_MAX 1024         /* 4.0 */
/* Lean is Q16.16 widths per layer. A unit of jolt leans the tower by a
 * hundredth of a width per layer along the pull. */
#define TIMBER_JOLT_LEAN 655

/* A block slips free of the tower once four fifths of it is out. */
#define TIMBER_SLIP_AT ((TIMBER_BLOCK_LENGTH * 4) / 5)

/* Tightness in Q8.8, 0 to 256. seat and load are clamped to their ranges. */
int32_t timber_pull_tightness(int seat, int load);
enum timber_class timber_pull_class(int seat, int load);
/* Largest per-tick travel the class allows before it jolts, Q8.8. */
int32_t timber_pull_limit(enum timber_class cls);
/* Travel absorbed before the block moves at all, Q8.8; 0 for a block that
 * moves at once. */
int32_t timber_pull_stiction(enum timber_class cls);
/* The jolt for travel excess over the limit at this tightness, Q8.8. 0 for
 * no excess. */
int32_t timber_pull_jolt(int32_t excess, int32_t tightness);

#endif
