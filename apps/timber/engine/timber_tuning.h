/*
 * PocketTimber hardware-sensitive tuning.
 *
 * Every value in this file depends on a number nobody has taken on a K230
 * yet: the frame cost of a sprite-heavy custom widget, the GT9895 drag
 * event rate and latency, and whether a one-to-three pixel sway reads on a
 * 330 ppi panel (docs/apps/POCKETTIMBER.md, "Hardware gates"). They are
 * conservative placeholders, each HARDWARE VALIDATION REQUIRED, kept in one
 * file so the first bench pass has one place to go. The engine treats them
 * as constants; nothing else in the engine encodes a hardware assumption.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_TUNING_H
#define POCKETTIMBER_TUNING_H

/* Engine step in milliseconds. 40 ms is 25 Hz, chosen so a thumb drag is
 * sampled finely enough for the speed limit to be fair; 50 ms (20 Hz, the
 * PocketRadar rate) is the fallback if the K230 frame budget says so.
 * Every duration in the engine is a whole number of these ticks and every
 * per-tick rate is stated for this value with its derivation beside it, so
 * changing it means re-deriving them.
 * HARDWARE VALIDATION REQUIRED: gate "20/25 Hz target choice". */
#define TIMBER_TICK_MS 40

/* ---- the pull ---------------------------------------------------------- */

/* Pull travel is the finger's movement along the pull track, converted by
 * the view into Q8.8 block widths (the P7 placeholder is 56 px per width)
 * and handed to the engine once per tick. The limits below are per tick at
 * TIMBER_TICK_MS; derivation: px/s / 56 px per width * 256 / 25 ticks/s.
 * HARDWARE VALIDATION REQUIRED: gates "GT9895 drag event rate" and "drag
 * latency" decide whether these are fair to a real thumb. */
#define TIMBER_SPEED_FREE 110       /* 600 px/s */
#define TIMBER_SPEED_EASY 73        /* 400 px/s */
#define TIMBER_SPEED_FIRM 40        /* 220 px/s */
#define TIMBER_SPEED_STUCK 22       /* 120 px/s */
/* Travel a tight block absorbs before it breaks free: 16 and 28 px. */
#define TIMBER_STICTION_FIRM 73
#define TIMBER_STICTION_STUCK 128
/* How far it lurches when it does: 2 px. */
#define TIMBER_LURCH 9
/* Ticks of travel averaged before the speed limit is judged, so one uneven
 * touch sample is not a jolt. Widen it if the panel delivers events in
 * bursts. */
#define TIMBER_TRAVEL_WINDOW 3

/* ---- the tower's answer ------------------------------------------------ */

/* Disturbance decays by this factor per tick: 225/256, about 0.88, so a
 * knock is a twentieth of itself a second later at 25 Hz. For another
 * tick rate keep the per-second decay: factor = 0.04^(TICK_MS/1000). */
#define TIMBER_DISTURB_DECAY 225
/* The sway period in ticks: 0.8 s at 25 Hz. The phase is a sixteen-bit
 * turn, so the step is the turn over the period. */
#define TIMBER_SWAY_PERIOD_TICKS 20
#define TIMBER_SWAY_STEP (65536 / TIMBER_SWAY_PERIOD_TICKS)
/* How far the top of the tower sways per unit of disturbance: 0.10 widths
 * (Q8.8 per 256 of disturbance). Whether that reads on the panel depends
 * on the view's pixels per width.
 * HARDWARE VALIDATION REQUIRED: gate "1 to 3 px sway readability". */
#define TIMBER_SWAY_AMP 26

#endif
