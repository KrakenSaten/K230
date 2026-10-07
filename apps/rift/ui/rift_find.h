/*
 * NODES' find bar: a search over the node list and the ZERO-HOP view.
 *
 * The node table is MeshCore's contact list - every node the service holds,
 * up to 256 - and this is how a reader finds one of them without scrolling
 * through all of it: part of a name, or the first hex of a key (the node
 * hash, which is how a hop is written). Case does not matter. The bar only
 * narrows what the list shows; it changes nothing the service or the cache
 * holds, and clearing it shows the whole list again.
 *
 * ZERO-HOP narrows the list to the repeaters this device hears with nothing
 * in between: those whose last advert came through no relay, or whose
 * learned route back has none (rift_node_zero_hop). That is information the
 * service already reports, and nothing is transmitted to gather it -
 * repeaters advert on their own schedule. Turning it on asks the service
 * for a fresh node list (mesh.nodes), which is a question to the service and
 * not a packet on the air.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_FIND_H
#define RIFT_FIND_H

#include "rift_app.h"

/* The bar, as the first row of the list pane. */
void rift_find_create(struct rift_app *app, lv_obj_t *parent);
/* The shape: a 56 px row in portrait, a data row in landscape, the way the
 * command line is (DS §37.2). */
void rift_find_shape(struct rift_app *app);
/* Draw the bar's own state: the ZERO-HOP toggle and whether CLEAR is there. */
void rift_find_refresh(struct rift_app *app);
/* The search field, for a test to type into. */
lv_obj_t *rift_find_field(const struct rift_app *app);
/* Set the query, as typing it would, and the zero-hop view. Neither sends
 * anything. */
void rift_find_set_query(struct rift_app *app, const char *query);
void rift_find_set_zero_hop(struct rift_app *app, int on);

#endif
