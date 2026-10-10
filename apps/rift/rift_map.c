/*
 * MAP's geometry. See rift_map.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
/* The most tile places looked at before the nearest are kept. */
#define TILES_SCAN_MAX 128

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The world's width in pixels at the view's zoom. */
static double world_px(const struct rift_map_view *v)
{
    return (double)RIFT_MAP_TILE_PX * (double)(1u << (unsigned)clampi(v->zoom, 0, 30));
}

/* 0..1, wrapped. */
static double wrap1(double x)
{
    x -= floor(x);
    return x >= 1.0 ? 0.0 : x;
}

static double world_x(double lon)
{
    return wrap1((lon + 180.0) / 360.0);
}

static double world_y(double lat)
{
    double s = sin(clamp(lat, -RIFT_MAP_LAT_MAX, RIFT_MAP_LAT_MAX) * PI / 180.0);

    return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * PI);
}

static double lat_of(double wy)
{
    return atan(sinh(PI * (1.0 - 2.0 * clamp(wy, 0.0, 1.0)))) * 180.0 / PI;
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
    double x_lo = 1.0;
    double x_hi = 0.0;
    double y_lo = 1.0;
    double y_hi = 0.0;
    double room_x;
    double room_y;
    int n = 0;
    int i;
    int z;

    memset(v, 0, sizeof(*v));
    v->w = w > 1 ? w : 1;
    v->h = h > 1 ? h : 1;
    v->cx = 0.5;
    v->cy = 0.5;
    v->zoom = RIFT_MAP_ZOOM_MIN;
    v->fitted = 1;
    for (i = 0; m && i < m->node_count; i++) {
        const struct rift_node *nd = &m->nodes[i];
        double x;
        double y;

        if (!nd->have_location) {
            continue;
        }
        x = world_x(nd->lon);
        y = world_y(nd->lat);
        x_lo = x < x_lo ? x : x_lo;
        x_hi = x > x_hi ? x : x_hi;
        y_lo = y < y_lo ? y : y_lo;
        y_hi = y > y_hi ? y : y_hi;
        n++;
    }
    if (n == 0) {
        return 0;
    }
    /* A mesh that straddles the antimeridian is not drawn across it: the
     * box is simply the box. A LoRa mesh does not span half the globe. */
    v->cx = (x_lo + x_hi) / 2.0;
    v->cy = (y_lo + y_hi) / 2.0;
    room_x = (double)(v->w - 2 * RIFT_MAP_FIT_MARGIN);
    room_y = (double)(v->h - 2 * RIFT_MAP_FIT_MARGIN);
    room_x = room_x < 1 ? 1 : room_x;
    room_y = room_y < 1 ? 1 : room_y;
    /* The closest whole level that holds the box. */
    for (z = RIFT_MAP_ZOOM_MAX; z > RIFT_MAP_ZOOM_MIN; z--) {
        double wpx = (double)RIFT_MAP_TILE_PX * (double)(1u << (unsigned)z);

        if ((x_hi - x_lo) * wpx <= room_x && (y_hi - y_lo) * wpx <= room_y) {
            break;
        }
    }
    v->zoom = z;
    return n;
}

void rift_map_resize(struct rift_map_view *v, int32_t w, int32_t h)
{
    v->w = w > 1 ? w : 1;
    v->h = h > 1 ? h : 1;
    v->zoom = clampi(v->zoom, RIFT_MAP_ZOOM_MIN, RIFT_MAP_ZOOM_MAX);
}

void rift_map_project(const struct rift_map_view *v, double lat, double lon, double *x,
                      double *y)
{
    double wpx = world_px(v);
    double dx = world_x(lon) - v->cx;

    /* The copy of the world nearest the centre. */
    dx -= floor(dx + 0.5);
    *x = (double)v->w / 2.0 + dx * wpx;
    *y = (double)v->h / 2.0 + (world_y(lat) - v->cy) * wpx;
}

void rift_map_unproject(const struct rift_map_view *v, double x, double y, double *lat,
                        double *lon)
{
    double wpx = world_px(v);

    *lon = wrap1(v->cx + (x - (double)v->w / 2.0) / wpx) * 360.0 - 180.0;
    *lat = lat_of(v->cy + (y - (double)v->h / 2.0) / wpx);
}

