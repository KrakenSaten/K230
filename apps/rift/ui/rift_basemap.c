/*
 * MAP's basemap on screen. See rift_basemap.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_basemap.h"

#include "pocketlog/pocketlog.h"
#include "pos_styles.h"
#include "rift_format.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The scrim over the tiles: the theme's surface, so a dark theme darkens a
 * bright map and the markers and names keep their contrast. */
#define SCRIM_OPA LV_OPA_40

struct rift_basemap {
    struct rift_tiles t;
    int on;
    lv_image_dsc_t dsc[RIFT_TILES_MEM_MAX];
    const uint16_t *dsc_px[RIFT_TILES_MEM_MAX]; /* what dsc[i] points at, or NULL */
    struct rift_map_tile view[RIFT_MAP_TILES_MAX];
};

/* Before rift_tiles frees a tile's pixels: LVGL's image cache must not keep
 * a pointer into them. */
static void evict(void *ctx, int slot)
{
    struct rift_basemap *b = ctx;

    if (slot >= 0 && slot < RIFT_TILES_MEM_MAX && b->dsc_px[slot]) {
        lv_image_cache_drop(&b->dsc[slot]);
        b->dsc_px[slot] = NULL;
    }
}

struct rift_basemap *rift_basemap_new(void)
{
    struct rift_basemap *b = calloc(1, sizeof(*b));

    if (b) {
        rift_tiles_init(&b->t);
        b->t.evict = evict;
        b->t.evict_ctx = b;
    }
    return b;
}

void rift_basemap_free(struct rift_basemap *b)
{
    if (b) {
        rift_tiles_stop(&b->t);
        free(b);
    }
}

void rift_basemap_set(struct rift_basemap *b, int on)
{
    if (!b) {
        return;
    }
    b->on = on ? 1 : 0;
    if (!b->on) {
        /* Also forgets a helper that gave up: on again is a fresh start. */
        rift_tiles_stop(&b->t);
    }
}

int rift_basemap_on(const struct rift_basemap *b)
{
    return b && b->on;
}

const struct rift_tiles *rift_basemap_tiles(const struct rift_basemap *b)
{
    return b ? &b->t : NULL;
}

/* The fake network, for the tests and the simulator's demo: no socket at
 * all. Which of its tile servers is a test's choice; nothing it names can
 * leave the device. The real tile server is never configurable here. */
static void config(struct rift_tiles_config *c)
{
    const char *backend = getenv("POCKETOS_RIFT_TILES");
    const char *url = getenv("POCKETOS_RIFT_TILE_URL");

    memset(c, 0, sizeof(*c));
    c->fake = backend && strcmp(backend, "fake") == 0;
    /* Any https template, for the fake network alone: it opens no socket. */
    c->url = c->fake ? (url && strncmp(url, RIFT_TILE_URL_FAKE, 8) == 0 ? url : RIFT_TILE_URL_FAKE)
                     : RIFT_TILE_URL_OSM;
}

int rift_basemap_pump(struct rift_basemap *b, const struct rift_map_view *v, int shown, int64_t now_ms)
{
    int n;
    int changed;

    if (!b) {
        return 0;
    }
    if (!b->on || !shown || !v) {
        if (b->t.on) {
            rift_tiles_stop(&b->t);
            return 1;
        }
        return 0;
    }
    if (!b->t.on) {
        struct rift_tiles_config c;

        config(&c);
        if (rift_tiles_start(&b->t, &c, now_ms) != 0) {
            LOG_WARN("rift: basemap: %s", b->t.err);
        }
        changed = 1;
    } else {
        changed = 0;
    }
    n = rift_map_tiles(v, b->view, RIFT_MAP_TILES_MAX);
    rift_tiles_want(&b->t, b->view, n, now_ms);
    changed |= rift_tiles_poll(&b->t, now_ms) ? 1 : 0;
    /* What arrived may make another want due (a helper that just said hello). */
    if (changed) {
        rift_tiles_want(&b->t, b->view, n, now_ms);
    }
    return changed;
}

