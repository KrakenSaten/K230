/*
 * The route line of a direct conversation: how the next message to this peer
 * will be sent, and the way to change it (rift_route.h).
 *
 * One row under the portrait thread, and in landscape the route pane of
 * DETAILS - the console keeps its height for messages (DS §37.2), and its
 * header says CHOSEN ROUTE when one is. A direct conversation only: a channel
 * has no route, and never shows it. It says which of the two a SEND does, in
 * words and never by colour alone:
 *
 *   ROUTE AUTO          the service's own choice: flood, or the route it has
 *                       learned. What every send did before this existed.
 *   ROUTE EXPLICIT      the repeaters a reader chose in the route builder,
 *                       in order, or DIRECT with no relay. Never AUTO.
 *
 * SET ROUTE (EDIT ROUTE once one is set) opens the builder on MAP for this
 * peer; AUTO goes back to the service's choice. The builder is offered only
 * when this run of meshcored has said it takes a chosen route; otherwise the
 * line says why not.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_ROUTE_BAR_H
#define RIFT_ROUTE_BAR_H

#include "rift_app.h"

struct rift_route_bar;

/* One row under the portrait thread; stacked - the words on a line of their
 * own and the buttons under them - in landscape's narrow DETAILS pane. */
struct rift_route_bar *rift_route_bar_create(struct rift_app *app, lv_obj_t *parent, int stacked);
void rift_route_bar_destroy(struct rift_route_bar *b);
/* Draw it for this conversation (NULL, or a channel, hides it). */
void rift_route_bar_refresh(struct rift_route_bar *b, const char *peer);

/* Open the route builder for this peer, on MAP. Sends nothing. */
void rift_route_bar_open_builder(struct rift_app *app, const char *peer);

#endif
