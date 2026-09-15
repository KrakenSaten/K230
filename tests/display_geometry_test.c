/*
 * Display geometry, safe area and touch transform (ui/pocketui/pos_display.c).
 *
 * The mapping between native panel pixels and logical pixels is checked
 * against hand-derived tables; the edge and corner insets against the same
 * mapping applied to points inside them, so a table cannot drift from the
 * mapping; and the evdev configuration by running LVGL's own swap-then-
 * calibrate arithmetic on the raw readings of real native pixels, for every
 * rotation, a swapped controller and mirrored ranges. A touch that lands
 * anywhere but the logical pixel the display shows there fails.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_display.h"

#include <stdio.h>
#include <stdlib.h>

static int checks;
static int failed;

#define CHECK(what, cond)                                                       \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failed++;                                                           \
            printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);              \
        }                                                                       \
    } while (0)

static const enum pos_rotation rotations[] = { POS_ROTATION_0, POS_ROTATION_90, POS_ROTATION_180,
                                               POS_ROTATION_270 };

/* ---- the mapping --------------------------------------------------------- */

static void test_mapping(void)
{
    const int32_t w = 5;
    const int32_t h = 8;
    char what[160];
    size_t i;

    for (i = 0; i < 4; i++) {
        enum pos_rotation r = rotations[i];
        int seen[64] = { 0 };
        int ok_bounds = 1;
        int ok_inverse = 1;
        int ok_bijective = 1;
        int32_t lw = (r == POS_ROTATION_90 || r == POS_ROTATION_270) ? h : w;
        int32_t px;
        int32_t py;

        for (py = 0; py < h; py++) {
            for (px = 0; px < w; px++) {
                int32_t lx;
                int32_t ly;
                int32_t bx;
                int32_t by;

                pos_rotation_native_to_logical(r, w, h, px, py, &lx, &ly);
                if (lx < 0 || ly < 0 || lx >= lw || ly >= (lw == w ? h : w)) {
                    ok_bounds = 0;
                    continue;
                }
                if (seen[ly * lw + lx]++) {
                    ok_bijective = 0;
                }
                pos_rotation_logical_to_native(r, w, h, lx, ly, &bx, &by);
                if (bx != px || by != py) {
                    ok_inverse = 0;
                }
            }
        }
        snprintf(what, sizeof(what), "rotation %d: every native pixel lands on the logical display",
                 pos_rotation_degrees(r));
        CHECK(what, ok_bounds);
        snprintf(what, sizeof(what), "rotation %d: no two native pixels share a logical pixel",
                 pos_rotation_degrees(r));
        CHECK(what, ok_bijective);
        snprintf(what, sizeof(what), "rotation %d: logical_to_native undoes native_to_logical",
                 pos_rotation_degrees(r));
        CHECK(what, ok_inverse);
    }

    /* The K230 panel, by hand: where the native top-left and bottom-right
     * pixels go (pos_display.h). */
    {
        static const struct {
            enum pos_rotation r;
            int32_t tl_x, tl_y, br_x, br_y;
        } want[] = {
            { POS_ROTATION_0, 0, 0, 567, 1231 },
            { POS_ROTATION_90, 0, 567, 1231, 0 },
            { POS_ROTATION_180, 567, 1231, 0, 0 },
            { POS_ROTATION_270, 1231, 0, 0, 567 },
        };

        for (i = 0; i < 4; i++) {
            int32_t x;
            int32_t y;
            int ok;

            pos_rotation_native_to_logical(want[i].r, 568, 1232, 0, 0, &x, &y);
            ok = x == want[i].tl_x && y == want[i].tl_y;
            pos_rotation_native_to_logical(want[i].r, 568, 1232, 567, 1231, &x, &y);
            ok = ok && x == want[i].br_x && y == want[i].br_y;
            snprintf(what, sizeof(what), "rotation %d: native corners land where pos_display.h says",
                     pos_rotation_degrees(want[i].r));
            CHECK(what, ok);
        }
    }

    {
        enum pos_rotation r = POS_ROTATION_0;

        CHECK("0, 90, 180, 270 degrees parse", pos_rotation_from_degrees(0, &r) == 0 && r == POS_ROTATION_0 &&
                                                 pos_rotation_from_degrees(90, &r) == 0 && r == POS_ROTATION_90 &&
                                                 pos_rotation_from_degrees(180, &r) == 0 && r == POS_ROTATION_180 &&
                                                 pos_rotation_from_degrees(270, &r) == 0 && r == POS_ROTATION_270);
        CHECK("45, -90 and 360 degrees are refused", pos_rotation_from_degrees(45, &r) < 0 &&
                                                       pos_rotation_from_degrees(-90, &r) < 0 &&
                                                       pos_rotation_from_degrees(360, &r) < 0);
        CHECK("portrait panel: 90 and 270 are landscape, 0 and 180 are not",
              pos_rotation_is_landscape_of(POS_ROTATION_90, 568, 1232) &&
                  pos_rotation_is_landscape_of(POS_ROTATION_270, 568, 1232) &&
                  !pos_rotation_is_landscape_of(POS_ROTATION_0, 568, 1232) &&
                  !pos_rotation_is_landscape_of(POS_ROTATION_180, 568, 1232));
    }
}

