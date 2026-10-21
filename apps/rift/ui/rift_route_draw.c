/*
 * The route builder's drawing over MAP (ui/rift_route_view.h): the route's
 * lines, the relays' numbers and the recipient's ring, drawn in MAP's
 * canvas callback after its markers. Split from rift_route_view.c, which
 * owns the panel, so neither file is everything (tests/rift_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_route_view.h"

#include "pos_styles.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The numbered disc on a relay's marker, the ring round the recipient, and
 * the route's lines. */
#define BADGE_PX 24
#define TO_RING_PX 31
#define LINE_W 3
#define DASH_ON 10
#define DASH_OFF 7
#define BACKDROP_OPA LV_OPA_80

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                 lv_color_t color)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = LINE_W;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

/* Dashes by hand: the line drawer dashes only straight-across lines. */
static void dashed(lv_layer_t *layer, double x1, double y1, double x2, double y2, lv_color_t color)
{
    double len = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    double t;

    if (len < 1) {
        return;
    }
    for (t = 0; t < len; t += DASH_ON + DASH_OFF) {
        double e = t + DASH_ON < len ? t + DASH_ON : len;

        line(layer, (int32_t)(x1 + (x2 - x1) * t / len), (int32_t)(y1 + (y2 - y1) * t / len),
             (int32_t)(x1 + (x2 - x1) * e / len), (int32_t)(y1 + (y2 - y1) * e / len), color);
    }
}

/* A filled disc (fill_opa cover) or a ring (fill_opa transparent), with a
 * border of border_w in border. */
static void disc(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t d, lv_color_t fill,
                 lv_opa_t fill_opa, lv_color_t border, int32_t border_w)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a = { cx - d / 2, cy - d / 2, cx - d / 2 + d - 1, cy - d / 2 + d - 1 };

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_opa = fill_opa;
    dsc.bg_color = fill;
    dsc.border_width = border_w;
    dsc.border_color = border;
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &a);
}

static void words(lv_layer_t *layer, lv_obj_t *canvas, int32_t x, int32_t y, const char *s,
                  lv_color_t color, int backdrop, int center)
{
    lv_draw_label_dsc_t dsc;
    const lv_font_t *font = lv_obj_get_style_text_font(canvas, LV_PART_MAIN);
    int32_t h = font ? lv_font_get_line_height(font) : 16;
    int32_t w = rift_cell_text_width(canvas, s) + 2;
    lv_area_t a;

    if (center) {
        x -= w / 2;
        y -= h / 2;
    }
    a.x1 = x;
    a.y1 = y;
    a.x2 = x + w;
    a.y2 = y + h;
    if (backdrop) {
        lv_draw_rect_dsc_t bg;
        lv_area_t b = { a.x1 - 4, a.y1 - 1, a.x2 + 3, a.y2 };

        lv_draw_rect_dsc_init(&bg);
        bg.radius = 3;
        bg.bg_opa = BACKDROP_OPA;
        bg.bg_color = pos_theme_color(POS_COLOR_SURFACE);
        lv_draw_rect(layer, &bg, &b);
    }
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.text = s;
    dsc.letter_space = lv_obj_get_style_text_letter_space(canvas, LV_PART_MAIN);
    lv_draw_label(layer, &dsc, &a);
}

int rift_route_view_dimmed(const struct rift_app *app, const struct rift_node *n)
{
    const struct rift_route *r = &app->model.route;

    if (!rift_route_view_active(app) || !n) {
        return 0;
    }
    return strcmp(n->key, r->draft.to) != 0 && rift_route_index(r, n->key) < 0 &&
           !rift_node_is_repeater(n);
}

/* Where a waypoint is on the canvas: only where its adverts put it. */
static int place(const struct rift_model *m, const char *key, const struct rift_map_view *view,
                 const lv_area_t *ar, double *x, double *y)
{
    const struct rift_node *n = key ? rift_model_find(m, key) : NULL;

    if (!n || !n->have_location) {
        return 0;
    }
    rift_map_project(view, n->lat, n->lon, x, y);
    *x += ar->x1;
    *y += ar->y1;
    return 1;
}

