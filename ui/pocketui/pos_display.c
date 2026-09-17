/*
 * Display geometry and safe area. See pos_display.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_display.h"

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

int pos_rotation_from_degrees(int degrees, enum pos_rotation *out)
{
    switch (degrees) {
    case 0: *out = POS_ROTATION_0; return 0;
    case 90: *out = POS_ROTATION_90; return 0;
    case 180: *out = POS_ROTATION_180; return 0;
    case 270: *out = POS_ROTATION_270; return 0;
    default: return -1;
    }
}

int pos_rotation_degrees(enum pos_rotation r)
{
    return (int)r * 90;
}

bool pos_rotation_is_landscape_of(enum pos_rotation r, int32_t native_w, int32_t native_h)
{
    bool quarter = r == POS_ROTATION_90 || r == POS_ROTATION_270;
    bool native_landscape = native_w > native_h;

    return quarter ? !native_landscape : native_landscape;
}

void pos_rotation_native_to_logical(enum pos_rotation r, int32_t native_w, int32_t native_h,
                                    int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
    switch (r) {
    case POS_ROTATION_90:
        *lx = py;
        *ly = native_w - 1 - px;
        break;
    case POS_ROTATION_180:
        *lx = native_w - 1 - px;
        *ly = native_h - 1 - py;
        break;
    case POS_ROTATION_270:
        *lx = native_h - 1 - py;
        *ly = px;
        break;
    case POS_ROTATION_0:
    default:
        *lx = px;
        *ly = py;
        break;
    }
}

void pos_rotation_logical_to_native(enum pos_rotation r, int32_t native_w, int32_t native_h,
                                    int32_t lx, int32_t ly, int32_t *px, int32_t *py)
{
    switch (r) {
    case POS_ROTATION_90:
        *px = native_w - 1 - ly;
        *py = lx;
        break;
    case POS_ROTATION_180:
        *px = native_w - 1 - lx;
        *py = native_h - 1 - ly;
        break;
    case POS_ROTATION_270:
        *px = ly;
        *py = native_h - 1 - lx;
        break;
    case POS_ROTATION_0:
    default:
        *px = lx;
        *py = ly;
        break;
    }
}

/* Which logical edge a logical point on the display's border lies on. The
 * points passed in are native edge midpoints, which land on exactly one. */
static enum pos_edge edge_of(const struct pos_display_geometry *g, int32_t lx, int32_t ly)
{
    if (lx == 0) {
        return POS_EDGE_LEFT;
    }
    if (ly == 0) {
        return POS_EDGE_TOP;
    }
    if (lx == g->width - 1) {
        return POS_EDGE_RIGHT;
    }
    return POS_EDGE_BOTTOM;
}

static void set_edge(struct pos_insets *in, enum pos_edge e, int32_t v)
{
    switch (e) {
    case POS_EDGE_LEFT: in->left = v; break;
    case POS_EDGE_TOP: in->top = v; break;
    case POS_EDGE_RIGHT: in->right = v; break;
    case POS_EDGE_BOTTOM: in->bottom = v; break;
    }
}

static void set_corner(struct pos_corners *c, int32_t lx, int32_t ly, int32_t v)
{
    bool left = lx == 0;
    bool top = ly == 0;

    if (top) {
        if (left) {
            c->top_left = v;
        } else {
            c->top_right = v;
        }
    } else if (left) {
        c->bottom_left = v;
    } else {
        c->bottom_right = v;
    }
}

void pos_display_geometry_init(struct pos_display_geometry *g, const struct pos_panel *panel,
                               enum pos_rotation r)
{
    const int32_t w = panel->width;
    const int32_t h = panel->height;
    const bool quarter = r == POS_ROTATION_90 || r == POS_ROTATION_270;
    int32_t lx;
    int32_t ly;

    g->rotation = r;
    g->native_width = w;
    g->native_height = h;
    g->width = quarter ? h : w;
    g->height = quarter ? w : h;

    pos_rotation_native_to_logical(r, w, h, 0, h / 2, &lx, &ly);
    set_edge(&g->edges, edge_of(g, lx, ly), panel->edges.left);
    pos_rotation_native_to_logical(r, w, h, w / 2, 0, &lx, &ly);
    set_edge(&g->edges, edge_of(g, lx, ly), panel->edges.top);
    pos_rotation_native_to_logical(r, w, h, w - 1, h / 2, &lx, &ly);
    set_edge(&g->edges, edge_of(g, lx, ly), panel->edges.right);
    pos_rotation_native_to_logical(r, w, h, w / 2, h - 1, &lx, &ly);
    set_edge(&g->edges, edge_of(g, lx, ly), panel->edges.bottom);

    pos_rotation_native_to_logical(r, w, h, 0, 0, &lx, &ly);
    set_corner(&g->corners, lx, ly, panel->corners.top_left);
    pos_rotation_native_to_logical(r, w, h, w - 1, 0, &lx, &ly);
    set_corner(&g->corners, lx, ly, panel->corners.top_right);
    pos_rotation_native_to_logical(r, w, h, w - 1, h - 1, &lx, &ly);
    set_corner(&g->corners, lx, ly, panel->corners.bottom_right);
    pos_rotation_native_to_logical(r, w, h, 0, h - 1, &lx, &ly);
    set_corner(&g->corners, lx, ly, panel->corners.bottom_left);
}

