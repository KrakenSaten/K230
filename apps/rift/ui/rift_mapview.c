/*
 * MAP. See rift_mapview.h, and rift_map.h for the geometry.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_mapview.h"

#include "pos_styles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_W 72
#define FIT_W 96
/* The pane beside the map in landscape. */
#define INFO_W_WIDE 400
/* Markers: a repeater's square and any other node's dot, and the ring
 * round the selected one. */
#define MARK_PX 11
#define RING_PX 25
/* Names are drawn for every marker while there are this few on screen;
 * past it only the selected node is named, and the rest are a tap away. */
#define NAMES_MAX 12
/* A press that moves further than this is a drag, not a tap. */
#define DRAG_PX 10
/* The most graticule lines drawn either way, whatever the zoom. */
#define GRID_LINES_MAX 40
#define SCALE_PX 120

struct rift_map_ui {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *map_col;
    lv_obj_t *caption;
    lv_obj_t *canvas;
    lv_obj_t *empty;
    lv_obj_t *info;
    lv_obj_t *info_name;
    lv_obj_t *info_meta;
    lv_obj_t *info_where;
    lv_obj_t *info_bar;
    lv_obj_t *info_detail;
    lv_obj_t *info_message;
    lv_obj_t *info_note;

    struct rift_map_view view;
    int have_view;
    int moved;      /* a reader zoomed or panned: no refit on its own */
    int located;    /* as last fitted */
    /* A press on the map: where it started and was last, and whether it
     * has become a drag. */
    lv_point_t press_at;
    lv_point_t press_last;
    int dragging;

    uint32_t drawn_seq;
    char drawn_sel[RIFT_KEY_HEX];
    int markers;
    unsigned draws;
};

static struct rift_map_ui *of(const struct rift_app *app)
{
    return app ? (struct rift_map_ui *)app->map : NULL;
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void show(lv_obj_t *o, int on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                 lv_color_t color, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a = { x1, y1, x2, y2 };

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    lv_draw_rect(layer, &dsc, &a);
}

static void ring(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t d, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a = { cx - d / 2, cy - d / 2, cx - d / 2 + d - 1, cy - d / 2 + d - 1 };

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_width = 2;
    dsc.border_color = color;
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &a);
}

/* A line of text at x, y; one that would run past the map's right edge is
 * drawn ending at left_of instead (a marker's name goes to its other side,
 * never over it). */
static void text(lv_layer_t *layer, lv_obj_t *obj, int32_t x, int32_t y, const char *s,
                 lv_color_t color, const lv_area_t *clip, int32_t left_of)
{
    lv_draw_label_dsc_t dsc;
    lv_area_t a;
    int32_t w = rift_cell_text_width(obj, s) + 2;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    int32_t h = font ? lv_font_get_line_height(font) : 16;

    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.text = s;
    dsc.letter_space = lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN);
    if (x + w > clip->x2) {
        x = (left_of > clip->x1 + w ? left_of : clip->x2) - w;
    }
    a.x1 = x;
    a.y1 = y;
    a.x2 = x + w;
    a.y2 = y + h;
    lv_draw_label(layer, &dsc, &a);
}

static void draw_grid(struct rift_map_ui *u, lv_layer_t *layer, const lv_area_t *ar)
{
    const struct rift_map_view *v = &u->view;
    lv_color_t line = pos_theme_color(POS_COLOR_LINE);
    double step = rift_map_grid_step(v);
    double lat_top;
    double lat_bot;
    double lon_l;
    double lon_r;
    double g;
    int n;

    rift_map_unproject(v, 0, 0, &lat_top, &lon_l);
    rift_map_unproject(v, (double)v->w, (double)v->h, &lat_bot, &lon_r);
    for (g = ceil(lat_bot / step) * step, n = 0; g <= lat_top && n < GRID_LINES_MAX;
         g += step, n++) {
        double x;
        double y;

        rift_map_project(v, g, v->c_lon, &x, &y);
        rect(layer, ar->x1, ar->y1 + (int32_t)y, ar->x2, ar->y1 + (int32_t)y, line, 0);
    }
    for (g = ceil(lon_l / step) * step, n = 0; g <= lon_r && n < GRID_LINES_MAX;
         g += step, n++) {
        double x;
        double y;

        rift_map_project(v, v->c_lat, g, &x, &y);
        rect(layer, ar->x1 + (int32_t)x, ar->y1, ar->x1 + (int32_t)x, ar->y2, line, 0);
    }
}

