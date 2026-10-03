/*
 * MAP's geometry. See rift_map.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_map.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Longitude pixels per degree: latitude's, narrowed by the cosine of the
 * centre, and never to nothing at a pole. */
static double ppd_lon(const struct rift_map_view *v)
{
    double c = cos(v->c_lat * PI / 180.0);

    return v->ppd * (c < 0.01 ? 0.01 : c);
}

static int32_t shorter(const struct rift_map_view *v)
{
    int32_t s = v->w < v->h ? v->w : v->h;

    return s > 1 ? s : 1;
}

/* The scale's bounds for this area: pixels per degree such that the shorter
 * side spans between RIFT_MAP_MIN_SPAN_M and RIFT_MAP_MAX_SPAN_DEG. */
static double ppd_max(const struct rift_map_view *v)
{
    return (double)shorter(v) / (RIFT_MAP_MIN_SPAN_M / RIFT_MAP_M_PER_DEG);
}

static double ppd_min(const struct rift_map_view *v)
{
    return (double)shorter(v) / RIFT_MAP_MAX_SPAN_DEG;
}

int rift_map_located(const struct rift_model *m)
{
    int n = 0;
    int i;

    for (i = 0; m && i < m->node_count; i++) {
        n += m->nodes[i].have_location ? 1 : 0;
    }
    return n;
}

int rift_map_fit(struct rift_map_view *v, const struct rift_model *m, int32_t w, int32_t h)
{
    double lat_lo = 90.0;
    double lat_hi = -90.0;
    double lon_lo = 180.0;
    double lon_hi = -180.0;
    double span_x;
    double span_y;
    double room_x;
    double room_y;
    double c;
    int n = 0;
    int i;

    memset(v, 0, sizeof(*v));
    v->w = w > 1 ? w : 1;
    v->h = h > 1 ? h : 1;
    for (i = 0; m && i < m->node_count; i++) {
        const struct rift_node *nd = &m->nodes[i];

        if (!nd->have_location) {
            continue;
        }
        lat_lo = nd->lat < lat_lo ? nd->lat : lat_lo;
        lat_hi = nd->lat > lat_hi ? nd->lat : lat_hi;
        lon_lo = nd->lon < lon_lo ? nd->lon : lon_lo;
        lon_hi = nd->lon > lon_hi ? nd->lon : lon_hi;
        n++;
    }
    if (n == 0) {
        v->ppd = ppd_min(v);
        v->fitted = 1;
        return 0;
    }
    /* A mesh that straddles the antimeridian is not drawn across it: the
     * box is simply the box. A LoRa mesh does not span half the globe. */
    v->c_lat = (lat_lo + lat_hi) / 2.0;
    v->c_lon = (lon_lo + lon_hi) / 2.0;
    c = cos(v->c_lat * PI / 180.0);
    c = c < 0.01 ? 0.01 : c;
    span_y = lat_hi - lat_lo;
    span_x = (lon_hi - lon_lo) * c;
    room_x = (double)(v->w - 2 * RIFT_MAP_FIT_MARGIN);
    room_y = (double)(v->h - 2 * RIFT_MAP_FIT_MARGIN);
    room_x = room_x < 1 ? 1 : room_x;
    room_y = room_y < 1 ? 1 : room_y;
    if (span_x <= 0 && span_y <= 0) {
        v->ppd = ppd_max(v);
    } else if (span_x <= 0) {
        v->ppd = room_y / span_y;
    } else if (span_y <= 0) {
        v->ppd = room_x / span_x;
    } else {
        v->ppd = room_x / span_x < room_y / span_y ? room_x / span_x : room_y / span_y;
    }
    v->ppd = clamp(v->ppd, ppd_min(v), ppd_max(v));
    v->fitted = 1;
    return n;
}

void rift_map_resize(struct rift_map_view *v, int32_t w, int32_t h)
{
    v->w = w > 1 ? w : 1;
    v->h = h > 1 ? h : 1;
    v->ppd = clamp(v->ppd, ppd_min(v), ppd_max(v));
}

void rift_map_project(const struct rift_map_view *v, double lat, double lon, double *x,
                      double *y)
{
    *x = (double)v->w / 2.0 + (lon - v->c_lon) * ppd_lon(v);
    *y = (double)v->h / 2.0 - (lat - v->c_lat) * v->ppd;
}

void rift_map_unproject(const struct rift_map_view *v, double x, double y, double *lat,
                        double *lon)
{
    *lon = v->c_lon + (x - (double)v->w / 2.0) / ppd_lon(v);
    *lat = v->c_lat - (y - (double)v->h / 2.0) / v->ppd;
}

void rift_map_zoom(struct rift_map_view *v, double factor)
{
    if (factor > 0) {
        v->ppd = clamp(v->ppd * factor, ppd_min(v), ppd_max(v));
    }
}

void rift_map_pan(struct rift_map_view *v, double dx, double dy)
{
    v->c_lon -= dx / ppd_lon(v);
    v->c_lat = clamp(v->c_lat + dy / v->ppd, -85.0, 85.0);
    while (v->c_lon > 180.0) {
        v->c_lon -= 360.0;
    }
    while (v->c_lon < -180.0) {
        v->c_lon += 360.0;
    }
}

int rift_map_hit(const struct rift_map_view *v, const struct rift_model *m, double x, double y)
{
    double best = (double)RIFT_MAP_HIT_PX * RIFT_MAP_HIT_PX;
    int at = -1;
    int i;

    for (i = 0; m && i < m->node_count; i++) {
        double px;
        double py;
        double d;

        if (!m->nodes[i].have_location) {
            continue;
        }
        rift_map_project(v, m->nodes[i].lat, m->nodes[i].lon, &px, &py);
        d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d <= best) {
            best = d;
            at = i;
        }
    }
    return at;
}

/* 1, 2 or 5 x 10^n, the smallest at least want. */
static double round_up(double want)
{
    double p = pow(10.0, floor(log10(want)));

    if (want <= p) {
        return p;
    }
    if (want <= 2 * p) {
        return 2 * p;
    }
    if (want <= 5 * p) {
        return 5 * p;
    }
    return 10 * p;
}

double rift_map_grid_step(const struct rift_map_view *v)
{
    /* About six lines over the shorter side. */
    double span = (double)shorter(v) / v->ppd;

    return round_up(span / 6.0);
}

double rift_map_scale(const struct rift_map_view *v, int32_t max_px, int32_t *px)
{
    double m_per_px = RIFT_MAP_M_PER_DEG / v->ppd;
    double metres = m_per_px * (double)(max_px > 1 ? max_px : 1);
    double p = pow(10.0, floor(log10(metres)));
    double len = metres >= 5 * p ? 5 * p : (metres >= 2 * p ? 2 * p : p);

    *px = (int32_t)(len / m_per_px + 0.5);
    return len;
}
