/*
 * The selected node, in full. One builder, two homes: the pane on the
 * right of the landscape split, and the screen a portrait DETAIL push
 * opens. Landscape is a recomposition, not a different feature
 * (docs/design/rift/HANDOFF.md §9), so the two cannot be allowed to drift
 * into saying different things about the same node.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_DETAIL_H
#define RIFT_DETAIL_H

#include "rift_app.h"

struct rift_detail;

/* compact: the landscape context pane, which has a title row of its own and
 * no pushed-screen chrome. Otherwise the portrait DETAIL screen. */
struct rift_detail *rift_detail_create(struct rift_app *app, lv_obj_t *parent, int compact);
void rift_detail_refresh(struct rift_detail *d, const struct rift_node *n);
/* The reader has left the detail - another section, the list, the other
 * orientation - with a FORGET confirmation up: that is a Cancel. */
void rift_detail_cancel_confirm(struct rift_detail *d);
void rift_detail_destroy(struct rift_detail *d);

#endif
