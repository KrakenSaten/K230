/*
 * MAP's geometry: where on the screen a node's claimed location goes, which
 * node a tap is on, and which basemap tiles the view covers. No LVGL, no
 * I/O: host-tested by tests/rift_tiles_test.c and tests/rift_app_test.c's
 * map section, drawn by ui/rift_mapview.c.
 *
 * The projection is Web Mercator (EPSG:3857) at whole zoom levels - the
 * projection and the levels of the slippy-map tiles the basemap draws
 * (docs/apps/RIFT.md, "MAP"), so a marker sits on the tile pixel of its
 * position whether or not the basemap is on. At zoom z the world is
 * 256 x 2^z pixels square; the view is a centre in world units (0..1 each
 * way, x east from the antimeridian, y south from the Mercator limit at
 * 85.0511 N) and a zoom. Longitude wraps; latitude stops at the limit.
 *
 * Only nodes whose adverts carried a valid location (rift_node.have_location)
 * are placed; nothing is ever given a position it did not claim.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_MAP_H
#define RIFT_MAP_H

#include "rift_model.h"

#include <stdint.h>

/* The zoom levels the view takes: 1 is the whole world in 512 px, 17 about
 * a metre a pixel at 60 N (a few hundred metres across the map), the
 * closest worth showing a mesh at and within what the tile server has. */
#define RIFT_MAP_ZOOM_MIN 1
#define RIFT_MAP_ZOOM_MAX 17
#define RIFT_MAP_TILE_PX 256
/* Room kept round the nodes when the view is fitted to them, in pixels. */
#define RIFT_MAP_FIT_MARGIN 36
/* A tap this close to a marker, in pixels, is on it. */
#define RIFT_MAP_HIT_PX 28
/* Web Mercator's latitude limit, where the square world ends. */
#define RIFT_MAP_LAT_MAX 85.05112878
/* The equator's length (WGS84 semi-major axis), for the scale bar. */
#define RIFT_MAP_EQUATOR_M 40075016.686
/* The most tile positions one view covers (rift_map_tiles). */
#define RIFT_MAP_TILES_MAX 48

struct rift_map_view {
    double cx;     /* the view's centre, world units */
    double cy;
    int zoom;
    int32_t w;     /* the drawing area */
    int32_t h;
    int fitted;    /* set by rift_map_fit; a reader's zoom or pan keeps it */
};

/* One tile on screen: which tile, and where its top left is drawn,
 * relative to the area's top left. A small world (zoom 1 on a wide area)
 * shows a tile more than once; each place is its own entry. */
struct rift_map_tile {
    int z;
    int x;
    int y;
    int32_t sx;
    int32_t sy;
};

/* How many nodes in the model have a location to place. */
int rift_map_located(const struct rift_model *m);

/* Fit the view to every located node (all of them in, RIFT_MAP_FIT_MARGIN
 * clear of the edge, at the closest zoom that holds them), in a w x h area.
 * With no located node the view is the whole world and 0 is returned;
 * otherwise the count placed. */
int rift_map_fit(struct rift_map_view *v, const struct rift_model *m, int32_t w, int32_t h);
/* Keep the centre and zoom for a new area size (a turn of the panel). */
void rift_map_resize(struct rift_map_view *v, int32_t w, int32_t h);

/* Screen position of a location, relative to the area's top left; of the
 * copies of the world, the one nearest the centre. */
void rift_map_project(const struct rift_map_view *v, double lat, double lon, double *x,
                      double *y);
/* And back: the location under a point (latitude held at the limit). */
void rift_map_unproject(const struct rift_map_view *v, double x, double y, double *lat,
                        double *lon);
/* The view's centre as a location. */
void rift_map_centre(const struct rift_map_view *v, double *lat, double *lon);

/* Zoom by whole levels (1 is in, -1 out) about the area's centre, held
 * between RIFT_MAP_ZOOM_MIN and RIFT_MAP_ZOOM_MAX. */
void rift_map_zoom(struct rift_map_view *v, int levels);
/* Move the map by dx, dy pixels (a drag): longitude wraps, the centre stays
 * inside the world north to south. */
void rift_map_pan(struct rift_map_view *v, double dx, double dy);

/* The located node nearest (x, y) and within RIFT_MAP_HIT_PX, as its index
 * in m->nodes, or -1. */
int rift_map_hit(const struct rift_map_view *v, const struct rift_model *m, double x, double y);

/* The graticule's step in degrees: 1, 2 or 5 times a power of ten, so that
 * about four to eight lines cross the shorter side. */
double rift_map_grid_step(const struct rift_map_view *v);
/* A scale bar of a round length (1, 2 or 5 x 10^n metres) at most max_px
 * long, true at the centre's latitude: its length in metres, and in
 * pixels in *px. */
double rift_map_scale(const struct rift_map_view *v, int32_t max_px, int32_t *px);

/* The tiles that intersect the area - only those, no margin - nearest the
 * centre first, at most max (RIFT_MAP_TILES_MAX at most). Rows past the
 * poles are none; columns wrap. Returns the count. */
int rift_map_tiles(const struct rift_map_view *v, struct rift_map_tile *out, int max);

#endif
