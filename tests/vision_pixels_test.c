/*
 * The pixel modes on synthetic pictures: a colour sampled and its matches
 * painted and located, the tolerance's reach, the highlight that never
 * hides its target; a step edge found where it is and nowhere else, grey
 * and thresholded; a dark line traced with its offset and lean, a light
 * one too, and a blank picture traced to nothing; pictures too small or
 * too wide refused.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_pixels.h"

#include <errno.h>
#include <stdio.h>
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

#define W 120
#define H 80

static uint16_t pic[H][W];

static struct vision_rgb rgb(uint8_t r, uint8_t g, uint8_t b)
{
    struct vision_rgb c = { r, g, b };

    return c;
}

static void fill(uint16_t v)
{
    int y;
    int x;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            pic[y][x] = v;
        }
    }
}

static void box(int x0, int y0, int w, int h, uint16_t v)
{
    int y;
    int x;

    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            pic[y][x] = v;
        }
    }
}

static void test_rgb(void)
{
    struct vision_rgb c = vision_rgb565_to_rgb(0xf800);

    check("red comes back as red", c.r == 255 && c.g == 0 && c.b == 0);
    c = vision_rgb565_to_rgb(0xffff);
    check("white as white", c.r == 255 && c.g == 255 && c.b == 255 && vision_luma(c) == 255);
    check("black is dark", vision_luma(vision_rgb565_to_rgb(0)) == 0);
    check("and back again", vision_rgb_to_rgb565(rgb(255, 0, 0)) == 0xf800 && vision_rgb_to_rgb565(rgb(0, 255, 0)) == 0x07e0);
}

static void test_color(void)
{
    struct vision_color_result r;
    struct vision_rgb c;
    uint16_t red = vision_rgb_to_rgb565(rgb(255, 0, 0));
    uint16_t dark_red = vision_rgb_to_rgb565(rgb(200, 30, 30));
    uint16_t green = vision_rgb_to_rgb565(rgb(0, 255, 0));

    fill(0);
    box(20, 10, 40, 20, red);       /* centre (40, 20) */
    box(80, 50, 10, 10, dark_red);
    check("sampling the middle of the red box gives red",
          vision_pixels_sample(&pic[0][0], W, H, W, 40, 20, &c) == 0 && c.r == 255 && c.g == 0 && c.b == 0);
    check("sampling a corner is clamped, not read past the picture",
          vision_pixels_sample(&pic[0][0], W, H, W, -5, 500, &c) == 0 && c.r == 0);
    check("a tight tolerance matches the red box alone, painted green, centred on it",
          vision_pixels_color(&pic[0][0], W, H, W, rgb(255, 0, 0), 30, &r) == 0 && r.matched == 800 &&
              r.total == W * H && r.cx == 39 && r.cy == 19 && pic[20][40] == green && pic[55][85] == dark_red);
    fill(0);
    box(20, 10, 40, 20, red);
    box(80, 50, 10, 10, dark_red);
    check("a wide tolerance takes the darker red too",
          vision_pixels_color(&pic[0][0], W, H, W, rgb(255, 0, 0), 120, &r) == 0 && r.matched == 900 &&
              pic[55][85] == green);
    fill(0);
    box(0, 0, 10, 10, green);
    check("a green target is painted magenta, so it stays visible",
          vision_pixels_color(&pic[0][0], W, H, W, rgb(0, 255, 0), 30, &r) == 0 && r.matched == 100 &&
              pic[5][5] == 0xf81f);
    fill(0);
    check("no match: no centroid", vision_pixels_color(&pic[0][0], W, H, W, rgb(255, 255, 0), 10, &r) == 0 &&
                                       r.matched == 0 && r.cx == -1 && r.cy == -1);
    check("a tolerance beyond the scale matches everything",
          vision_pixels_color(&pic[0][0], W, H, W, rgb(255, 255, 255), 10000, &r) == 0 && r.matched == W * H);
    check("a picture too small or too wide is refused",
          vision_pixels_color(&pic[0][0], 2, H, W, rgb(0, 0, 0), 10, &r) == -EINVAL &&
              vision_pixels_color(&pic[0][0], 2000, H, 2000, rgb(0, 0, 0), 10, &r) == -EINVAL &&
              vision_pixels_sample(NULL, W, H, W, 0, 0, &c) == -EINVAL);
}