static void draw_scale(struct rift_map_ui *u, lv_layer_t *layer, const lv_area_t *ar)
{
    lv_color_t ink = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
    int32_t px = 0;
    double metres = rift_map_scale(&u->view, SCALE_PX, &px);
    int32_t x = ar->x1 + 12;
    int32_t y = ar->y2 - 12;
    char words[24];

    if (metres >= 1000) {
        snprintf(words, sizeof(words), "%g km", metres / 1000.0);
    } else {
        snprintf(words, sizeof(words), "%g m", metres);
    }
    rect(layer, x, y, x + px, y + 1, ink, 0);
    rect(layer, x, y - 5, x + 1, y + 1, ink, 0);
    rect(layer, x + px - 1, y - 5, x + px, y + 1, ink, 0);
    text(layer, u->canvas, x, y - 6 - lv_font_get_line_height(
                                  lv_obj_get_style_text_font(u->canvas, LV_PART_MAIN)),
         words, ink, ar, ar->x2);
}

static void canvas_draw(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const struct rift_model *m;
    const struct rift_node *sel;
    lv_area_t ar;
    int64_t now;
    int on_screen = 0;
    int pass;
    int i;

    if (!u || !layer || !u->have_view) {
        return;
    }
    m = &u->app->model;
    sel = rift_app_selected(u->app);
    now = rift_app_now(u->app);
    lv_obj_get_coords(u->canvas, &ar);
    u->draws++;
    rect(layer, ar.x1, ar.y1, ar.x2, ar.y2, pos_theme_color(POS_COLOR_SURFACE), 0);
    draw_grid(u, layer, &ar);
    /* Count first: names for all only when there are few. */
    for (i = 0; i < m->node_count; i++) {
        double x;
        double y;

        if (!m->nodes[i].have_location) {
            continue;
        }
        rift_map_project(&u->view, m->nodes[i].lat, m->nodes[i].lon, &x, &y);
        if (x >= 0 && y >= 0 && x < u->view.w && y < u->view.h) {
            on_screen++;
        }
    }
    u->markers = on_screen;
    /* Two passes: every marker, then the selected one on top of them. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < m->node_count; i++) {
            const struct rift_node *n = &m->nodes[i];
            int is_sel = sel && n == sel;
            int repeater = rift_node_is_repeater(n);
            lv_color_t color;
            int32_t cx;
            int32_t cy;
            double x;
            double y;

            if (!n->have_location || is_sel != (pass == 1)) {
                continue;
            }
            rift_map_project(&u->view, n->lat, n->lon, &x, &y);
            if (x < -RING_PX || y < -RING_PX || x > u->view.w + RING_PX ||
                y > u->view.h + RING_PX) {
                continue;
            }
            cx = ar.x1 + (int32_t)x;
            cy = ar.y1 + (int32_t)y;
            /* The shape says the kind; the colour agrees with it and with
             * how lately it was heard, and never carries either alone. */
            if (rift_node_is_stale(n, now)) {
                color = pos_theme_color(POS_COLOR_TEXT_MUTED);
            } else if (repeater) {
                color = pos_theme_color(POS_COLOR_RADIO_RX);
            } else {
                color = pos_theme_color(POS_COLOR_ACCENT_SECONDARY);
            }
            rect(layer, cx - MARK_PX / 2, cy - MARK_PX / 2, cx + MARK_PX / 2, cy + MARK_PX / 2,
                 color, repeater ? 1 : LV_RADIUS_CIRCLE);
            if (is_sel) {
                ring(layer, cx, cy, RING_PX, pos_theme_color(POS_COLOR_ACCENT_PRIMARY));
            }
            if (is_sel || on_screen <= NAMES_MAX) {
                char label[RIFT_LABEL_MAX];

                rift_fmt_label(n, label, sizeof(label));
                text(layer, u->canvas, cx + RING_PX / 2 + 2, cy - 8, label,
                     pos_theme_color(is_sel ? POS_COLOR_TEXT_PRIMARY : POS_COLOR_TEXT_SECONDARY),
                     &ar, cx - RING_PX / 2 - 2);
            }
        }
    }
    draw_scale(u, layer, &ar);
}

