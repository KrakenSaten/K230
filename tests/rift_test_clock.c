/*
 * The virtual clock rift_app_test links in place of apps/rift/rift_clock.c.
 * See tests/rift_test_clock.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_test_clock.h"

#include "rift_model.h"
#include "rift_rxlog.h"

static int64_t virtual_ms = RIFT_TEST_CLOCK_START_MS;

/* The wall clock moves with the monotonic one, from a fixed instant:
 * 2026-10-05T12:00:00.000Z when the monotonic clock is at its start. */
int64_t rift_rxlog_wall_now(void)
{
    return RIFT_TEST_WALL_START_MS + (virtual_ms - RIFT_TEST_CLOCK_START_MS);
}

int64_t rift_mono_ms(void)
{
    return virtual_ms;
}

void rift_test_clock_advance(int64_t ms)
{
    if (ms > 0) {
        virtual_ms += ms;
    }
}
