/*
 * PocketClock persistence: the alarms and the last timer duration, in one
 * small text file.
 *
 * What is NOT here is as deliberate as what is. The stopwatch and a running
 * timer are elapsed time on the monotonic clock, which does not survive a
 * reboot and cannot be reconstructed from anything on disk, so writing them
 * down would only produce a stored number that becomes a lie the moment the
 * board powers off - and writing them down on every tick would put a flash
 * write behind every centisecond of the display.
 *
 * The write is the Notes one: temp file, fsync, rename, so a reader sees
 * either the previous set of alarms or this one and never half of either.
 *
 * This is the only file in the app that touches the filesystem
 * (tests/clock_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

const char *clock_store_dir(void);
/* Absolute path of the settings file. Returns 0, or -1 when it would not
 * fit. */
int clock_store_path(char *out, size_t out_len);

/* Load the alarms and the timer duration into an already-initialised engine.
 * Returns 0 on success, 1 when there is no file yet, and -1 when one exists
 * but could not be used - in which case the engine is left as it was, with
 * no alarms, rather than half-filled.
 *
 * fired_day is never restored. Whether an alarm has already rung is a fact
 * about today, and the engine works it out on its first valid reading; a
 * stored value would only be wrong after a reboot on another day. */
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

#endif
