/*
 * The one clock runtime: the alarms and the countdown that keep running when
 * PocketClock is not on screen.
 *
 * WHY THIS EXISTS. An alarm that only rings while its app is open is not an
 * alarm. The v0.1 lifecycle has no background apps - an app is created when
 * it is opened and destroyed when it is left (ADR-002) - so the engine had
 * to move out of the app and somewhere that outlives it. It did not move
 * into a new daemon: a service would need a binary, an init script, a
 * supervisor entry and an IPC push to the shell to draw the alert, and the
 * shell is already the process with the panel, a once-a-second tick and the
 * one-instance pattern that the touch keyboard set (DS §17.4).
 *
 * So: exactly one engine, created and stepped by the shell, and PocketClock
 * is the client that configures and displays it - the same relationship the
 * app has with the keyboard. The shell steps; nothing else does.
 *
 * WHAT DID NOT MOVE. The timing rules are still clock_engine.c, the alarms
 * are still stored by clock_store.c, alarms are still wall-clock and the
 * stopwatch and the countdown are still monotonic. This file owns an
 * instance and a tick, and decides nothing about time.
 *
 * ONE ENGINE, AND MORE THAN ONE SHELL. An orientation change does not end
 * the boot, it ends the process: the shell execs itself in place (DS §21.2),
 * and this instance - a static in a process image that is about to be thrown
 * away - goes with it. So there are two calls at the two ends of that seam,
 * clock_runtime_handoff_save() on the way out and the handoff that
 * clock_runtime_init() takes on the way in, and between them the stopwatch,
 * the countdown and the snoozes keep running across a restart the way they
 * always did across an app being closed. The file they travel in is
 * boot-scoped, and why that is the whole of the argument is clock_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETCLOCK_RUNTIME_H
#define POCKETCLOCK_RUNTIME_H

#include "clock_engine.h"

/* Create the engine, load the stored alarms, and take the restart handoff if
 * the shell before this one left one. Returns what clock_store_load()
 * returned: 0 loaded, 1 nothing stored yet, -1 a stored file that could not
 * be used. Safe to call again; the second call is the first one's result.
 *
 * The handoff is not in that answer, because its absence is not news: the
 * first shell of a boot finds none, and so does the one after a crash, which
 * had no chance to write anything. clock_runtime_handoff_result() has it for
 * the caller that wants to log the difference.
 *
 * on_ring_change is called whenever something starts or stops ringing, so
 * the shell can put its alert up without waiting for its next tick. It may
 * be NULL. A ring the handoff brought back is announced on the first step,
 * like any other. */
int clock_runtime_init(void (*on_ring_change)(void));
/* What clock_handoff_load() said at init: 0 a handoff was taken, 1 there was
 * none, -1 there was one and it could not be used. */
int clock_runtime_handoff_result(void);
/* Forget everything, for tests and for an orderly shutdown. */
void clock_runtime_deinit(void);

/* Re-read both clocks without advancing anything. This is what a view calls
 * when it wants a fresh number to draw; it must never be the thing that
 * decides an alarm has gone off. */
void clock_runtime_read(void);

/* Re-read both clocks and advance the engine. THE SHELL'S TICK, AND NOTHING
 * ELSE. One stepper means an alarm cannot fire twice because two callers
 * both stepped, and it means the app being open changes nothing. */
void clock_runtime_step(void);

/* Advance to a reading supplied by the caller. For tests, which have to be
 * able to reach 07:30 without waiting until 07:30. */
void clock_runtime_step_at(const struct clock_now *now);

/* The engine, for the app to read and configure. The app must call
 * clock_runtime_save() after changing anything worth keeping. */
struct clock_engine *clock_runtime_engine(void);
/* The last reading, from whichever of the two calls above happened last. */
const struct clock_now *clock_runtime_now(void);

/* Write the alarms and the timer duration. Returns 0, or -1. Call it on a
 * change, never from a tick. */
int clock_runtime_save(void);

/* Hand this run's stopwatch, countdown, snoozes, ringing and fired-today to
 * whatever starts next in this boot. THE SHELL'S WAY OUT, AND NOTHING ELSE:
 * once, after the open app has been closed, whether the shell is stopping or
 * replacing itself for an orientation change.
 *
 * Returns what clock_handoff_save() returned: 0 written, 1 there was nothing
 * to hand off, -1 it could not be written. It is never called from a tick,
 * and it writes to a tmpfs, so it costs no flash write at all. */
int clock_runtime_handoff_save(void);

/* Stop whatever is ringing. An alarm is acknowledged - which switches a
 * one-shot alarm off, so this also saves - and an expired timer is
 * dismissed. Does nothing when nothing is ringing. */
void clock_runtime_stop_ringing(void);
/* Snooze a ringing alarm. Does nothing unless an alarm is ringing. */
void clock_runtime_snooze(void);

#endif
