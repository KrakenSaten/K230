/*
 * The probe app of the simulator's tests (app_probe.c).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_APP_PROBE_H
#define POCKETOS_APP_PROBE_H

#include "app.h"

#include <stddef.h>

/* The apps a test asked for through POCKETOS_TEST_PROBE, at most `max`:
 * none unless it is set, and none at all outside the simulator. */
size_t app_probe_apps(const struct pocketos_app **out, size_t max);

#endif
