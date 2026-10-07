/*
 * MAP: the nodes that said where they are, placed by their claimed
 * latitude and longitude on a dark graticule (rift_map.h for the geometry).
 *
 * One drawn object for the whole map - markers, names, grid and scale bar
 * are drawn in its draw callback - so a thousand known nodes cost no LVGL
 * objects, and a repaint happens only when what is drawn changed. A
 * repeater is a square, any other node a dot, the selected node ringed in
 * the accent and named; a node not heard for 12 hours is drawn muted. A tap
 * selects the nearest marker (the same selection NODES and NET share) and
 * the panel beside or under the map says what it is, with DETAIL and - for a
 * node that takes direct messages - MESSAGE. Drag pans; + and - zoom; FIT
 * puts every located node back in view.
 *
 * Nothing is placed that did not claim a place: nodes without a location
 * are counted, never drawn, and this device - whose adverts carry no
 * location - is said to have none. There is no basemap: no tiles, no
 * download, no service (docs/apps/RIFT.md, "MAP").
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_MAPVIEW_H
#define RIFT_MAPVIEW_H

#include "rift_app.h"
#include "rift_map.h"

lv_obj_t *rift_map_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_map_view_destroy(struct rift_app *app);
void rift_map_view_shape(struct rift_app *app);
void rift_map_view_refresh(struct rift_app *app);

/* For the tests: the drawn area, its view, the markers the last draw placed
 * (located nodes inside the area) and how many draws there have been. */
lv_obj_t *rift_map_view_canvas(const struct rift_app *app);
const struct rift_map_view *rift_map_view_geometry(const struct rift_app *app);
int rift_map_view_markers(const struct rift_app *app);
unsigned rift_map_view_draws(const struct rift_app *app);

#endif
