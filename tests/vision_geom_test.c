/*
 * Frame to picture geometry, held against the converter that draws the
 * picture: a single lit pixel in an RGB565 frame is drawn by
 * pocketcam_to_rgb565() and must land where vision_map_point() says, for
 * every rotation, mirrored and not, and for pictures the cover fit cuts on
 * either axis. Then boxes: clamped to the picture, or reported outside it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam/pocketcam_convert.h"
#include "pocketvision/vision_geom.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

enum { FW = 64, FH = 36 };

/* Where the converter puts frame pixel (sx, sy) in a vw x vh picture, or
 * -1 when it is cut off. Nearest-neighbour: the pixel may be drawn several
 * times or not at all; the first hit (top-left) is what is compared. */
static int drawn_at(uint32_t sx, uint32_t sy, int rotation, bool mirror, uint32_t vw, uint32_t vh,
                    int32_t *ox, int32_t *oy)
{
    static uint8_t rgb[FW * FH * 2];
    static uint16_t out[1024 * 1024 / 4];
    struct pocketcam_frame f = { POCKETCAM_FMT_RGB565, FW, FH, FW * 2, rgb, sizeof(rgb), 1, 0, 0 };
    uint32_t x;
    uint32_t y;

    memset(rgb, 0, sizeof(rgb));
    rgb[(sy * FW + sx) * 2] = 0xff;
    rgb[(sy * FW + sx) * 2 + 1] = 0xff;
    if (pocketcam_to_rgb565(&f, rotation, mirror, POCKETCAM_FIT_COVER, out, vw, vh, vw) != 0) {
        return -2;
    }
    for (y = 0; y < vh; y++) {
        for (x = 0; x < vw; x++) {
            if (out[y * vw + x] == 0xffff) {
                *ox = (int32_t)x;
                *oy = (int32_t)y;
                return 0;
            }
        }
    }
    return -1;
}

static void agree(const char *what, int rotation, bool mirror, uint32_t vw, uint32_t vh)
{
    struct vision_view v = { FW, FH, rotation, mirror, vw, vh };
    uint32_t sx;
    uint32_t sy;
    int ok = 1;
    int seen = 0;
    int cut = 0;
    int slack_ok = 1;
    char name[160];

    for (sy = 0; sy < FH; sy += 5) {
        for (sx = 0; sx < FW; sx += 7) {
            int32_t dx;
            int32_t dy;
            int32_t mx;
            int32_t my;
            int r = drawn_at(sx, sy, rotation, mirror, vw, vh, &dx, &dy);

            ok &= vision_map_point(&v, (int32_t)sx, (int32_t)sy, &mx, &my) == 0;
            if (r == 0) {
                seen++;
                /* Nearest-neighbour picks one source pixel per destination
                 * pixel; a scaled-up picture draws a source pixel over a
                 * block, so the first hit is within the block's size of the
                 * rounded centre. */
                {
                    int32_t tol = (int32_t)((vw + FW - 1) / FW) + (int32_t)((vh + FH - 1) / FH) + 1;

                    slack_ok &= abs(mx - dx) <= tol && abs(my - dy) <= tol;
                }
            } else if (r == -1) {
                cut++;
                /* Cut off by the cover fit: the mapping says outside, or
                 * within a pixel of the edge. */
                slack_ok &= mx < 1 || my < 1 || mx >= (int32_t)vw - 1 || my >= (int32_t)vh - 1;
            } else {
                ok = 0;
            }
        }
    }
    snprintf(name, sizeof(name), "%s: %d drawn pixels land where the map says (%d cut off)", what,
             seen, cut);
    check(name, ok && slack_ok && seen > 0);
}

static void test_points(void)
{
    /* The frame's own shape, no turn: identity at the same size. */
    agree("0, same size", 0, false, FW, FH);
    agree("0, twice the size", 0, false, FW * 2, FH * 2);
    agree("0, mirrored", 0, true, FW, FH);
    /* A quarter turn: the picture is tall. Unit A's mount (90) and the
     * shell's landscape (180). */
    agree("90, tall picture", 90, false, FH, FW);
    agree("90, tall picture, scaled", 90, false, FH * 3, FW * 3);
    agree("90, mirrored", 90, true, FH, FW);
    agree("180", 180, false, FW, FH);
    agree("180, mirrored, scaled", 180, true, FW * 2, FH * 2);
    agree("270, tall", 270, false, FH, FW);
    /* Shapes the cover fit has to cut: a square picture from a wide frame
     * (the sides go), and a very wide one (top and bottom go). */
    agree("0, square picture (sides cut)", 0, false, 36, 36);
    agree("0, very wide picture (top and bottom cut)", 0, false, 128, 32);
    agree("90, square from a tall turned frame (top and bottom cut)", 90, false, 36, 36);
}

