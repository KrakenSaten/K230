/*
 * RIFT's clock: CLOCK_MONOTONIC in milliseconds, the clock meshcored stamps
 * its events with. See rift_mono_ms in rift_model.h.
 *
 * It is alone in this file so that a test can link a clock of its own in its
 * place: tests/rift_test_clock.c gives rift_app_test a virtual clock that
 * moves only when the test moves it, so two runs of the same fixtures draw
 * the same ages. Nothing here knows about that, and nothing the shell links
 * can reach it - the shell and every host test link this file.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_model.h"
#include "rift_rxlog.h"

#include <time.h>

int64_t rift_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The wall clock, for RX LOG's times of day only: everything else in RIFT
 * is an age on the monotonic clock above. */
int64_t rift_rxlog_wall_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
