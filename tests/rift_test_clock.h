/*
 * A virtual monotonic clock for rift_app_test (tests/rift_test_clock.c).
 *
 * rift_app_test links this in place of apps/rift/rift_clock.c, so every
 * rift_mono_ms() the app, its model and its client read - and every fixture
 * the test stamps - comes from here. It starts at a fixed instant and moves
 * only when the test moves it, in step with the LVGL tick, so an age drawn
 * as "45 s" is 45 s in every run however slow the run is. Test-only: no
 * target the shell or an image is built from links this file.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_TEST_CLOCK_H
#define RIFT_TEST_CLOCK_H

#include <stdint.h>

/* The instant the virtual clock starts at, in milliseconds. Arbitrary but
 * fixed, and far enough from zero that fixtures stamped minutes in the past
 * are still positive. */
#define RIFT_TEST_CLOCK_START_MS 1000000000LL
/* The virtual wall clock's start (RX LOG's times of day):
 * 2026-10-05T12:00:00.000Z. */
#define RIFT_TEST_WALL_START_MS 1791201600000LL

/* Move the clock forward by ms (negative values are ignored: a monotonic
 * clock does not go back). */
void rift_test_clock_advance(int64_t ms);

#endif
