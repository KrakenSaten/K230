/*
 * NODES: every node this service has heard, densely, and the one that is
 * selected in full.
 *
 * Portrait is a single column of 36 px rows; the selected row expands in
 * place into a panel with the state, the path and a 56 px action bar
 * (RIFT-DEV-1: a 36 px row selects and never acts). The list is virtual -
 * a pool of rows over the part that is on screen - so it holds every node
 * the service does (RIFT_MAX_NODES) at the cost of a screenful. Landscape is the same
 * list beside a context pane holding the same detail - a recomposition,
 * not a second design (docs/design/rift/HANDOFF.md §9).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_NODES_H
#define RIFT_NODES_H

#include "rift_app.h"

lv_obj_t *rift_nodes_create(struct rift_app *app, lv_obj_t *parent);
void rift_nodes_refresh(struct rift_app *app);
/* Re-choose the shape from the room the app was given. */
void rift_nodes_shape(struct rift_app *app);
/* Arrows move the selection, Enter opens the detail, Esc leaves it: the
 * ordinary behaviour of a list in the one Doors focus group (DS §17.2,
 * §17.4), not a shortcut layer. Returns 1 when the key was used. */
int rift_nodes_key(struct rift_app *app, uint32_t key);
/* The reader left the node's detail: any FORGET confirmation it had up is
 * cancelled, in both the landscape pane and the portrait screen. */
void rift_nodes_cancel_confirm(struct rift_app *app);
void rift_nodes_destroy(struct rift_app *app);
/* How many row objects the list has built. The list is virtual: rows are a
 * pool placed over the part of it that is on screen, so this is bounded by
 * the height of the pane and not by the number of nodes. For tests. */
int rift_nodes_rows_built(const struct rift_app *app);

#endif