void rift_route_draw(const struct rift_app *app, lv_layer_t *layer, const lv_area_t *ar,
                     const struct rift_map_view *view, lv_obj_t *canvas, int *marks, int *lines)
{
    const struct rift_model *m;
    const struct rift_route_draft *d;
    lv_color_t accent = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
    double px[RIFT_ROUTE_MAX + 2];
    double py[RIFT_ROUTE_MAX + 2];
    int at[RIFT_ROUTE_MAX + 2];
    char text[96];
    int unplaced = 0;
    int last;
    int i;

    *marks = 0;
    *lines = 0;
    if (!rift_route_view_active(app)) {
        return;
    }
    m = &app->model;
    d = &m->route.draft;
    /* Sender, relays in order, recipient. This device's own adverts carry
     * no location, so the first leg is drawn only if it ever says one. */
    last = d->count + 1;
    at[0] = place(m, m->have_identity ? m->self_key : NULL, view, ar, &px[0], &py[0]);
    for (i = 0; i < d->count; i++) {
        at[i + 1] = place(m, d->via[i], view, ar, &px[i + 1], &py[i + 1]);
        unplaced += !at[i + 1];
    }
    at[last] = place(m, d->to, view, ar, &px[last], &py[last]);
    /* A line only between two waypoints that are both placed, and never
     * across one that is not: that would be a link between nodes the route
     * does not join. The leg into the recipient is dashed until the service
     * has checked the route as it stands. */
    for (i = 0; i < last; i++) {
        if (!at[i] || !at[i + 1]) {
            continue;
        }
        if (i + 1 == last && d->check != RIFT_ROUTE_VALID) {
            dashed(layer, px[i], py[i], px[i + 1], py[i + 1], accent);
        } else {
            line(layer, (int32_t)px[i], (int32_t)py[i], (int32_t)px[i + 1], (int32_t)py[i + 1],
                 accent);
        }
        (*lines)++;
    }
    if (at[last]) {
        disc(layer, (int32_t)px[last], (int32_t)py[last], TO_RING_PX, accent, LV_OPA_TRANSP,
             accent, 3);
        /* "TO" on the ring's left: MAP names a marker on its right. */
        words(layer, canvas,
              (int32_t)px[last] - TO_RING_PX / 2 - 8 - rift_cell_text_width(canvas, "TO") / 2,
              (int32_t)py[last], "TO", pos_theme_color(POS_COLOR_TEXT_PRIMARY), 1, 1);
    }
    /* The relays' numbers, the same as the list's, on their markers. */
    for (i = 1; i < last; i++) {
        if (!at[i]) {
            continue;
        }
        snprintf(text, sizeof(text), "%d", i);
        /* The waypoint picked in the list is ringed here too. */
        disc(layer, (int32_t)px[i], (int32_t)py[i], BADGE_PX, accent, LV_OPA_COVER,
             pos_theme_color(POS_COLOR_TEXT_PRIMARY), i - 1 == d->marked ? 2 : 0);
        words(layer, canvas, (int32_t)px[i], (int32_t)py[i], text,
              pos_theme_color(POS_COLOR_TEXT_ON_ACCENT), 0, 1);
        (*marks)++;
    }
    /* What the lines are, over the map itself, below the basemap's line. */
    {
        const lv_font_t *font = lv_obj_get_style_text_font(canvas, LV_PART_MAIN);
        int32_t lh = font ? lv_font_get_line_height(font) : 16;
        int32_t y = ar->y1 + 8 + lh + 8;

        words(layer, canvas, ar->x1 + 12, y, "PLANNED ROUTE, NOT A RADIO LINK",
              pos_theme_color(POS_COLOR_TEXT_PRIMARY), 1, 0);
        if (unplaced) {
            snprintf(text, sizeof(text), "%d RELAY%s WITH NO LOCATION: IN THE LIST ONLY", unplaced,
                     unplaced == 1 ? "" : "S");
            words(layer, canvas, ar->x1 + 12, y + lh + 6, text,
                  pos_theme_color(POS_COLOR_TEXT_PRIMARY), 1, 0);
        }
    }
}

