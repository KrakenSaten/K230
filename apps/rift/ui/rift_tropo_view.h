/*
 * ACTIVITY's TROPO line: the warning sign over the MESH ACTIVITY graph and
 * what it is about (rift_tropo.h).
 *
 *   ▲ TROPO DETECTED                                   4m
 *     ACTIVE · PEAK 27 HOPS · 6 DEEP PACKETS OVER 9m
 *     Unusual mesh propagation: not proof of weather or distance.
 *
 * Its look follows the opening's age (rift_tropo_level): red with a red
 * outline in its first hour, yellow to five hours, grey after; with nothing
 * detected since RIFT's session began, an outlined grey sign and one line
 * that says what it is watching for. When there is nothing to watch with -
 * a meshcored that keeps no receive log, or none answering - it says so
 * rather than reading as a quiet mesh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_TROPO_VIEW_H
#define RIFT_TROPO_VIEW_H

#include "rift_app.h"

lv_obj_t *rift_tropo_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_tropo_view_refresh(lv_obj_t *view, struct rift_app *app, int64_t now);
/* For the tests: the headline and the detail as drawn, and the sign. */
const char *rift_tropo_view_headline(lv_obj_t *view);
const char *rift_tropo_view_detail(lv_obj_t *view);
lv_obj_t *rift_tropo_view_sign(lv_obj_t *view);

#endif