void rift_map_centre(const struct rift_map_view *v, double *lat, double *lon)
{
    *lat = lat_of(v->cy);
    *lon = wrap1(v->cx) * 360.0 - 180.0;
}

void rift_map_zoom(struct rift_map_view *v, int levels)
{
    v->zoom = clampi(v->zoom + levels, RIFT_MAP_ZOOM_MIN, RIFT_MAP_ZOOM_MAX);
}

void rift_map_pan(struct rift_map_view *v, double dx, double dy)
{
    double wpx = world_px(v);

    v->cx = wrap1(v->cx - dx / wpx);
    v->cy = clamp(v->cy - dy / wpx, 0.0, 1.0);
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

/* The cosine of the centre's latitude, never nothing. */
static double cos_centre(const struct rift_map_view *v)
{
    double c = cos(lat_of(v->cy) * PI / 180.0);

    return c < 0.01 ? 0.01 : c;
}

double rift_map_grid_step(const struct rift_map_view *v)
{
    /* About six lines over the shorter side, in degrees of latitude there. */
    int32_t s = v->w < v->h ? v->w : v->h;
    double span = (double)(s > 1 ? s : 1) * 360.0 * cos_centre(v) / world_px(v);

    return round_up(span / 6.0);
}

double rift_map_scale(const struct rift_map_view *v, int32_t max_px, int32_t *px)
{
    double m_per_px = RIFT_MAP_EQUATOR_M * cos_centre(v) / world_px(v);
    double metres = m_per_px * (double)(max_px > 1 ? max_px : 1);
    double p = pow(10.0, floor(log10(metres)));
    double len = metres >= 5 * p ? 5 * p : (metres >= 2 * p ? 2 * p : p);

    *px = (int32_t)(len / m_per_px + 0.5);
    return len;
}

struct place {
    struct rift_map_tile t;
    double d;
};

static int by_distance(const void *a, const void *b)
{
    const struct place *pa = a;
    const struct place *pb = b;

    return pa->d < pb->d ? -1 : pa->d > pb->d ? 1 : 0;
}

int rift_map_tiles(const struct rift_map_view *v, struct rift_map_tile *out, int max)
{
    struct place p[TILES_SCAN_MAX];
    double wpx = world_px(v);
    double left = v->cx * wpx - (double)v->w / 2.0;
    double top = v->cy * wpx - (double)v->h / 2.0;
    long n = 1L << clampi(v->zoom, 0, 30);
    long c0 = (long)floor(left / RIFT_MAP_TILE_PX);
    long c1 = (long)floor((left + (double)v->w - 1.0) / RIFT_MAP_TILE_PX);
    long r0 = (long)floor(top / RIFT_MAP_TILE_PX);
    long r1 = (long)floor((top + (double)v->h - 1.0) / RIFT_MAP_TILE_PX);
    int count = 0;
    long r;
    long c;
    int i;

    if (max > RIFT_MAP_TILES_MAX) {
        max = RIFT_MAP_TILES_MAX;
    }
    if (max <= 0 || !out) {
        return 0;
    }
    r0 = r0 < 0 ? 0 : r0;
    r1 = r1 > n - 1 ? n - 1 : r1;
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1 && count < TILES_SCAN_MAX; c++) {
            double ox = (double)(c * RIFT_MAP_TILE_PX) - left;
            double oy = (double)(r * RIFT_MAP_TILE_PX) - top;
            double mx = ox + RIFT_MAP_TILE_PX / 2.0 - (double)v->w / 2.0;
            double my = oy + RIFT_MAP_TILE_PX / 2.0 - (double)v->h / 2.0;

            p[count].t.z = v->zoom;
            p[count].t.x = (int)(((c % n) + n) % n);
            p[count].t.y = (int)r;
            p[count].t.sx = (int32_t)floor(ox);
            p[count].t.sy = (int32_t)floor(oy);
            p[count].d = mx * mx + my * my;
            count++;
        }
    }
    qsort(p, (size_t)count, sizeof(p[0]), by_distance);
    if (count > max) {
        count = max;
    }
    for (i = 0; i < count; i++) {
        out[i] = p[i].t;
    }
    return count;
}
