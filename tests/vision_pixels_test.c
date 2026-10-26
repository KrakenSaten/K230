/*
 * The pixel modes on synthetic pictures: a colour sampled and its matches
 * painted and located, the tolerance's reach, the highlight that never
 * hides its target; a step edge found where it is and nowhere else, grey
 * and thresholded; LINE TRACE's line followed with its offset, lean and
 * ends, told apart from the other candidates, found by its contrast on a
 * grey floor, not found in blanks, dashes, stripes or the upper half alone,
 * and over pictures smoothed, lost at once and reacquired where it was;
 * pictures too small or too wide refused.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* A vertical dark line, `wd` px wide, from row y0 to the bottom, its left
 * edge at x + lean * (y - y0) / 8. */
static void line_at(int x, int lean8, int wd, int y0, uint16_t v)
{
    int y;

    for (y = y0; y < H; y++) {
        box(x + (lean8 * (y - y0)) / 8, y, wd, 1, v);
    }
}

static void test_trace(void)
{
    struct vision_trace_result r;
    struct vision_trace_state st;
    int y;
    int ok;
    uint16_t white = 0xffff;
    uint16_t black = 0;
    uint16_t floor_grey = vision_rgb_to_rgb565(rgb(200, 200, 200));
    uint16_t line_grey = vision_rgb_to_rgb565(rgb(110, 110, 110));

    /* A dark line leaning right as it goes down, on a white floor:
     * x = 30 + y / 2, 6 px wide. */
    fill(white);
    for (y = 0; y < H; y++) {
        box(30 + y / 2, y, 6, 1, black);
    }
    check("a dark line is found, on every row", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 &&
                                                    r.found && r.rows == H && r.candidates == H);
    check("it leans right going down: about 500 per 1000 rows", r.slope_pm > 400 && r.slope_pm < 600);
    /* At the bottom row the run is x 69..74: 11 px right of the centre. */
    check("its bottom is a little right of the centre", r.offset_pm > 120 && r.offset_pm < 250);
    check("the fitted line runs from its top row to its bottom row",
          r.y0 == 0 && r.y1 == H - 1 && r.x0 >= 30 && r.x0 <= 35 && r.x1 >= 69 && r.x1 <= 74);
    check("the line followed is marked in yellow, its edges in cyan",
          pic[40][30 + 20 + 3] == 0xffe0 && pic[40][30 + 20] == 0x07ff && pic[40][30 + 20 + 5] == 0x07ff);
    check("without a state a picture with no line says lost, never found before",
          (fill(white), vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r)) == 0 && !r.found && r.lost == 1 &&
              !r.ever && r.rows == 0 && r.candidates == 0);
    /* A light line on a dark floor, straight, left of centre. */
    fill(black);
    box(20, 0, 8, H, white);
    check("a light line straight down, left of centre",
          vision_pixels_trace(&pic[0][0], W, H, W, false, NULL, &r) == 0 && r.found && r.slope_pm > -30 &&
              r.slope_pm < 30 && r.offset_pm < -500 && r.offset_pm > -700);
    /* The threshold follows the light: a mid-grey line on a light-grey floor
     * (luma 110 on 200), which a fixed "below 64" never saw. */
    fill(floor_grey);
    line_at(56, 0, 8, 0, line_grey);
    check("a grey line on a grey floor is found by its contrast", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL,
                                                                                      &r) == 0 &&
                                                                      r.found && r.offset_pm > -50 && r.offset_pm < 50);
    /* Nothing to trace. */
    fill(white);
    check("a blank picture has no line", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && !r.found &&
                                             r.rows == 0);
    fill(black);
    check("nor does one that is all dark", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && !r.found);
    fill(white);
    box(50, H - 5, 6, 5, black);
    check("a short dash at the bottom is a candidate, not a line",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && !r.found && r.rows == 0 && r.candidates == 5 &&
              pic[H - 1][50] == 0x07ff && pic[H - 1][52] == black);
    /* A squarish dark blob (a shadow, a mat) tall enough to pass the row
     * count: no longer than twice its width, so not a line. */
    fill(white);
    box(70, H - 30, 24, 30, black);
    check("a blob as tall as wide is not a line, however many rows",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && !r.found && r.candidates == 30);
    fill(white);
    box(70, H - 30, 10, 30, black);
    check("the same height but narrow is a line", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 &&
                                                       r.found && r.rows == 30);
    fill(white);
    box(50, 0, 6, H / 2 - 2, black);
    check("a line in the upper half alone is not followed: it starts at the bottom",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && !r.found);
    /* Stripes: more runs on a row than a line has are texture. */
    fill(white);
    for (y = 0; y < W; y += 10) {
        box(y, 0, 4, H, black);
    }
    check("stripes are texture, not a line", vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 &&
                                                 !r.found && r.candidates == 0);
    /* A gap of a few rows (a worn tape) is crossed. */
    fill(white);
    line_at(40, 0, 6, 0, black);
    box(40, 30, 6, 3, white);
    check("a short gap is crossed: one line, the gap's rows missing",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && r.found && r.rows == H - 3);
    /* A distractor beside the line: a dark blob in the lower half. The
     * longest chain is the line; the blob's edges are candidates. */
    fill(white);
    line_at(30, 0, 6, 0, black);
    box(80, 50, 14, 30, black);
    check("the line, not a shorter blob beside it, is followed",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && r.found && r.x1 >= 30 && r.x1 <= 36 &&
              r.rows == H && pic[60][80] == 0x07ff && pic[60][86] == black);
    check("a picture too small is refused", vision_pixels_trace(&pic[0][0], 2, H, W, true, NULL, &r) == -EINVAL);
    /* Unit A, 2026-10-26: specks on the lowest rows (a table edge below the
     * paper) took every start, and a line in plain view above them was said
     * lost. Here five rows of specks under a line ending at three quarters
     * of the height. */
    fill(white);
    line_at(40, 0, 6, 0, black);
    box(0, (3 * H) / 4, W, H - (3 * H) / 4, white);
    for (y = H - 5; y < H; y++) {
        box(10 + 7 * (y % 3), y, 2, 1, black);
        box(100 - 5 * (y % 2), y, 2, 1, black);
    }
    check("specks on the lowest rows do not hide the line above them",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && r.found && r.x1 >= 40 && r.x1 <= 46 &&
              r.y1 == (3 * H) / 4 - 1);
    /* Unit A: a speck at the picture's lower edge, within reach of the line's
     * end across a gap, became the chain's start and so the line's "bottom".
     * Here the line ends at row 69 at x 40..45, a 3-row speck sits at x 52
     * on rows 74..76, within the gap a line may cross. */
    fill(white);
    line_at(40, 0, 6, 0, black);
    box(0, 70, W, H - 70, white);
    box(52, 74, 3, 3, black);
    check("a speck bridged below the line's end is not its bottom",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && r.found && r.y1 == 69 && r.x1 >= 40 &&
              r.x1 <= 45 && r.rows == 70 && pic[77][53] != 0xffe0);
    /* Unit A: an S-shaped line read LEFT 99% from the straight fit while its
     * end was far nearer the middle. Its bottom here runs straight down at
     * x 20..25 for the last third, the upper part leans far right. */
    fill(white);
    for (y = 0; y < H; y++) {
        box(y < (2 * H) / 3 ? 20 + ((2 * H) / 3 - y) * 2 : 20, y, 6, 1, black);
    }
    check("the offset is the line's own bottom, not the fit carried down",
          vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r) == 0 && r.found && r.x1 >= 20 && r.x1 <= 25 &&
              r.offset_pm > -650 && r.offset_pm < -550 && r.slope_pm < 0);

    /* ---- over pictures, with a state ---- */
    vision_trace_reset(&st);
    fill(white);
    line_at(40, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("state: found, ever, nothing lost", r.found && r.ever && r.lost == 0 && r.x1 == 42);
    fill(white);
    line_at(46, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("a line that moves a little is smoothed: between where it was and is", r.found && r.x1 > 42 && r.x1 < 48);
    fill(white);
    line_at(46, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("and settles where it is", r.found && r.x1 >= 46 && r.x1 <= 48);
    fill(white);
    line_at(100, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("a jump of more than a sixth of the width is another line, taken as it is", r.found && r.x1 == 102);
    /* Gone: said at once, counted, nothing drawn as the line. */
    fill(white);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    ok = 1;
    for (y = 0; y < H; y++) {
        ok &= pic[y][102] == white;
    }
    check("the line gone: not found in the first picture without it, lost 1, nothing painted",
          !r.found && r.lost == 1 && r.ever && ok && r.x0 == 0 && r.x1 == 0);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("and lost 2 in the next", !r.found && r.lost == 2);
    /* Back, with a decoy of the same length on the other side: the one
     * where the line was is taken again. Without the state the decoy, the
     * nearer the centre, would be. */
    fill(white);
    line_at(98, 0, 6, 0, black);
    line_at(50, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("reacquired where it was last seen, not the decoy", r.found && r.lost == 0 && r.x1 >= 98 && r.x1 <= 102);
    fill(white);
    line_at(98, 0, 6, 0, black);
    line_at(50, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, NULL, &r);
    check("(without the state the decoy nearer the centre is taken)", r.found && r.x1 >= 50 && r.x1 <= 54);
    /* Lost for longer than VISION_TRACE_REACQUIRE: the old place no longer
     * counts. */
    fill(white);
    for (y = 0; y <= VISION_TRACE_REACQUIRE; y++) {
        vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    }
    fill(white);
    line_at(98, 0, 6, 0, black);
    line_at(50, 0, 6, 0, black);
    vision_pixels_trace(&pic[0][0], W, H, W, true, &st, &r);
    check("after a long loss the line is looked for afresh", r.found && r.x1 >= 50 && r.x1 <= 54);
    vision_trace_reset(&st);
    check("reset: never found", st.ever == false && st.lost == 0);
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
