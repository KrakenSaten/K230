/*
 * A scripted vision provider, for development and tests, until the real
 * pipeline exists. It is a provider like any other (db_vision.h): the brain
 * cannot tell it from a camera.
 *
 * A script is a list of steps, separated by commas or spaces:
 *
 *     <ms>:<kind>[@<confidence>]     e.g.  500:person 1500:owner@930 6000:none
 *     loop                           start again 1 s after the last step
 *
 * <ms> counts from start() and never goes backwards; <kind> is a
 * db_vision_kind_name(); <confidence> is 0..1000 per-mille. A script with a
 * malformed step is refused whole - a half-run demo would show something
 * nobody asked for.
 *
 * The app reads a script from $DESKBUDDY_SIM (docs/apps/DESKBUDDY.md, "Test
 * and demo controls"); with the variable unset the app uses
 * db_vision_none_ops and this file does nothing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_VISION_MOCK_H
#define DB_VISION_MOCK_H

#include "db_vision.h"

#define DB_MOCK_MAX_STEPS 64
#define DB_MOCK_LOOP_GAP_MS 1000

struct db_mock_step {
    int64_t at_ms;
    enum db_vision_kind kind;
    int16_t confidence_pm;
};

struct db_vision_mock {
    struct db_mock_step step[DB_MOCK_MAX_STEPS];
    int n;
    int next;
    bool loop;
    bool running;
    int64_t t0;
};

/* Replace the script. Returns the number of steps, or -1 (malformed, too
 * many steps, or NULL) leaving the mock with no steps. */
int db_vision_mock_load(struct db_vision_mock *m, const char *script);
/* One event now, outside the script (the developer keys). */
void db_vision_mock_inject(struct db_vision_mock *m, enum db_vision_kind kind, int64_t now_ms,
                           struct db_vision_queue *q);

extern const struct db_vision_provider_ops db_vision_mock_ops;

#endif
