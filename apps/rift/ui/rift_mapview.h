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
 * location - is said to have none.
 *
 * BASEMAP, off unless the reader turns it on (and kept as their choice),
 * draws OpenStreetMap tiles under the graticule and the markers while MAP
 * is shown (ui/rift_basemap.h, rift_tiles.h): the tiles in view and no
 * others, through the tile helper, with "(c) OpenStreetMap contributors"
 * on the map for as long as it is on, and what it is doing - loading,
 * offline with the tiles seen before, the clock not set, a server error,
 * unavailable - said over the map and in the panel (docs/apps/RIFT.md,
 * "MAP").
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
/* Every pass of the app's timer, from outside any LVGL event: the basemap's
 * helper started or stopped, its tiles asked for and taken. */
void rift_map_view_pump(struct rift_app *app, int64_t now_ms);

/* For the tests: the drawn area, its view, the markers the last draw placed
 * (located nodes inside the area) and how many draws there have been. */
lv_obj_t *rift_map_view_canvas(const struct rift_app *app);
const struct rift_map_view *rift_map_view_geometry(const struct rift_app *app);
int rift_map_view_markers(const struct rift_app *app);
unsigned rift_map_view_draws(const struct rift_app *app);
/* The basemap: its block, how many tiles the last draw put down, whether
 * it drew the attribution and where, and the words it put over the map. */
struct rift_basemap *rift_map_view_basemap(const struct rift_app *app);
int rift_map_view_tiles_drawn(const struct rift_app *app);
int rift_map_view_attribution(const struct rift_app *app, lv_area_t *at);
const char *rift_map_view_basemap_line(const struct rift_app *app);

#endif