struct pos_insets pos_display_bar_insets(const struct pos_display_geometry *g, enum pos_edge edge)
{
    struct pos_insets in = { 0, 0, 0, 0 };
    const struct pos_insets *e = &g->edges;
    const struct pos_corners *c = &g->corners;

    switch (edge) {
    case POS_EDGE_TOP:
        in.top = e->top;
        in.left = max32(e->left, c->top_left);
        in.right = max32(e->right, c->top_right);
        break;
    case POS_EDGE_BOTTOM:
        in.bottom = e->bottom;
        in.left = max32(e->left, c->bottom_left);
        in.right = max32(e->right, c->bottom_right);
        break;
    case POS_EDGE_LEFT:
        in.left = e->left;
        in.top = max32(e->top, c->top_left);
        in.bottom = max32(e->bottom, c->bottom_left);
        break;
    case POS_EDGE_RIGHT:
        in.right = e->right;
        in.top = max32(e->top, c->top_right);
        in.bottom = max32(e->bottom, c->bottom_right);
        break;
    }
    return in;
}

bool pos_display_rect_is_safe(const struct pos_display_geometry *g, int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2)
{
    const struct pos_corners *c = &g->corners;
    const int32_t w = g->width;
    const int32_t h = g->height;

    if (x1 > x2 || y1 > y2) {
        return false;
    }
    if (x1 < g->edges.left || y1 < g->edges.top || x2 > w - 1 - g->edges.right ||
        y2 > h - 1 - g->edges.bottom) {
        return false;
    }
    if (c->top_left > 0 && x1 < c->top_left && y1 < c->top_left) {
        return false;
    }
    if (c->top_right > 0 && x2 > w - 1 - c->top_right && y1 < c->top_right) {
        return false;
    }
    if (c->bottom_right > 0 && x2 > w - 1 - c->bottom_right && y2 > h - 1 - c->bottom_right) {
        return false;
    }
    if (c->bottom_left > 0 && x1 < c->bottom_left && y2 > h - 1 - c->bottom_left) {
        return false;
    }
    return true;
}

struct pos_insets pos_display_rect_insets(const struct pos_display_geometry *g, int32_t x1,
                                          int32_t y1, int32_t x2, int32_t y2)
{
    const struct pos_corners *c = &g->corners;
    const int32_t w = g->width;
    const int32_t h = g->height;
    struct pos_insets in = {
        .left = max32(0, g->edges.left - x1),
        .top = max32(0, g->edges.top - y1),
        .right = max32(0, x2 - (w - 1 - g->edges.right)),
        .bottom = max32(0, y2 - (h - 1 - g->edges.bottom)),
    };

    if (c->top_left > 0 && x1 < c->top_left && y1 < c->top_left) {
        in.top = max32(in.top, c->top_left - y1);
    }
    if (c->top_right > 0 && x2 >= w - c->top_right && y1 < c->top_right) {
        in.top = max32(in.top, c->top_right - y1);
    }
    if (c->bottom_right > 0 && x2 >= w - c->bottom_right && y2 >= h - c->bottom_right) {
        in.bottom = max32(in.bottom, y2 - (h - c->bottom_right) + 1);
    }
    if (c->bottom_left > 0 && x1 < c->bottom_left && y2 >= h - c->bottom_left) {
        in.bottom = max32(in.bottom, y2 - (h - c->bottom_left) + 1);
    }
    return in;
}

void pos_display_touch_config(enum pos_rotation r, const struct pos_touch_raw *raw,
                              struct pos_evdev_config *out)
{
    /* The raw axis that runs along the panel's native X is the controller's
     * X unless the controller is mounted swapped. Logical X comes from the
     * native X axis at 0 and 180 degrees and from the native Y axis at 90
     * and 270, so LVGL is asked to swap exactly when those two differ. The
     * ranges follow the mapping in pos_display.h: a logical axis that runs
     * against a native one reads that native range backwards. */
    switch (r) {
    case POS_ROTATION_90:
        out->swap_axes = !raw->swap;
        out->cal_x1 = raw->y_at_top;
        out->cal_x2 = raw->y_at_bottom;
        out->cal_y1 = raw->x_at_right;
        out->cal_y2 = raw->x_at_left;
        break;
    case POS_ROTATION_180:
        out->swap_axes = raw->swap;
        out->cal_x1 = raw->x_at_right;
        out->cal_x2 = raw->x_at_left;
        out->cal_y1 = raw->y_at_bottom;
        out->cal_y2 = raw->y_at_top;
        break;
    case POS_ROTATION_270:
        out->swap_axes = !raw->swap;
        out->cal_x1 = raw->y_at_bottom;
        out->cal_x2 = raw->y_at_top;
        out->cal_y1 = raw->x_at_left;
        out->cal_y2 = raw->x_at_right;
        break;
    case POS_ROTATION_0:
    default:
        out->swap_axes = raw->swap;
        out->cal_x1 = raw->x_at_left;
        out->cal_x2 = raw->x_at_right;
        out->cal_y1 = raw->y_at_top;
        out->cal_y2 = raw->y_at_bottom;
        break;
    }
}
