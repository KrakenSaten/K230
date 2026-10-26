/*
 * TRAFFIC: MAP's mode for the traffic this device has heard.
 *
 * MAP's TRAFFIC button turns it on and off; it is not a section and not a
 * tab. While it is on, the panel beside (landscape) or under (portrait) the
 * map is the traffic panel, and the map adds to its own drawing the observed
 * path of one packet. Everything else on the map - markers, basemap, pan,
 * zoom, FIT, the node selection a tap makes - is MAP's own and unchanged.
 *
 *   - The packets are RX LOG's ring (rift_rxlog.h), the session's: what the
 *     subscription RIFT already holds has reported since the session began,
 *     the newest RIFT_RXLOG_MAX entries, kept while RIFT runs in the
 *     background and gone with CLOSE RIFT or the shell. Nothing here asks
 *     the service for anything, opens a connection, starts a timer or keeps
 *     a second copy of the nodes: it reads the session's model and ring.
 *   - LIVE draws the newest flood path heard in the last
 *     RIFT_SCOPE_LIVE_MS. A packet picked from the list is copied, so its
 *     detail and its path stay as they were while the ring moves on or
 *     evicts it; the list holds its place from then on, and LIVE lets go.
 *   - SHOW PATH fits the map to the picked packet's located hops. Nothing
 *     else moves the map: a packet arriving never does.
 *   - The path is an observation (rift_scope.h): hops resolved from
 *     prefixes at the width the packet reported, unknown and colliding
 *     ones left unresolved, a line only between adjacent hops that are
 *     both resolved and located. It is never offered as a route to send
 *     on; the route builder (rift_route_view.h) is where a route is chosen.
 *   - A route draft open in the builder is kept, not drawn, while TRAFFIC
 *     is on, and the panel says so; TRAFFIC off shows the builder again.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_SCOPE_VIEW_H
#define RIFT_SCOPE_VIEW_H

#include "rift_app.h"
#include "rift_map.h"
#include "rift_rxlog.h"

struct rift_scope_view;

/* MAP's TRAFFIC button, in MAP's bar: on and off. Made before the panel,
 * which is handed it to paint primary while TRAFFIC is on. */
lv_obj_t *rift_scope_view_button(struct rift_app *app, lv_obj_t *bar);
/* The panel, a child of MAP's root, hidden until TRAFFIC is turned on. */
struct rift_scope_view *rift_scope_view_create(struct rift_app *app, lv_obj_t *parent,
                                               lv_obj_t *button);
void rift_scope_view_destroy(struct rift_scope_view *v);
/* Whether MAP is showing TRAFFIC right now. */
int rift_scope_view_on(const struct rift_app *app);
/* TRAFFIC on or off. Off forgets the picked packet. Sends nothing. */
void rift_scope_view_set(struct rift_app *app, int on);
void rift_scope_view_shape(struct rift_scope_view *v, int wide);
/* Paint the panel. Returns a number that changes whenever the drawn path
 * does, so MAP repaints its canvas only then. */
unsigned rift_scope_view_refresh(struct rift_scope_view *v);
/* MAP's canvas, after its markers: the observed path over them. */
void rift_scope_view_draw(struct rift_scope_view *v, lv_layer_t *layer, const lv_area_t *ar,
                          const struct rift_map_view *view);
/* Esc, and the shell's Back: the packet's detail back to the list, the
 * list back to the plain map. 1 when used. */
int rift_scope_view_key(struct rift_app *app, uint32_t key);
int rift_scope_view_back(struct rift_app *app);
/* MAP's TRAFFIC (ui/rift_mapview.c holds it), or NULL. */
struct rift_scope_view *rift_map_view_scope(const struct rift_app *app);

/* For the tests. */
lv_obj_t *rift_scope_view_panel(const struct rift_scope_view *v);
lv_obj_t *rift_scope_view_row(const struct rift_scope_view *v, int i);
const struct rift_rx_entry *rift_scope_view_picked(const struct rift_scope_view *v);
int rift_scope_view_detail_open(const struct rift_scope_view *v);
int rift_scope_view_drawn_segments(const struct rift_scope_view *v);
int rift_scope_view_drawn_hops(const struct rift_scope_view *v);

#endif
