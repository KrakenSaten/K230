/*
 * The conversation list, virtual: as NODES draws its nodes (ui/rift_nodes.c),
 * a spacer as tall as every row would be and a small pool of rows placed over
 * whatever part of it is on screen. A conversation list of 256 costs the same
 * objects as one of 12; a re-ordering moves rows and rebuilds nothing; the
 * open conversation is found by key, never by index.
 *
 * Owned by rift_comms.c, which decides what the conversations are (the model's
 * plus every joined channel plus the one being written to) and hands them
 * over on every refresh; this file only draws them.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_CONV_LIST_H
#define RIFT_CONV_LIST_H

#include "rift_app.h"

struct rift_conv_list;

/* Build the list into parent: the scrolling object and nothing in it yet. */
struct rift_conv_list *rift_conv_list_create(struct rift_app *app, lv_obj_t *parent);
void rift_conv_list_destroy(struct rift_conv_list *l);
/* The scrolling object, for the pane to size (portrait gives it a height). */
lv_obj_t *rift_conv_list_obj(struct rift_conv_list *l);
/* The shape changed: every row is rebound from nothing at the new widths.
 * wide hides the preview and the route (the thread's header has both). */
void rift_conv_list_shape(struct rift_conv_list *l, int wide);
/* Draw these conversations, in this order, with `open` (may be NULL) as the
 * selected row. reveal brings the open row into view. The list keeps a copy,
 * because a scroll binds rows between refreshes. */
void rift_conv_list_refresh(struct rift_conv_list *l, const struct rift_conv *conv, int count,
                            const char *open, int64_t now, int reveal);
/* Rows in the pool, for the test: bounded by the pane, not by the count. */
int rift_conv_list_rows_built(const struct rift_conv_list *l);
/* The height every row would take laid out, for the portrait pane. */
int32_t rift_conv_list_total_h(const struct rift_conv_list *l);

#endif