static void test_boxes(void)
{
    struct vision_view v = { 640, 360, 90, false, 528, 938 };
    struct vision_box in;
    struct vision_box out;
    int32_t x;
    int32_t y;

    /* Unit A's portrait: 640 x 360 turned 90 is 360 x 640, covered by a
     * 528 x 938 picture: scale 528/360, and 640 x 528/360 = 938.6 so nothing
     * is cut. The frame's top-left corner turns to the top-right. */
    check("the frame's origin turns to the picture's top-right corner",
          vision_map_point(&v, 0, 0, &x, &y) == 0 && x >= 526 && y == 0);
    check("the frame's bottom-left corner is the picture's top-left",
          vision_map_point(&v, 0, 359, &x, &y) == 0 && x == 0 && y == 0);
    in = (struct vision_box) { 100, 50, 80, 160 };
    check("a box maps to a box inside the picture, turned",
          vision_map_box(&v, &in, &out) == 1 && out.x >= 0 && out.y >= 0 && out.x + out.w <= 528 &&
              out.y + out.h <= 938 && out.w > out.h);
    /* A box entirely in the part the cover fit cuts off. */
    v = (struct vision_view) { 640, 360, 0, false, 360, 360 };
    in = (struct vision_box) { 0, 0, 50, 50 };
    check("a box in the cut-off margin is outside", vision_map_box(&v, &in, &out) == 0);
    in = (struct vision_box) { 100, 0, 200, 50 };
    check("one over the edge is clamped to it", vision_map_box(&v, &in, &out) == 1 && out.x == 0 && out.w < 200);
    in = (struct vision_box) { 100, 0, 0, 50 };
    check("an empty box is refused", vision_map_box(&v, &in, &out) == -EINVAL);
    v.rotation = 45;
    check("a view with an impossible rotation is refused",
          vision_map_point(&v, 1, 1, &x, &y) == -EINVAL);
    v = (struct vision_view) { 0, 360, 0, false, 360, 360 };
    check("a view with no frame is refused", vision_map_point(&v, 1, 1, &x, &y) == -EINVAL);
}

/* The way back: a picture point unmaps to the frame pixel the converter
 * samples for it, so mapping it again lands on the same picture point. */
static void test_unmap(void)
{
    static const struct vision_view views[] = {
        { 640, 360, 90, false, 360, 640 },  /* unit A portrait */
        { 640, 360, 180, false, 802, 452 }, /* unit A landscape */
        { 640, 360, 0, true, 128, 32 },     /* mirrored, cut top and bottom */
        { 640, 360, 270, true, 36, 36 },
    };
    size_t k;
    int ok = 1;
    int32_t sx;
    int32_t sy;

    for (k = 0; k < sizeof(views) / sizeof(views[0]); k++) {
        const struct vision_view *v = &views[k];
        int32_t vx;
        int32_t vy;

        for (vy = 0; vy < (int32_t)v->view_h; vy += 7) {
            for (vx = 0; vx < (int32_t)v->view_w; vx += 5) {
                int32_t bx;
                int32_t by;

                ok &= vision_unmap_point(v, vx, vy, &sx, &sy) == 0 && sx >= 0 && sy >= 0 &&
                      sx < (int32_t)v->frame_w && sy < (int32_t)v->frame_h;
                ok &= vision_map_point(v, sx, sy, &bx, &by) == 0 && abs(bx - vx) <= 2 && abs(by - vy) <= 2;
            }
        }
    }
    check("every picture point unmaps into the frame and maps back onto itself", ok);
    {
        struct vision_view v = { 640, 360, 90, false, 360, 640 };

        check("the picture's top-left is the frame's bottom-left corner",
              vision_unmap_point(&v, 0, 0, &sx, &sy) == 0 && sx == 0 && sy == 359);
        check("the middle of the picture's height is the middle of the frame's width",
              vision_unmap_point(&v, 180, 320, &sx, &sy) == 0 && sx == 320);
        check("a point outside the picture is refused", vision_unmap_point(&v, 360, 0, &sx, &sy) == -EINVAL);
    }
}

/* TRAFFIC's region of interest fitted for the detector: clipped to the
 * frame, even, and refused when too small to be worth a run. */
