/*
 * PocketTimber table. See timber_table.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_table.h"

#include "pocketui.h"

#include <stdlib.h>

struct timber_table {
    const struct timber_run *run;
    const struct timber_view *view;
    int ghost;
    void (*tap)(void *user, int id);
    void *user;
};

static struct timber_table *state_of(const lv_obj_t *obj)
{
    return obj ? lv_obj_get_user_data((lv_obj_t *)obj) : NULL;
}

/* ---- drawing ----------------------------------------------------------- */

static void triangle(lv_layer_t *layer, int x0, int y0, int x1, int y1, int x2, int y2,
                     lv_color_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t dsc;

    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

static void quad_fill(lv_layer_t *layer, const struct timber_quad *q, int ox, int oy,
                      lv_color_t color, lv_opa_t opa)
{
    triangle(layer, ox + q->x[0], oy + q->y[0], ox + q->x[1], oy + q->y[1], ox + q->x[2],
             oy + q->y[2], color, opa);
    triangle(layer, ox + q->x[0], oy + q->y[0], ox + q->x[2], oy + q->y[2], ox + q->x[3],
             oy + q->y[3], color, opa);
}

static void quad_outline(lv_layer_t *layer, const struct timber_quad *q, int ox, int oy,
                         lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    int i;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    for (i = 0; i < 4; i++) {
        int j = (i + 1) % 4;

        dsc.p1.x = ox + q->x[i];
        dsc.p1.y = oy + q->y[i];
        dsc.p2.x = ox + q->x[j];
        dsc.p2.y = oy + q->y[j];
        lv_draw_line(layer, &dsc);
    }
}

/* A block: top, +x face, +y face, in three tones of the surface tokens so
 * the box reads with no light source at all; a hairline round the top. A
 * loose block's tilt raises the far edge of its top a little. */
static void draw_block(lv_layer_t *layer, struct timber_shape *s, int ox, int oy, int selected)
{
    if (s->tilt) {
        s->top.y[0] -= s->tilt * 3;
        s->top.y[1] -= s->tilt * 3;
    }
    quad_fill(layer, &s->left, ox, oy, pos_theme_color(POS_COLOR_SURFACE), LV_OPA_COVER);
    quad_fill(layer, &s->right, ox, oy, pos_theme_color(POS_COLOR_SURFACE_RAISED), LV_OPA_COVER);
    quad_fill(layer, &s->top, ox, oy, pos_theme_color(POS_COLOR_LINE), LV_OPA_COVER);
    quad_outline(layer, &s->top, ox, oy, pos_theme_color(POS_COLOR_TEXT_MUTED), 1, LV_OPA_60);
    if (selected) {
        quad_outline(layer, s->end_is_right ? &s->right : &s->left, ox, oy,
                     pos_theme_color(POS_COLOR_ACCENT_PRIMARY), 2, LV_OPA_COVER);
    }
}

static void table_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct timber_table *t = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    int ids[TIMBER_BLOCKS];
    int n;
    int i;
    int selected;

    if (!t || !layer || !t->run || !t->view) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    selected = timber_run_selected(t->run);
    n = timber_view_order(t->run, ids);
    for (i = 0; i < n; i++) {
        struct timber_shape s;

        if (timber_view_block(t->view, t->run, ids[i], &s) == 0) {
            draw_block(layer, &s, coords.x1, coords.y1, ids[i] == selected);
        }
    }
    if (t->ghost >= 0) {
        struct timber_shape s;

        if (timber_view_ghost(t->view, t->run, t->ghost, &s) == 0) {
            lv_color_t accent = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);

            quad_outline(layer, &s.top, coords.x1, coords.y1, accent, 2, LV_OPA_70);
            quad_outline(layer, s.end_is_right ? &s.right : &s.left, coords.x1, coords.y1, accent, 2,
                         LV_OPA_70);
        }
    }
}

/* ---- input --------------------------------------------------------------- */

static void table_click(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct timber_table *t = state_of(obj);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    lv_area_t coords;

    if (!t || !t->tap || !indev || !t->run || !t->view) {
        return;
    }
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(obj, &coords);
    t->tap(t->user, timber_view_pick(t->view, t->run, point.x - coords.x1, point.y - coords.y1));
}

static void table_delete(lv_event_t *e)
{
    free(state_of(lv_event_get_target_obj(e)));
}

static void table_theme_changed(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

lv_obj_t *timber_table_create(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct timber_table *t = calloc(1, sizeof(*t));

    if (!t) {
        lv_obj_delete(obj);
        return NULL;
    }
    t->ghost = -1;
    lv_obj_remove_style_all(obj);
    /* The viewport is a panel: the hairline frame around the scene the
     * design review asked for, and no colour named here. */
    pos_style_add(obj, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_user_data(obj, t);
    lv_obj_set_size(obj, width, height);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, table_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, table_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, table_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, table_theme_changed, (lv_event_code_t)pos_event_theme_changed(),
                            NULL);
    }
    return obj;
}

void timber_table_bind(lv_obj_t *table, const struct timber_run *run, const struct timber_view *view)
{
    struct timber_table *t = state_of(table);

    if (t) {
        t->run = run;
        t->view = view;
        lv_obj_invalidate(table);
    }
}

void timber_table_set_ghost(lv_obj_t *table, int slot)
{
    struct timber_table *t = state_of(table);

    if (t && t->ghost != slot) {
        t->ghost = slot;
        lv_obj_invalidate(table);
    }
}

void timber_table_set_tap(lv_obj_t *table, void (*cb)(void *user, int id), void *user)
{
    struct timber_table *t = state_of(table);

    if (t) {
        t->tap = cb;
        t->user = user;
    }
}
