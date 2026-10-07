/*
 * The only place in PocketClock that reads a real clock.
 *
 * Everything else takes a struct clock_now it was handed, which is what lets
 * the engine and its rules be tested at any instant in history without the
 * test suite sleeping (tests/clock_lint.sh checks that the engine contains no
 * clock call of its own).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETCLOCK_TIME_H
#define POCKETCLOCK_TIME_H

#include "clock_engine.h"

/* Break a UTC epoch down into local time. Sets valid from
 * CLOCK_WALL_VALID_FROM: below it this is not a time, it is a board that
 * booted without an RTC, and saying so is the whole point.
 *
 * Pure apart from localtime_r, so the validity rule and the date arithmetic
 * are testable for any epoch (tests/clock_time_test.c runs under TZ=UTC). */
void clock_wall_from_epoch(struct clock_wall *w, int64_t epoch);

/* Read both clocks, together. CLOCK_REALTIME for the wall, CLOCK_MONOTONIC
 * for elapsed time - never one derived from the other. */
void clock_now_read(struct clock_now *now);

/* "Monday 11 September", or "" when the wall clock is not valid. */
void clock_format_date(const struct clock_wall *w, char *out, size_t out_len);
/* "07:30". Always two digits, 24-hour, and never invented: out is "--:--"
 * when the clock is not set. */
void clock_format_wall(const struct clock_wall *w, char *out, size_t out_len);
/* "07:30" for an arbitrary hour and minute (an alarm, not the clock). */
void clock_format_hm(int hour, int minute, char *out, size_t out_len);

#endif