/* ---- touch: tap selects, drag pans ---------------------------------------------- */

static void canvas_press(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;

    if (!u || !indev) {
        return;
    }
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        u->press_at = p;
        u->press_last = p;
        u->dragging = 0;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!u->dragging && LV_ABS(p.x - u->press_at.x) + LV_ABS(p.y - u->press_at.y) > DRAG_PX) {
            u->dragging = 1;
        }
        if (u->dragging && u->have_view) {
            rift_map_pan(&u->view, (double)(p.x - u->press_last.x),
                         (double)(p.y - u->press_last.y));
            u->moved = 1;
            lv_obj_invalidate(u->canvas);
        }
        u->press_last = p;
        return;
    }
    if (code == LV_EVENT_RELEASED && !u->dragging && u->have_view) {
        lv_area_t ar;
        int at;

        lv_obj_get_coords(u->canvas, &ar);
        at = rift_map_hit(&u->view, &u->app->model, (double)(p.x - ar.x1),
                          (double)(p.y - ar.y1));
        if (at >= 0) {
            /* The selection NODES and NET share: it asks the service about
             * this node, and sends nothing. */
            rift_app_select(u->app, u->app->model.nodes[at].key);
            lv_obj_invalidate(u->canvas);
        }
    }
    u->dragging = 0;
}

static void on_zoom_in(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);

    if (u->have_view) {
        rift_map_zoom(&u->view, 2.0);
        u->moved = 1;
        lv_obj_invalidate(u->canvas);
    }
}

static void on_zoom_out(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);

    if (u->have_view) {
        rift_map_zoom(&u->view, 0.5);
        u->moved = 1;
        lv_obj_invalidate(u->canvas);
    }
}

static void on_fit(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);

    u->moved = 0;
    u->have_view = 0; /* refitted on the refresh, against the settled size */
    u->app->refresh_pending = 1;
}

static void on_detail(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);

    if (rift_app_selected(u->app)) {
        rift_app_show_section(u->app, RIFT_SEC_NODES);
        rift_app_open_detail(u->app, 1);
    }
}

static void on_message(lv_event_t *e)
{
    struct rift_map_ui *u = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(u->app);

    /* Opens COMMS on this peer, as NODES' MESSAGE does; refused for a node
     * that takes no messages (rift_app_open_conversation). Sends nothing. */
    if (n) {
        rift_app_open_conversation(u->app, n->key);
    }
}

/* ---- building -------------------------------------------------------------------- */

