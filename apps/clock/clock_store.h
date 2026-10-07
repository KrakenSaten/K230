/*
 * PocketClock persistence, in two files with two different lifetimes.
 *
 * 1. THE SETTINGS, $POCKETOS_STATE_DIR/clock/clock.conf. The alarms and the
 *    last timer duration, in one small text file that outlives a power cut.
 *
 *    What is NOT here is as deliberate as what is. The stopwatch and a
 *    running timer are elapsed time on the monotonic clock, which does not
 *    survive a reboot and cannot be reconstructed from anything on disk, so
 *    writing them down would only produce a stored number that becomes a lie
 *    the moment the board powers off - and writing them down on every tick
 *    would put a flash write behind every centisecond of the display.
 *
 * 2. THE RESTART HANDOFF, $POCKETOS_RUNTIME_DIR/clock/restart.state. What
 *    only this run knows, for the one case where the process ends but the
 *    boot does not: an orientation change restarts the shell in place with
 *    execv (DS §21.2, restart_in_place() in ui/shell/shell.c), which throws
 *    the one engine away and builds a new one from the settings file. A
 *    running stopwatch, a running countdown and a snooze were lost there.
 *
 *    This is not the lie the paragraph above refuses, and the difference is
 *    the directory. CLOCK_MONOTONIC is a per-boot clock: an exec does not
 *    disturb it, so a monotonic instant written before the exec still means
 *    the same instant after it. The one thing that must never happen is a
 *    monotonic instant outliving its boot - and /run is a tmpfs that starts
 *    empty on every boot (docs/hardware/T-DISPLAY-K230.md, VERIFIED on unit
 *    A), so it cannot. The handoff is boot-scoped by where it lives, exactly
 *    as pos-supervise's state files are. A monotonic reading that has gone
 *    backwards since the write refuses it as well, as a second line.
 *
 *    It is written once, on the way out of the shell, and never from a tick;
 *    it is on a tmpfs, so it costs no flash write at all. It is consumed -
 *    read and removed - so one exit hands off to exactly one start, and a
 *    crash, which writes nothing, hands off nothing.
 *
 * Both writes are the Notes one: temp file, fsync, rename, so a reader sees
 * one whole version or the other and never half of either.
 *
 * This is the only file in the app that touches the filesystem
 * (tests/clock_lint.sh), and like the engine it reads no clock: every
 * reading it needs is handed in.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETCLOCK_STORE_H
#define POCKETCLOCK_STORE_H

#include "clock_engine.h"

#include <stddef.h>

/* $POCKETOS_STATE_DIR/clock, default below. */
#define CLOCK_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define CLOCK_STORE_SUBDIR "clock"
#define CLOCK_STORE_FILE "clock.conf"
/* The first line, so a file from a later format is refused rather than
 * misread into plausible-looking alarms. */
#define CLOCK_STORE_MAGIC "pocketclock 1"

/* $POCKETOS_RUNTIME_DIR/clock, default below. The two roots are spelled out
 * here rather than taken from core/pocketpaths.h for the same reason the
 * settings root is: the app stores keep their own copies until the storage
 * work adopts that module (core/pocketpaths.h). They are the same strings. */
#define CLOCK_HANDOFF_DEFAULT_DIR "/run/pocketos"
#define CLOCK_HANDOFF_SUBDIR "clock"
#define CLOCK_HANDOFF_FILE "restart.state"
#define CLOCK_HANDOFF_MAGIC "pocketclock-restart 1"

const char *clock_store_dir(void);
/* Absolute path of the settings file. Returns 0, or -1 when it would not
 * fit. */
int clock_store_path(char *out, size_t out_len);

/* Load the alarms and the timer duration into an already-initialised engine.
 * Returns 0 on success, 1 when there is no file yet, and -1 when one exists
 * but could not be used - in which case the engine is left as it was, with
 * no alarms, rather than half-filled.
 *
 * fired_day is never restored from here. Whether an alarm has already rung
 * is a fact about today, and the engine works it out on its first valid
 * reading; a stored value would only be wrong after a reboot on another day.
 * The handoff below is a different matter: it is the same day by
 * construction, because it is the same boot. */
int clock_store_load(struct clock_engine *e);

/* Write the alarms and the timer duration. Returns 0, or -1.
 *
 * Call it when something changes - an alarm added, removed, switched,
 * acknowledged, a duration set - and never from a tick. */
int clock_store_save(const struct clock_engine *e);

/* True when the label is something this file can hold and give back exactly:
 * no newline, no control character, and short enough. The store refuses a
 * label it cannot round-trip rather than storing a mangled one. */
int clock_label_is_storable(const char *label);

/* ---- the restart handoff ----------------------------------------------- */

const char *clock_handoff_dir(void);
/* Absolute path of the handoff file. Returns 0, or -1 when it would not
 * fit. */
int clock_handoff_path(char *out, size_t out_len);

/* Remove the handoff file. Returns 0 whether or not one was there, -1 when
 * one is there and could not be removed. */
int clock_handoff_clear(void);

/* Write what the engine holds that the settings file does not and that only
 * this boot can make sense of: the stopwatch, the countdown, every snooze,
 * what is ringing, and which alarms have already rung today.
 *
 * now is the reading the caller has just taken; it is written down so the
 * next start can tell that the monotonic clock has not gone backwards since.
 *
 * Returns 0 when a handoff was written, 1 when there was nothing to hand off
 * - in which case any stale handoff is removed, so an exit with an idle
 * stopwatch cannot resurrect the one before it - and -1 on failure.
 *
 * Call it once, on the way out, and never from a tick. */
int clock_handoff_save(const struct clock_engine *e, const struct clock_now *now);

/* Take the handoff, if there is one, into an engine that clock_store_load()
 * has already filled: the alarms have to be there first, because the handoff
 * describes them by position and is refused if the list is not the one it
 * was written against.
 *
 * now is this start's reading. A countdown whose deadline went by while the
 * process was being replaced comes back EXPIRED, never RUNNING with a
 * deadline in the past; a snooze that came due in the same gap comes back
 * waiting, and the engine's next step rings it as it would have done.
 *
 * Returns 0 when a handoff was taken, 1 when there was none, and -1 when one
 * was there and could not be used - in which case the engine keeps exactly
 * what clock_store_load() left it.
 *
 * Either way the file is gone afterwards: one exit hands off to one start,
 * and a handoff that could not be read is not left to be refused again at
 * every start for the rest of the boot. */
int clock_handoff_load(struct clock_engine *e, const struct clock_now *now);

#endif
