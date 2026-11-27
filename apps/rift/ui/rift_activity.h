/*
 * ACTIVITY: the mesh as this node hears it, live.
 *
 * No settings - those are SYSTEM's. Led by MESH ACTIVITY: the figures (frames
 * heard in the last minute and in the span, how long since the last, the
 * mean RSSI, nodes heard, frames sent), the live graph (ui/rift_graph.h: a
 * sixty-second scope over sixty bars stacked by payload type, in the packet
 * palette), the mix of types and the legend that names them. A tap on the
 * graph turns its span between twenty minutes and five; a tap on a name in
 * the legend shows that type alone. Then the LIVE FEED frame by frame, what
 * state the radio service is in and why, the repeaters heard directly and
 * the nodes heard most recently. Every figure is one the service reported;
 * the signal is shown only where one was measured.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_ACTIVITY_H
#define RIFT_ACTIVITY_H

#include "rift_app.h"

lv_obj_t *rift_activity_create(struct rift_app *app, lv_obj_t *parent);
void rift_activity_refresh(struct rift_app *app);
void rift_activity_shape(struct rift_app *app);
void rift_activity_destroy(struct rift_app *app);
/* The traffic graph (ui/rift_graph.h), for tests; NULL before the screen
 * exists. */
lv_obj_t *rift_activity_graph(const struct rift_app *app);
/* The RX LOG action under the feed, for tests; NULL before the screen
 * exists. */
lv_obj_t *rift_activity_rxlog_button(const struct rift_app *app);
/* The TROPO line over the graph (ui/rift_tropo_view.h), for tests; NULL
 * before the screen exists. */
lv_obj_t *rift_activity_tropo(const struct rift_app *app);

#endif
