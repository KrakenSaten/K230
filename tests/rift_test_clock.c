/*
 * The virtual clock rift_app_test links in place of apps/rift/rift_clock.c.
 * See tests/rift_test_clock.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_test_clock.h"

#include "rift_model.h"

static int64_t virtual_ms = RIFT_TEST_CLOCK_START_MS;

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