int rift_basemap_draw(struct rift_basemap *b, lv_layer_t *layer, const lv_area_t *a,
                      const struct rift_map_view *v)
{
    int n;
    int drawn = 0;
    int i;

    if (!b || !b->on || !b->t.on) {
        return 0;
    }
    n = rift_map_tiles(v, b->view, RIFT_MAP_TILES_MAX);
    for (i = 0; i < n; i++) {
        const struct rift_map_tile *mt = &b->view[i];
        int slot = -1;
        bool stale = false;
        const uint16_t *px = rift_tiles_pixels(&b->t, mt->z, mt->x, mt->y, &slot, &stale);
        lv_draw_image_dsc_t d;
        lv_draw_rect_dsc_t s;
        lv_area_t at;

        if (!px || slot < 0) {
            continue; /* the graticule shows through */
        }
        if (b->dsc_px[slot] != px) {
            lv_image_dsc_t *im = &b->dsc[slot];

            if (b->dsc_px[slot]) {
                lv_image_cache_drop(im);
            }
            memset(im, 0, sizeof(*im));
            im->header.magic = LV_IMAGE_HEADER_MAGIC;
            im->header.cf = LV_COLOR_FORMAT_RGB565;
            im->header.w = RIFT_MAP_TILE_PX;
            im->header.h = RIFT_MAP_TILE_PX;
            im->header.stride = RIFT_MAP_TILE_PX * 2;
            im->data = (const uint8_t *)px;
            im->data_size = RIFT_MAP_TILE_PX * RIFT_MAP_TILE_PX * 2;
            b->dsc_px[slot] = px;
        }
        at.x1 = a->x1 + mt->sx;
        at.y1 = a->y1 + mt->sy;
        at.x2 = at.x1 + RIFT_MAP_TILE_PX - 1;
        at.y2 = at.y1 + RIFT_MAP_TILE_PX - 1;
        lv_draw_image_dsc_init(&d);
        d.src = &b->dsc[slot];
        lv_draw_image(layer, &d, &at);
        lv_draw_rect_dsc_init(&s);
        s.bg_color = pos_theme_color(POS_COLOR_SURFACE);
        s.bg_opa = SCRIM_OPA;
        lv_draw_rect(layer, &s, &at);
        drawn++;
    }
    return drawn;
}

/* "http-503" -> "HTTP 503", "tls" -> "TLS". */
static void shout(const char *why, char *out, size_t len)
{
    size_t i;

    if (strcmp(why, "broken") == 0 || strcmp(why, "format") == 0) {
        snprintf(out, len, "A TILE COULD NOT BE READ");
        return;
    }
    if (strcmp(why, "no-network-build") == 0) {
        snprintf(out, len, "THIS BUILD HAS NO HTTP CLIENT");
        return;
    }
    for (i = 0; why[i] && i + 1 < len; i++) {
        out[i] = why[i] == '-' ? ' ' : (char)toupper((unsigned char)why[i]);
    }
    out[i] = '\0';
}

enum rift_tiles_status rift_basemap_words(const struct rift_basemap *b, char *line, size_t len, char *detail,
                                          size_t dlen)
{
    enum rift_tiles_status st;
    const char *why = "";
    char loud[48];

    line[0] = '\0';
    detail[0] = '\0';
    if (!b || !b->on) {
        snprintf(detail, dlen, "BASEMAP OFF: NOTHING IS FETCHED");
        return RIFT_TILES_OFF;
    }
    st = rift_tiles_status(&b->t, &why);
    switch (st) {
    case RIFT_TILES_OFF:
    case RIFT_TILES_LOADING:
        snprintf(line, len, "LOADING MAP TILES");
        snprintf(detail, dlen, "BASEMAP: OPENSTREETMAP, LOADING");
        break;
    case RIFT_TILES_SHOWN:
        snprintf(detail, dlen, "BASEMAP: OPENSTREETMAP");
        break;
    case RIFT_TILES_OFFLINE:
        snprintf(line, len, "OFFLINE" RIFT_SEP "SAVED TILES ONLY");
        snprintf(detail, dlen, "BASEMAP: NO NETWORK, TILES SEEN BEFORE ONLY");
        break;
    case RIFT_TILES_NO_CLOCK:
        snprintf(line, len, "CLOCK NOT SET" RIFT_SEP "SAVED TILES ONLY");
        snprintf(detail, dlen, "BASEMAP: THE CLOCK IS NOT SET, SO NO CERTIFICATE CAN BE CHECKED");
        break;
    case RIFT_TILES_ERROR:
        shout(why, loud, sizeof(loud));
        snprintf(line, len, "TILE ERROR: %s", loud);
        snprintf(detail, dlen, "BASEMAP: THE TILE SERVER FAILED (%s)", loud);
        break;
    case RIFT_TILES_UNAVAILABLE:
        snprintf(line, len, "BASEMAP UNAVAILABLE");
        snprintf(detail, dlen, "BASEMAP UNAVAILABLE: %s", why[0] ? why : "no tile helper");
        break;
    }
    return st;
}
