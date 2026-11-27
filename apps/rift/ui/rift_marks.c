/*
 * The warning sign and the favourite star. See rift_marks.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_marks.h"

#include "pos_styles.h"

#include <stdint.h>
#include <stdlib.h>

/* What a mark is showing, in its user data. */
struct mark_state {
    int value;
};

static void mark_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static lv_obj_t *mark_create(lv_obj_t *parent, int32_t w, int32_t h, lv_event_cb_t draw, int value)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct mark_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    s->value = value;
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, mark_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

static void mark_set(lv_obj_t *obj, int value)
{
    struct mark_state *s = obj ? lv_obj_get_user_data(obj) : NULL;

    if (s && s->value != value) {
        s->value = value;
        lv_obj_invalidate(obj);
    }
}

static int mark_get(lv_obj_t *obj)
{
    const struct mark_state *s = obj ? lv_obj_get_user_data(obj) : NULL;

    return s ? s->value : 0;
}

static void triangle(lv_layer_t *layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2,
                     int32_t y2, lv_color_t color)
{
    lv_draw_triangle_dsc_t dsc;

    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

static void stroke(lv_layer_t *layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   lv_color_t color, int32_t width)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x0;
    dsc.p1.y = y0;
    dsc.p2.x = x1;
    dsc.p2.y = y1;
    lv_draw_line(layer, &dsc);
}

static void box(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color,
                int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &dsc, &a);
}

/* ---- the warning sign --------------------------------------------------------- */

static void warn_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    enum rift_warn_tone tone = (enum rift_warn_tone)mark_get(obj);
    lv_area_t a;
    lv_color_t fill;
    lv_color_t ink;
    int32_t x1;
    int32_t x2;
    int32_t y1;
    int32_t y2;
    int32_t cx;
    int32_t h;

    if (!layer) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    x1 = a.x1 + 1;
    x2 = a.x2 - 1;
    y1 = a.y1 + 1;
    y2 = a.y2 - 1;
    cx = (x1 + x2) / 2;
    h = y2 - y1;
    switch (tone) {
    case RIFT_WARN_ERROR:
        fill = pos_theme_color(POS_COLOR_STATUS_ERROR);
        break;
    case RIFT_WARN_WARN:
        fill = pos_theme_color(POS_COLOR_STATUS_WARN);
        break;
    case RIFT_WARN_MUTED:
        fill = pos_theme_color(POS_COLOR_TEXT_MUTED);
        break;
    case RIFT_WARN_NEUTRAL:
    default:
        fill = pos_theme_color(POS_COLOR_TEXT_MUTED);
        break;
    }
    if (tone == RIFT_WARN_NEUTRAL) {
        /* Nothing to warn of: the sign's outline only, so the place is known
         * before it is ever needed and an empty one does not shout. */
        stroke(layer, cx, y1 + 1, x1 + 1, y2 - 1, fill, 2);
        stroke(layer, x1 + 1, y2 - 1, x2 - 1, y2 - 1, fill, 2);
        stroke(layer, x2 - 1, y2 - 1, cx, y1 + 1, fill, 2);
        ink = fill;
    } else {
        triangle(layer, cx, y1, x1, y2, x2, y2, fill);
        /* The mark in the background's colour, cut out of the fill. */
        ink = pos_theme_color(POS_COLOR_BG);
    }
    /* The exclamation mark: a bar from a third of the way down to just
     * under three quarters, and a dot under it. */
    box(layer, cx - 1, y1 + h * 36 / 100, cx + 1, y1 + h * 70 / 100, ink, 1);
    box(layer, cx - 1, y1 + h * 78 / 100, cx + 1, y1 + h * 86 / 100, ink, 1);
}

lv_obj_t *rift_warn_create(lv_obj_t *parent)
{
    return mark_create(parent, RIFT_WARN_W, RIFT_WARN_H, warn_draw, RIFT_WARN_NEUTRAL);
}

void rift_warn_set(lv_obj_t *sign, enum rift_warn_tone tone)
{
    mark_set(sign, (int)tone);
}

enum rift_warn_tone rift_warn_get(lv_obj_t *sign)
{
    return (enum rift_warn_tone)mark_get(sign);
}

/* ---- the star ----------------------------------------------------------------- */

/* A five-pointed star's ten corners on a unit circle, outer and inner in
 * turn from the top, in thousandths: cos and sin of -90 + 36k degrees. The
 * inner ones are scaled by 0.382 when drawn, the regular star's ratio. */
static const int16_t STAR_X[10] = { 0, 588, 951, 951, 588, 0, -588, -951, -951, -588 };
static const int16_t STAR_Y[10] = { -1000, -809, -309, 309, 809, 1000, 809, 309, -309, -809 };

/* A filled star of outer radius r (in 1/10 px) about cx, cy: a fan of ten
 * triangles from the centre, the star being star-shaped about it. */
static void star_fill(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r10, lv_color_t c)
{
    int32_t px[10];
    int32_t py[10];
    int k;

    for (k = 0; k < 10; k++) {
        int32_t rr = (k % 2) ? r10 * 382 / 1000 : r10;

        px[k] = cx + (STAR_X[k] * rr / 1000 + (STAR_X[k] >= 0 ? 5 : -5)) / 10;
        py[k] = cy + (STAR_Y[k] * rr / 1000 + (STAR_Y[k] >= 0 ? 5 : -5)) / 10;
    }
    for (k = 0; k < 10; k++) {
        triangle(layer, cx, cy, px[k], py[k], px[(k + 1) % 10], py[(k + 1) % 10], c);
    }
}

/* The fill the star is drawn on: the nearest ancestor that paints one, in
 * the state it is in now (a row being pressed is raised), else the screen's
 * background. Read from the styles, so nothing here names a colour. */
static lv_color_t under(lv_obj_t *obj)
{
    lv_obj_t *o;

    for (o = lv_obj_get_parent(obj); o; o = lv_obj_get_parent(o)) {
        if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) >= LV_OPA_COVER) {
            return lv_obj_get_style_bg_color(o, LV_PART_MAIN);
        }
    }
    return pos_theme_color(POS_COLOR_BG);
}

static void star_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int on = mark_get(obj);
    lv_area_t a;
    int32_t cx;
    int32_t cy;
    int32_t r10 = RIFT_STAR_PX * 10 / 2;

    if (!layer) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    cx = a.x1 + lv_area_get_width(&a) / 2;
    /* A star's visual centre sits under its geometric one: down a little. */
    cy = a.y1 + lv_area_get_height(&a) / 2 + 1;
    if (on) {
        star_fill(layer, cx, cy, r10, pos_theme_color(POS_COLOR_ACCENT_PRIMARY));
        return;
    }
    /* An outline: the star, and a smaller one cut out of it. Lines with
     * round caps at a star's sharp inner corners draw a tangle at this
     * size; two fills draw a clean edge. */
    star_fill(layer, cx, cy, r10, pos_theme_color(POS_COLOR_TEXT_SECONDARY));
    star_fill(layer, cx, cy, r10 - 45, under(obj));
}

lv_obj_t *rift_star_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    return mark_create(parent, w, h, star_draw, 0);
}

void rift_star_set(lv_obj_t *star, int on)
{
    mark_set(star, on ? 1 : 0);
}

int rift_star_get(lv_obj_t *star)
{
    return mark_get(star);
}