lv_obj_t *rift_map_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_map_ui *u = calloc(1, sizeof(*u));
    lv_obj_t *bar;
    lv_obj_t *b;

    if (!u) {
        return NULL;
    }
    app->map = u;
    u->app = app;

    u->root = lv_obj_create(parent);
    lv_obj_remove_style_all(u->root);
    lv_obj_set_size(u->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(u->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(u->root, RIFT_PAD, 0);
    lv_obj_set_style_pad_gap(u->root, 12, 0);
    lv_obj_remove_flag(u->root, LV_OBJ_FLAG_SCROLLABLE);

    u->map_col = lv_obj_create(u->root);
    lv_obj_remove_style_all(u->map_col);
    lv_obj_set_flex_flow(u->map_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(u->map_col, 8, 0);
    lv_obj_remove_flag(u->map_col, LV_OBJ_FLAG_SCROLLABLE);

    bar = lv_obj_create(u->map_col);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, RIFT_TOUCH_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    u->caption = rift_cell(bar, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(u->caption, 1);
    lv_obj_set_width(u->caption, 1);
    b = rift_action(bar, "\xE2\x88\x92", 0, 1, on_zoom_out, u);
    lv_obj_set_flex_grow(b, 0);
    lv_obj_set_width(b, BTN_W);
    b = rift_action(bar, "+", 0, 1, on_zoom_in, u);
    lv_obj_set_flex_grow(b, 0);
    lv_obj_set_width(b, BTN_W);
    b = rift_action(bar, "FIT", 0, 1, on_fit, u);
    lv_obj_set_flex_grow(b, 0);
    lv_obj_set_width(b, FIT_W);

    u->canvas = lv_obj_create(u->map_col);
    lv_obj_remove_style_all(u->canvas);
    /* The caption's type for the names and the scale, drawn by hand. */
    pos_style_add(u->canvas, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(u->canvas, LV_PCT(100));
    lv_obj_set_flex_grow(u->canvas, 1);
    lv_obj_remove_flag(u->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(u->canvas, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(u->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(u->canvas, canvas_draw, LV_EVENT_DRAW_MAIN, u);
    lv_obj_add_event_cb(u->canvas, canvas_press, LV_EVENT_PRESSED, u);
    lv_obj_add_event_cb(u->canvas, canvas_press, LV_EVENT_PRESSING, u);
    lv_obj_add_event_cb(u->canvas, canvas_press, LV_EVENT_RELEASED, u);
    /* Over the map when nothing can be placed on it. */
    u->empty = wrapping(u->canvas, POS_STYLE_TEXT_MUTED);
    lv_obj_add_flag(u->empty, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_width(u->empty, LV_PCT(80));
    lv_obj_align(u->empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(u->empty, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(u->empty, "No node has said where it is. A MeshCore node puts a location "
                                "in its adverts only when its owner has set one.");

    u->info = rift_panel(u->root, "NODE");
    u->info_name = wrapping(u->info, POS_STYLE_ROW_TITLE);
    u->info_meta = wrapping(u->info, POS_STYLE_CAPTION);
    u->info_where = wrapping(u->info, POS_STYLE_CAPTION);
    u->info_bar = lv_obj_create(u->info);
    lv_obj_remove_style_all(u->info_bar);
    lv_obj_set_width(u->info_bar, LV_PCT(100));
    lv_obj_set_height(u->info_bar, RIFT_TOUCH_H);
    lv_obj_set_flex_flow(u->info_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(u->info_bar, 12, 0);
    lv_obj_remove_flag(u->info_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(u->info_bar, LV_OBJ_FLAG_CLICKABLE);
    u->info_message = rift_action(u->info_bar, "MESSAGE", 0, 1, on_message, u);
    u->info_detail = rift_action(u->info_bar, "DETAIL \xE2\x80\xBA", 0, 1, on_detail, u);
    u->info_note = wrapping(u->info, POS_STYLE_CAPTION);
    return u->root;
}

void rift_map_view_destroy(struct rift_app *app)
{
    if (of(app)) {
        free(app->map);
        app->map = NULL;
    }
}

void rift_map_view_shape(struct rift_app *app)
{
    struct rift_map_ui *u = of(app);

    if (!u) {
        return;
    }
    /* Landscape: the map, and the node beside it. Portrait: the node under
     * the map. The map takes whatever the panel leaves. */
    lv_obj_set_flex_flow(u->root, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(u->map_col, 1);
    if (app->wide) {
        lv_obj_set_size(u->map_col, 1, LV_PCT(100));
        lv_obj_set_size(u->info, INFO_W_WIDE, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_size(u->map_col, LV_PCT(100), 1);
        lv_obj_set_size(u->info, LV_PCT(100), LV_SIZE_CONTENT);
    }
    /* Fitted again against the new shape (from the refresh, outside the
     * layout pass), unless the reader has moved it - then the centre and
     * scale are kept for the new size. */
    if (!u->moved) {
        u->have_view = 0;
    }
}

/* ---- refresh -------------------------------------------------------------------- */

static void fmt_coord(double v, char pos, char neg, char *out, size_t len)
{
    snprintf(out, len, "%.5f\xC2\xB0 %c", v < 0 ? -v : v, v < 0 ? neg : pos);
}

static void refresh_info(struct rift_map_ui *u, int located)
{
    const struct rift_model *m = &u->app->model;
    const struct rift_node *n = rift_app_selected(u->app);
    int64_t now = rift_app_now(u->app);
    char text[RIFT_STATE_MAX + 64];
    char label[RIFT_LABEL_MAX];

    if (!n) {
        rift_label_set(u->info_name, located ? "Tap a marker" : "Nothing to place");
        rift_label_set(u->info_meta, "");
        rift_label_set(u->info_where, "");
        show(u->info_bar, 0);
    } else {
        const char *tag = rift_type_tag(n->type, n->have_type);
        char state[RIFT_STATE_MAX];
        char age[RIFT_AGE_MAX];

        rift_fmt_label(n, label, sizeof(label));
        rift_label_set(u->info_name, label);
        rift_fmt_state(n, state, sizeof(state));
        rift_fmt_age(now - n->heard_mono_ms, n->have_heard, age, sizeof(age));
        snprintf(text, sizeof(text), "%s%s%s" RIFT_SEP "%s" RIFT_SEP "HEARD %s", tag ? tag : "",
                 tag ? RIFT_SEP : "", n->hash, state, age);
        rift_label_set(u->info_meta, text);
        if (n->have_location) {
            char lat[32];
            char lon[32];

            fmt_coord(n->lat, 'N', 'S', lat, sizeof(lat));
            fmt_coord(n->lon, 'E', 'W', lon, sizeof(lon));
            snprintf(text, sizeof(text), "%s  %s" RIFT_SEP "AS ITS ADVERTS SAY", lat, lon);
        } else {
            snprintf(text, sizeof(text), "NO LOCATION: ITS ADVERTS CARRY NONE");
        }
        rift_label_set(u->info_where, text);
        show(u->info_bar, 1);
        show(u->info_message, rift_node_can_message(n));
    }
    snprintf(text, sizeof(text),
             "%d OF %d KNOWN NODES HAVE A LOCATION" RIFT_SEP
             "THIS DEVICE HAS NONE: ITS ADVERTS CARRY NO LOCATION" RIFT_SEP "NO BASEMAP",
             located, m->node_count);
    rift_label_set(u->info_note, text);
}

void rift_map_view_refresh(struct rift_app *app)
{
    struct rift_map_ui *u = of(app);
    const struct rift_model *m;
    int32_t w;
    int32_t h;
    int located;
    const char *legend[3];
    char full[96];

    if (!u) {
        return;
    }
    m = &app->model;
    located = rift_map_located(m);
    w = lv_obj_get_content_width(u->canvas);
    h = lv_obj_get_content_height(u->canvas);
    if (w > 1 && h > 1) {
        if (!u->have_view || (!u->moved && located != u->located)) {
            rift_map_fit(&u->view, m, w, h);
            u->have_view = 1;
            u->located = located;
            lv_obj_invalidate(u->canvas);
        } else if (w != u->view.w || h != u->view.h) {
            rift_map_resize(&u->view, w, h);
            lv_obj_invalidate(u->canvas);
        }
    }
    /* A repaint only when something drawn changed: a node, or the selection. */
    if (m->seq != u->drawn_seq ||
        strcmp(u->drawn_sel, app->have_selected ? app->selected : "") != 0) {
        u->drawn_seq = m->seq;
        snprintf(u->drawn_sel, sizeof(u->drawn_sel), "%s", app->have_selected ? app->selected : "");
        lv_obj_invalidate(u->canvas);
    }
    show(u->empty, located == 0);
    /* The shapes in words: the font is not asked for glyphs it may lack. */
    snprintf(full, sizeof(full), "SQUARE REPEATER" RIFT_SEP "DOT NODE" RIFT_SEP "%d PLACED",
             located);
    legend[0] = full;
    legend[1] = "SQUARE RPT" RIFT_SEP "DOT NODE";
    legend[2] = "";
    rift_cell_set_text_first_fit(u->caption, legend, 3);
    refresh_info(u, located);
}

lv_obj_t *rift_map_view_canvas(const struct rift_app *app)
{
    return of(app) ? of(app)->canvas : NULL;
}

const struct rift_map_view *rift_map_view_geometry(const struct rift_app *app)
{
    return (of(app) && of(app)->have_view) ? &of(app)->view : NULL;
}

int rift_map_view_markers(const struct rift_app *app)
{
    return of(app) ? of(app)->markers : 0;
}

unsigned rift_map_view_draws(const struct rift_app *app)
{
    return of(app) ? of(app)->draws : 0;
}
