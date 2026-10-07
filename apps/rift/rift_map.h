/*
 * MAP's geometry: where on the screen a node's claimed location goes, and
 * which node a tap is on. No LVGL, no I/O: host-tested by
 * tests/rift_notify_test.c's map section and drawn by ui/rift_mapview.c.
 *
 * The projection is equirectangular about the view's centre - longitude
 * scaled by the cosine of the centre latitude, so a degree east and a degree
 * north are the same length on screen there. Over the few tens of
 * kilometres a LoRa mesh spans that is indistinguishable from Web Mercator,
 * needs no tiles and no library, and is exact about what it is: positions as
 * the nodes claimed them, relative to each other. There is no basemap
 * (docs/apps/RIFT.md, "MAP").
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

/* The closest the view zooms in: this many metres across the shorter side,
 * so one node alone, or two at the same mast, is not an infinite zoom. */
#define RIFT_MAP_MIN_SPAN_M 400.0
/* And the furthest out: the whole world's latitude on the shorter side. */
#define RIFT_MAP_MAX_SPAN_DEG 180.0
/* Room kept round the nodes when the view is fitted to them, in pixels. */
#define RIFT_MAP_FIT_MARGIN 36
/* A tap this close to a marker, in pixels, is on it. */
#define RIFT_MAP_HIT_PX 28
/* A degree of latitude, in metres (the mean; good to 0.5 %). */
#define RIFT_MAP_M_PER_DEG 111320.0

struct rift_map_view {
    double c_lat;  /* the view's centre */
    double c_lon;
    double ppd;    /* pixels per degree of latitude */
    int32_t w;     /* the drawing area */
    int32_t h;
    int fitted;    /* set by rift_map_fit; a reader's zoom or pan keeps it */
};

/* How many nodes in the model have a location to place. */
int rift_map_located(const struct rift_model *m);

/* Fit the view to every located node (all of them in, RIFT_MAP_FIT_MARGIN
 * clear of the edge), in a w x h area. With no located node the view is the
 * whole world at the centre and 0 is returned; otherwise the count placed. */
int rift_map_fit(struct rift_map_view *v, const struct rift_model *m, int32_t w, int32_t h);
/* Keep the centre and scale for a new area size (a turn of the panel). */
void rift_map_resize(struct rift_map_view *v, int32_t w, int32_t h);

/* Screen position of a location, relative to the area's top left. */
void rift_map_project(const struct rift_map_view *v, double lat, double lon, double *x,
                      double *y);
/* And back: the location under a point. */
void rift_map_unproject(const struct rift_map_view *v, double x, double y, double *lat,
                        double *lon);

/* Zoom by factor (2 is in, 0.5 out) about the area's centre, held between
 * RIFT_MAP_MIN_SPAN_M and RIFT_MAP_MAX_SPAN_DEG. */
void rift_map_zoom(struct rift_map_view *v, double factor);
/* Move the map by dx, dy pixels (a drag), the latitude held in range. */
void rift_map_pan(struct rift_map_view *v, double dx, double dy);

/* The located node nearest (x, y) and within RIFT_MAP_HIT_PX, as its index
 * in m->nodes, or -1. */
int rift_map_hit(const struct rift_map_view *v, const struct rift_model *m, double x, double y);

/* The graticule's step in degrees: 1, 2 or 5 times a power of ten, so that
 * about four to eight lines cross the shorter side. */
double rift_map_grid_step(const struct rift_map_view *v);
/* A scale bar of a round length (1, 2 or 5 x 10^n metres) at most max_px
 * long: its length in metres, and in pixels in *px. */
double rift_map_scale(const struct rift_map_view *v, int32_t max_px, int32_t *px);

#endif
