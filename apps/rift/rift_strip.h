/*
 * The section strip: RIFT's own top row. The four section tabs with the
 * unread pill on COMMS, the accent underline of the active one (handoff §3),
 * the right-hand caption with the landscape key hints and counts, and in
 * landscape - where the shell builds no app header for RIFT (app.h `header`,
 * DS §37.2) - the back slab at its left. A 64 px row in both orientations,
 * the back slab and every tab a visible 56 px face in one look (DS §51.3).
 *
 * Split from rift_app.c, which owns the frame and the lifecycle, so neither
 * is a monolith (tests/rift_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_STRIP_H
#define RIFT_STRIP_H

#include "rift_app.h"

void rift_tabs_build(struct rift_app *a);
/* The active tab and the unread pill. */
void rift_tabs_paint(struct rift_app *a);
/* The right caption: key hints and mesh counts, landscape only. */
void rift_tabs_paint_caption(struct rift_app *a);
/* A tab's word: its full one, or the short one a narrow row takes. */
const char *rift_tab_word(int tab, int short_word);
/* Heights and the back slab for the shape. */
void rift_tabs_shape(struct rift_app *a);

#endif
