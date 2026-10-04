/*
 * ACTIVITY's REPEATERS 0-HOP panel: the SCAN 0-HOP button and the repeaters
 * that answered it - the ones THIS node hears directly (rift_repeater.h,
 * docs/api/mesh.md "Repeater control"). A row opens that repeater's page.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_SCAN_H
#define RIFT_SCAN_H

#include "rift_app.h"

/* Rows the panel shows; meshcored keeps 16, and the rest are a count. */
#define RIFT_SCAN_ROWS 8

void rift_scan_build(struct rift_app *app, lv_obj_t *parent);
void rift_scan_refresh(struct rift_app *app);
void rift_scan_destroy(struct rift_app *app);

/* For the tests. */
lv_obj_t *rift_scan_button(const struct rift_app *app);
lv_obj_t *rift_scan_row(const struct rift_app *app, int i);
lv_obj_t *rift_scan_caption(const struct rift_app *app);

#endif
