/*
 * PacketShark's traffic graph. See psk_graph.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_graph.h"

#include "pos_styles.h"

#include <stdlib.h>
#include <string.h>

#define HELD_MARK_W 3
#define HELD_MARK_H 2
#define OUTLINE 2

struct graph_state {
    struct psk_graph_window w;
    int have;
    int64_t selected; /* absolute second, -1 none */
};

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &dsc, &a);
}

static lv_color_t class_color(unsigned cls)
{
    int hue = psk_class_hue(cls);

    return hue < 0 ? pos_theme_color(POS_COLOR_TEXT_SECONDARY) : pos_identity_hue((unsigned)hue);
}

/* A column's height shared between the classes in it, in proportion, every
 * class present getting at least one pixel taken from the largest
 * (rift_graph.c's split, DS §59.5). */
static void split(const uint32_t count[PPC_CLASS_COUNT], int32_t h, int32_t px[PPC_CLASS_COUNT])
{
    uint64_t total = 0;
    int32_t given = 0;
    int biggest = 0;
    int c;

    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        total += count[c];
        if (count[c] > count[biggest]) {
            biggest = c;
        }
    }
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        px[c] = 0;
        if (total == 0 || count[c] == 0) {
            continue;
        }
        px[c] = (int32_t)(((int64_t)h * count[c] + (int64_t)(total / 2)) / (int64_t)total);
        if (px[c] < 1) {
            px[c] = 1;
        }
        given += px[c];
    }
    px[biggest] += h - given;
    if (px[biggest] < 1 && count[biggest] > 0) {
        px[biggest] = 1;
    }
}

static int64_t column_second(const struct graph_state *s, int i)
{
    return s->w.newest - (PSK_GRAPH_SECONDS - 1) + i;
}

static void graph_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct graph_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    int32_t base;
    int32_t x;
    int32_t sel_x = -1;
    int32_t sel_h = 0;
    int k;
    int i;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    base = area.y2;
    /* The ladder's rules at 10, 100, 1k and 10k, under the columns. */
    for (k = 1; k <= 4; k++) {
        static const uint64_t decade[] = { 1, 10, 100, 1000, 10000 };
        int32_t y = base - psk_graph_height_of(decade[k]);

        fill(layer, area.x1, y, area.x1 + PSK_GRAPH_W - 1, y, pos_theme_color(POS_COLOR_SURFACE_RAISED));
    }
    if (!s->have) {
        return;
    }
    x = area.x1;
    for (i = 0; i < PSK_GRAPH_SECONDS; i++, x += PSK_GRAPH_COL_W + PSK_GRAPH_COL_GAP) {
        int32_t px[PPC_CLASS_COUNT];
        uint64_t total = 0;
        int32_t top = base - 1;
        int32_t h;
        int c;

        if (!s->w.watched[i]) {
            continue;
        }
        /* A baseline tick under every watched second: a quiet second was
         * watched, not missed (DS §59.5). */
        fill(layer, x, base, x + PSK_GRAPH_COL_W - 1, base, pos_theme_color(POS_COLOR_LINE));
        for (c = 0; c < PPC_CLASS_COUNT; c++) {
            total += s->w.cls[i][c];
        }
        h = psk_graph_height_of(total);
        if (h > 0) {
            split(s->w.cls[i], h, px);
            for (c = 0; c < PPC_CLASS_COUNT; c++) {
                if (px[c] <= 0) {
                    continue;
                }
                fill(layer, x, top - px[c] + 1, x + PSK_GRAPH_COL_W - 1, top, class_color((unsigned)c));
                top -= px[c];
            }
        } else if (s->w.held[i] > 0) {
            /* Held (HOLD, SAVING): counted, not stored. */
            fill(layer, x + 1, base - 1 - HELD_MARK_H, x + HELD_MARK_W, base - 2,
                 pos_theme_color(POS_COLOR_TEXT_MUTED));
        }
        if (column_second(s, i) == s->selected) {
            sel_x = x;
            sel_h = h > 4 ? h : 4;
        }
    }
    /* The selection last, over its neighbours: a 2 px focus outline. */
    if (sel_x >= 0) {
        lv_draw_rect_dsc_t dsc;
        lv_area_t a;

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_opa = LV_OPA_COVER;
        dsc.border_width = OUTLINE;
        dsc.border_color = pos_theme_color(POS_COLOR_FOCUS);
        dsc.radius = 0;
        a.x1 = sel_x - OUTLINE - 1;
        a.x2 = sel_x + PSK_GRAPH_COL_W + OUTLINE;
        a.y1 = base - sel_h - OUTLINE - 2;
        a.y2 = base;
        if (a.y1 < area.y1) {
            a.y1 = area.y1;
        }
        lv_draw_rect(layer, &dsc, &a);
    }
}

static void graph_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);

    free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

lv_obj_t *psk_graph_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct graph_state *s = calloc(1, sizeof(*s));

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, PSK_GRAPH_W, PSK_GRAPH_H);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (s) {
        s->selected = -1;
        s->w.newest = -1;
    }
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, graph_delete, LV_EVENT_DELETE, NULL);
    /* Colours are read while drawing: a theme or display-mode change must
     * invalidate this object itself (pos_styles.h). */
    pos_theme_watch(obj);
    return obj;
}

static void drop_stale(struct graph_state *s)
{
    if (s->selected >= 0 && (s->selected > s->w.newest || s->w.newest - s->selected >= PSK_GRAPH_SECONDS)) {
        s->selected = -1;
    }
}

void psk_graph_set(lv_obj_t *graph, const struct psk_graph_window *w)
{
    struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;

    if (!s || (s->have && memcmp(&s->w, w, sizeof(*w)) == 0)) {
        return;
    }
    s->w = *w;
    s->have = 1;
    drop_stale(s);
    lv_obj_invalidate(graph);
}

int64_t psk_graph_tap(lv_obj_t *graph, int32_t x)
{
    struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;
    lv_area_t area;
    int32_t col;
    int64_t sec;

    if (!s || s->w.newest < 0) {
        return -1;
    }
    lv_obj_get_coords(graph, &area);
    /* The nearest column: the whole band is the target (DS §59.5). */
    col = (x - area.x1 + PSK_GRAPH_COL_GAP / 2) / (PSK_GRAPH_COL_W + PSK_GRAPH_COL_GAP);
    if (col < 0) {
        col = 0;
    }
    if (col >= PSK_GRAPH_SECONDS) {
        col = PSK_GRAPH_SECONDS - 1;
    }
    sec = column_second(s, col);
    if (sec < 0 || !s->w.watched[col]) {
        return s->selected;
    }
    s->selected = sec == s->selected ? -1 : sec;
    lv_obj_invalidate(graph);
    return s->selected;
}

int64_t psk_graph_selected(lv_obj_t *graph)
{
    struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;

    return s ? s->selected : -1;
}

void psk_graph_clear(lv_obj_t *graph)
{
    struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;

    if (s && s->selected >= 0) {
        s->selected = -1;
        lv_obj_invalidate(graph);
    }
}
