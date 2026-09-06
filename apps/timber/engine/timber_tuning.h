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

#endif
