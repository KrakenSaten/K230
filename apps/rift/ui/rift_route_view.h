/*
 * The route builder: MAP while a route draft is open (rift_route.h).
 *
 * Opened from a direct conversation's SET ROUTE (ui/rift_route_bar.h), it
 * takes over MAP's panel and adds to MAP's drawing; the map itself - its
 * markers, basemap, pan, zoom and FIT - is MAP's own and unchanged.
 *
 *   - The recipient is named at the top and cannot change: the builder is
 *     the conversation's.
 *   - A tap on a repeater's marker appends it to the route, by its whole
 *     public key. A tap on anything else says why it cannot be a relay. The
 *     list (REPEATERS) offers every eligible repeater, including those that
 *     never said where they are, which no map can place.
 *   - The route is drawn in its order: each relay numbered on its marker, a
 *     line from each to the next where both have a location, the last one
 *     to the recipient dashed until the service has checked the route. A
 *     waypoint with no location is never drawn and never bridged: the list
 *     says "NO LOCATION", and no line pretends to cross the gap.
 *   - The ordered list beside or under the map is the same route, the same
 *     numbers, the same order. A row picked there can be cut, with every
 *     waypoint after it; UNDO, CLEAR and CANCEL do what they say.
 *   - Every edit is checked with mesh.path_preview, which transmits
 *     nothing; USE ROUTE is offered only for the route as checked, and stages
 *     it on the conversation. CANCEL stages nothing and changes nothing.
 *
 * The lines are the route a reader is asking for. They are not radio links
 * anyone has seen, and they say nothing about delivery: the panel says so
 * whenever it is open.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_ROUTE_VIEW_H
#define RIFT_ROUTE_VIEW_H

#include "rift_app.h"
#include "rift_map.h"

struct rift_route_view;

/* The builder's panel, a child of MAP's root, hidden until a draft opens. */
struct rift_route_view *rift_route_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_route_view_destroy(struct rift_route_view *v);
lv_obj_t *rift_route_view_panel(const struct rift_route_view *v);

/* Whether MAP is the route builder right now. */
int rift_route_view_active(const struct rift_app *app);
/* Lay the panel out for the shape (beside the map, or under it). */
void rift_route_view_shape(struct rift_route_view *v, int wide);
/* Paint the panel from the draft, and ask for a preview when the draft
 * wants one. Returns a number that changes whenever the drawn route does, so
 * MAP repaints its canvas only then. */
unsigned rift_route_view_refresh(struct rift_route_view *v);

/* MAP's canvas, after its markers: the route over them. */
void rift_route_view_draw(struct rift_route_view *v, lv_layer_t *layer, const lv_area_t *ar,
                          const struct rift_map_view *view, lv_obj_t *canvas);
/* The drawing itself (ui/rift_route_draw.c): what it placed goes to marks
 * (relays numbered on the map) and lines (legs drawn). */
void rift_route_draw(const struct rift_app *app, lv_layer_t *layer, const lv_area_t *ar,
                     const struct rift_map_view *view, lv_obj_t *canvas, int *marks, int *lines);
/* Whether a node's marker is one the builder dims: not a relay it could
 * take and not the recipient. */
int rift_route_view_dimmed(const struct rift_app *app, const struct rift_node *n);
/* A tap on the marker of m->nodes[index] (or -1: nothing there). */
void rift_route_view_tap(struct rift_route_view *v, int index);
/* Keys while the builder is open. 1 when the key was used. */
int rift_route_view_key(struct rift_app *app, uint32_t key);
/* MAP's builder (ui/rift_mapview.c holds it), or NULL. */
struct rift_route_view *rift_map_view_route(const struct rift_app *app);

/* For the tests: the waypoints the last draw put on the map, and how many
 * route lines it drew. */
int rift_route_view_drawn_marks(const struct rift_route_view *v);
int rift_route_view_drawn_lines(const struct rift_route_view *v);

#endif