static void test_crop_fit(void)
{
    struct vision_box in = { 101, 51, 301, 121 };
    struct vision_box out = { 0, 0, 0, 0 };

    check("a region inside the frame keeps its place, rounded to even",
          vision_crop_fit(640, 360, &in, &out) == 0 && out.x == 100 && out.y == 50 && out.w == 302 &&
              out.h == 122);
    in = (struct vision_box) { -20, 300, 700, 100 };
    check("one over the edges is clipped to the frame",
          vision_crop_fit(640, 360, &in, &out) == 0 && out.x == 0 && out.y == 300 && out.w == 640 && out.h == 60);
    in = (struct vision_box) { 0, 0, 31, 100 };
    check("one narrower than VISION_CROP_MIN is refused", vision_crop_fit(640, 360, &in, &out) == -EINVAL);
    in = (struct vision_box) { 0, 340, 100, 100 };
    check("and one that the frame's edge leaves too short", vision_crop_fit(640, 360, &in, &out) == -EINVAL);
    in = (struct vision_box) { 0, 0, 100, 100 };
    check("no frame, no crop", vision_crop_fit(0, 360, &in, &out) == -EINVAL);
}

/* The upright picture for the detector: every pixel lands where the preview
 * draws it (vision_map_point on a view of the turned size), in all three
 * planes, for all four turns; a box turned and turned back is the box. */
static void test_turn(void)
{
    enum { W = 7, H = 5, S = 9 }; /* a stride wider than the frame */
    static const int rot[4] = { 0, 90, 180, 270 };
    uint8_t src[3 * S * H];
    uint8_t dst[3 * W * H];
    int k;
    int pix_ok = 1;
    int box_ok = 1;
    uint32_t tw;
    uint32_t th;

    for (k = 0; k < (int)sizeof(src); k++) {
        src[k] = (uint8_t)(k * 7 + 3);
    }
    for (k = 0; k < 4; k++) {
        struct vision_view v;
        int32_t sx;
        int32_t sy;
        int p;

        vision_turned_size(W, H, rot[k], &tw, &th);
        v = (struct vision_view) { W, H, rot[k], false, tw, th };
        pix_ok &= vision_turn_planes(src, W, H, S, 3, rot[k], dst) == 0;
        for (sy = 0; sy < H; sy++) {
            for (sx = 0; sx < W; sx++) {
                int32_t vx;
                int32_t vy;

                pix_ok &= vision_map_point(&v, sx, sy, &vx, &vy) == 0;
                for (p = 0; p < 3; p++) {
                    pix_ok &= dst[p * W * H + vy * (int32_t)tw + vx] == src[p * S * H + sy * S + sx];
                }
            }
        }
        {
            struct vision_box in = { 1, 2, 4, 3 };
            struct vision_box t;
            struct vision_box back;
            int32_t ax;
            int32_t ay;
            int32_t bx;
            int32_t by;

            box_ok &= vision_box_turn(W, H, rot[k], &in, &t) == 0 && vision_box_unturn(W, H, rot[k], &t, &back) == 0 &&
                      back.x == in.x && back.y == in.y && back.w == in.w && back.h == in.h;
            /* and the turned box is where the preview draws the box's corners */
            vision_map_point(&v, in.x, in.y, &ax, &ay);
            vision_map_point(&v, in.x + in.w - 1, in.y + in.h - 1, &bx, &by);
            box_ok &= t.x == (ax < bx ? ax : bx) && t.y == (ay < by ? ay : by) &&
                      t.w == abs(bx - ax) + 1 && t.h == abs(by - ay) + 1;
        }
    }
    check("every pixel of the turned planes is where the preview draws it (0, 90, 180, 270)", pix_ok);
    check("a box turned is where the preview draws it, and turned back is itself", box_ok);
    vision_turned_size(640, 360, 90, &tw, &th);
    check("a quarter turn swaps the size", tw == 360 && th == 640);
    {
        struct vision_box b = { 0, 0, 10, 10 };
        struct vision_box o;

        check("a turn that is not one of the four is refused",
              vision_box_turn(640, 360, 45, &b, &o) == -EINVAL && vision_turn_planes(src, W, H, S, 3, 45, dst) == -EINVAL);
        b.w = 0;
        check("so is an empty box", vision_box_unturn(640, 360, 90, &b, &o) == -EINVAL);
    }
}

int main(void)
{
    test_points();
    test_boxes();
    test_unmap();
    test_crop_fit();
    test_turn();
    printf("vision_geom_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
