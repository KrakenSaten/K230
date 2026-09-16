/*
 * Display geometry: the physical panel, the effective rotation, and what
 * follows from them - the logical size PocketUI lays out in and the safe
 * area content must stay inside. Pure C, no LVGL, so the shell, the display
 * backends, the widgets and the host tests share one definition.
 *
 *   physical panel (native size, edge and corner insets)
 *           |  effective rotation (one value, owned by the shell)
 *           v
 *   logical width x height, logical edge and corner insets
 *           |
 *           v
 *   PocketUI, status bar, launcher, apps
 *
 * One mapping between native panel pixels and logical pixels is defined
 * here (pos_rotation_native_to_logical). The safe-area transform and the
 * touch calibration are both derived from it, so display and touch cannot
 * disagree about which way is up.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POS_DISPLAY_H
#define POS_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* The index is the DRM plane rotation the vendor LVGL patch takes
 * (lv_linux_drm_set_rotation: 0, 1, 2, 3 = 0, 90, 180, 270 degrees). With
 * native size W x H and a native pixel (px, py), the logical pixel is:
 *   0:   (px,          py)
 *   90:  (py,          W - 1 - px)
 *   180: (W - 1 - px,  H - 1 - py)
 *   270: (H - 1 - py,  px)
 * This is the vendor launcher's touch transform for the same DRM index
 * (k230_phone_ui apply_touch_transform), which it uses together with that
 * DRM rotation on this board. */
enum pos_rotation {
    POS_ROTATION_0 = 0,
    POS_ROTATION_90 = 1,
    POS_ROTATION_180 = 2,
    POS_ROTATION_270 = 3,
};

struct pos_insets {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

/* For each corner, the side of the square at that corner that a rounded
 * panel corner may cut: content inside it can be hidden. The rounded mask
 * lies inside the square, so the square is the conservative description. */
struct pos_corners {
    int32_t top_left;
    int32_t top_right;
    int32_t bottom_right;
    int32_t bottom_left;
};

/* A physical panel, in its native scan-out orientation. */
struct pos_panel {
    int32_t width;
    int32_t height;
    struct pos_insets edges;     /* strips along straight edges that are not visible */
    struct pos_corners corners;  /* rounded corners */
};

/* What a layout sees: logical size and insets after rotation. */
struct pos_display_geometry {
    enum pos_rotation rotation;
    int32_t native_width;
    int32_t native_height;
    int32_t width;               /* logical */
    int32_t height;              /* logical */
    struct pos_insets edges;     /* logical edges */
    struct pos_corners corners;  /* logical corners */
};

enum pos_edge {
    POS_EDGE_LEFT,
    POS_EDGE_TOP,
    POS_EDGE_RIGHT,
    POS_EDGE_BOTTOM,
};

/* Degrees (0, 90, 180, 270) to a rotation; -1 for anything else. */
int pos_rotation_from_degrees(int degrees, enum pos_rotation *out);
int pos_rotation_degrees(enum pos_rotation r);
bool pos_rotation_is_landscape_of(enum pos_rotation r, int32_t native_w, int32_t native_h);

/* The one mapping (see enum pos_rotation), and its inverse. */
void pos_rotation_native_to_logical(enum pos_rotation r, int32_t native_w, int32_t native_h,
                                    int32_t px, int32_t py, int32_t *lx, int32_t *ly);
void pos_rotation_logical_to_native(enum pos_rotation r, int32_t native_w, int32_t native_h,
                                    int32_t lx, int32_t ly, int32_t *px, int32_t *py);

/* Logical geometry of a panel under a rotation. Edges and corners are carried
 * by mapping the native edges and corners through the mapping above. */
void pos_display_geometry_init(struct pos_display_geometry *g, const struct pos_panel *panel,
                               enum pos_rotation r);

/* Insets a bar lying along a logical edge needs so that its content is
 * visible: the edge's own inset across the bar, and at each end the larger
 * of the neighbouring edge's inset and the corner at that end. A status bar
 * is a bar along POS_EDGE_TOP. */
struct pos_insets pos_display_bar_insets(const struct pos_display_geometry *g, enum pos_edge edge);

/* Whether a logical rectangle (inclusive coordinates) is fully visible: on
 * the display, outside every edge strip and every corner square. */
bool pos_display_rect_is_safe(const struct pos_display_geometry *g, int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2);

/* ---- touch -------------------------------------------------------------- */

/* A touch controller as mounted on the panel: whether its X and Y axes are
 * swapped relative to the panel's, and, after that swap, the raw values read
 * at native pixel (0, 0) and at native pixel (W - 1, H - 1). Either end may
 * be the larger, so a mirrored axis is a reversed range. At rotation 0 this
 * is exactly LVGL's own swap and calibration, so POCKETOS_TOUCH_SWAP and
 * POCKETOS_TOUCH_CALIB keep their meaning. */
struct pos_touch_raw {
    int32_t x_at_left;
    int32_t y_at_top;
    int32_t x_at_right;
    int32_t y_at_bottom;
    bool swap;
};

/* The LVGL evdev configuration (lv_evdev_set_swap_axes and
 * lv_evdev_set_calibration) that turns raw events into logical coordinates
 * under a rotation. LVGL's evdev driver swaps first, then maps the first
 * value linearly from [cal_x1, cal_x2] onto [0, logical width - 1] and the
 * second from [cal_y1, cal_y2] onto [0, logical height - 1]. */
struct pos_evdev_config {
    bool swap_axes;
    int32_t cal_x1;
    int32_t cal_y1;
    int32_t cal_x2;
    int32_t cal_y2;
};

void pos_display_touch_config(enum pos_rotation r, const struct pos_touch_raw *raw,
                              struct pos_evdev_config *out);

#endif