/* ---- geometry, edges and corners ---------------------------------------- */

static const struct pos_panel asym = {
    .width = 568,
    .height = 1232,
    .edges = { .left = 1, .top = 2, .right = 3, .bottom = 4 },
    .corners = { .top_left = 10, .top_right = 20, .bottom_right = 30, .bottom_left = 40 },
};

static void test_geometry(void)
{
    /* Hand-derived: which native edge and corner each logical one is. */
    static const struct {
        enum pos_rotation r;
        int32_t w, h;
        struct pos_insets e;
        struct pos_corners c;
    } want[] = {
        { POS_ROTATION_0, 568, 1232, { 1, 2, 3, 4 }, { 10, 20, 30, 40 } },
        { POS_ROTATION_90, 1232, 568, { 2, 3, 4, 1 }, { 20, 30, 40, 10 } },
        { POS_ROTATION_180, 568, 1232, { 3, 4, 1, 2 }, { 30, 40, 10, 20 } },
        { POS_ROTATION_270, 1232, 568, { 4, 1, 2, 3 }, { 40, 10, 20, 30 } },
    };
    char what[160];
    size_t i;

    for (i = 0; i < 4; i++) {
        struct pos_display_geometry g;

        pos_display_geometry_init(&g, &asym, want[i].r);
        snprintf(what, sizeof(what), "rotation %d: logical size %dx%d", pos_rotation_degrees(want[i].r),
                 (int)want[i].w, (int)want[i].h);
        CHECK(what, g.width == want[i].w && g.height == want[i].h && g.native_width == 568 &&
                        g.native_height == 1232 && g.rotation == want[i].r);
        snprintf(what, sizeof(what), "rotation %d: edge insets follow their native edges",
                 pos_rotation_degrees(want[i].r));
        CHECK(what, g.edges.left == want[i].e.left && g.edges.top == want[i].e.top &&
                        g.edges.right == want[i].e.right && g.edges.bottom == want[i].e.bottom);
        snprintf(what, sizeof(what), "rotation %d: corner insets follow their native corners",
                 pos_rotation_degrees(want[i].r));
        CHECK(what, g.corners.top_left == want[i].c.top_left && g.corners.top_right == want[i].c.top_right &&
                        g.corners.bottom_right == want[i].c.bottom_right &&
                        g.corners.bottom_left == want[i].c.bottom_left);
    }

    /* The same, derived from points rather than the table: a pixel just inside
     * each native corner square must be just inside the logical corner square
     * of the same size, and a pixel in a native edge strip inside the logical
     * strip of the same width. */
    for (i = 0; i < 4; i++) {
        static const struct {
            int32_t px, py;
            int32_t size;
        } corner_px[] = { { 9, 9, 10 }, { 568 - 20, 19, 20 }, { 568 - 30, 1232 - 30, 30 }, { 39, 1232 - 40, 40 } };
        enum pos_rotation r = rotations[i];
        struct pos_display_geometry g;
        int ok = 1;
        size_t k;

        pos_display_geometry_init(&g, &asym, r);
        for (k = 0; k < 4; k++) {
            int32_t lx;
            int32_t ly;
            int32_t s = corner_px[k].size;
            int in_tl;
            int in_tr;
            int in_br;
            int in_bl;

            pos_rotation_native_to_logical(r, 568, 1232, corner_px[k].px, corner_px[k].py, &lx, &ly);
            in_tl = lx < s && ly < s && g.corners.top_left == s;
            in_tr = lx > g.width - 1 - s && ly < s && g.corners.top_right == s;
            in_br = lx > g.width - 1 - s && ly > g.height - 1 - s && g.corners.bottom_right == s;
            in_bl = lx < s && ly > g.height - 1 - s && g.corners.bottom_left == s;
            ok = ok && (in_tl + in_tr + in_br + in_bl) == 1;
            ok = ok && !pos_display_rect_is_safe(&g, lx, ly, lx, ly);
        }
        snprintf(what, sizeof(what), "rotation %d: corner squares move with the pixels in them",
                 pos_rotation_degrees(r));
        CHECK(what, ok);
    }

    /* Bars along an edge. */
    {
        struct pos_display_geometry g;
        struct pos_insets in;

        pos_display_geometry_init(&g, &asym, POS_ROTATION_0);
        in = pos_display_bar_insets(&g, POS_EDGE_TOP);
        CHECK("portrait top bar: its ends clear the top corners (10, 20)",
              in.left == 10 && in.right == 20 && in.top == 2 && in.bottom == 0);
        in = pos_display_bar_insets(&g, POS_EDGE_BOTTOM);
        CHECK("portrait bottom bar: its ends clear the bottom corners (40, 30)",
              in.left == 40 && in.right == 30 && in.bottom == 4 && in.top == 0);
        pos_display_geometry_init(&g, &asym, POS_ROTATION_270);
        in = pos_display_bar_insets(&g, POS_EDGE_TOP);
        CHECK("landscape (270) top bar: the native left edge's corners (40 at native bottom-left, 10 at top-left)",
              in.left == 40 && in.right == 10 && in.top == 1);
        in = pos_display_bar_insets(&g, POS_EDGE_LEFT);
        CHECK("landscape (270) left bar: top and bottom ends clear corners 40 and 30",
              in.left == 4 && in.top == 40 && in.bottom == 30);
        pos_display_geometry_init(&g, &asym, POS_ROTATION_90);
        in = pos_display_bar_insets(&g, POS_EDGE_TOP);
        CHECK("landscape (90) top bar: the native right edge's corners (20 at native top-right, 30 at bottom-right)",
              in.left == 20 && in.right == 30 && in.top == 3);
    }

    /* An edge inset larger than the corner wins. */
    {
        struct pos_panel p = { 100, 200, { 50, 0, 0, 0 }, { 10, 10, 10, 10 } };
        struct pos_display_geometry g;
        struct pos_insets in;

        pos_display_geometry_init(&g, &p, POS_ROTATION_0);
        in = pos_display_bar_insets(&g, POS_EDGE_TOP);
        CHECK("a bar's end inset is the larger of edge strip and corner", in.left == 50 && in.right == 10);
    }

    /* A rectangular platform: nothing is unsafe. */
    {
        struct pos_panel p = { 568, 1232, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
        int ok = 1;

        for (i = 0; i < 4; i++) {
            struct pos_display_geometry g;
            struct pos_insets in;

            pos_display_geometry_init(&g, &p, rotations[i]);
            in = pos_display_bar_insets(&g, POS_EDGE_TOP);
            ok = ok && in.left == 0 && in.right == 0 && in.top == 0 && in.bottom == 0;
            ok = ok && pos_display_rect_is_safe(&g, 0, 0, g.width - 1, g.height - 1);
            ok = ok && !pos_display_rect_is_safe(&g, 0, 0, g.width, g.height - 1);
        }
        CHECK("rectangular panel: zero bar insets, the whole display is safe, nothing beyond it", ok);
    }

    /* The status bar case on a panel with 30 px corner squares. */
    {
        struct pos_panel p = { 568, 1232, { 0, 0, 0, 0 }, { 30, 30, 30, 30 } };
        struct pos_display_geometry g;

        pos_display_geometry_init(&g, &p, POS_ROTATION_0);
        CHECK("a label starting at x 20 in the top 30 px enters the top-left corner",
              !pos_display_rect_is_safe(&g, 20, 19, 70, 37));
        CHECK("the same label from x 30 is safe", pos_display_rect_is_safe(&g, 30, 19, 80, 37));
        CHECK("a clock ending at x 547 enters the top-right corner", !pos_display_rect_is_safe(&g, 500, 19, 547, 37));
        CHECK("the same clock ending at x 537 is safe", pos_display_rect_is_safe(&g, 490, 19, 537, 37));
        CHECK("content below the corner band may start at x 0", pos_display_rect_is_safe(&g, 0, 30, 100, 100));
        pos_display_geometry_init(&g, &p, POS_ROTATION_270);
        CHECK("landscape: the clock at the right end of a 1232 px bar must end by x 1201",
              pos_display_rect_is_safe(&g, 1150, 19, 1201, 37) && !pos_display_rect_is_safe(&g, 1150, 19, 1202, 37));
    }
}

/* ---- touch ---------------------------------------------------------------- */

/* LVGL 9.5 lv_evdev.c _evdev_calibrate, verbatim arithmetic. */
static int32_t evdev_calibrate(int32_t v, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max)
{
    if (in_min != in_max) {
        v = (v - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    }
    return v < out_min ? out_min : v > out_max ? out_max : v;
}

/* LVGL's swap-then-calibrate on a raw (rx, ry) reading. */
static void evdev_point(const struct pos_evdev_config *c, int32_t lw, int32_t lh, int32_t rx, int32_t ry,
                        int32_t *lx, int32_t *ly)
{
    int32_t sx = c->swap_axes ? ry : rx;
    int32_t sy = c->swap_axes ? rx : ry;

    *lx = evdev_calibrate(sx, c->cal_x1, c->cal_x2, 0, lw - 1);
    *ly = evdev_calibrate(sy, c->cal_y1, c->cal_y2, 0, lh - 1);
}

/* What a controller described by raw reports at native pixel (px, py), for
 * ranges that are whole multiples of the panel size so it is exact. */
static void raw_at(const struct pos_touch_raw *raw, int32_t w, int32_t h, int32_t px, int32_t py, int32_t *rx,
                   int32_t *ry)
{
    int32_t nx = raw->x_at_left + (raw->x_at_right - raw->x_at_left) / (w - 1) * px;
    int32_t ny = raw->y_at_top + (raw->y_at_bottom - raw->y_at_top) / (h - 1) * py;

    *rx = raw->swap ? ny : nx;
    *ry = raw->swap ? nx : ny;
}

static void test_touch(void)
{
    const int32_t w = 568;
    const int32_t h = 1232;
    const struct pos_touch_raw raws[] = {
        { 0, 0, 567 * 2, 1231 * 2, false },            /* plain */
        { 0, 0, 567 * 2, 1231 * 2, true },             /* controller axes swapped */
        { 567 * 3, 0, 0, 1231 * 2, false },            /* X mirrored */
        { 100, 1231 * 2 + 50, 100 + 567 * 2, 50, true }, /* offset, Y mirrored, swapped */
    };
    const int32_t sample[][2] = { { 0, 0 }, { w - 1, 0 }, { w - 1, h - 1 }, { 0, h - 1 },
                                  { (w - 1) / 2, (h - 1) / 2 }, { 37, 1000 }, { 500, 12 } };
    char what[200];
    size_t i;
    size_t k;
    size_t s;

    for (k = 0; k < sizeof(raws) / sizeof(raws[0]); k++) {
        for (i = 0; i < 4; i++) {
            enum pos_rotation r = rotations[i];
            struct pos_display_geometry g;
            struct pos_panel p = { w, h, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
            struct pos_evdev_config c;
            int ok = 1;

            pos_display_geometry_init(&g, &p, r);
            pos_display_touch_config(r, &raws[k], &c);
            for (s = 0; s < sizeof(sample) / sizeof(sample[0]); s++) {
                int32_t rx;
                int32_t ry;
                int32_t tx;
                int32_t ty;
                int32_t dx;
                int32_t dy;

                raw_at(&raws[k], w, h, sample[s][0], sample[s][1], &rx, &ry);
                evdev_point(&c, g.width, g.height, rx, ry, &tx, &ty);
                pos_rotation_native_to_logical(r, w, h, sample[s][0], sample[s][1], &dx, &dy);
                if (tx != dx || ty != dy) {
                    ok = 0;
                    printf("     rotation %d raw case %zu: native (%d,%d) touched at logical (%d,%d), drawn at (%d,%d)\n",
                           pos_rotation_degrees(r), k, (int)sample[s][0], (int)sample[s][1], (int)tx, (int)ty,
                           (int)dx, (int)dy);
                }
            }
            snprintf(what, sizeof(what),
                     "rotation %d, controller case %zu: a touch on any native pixel lands on the logical pixel "
                     "drawn there (corners, centre, off-axis points)",
                     pos_rotation_degrees(r), k);
            CHECK(what, ok);
        }
    }

    /* The board's own ranges (0..1060 x 0..2400, not multiples): corners
     * exact, centre on the centre, everything within a pixel. */
    {
        const struct pos_touch_raw k230 = { 0, 0, 1060, 2400, false };

        for (i = 0; i < 4; i++) {
            enum pos_rotation r = rotations[i];
            struct pos_panel p = { w, h, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
            struct pos_display_geometry g;
            struct pos_evdev_config c;
            int32_t tx;
            int32_t ty;
            int32_t dx;
            int32_t dy;
            int corners_ok = 1;
            int32_t cx;
            int32_t cy;

            pos_display_geometry_init(&g, &p, r);
            pos_display_touch_config(r, &k230, &c);
            for (s = 0; s < 4; s++) {
                int32_t rx = sample[s][0] ? 1060 : 0;
                int32_t ry = sample[s][1] ? 2400 : 0;

                evdev_point(&c, g.width, g.height, rx, ry, &tx, &ty);
                pos_rotation_native_to_logical(r, w, h, sample[s][0], sample[s][1], &dx, &dy);
                corners_ok = corners_ok && tx == dx && ty == dy;
            }
            snprintf(what, sizeof(what), "K230 ranges, rotation %d: the four raw extremes are the four logical corners",
                     pos_rotation_degrees(r));
            CHECK(what, corners_ok);
            evdev_point(&c, g.width, g.height, 530, 1200, &cx, &cy);
            snprintf(what, sizeof(what), "K230 ranges, rotation %d: the raw centre is the logical centre (%d,%d)",
                     pos_rotation_degrees(r), (int)cx, (int)cy);
            CHECK(what, abs(cx - (g.width - 1) / 2) <= 1 && abs(cy - (g.height - 1) / 2) <= 1);
        }
    }

    /* The vendor launcher's per-angle evdev settings for the same board and
     * DRM index (k230_phone_ui apply_touch_transform), which it runs on unit
     * A: the configuration here must be the same, or the two disagree about
     * which way the panel is turned. */
    {
        const struct pos_touch_raw k230 = { 0, 0, 1060, 2400, false };
        static const struct {
            enum pos_rotation r;
            struct pos_evdev_config vendor;
        } want[] = {
            { POS_ROTATION_0, { false, 0, 0, 1060, 2400 } },
            { POS_ROTATION_90, { true, 0, 1060, 2400, 0 } },
            { POS_ROTATION_180, { false, 1060, 2400, 0, 0 } },
            { POS_ROTATION_270, { true, 2400, 0, 0, 1060 } },
        };

        for (i = 0; i < 4; i++) {
            struct pos_evdev_config c;

            pos_display_touch_config(want[i].r, &k230, &c);
            snprintf(what, sizeof(what), "rotation %d: evdev swap and calibration equal the vendor launcher's",
                     pos_rotation_degrees(want[i].r));
            CHECK(what, c.swap_axes == want[i].vendor.swap_axes && c.cal_x1 == want[i].vendor.cal_x1 &&
                            c.cal_y1 == want[i].vendor.cal_y1 && c.cal_x2 == want[i].vendor.cal_x2 &&
                            c.cal_y2 == want[i].vendor.cal_y2);
        }
    }
}

int main(void)
{
    test_mapping();
    test_geometry();
    test_touch();
    printf("display_geometry_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