static void test_edge(void)
{
    struct vision_edge_result r;
    int y;
    int ok = 1;
    uint16_t white = 0xffff;

    /* Black left, white right: one vertical edge at x = 60. */
    fill(0);
    box(60, 0, 60, H, white);
    check("a step edge is found", vision_pixels_edge(&pic[0][0], W, H, W, 0, &r) == 0 && r.total == W * H);
    for (y = 1; y < H - 1; y++) {
        ok &= vision_rgb565_to_rgb(pic[y][59]).r > 100 && vision_rgb565_to_rgb(pic[y][60]).r > 100; /* bright at the step */
        ok &= pic[y][30] == 0 && pic[y][100] == 0;         /* flat is black */
    }
    check("bright exactly along the step, black on the flats", ok);
    check("the strong edges are the two columns", r.strong == (uint32_t)(2 * H));
    fill(0);
    box(60, 0, 60, H, white);
    check("thresholded: white at the step, black elsewhere",
          vision_pixels_edge(&pic[0][0], W, H, W, 100, &r) == 0 && pic[40][60] == white && pic[40][30] == 0 &&
              pic[40][100] == 0);
    fill(0x7bef); /* flat grey */
    check("a flat picture has no edges", vision_pixels_edge(&pic[0][0], W, H, W, 0, &r) == 0 && r.strong == 0 &&
                                              pic[10][10] == 0);
    /* A horizontal edge too, and the top and bottom rows are handled. */
    fill(0);
    box(0, 40, W, 40, white);
    vision_pixels_edge(&pic[0][0], W, H, W, 0, &r);
    check("a horizontal step is found on its rows", vision_rgb565_to_rgb(pic[39][50]).r > 100 && vision_rgb565_to_rgb(pic[40][50]).r > 100 && pic[10][50] == 0 &&
                                                        pic[70][50] == 0 && pic[0][50] == 0 && pic[H - 1][50] == 0);
    check("a picture too small is refused", vision_pixels_edge(&pic[0][0], W, 2, W, 0, &r) == -EINVAL);
}

static void test_trace(void)
{
    struct vision_trace_result r;
    int y;
    uint16_t white = 0xffff;
    uint16_t black = 0;

    /* A dark line leaning right as it goes down, on a white floor:
     * x = 30 + y / 2, 6 px wide. */
    fill(white);
    for (y = 0; y < H; y++) {
        box(30 + y / 2, y, 6, 1, black);
    }
    check("a dark line is found", vision_pixels_trace(&pic[0][0], W, H, W, true, &r) == 0 && r.found && r.rows == H);
    check("it leans right going down: about 500 per 1000 rows", r.slope_pm > 400 && r.slope_pm < 600);
    /* The bottom band (rows 60..79) is centred about x = 30 + 70/2 + 3 = 68: 8 px right of 60. */
    check("the bottom band's offset is a little right of the centre", r.offset_pm > 50 && r.offset_pm < 250);
    check("the centroids are marked", pic[40][30 + 20 + 3] == 0xffe0);
    /* A light line on a dark floor, straight, left of centre. */
    fill(black);
    box(20, 0, 8, H, white);
    check("a light line straight down, left of centre",
          vision_pixels_trace(&pic[0][0], W, H, W, false, &r) == 0 && r.found && r.slope_pm > -30 && r.slope_pm < 30 &&
              r.offset_pm < -500 && r.offset_pm > -700);
    /* Nothing to trace. */
    fill(white);
    check("a blank picture has no line", vision_pixels_trace(&pic[0][0], W, H, W, true, &r) == 0 && !r.found && r.rows == 0);
    fill(black);
    check("nor does one that is all dark: the run is too wide", vision_pixels_trace(&pic[0][0], W, H, W, true, &r) == 0 && !r.found);
    /* Too few rows: a short dash. */
    fill(white);
    box(50, 0, 6, 5, black);
    check("a short dash is not a line", vision_pixels_trace(&pic[0][0], W, H, W, true, &r) == 0 && !r.found && r.rows == 5);
    check("a picture too small is refused", vision_pixels_trace(&pic[0][0], 2, H, W, true, &r) == -EINVAL);
}

int main(void)
{
    test_rgb();
    test_color();
    test_edge();
    test_trace();
    printf("vision_pixels_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
