/*
 * COLOR's reading on deterministic pictures: every category named from a
 * plain colour; the square moved inside the picture, never read past it; a
 * red patch in a white margin read red and shown red; a square split
 * between colours read as mixed with its two largest; a dark square named
 * dark, a pale one grey or white. The filter: the first reading shown at
 * once, a single odd frame never flipping the name, a real change taking
 * exactly VISION_SWATCH_CONFIRM readings, the colour a bounded mean that
 * starts again when the name changes (so red to blue never shows purple),
 * and reset.
 *
 * Synthetic pictures only: this says nothing of how a camera sees colour.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketvision/vision_swatch.h"

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

#define W 160
#define H 90

static uint16_t pic[H][W];

static uint16_t c565(uint8_t r, uint8_t g, uint8_t b)
{
    struct vision_rgb c = { r, g, b };

    return vision_rgb_to_rgb565(c);
}

static void fill(uint16_t v)
{
    int x;
    int y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            pic[y][x] = v;
        }
    }
}

static void box(int x0, int y0, int w, int h, uint16_t v)
{
    int x;
    int y;

    for (y = y0; y < y0 + h && y < H; y++) {
        for (x = x0; x < x0 + w && x < W; x++) {
            pic[y][x] = v;
        }
    }
}

static struct vision_swatch_read read_mid(void)
{
    struct vision_swatch_read r;

    memset(&r, 0, sizeof(r));
    vision_swatch_read(&pic[0][0], W, H, W, W / 2, H / 2, vision_swatch_half(W, H), &r);
    return r;
}

static int named(uint8_t r, uint8_t g, uint8_t b, enum vision_swatch_cat want)
{
    struct vision_rgb c = { r, g, b };

    return vision_swatch_classify(c) == want;
}

static void test_classify(void)
{
    check("red", named(220, 30, 30, VISION_SWATCH_RED) && named(130, 0, 0, VISION_SWATCH_RED));
    check("orange", named(240, 140, 20, VISION_SWATCH_ORANGE));
    check("brown: a dark orange", named(120, 70, 30, VISION_SWATCH_BROWN));
    check("yellow", named(230, 220, 40, VISION_SWATCH_YELLOW));
    check("green", named(40, 180, 60, VISION_SWATCH_GREEN));
    check("cyan", named(30, 200, 210, VISION_SWATCH_CYAN));
    check("blue", named(30, 60, 220, VISION_SWATCH_BLUE));
    check("purple", named(130, 40, 200, VISION_SWATCH_PURPLE));
    check("pink: magenta and a pale red", named(230, 40, 170, VISION_SWATCH_PINK) &&
                                             named(240, 150, 160, VISION_SWATCH_PINK));
    check("white: bright and without hue", named(245, 245, 240, VISION_SWATCH_WHITE));
    check("grey: neither bright nor coloured", named(128, 128, 120, VISION_SWATCH_GREY));
    check("dark: too little light for a hue, whatever the hue", named(30, 5, 5, VISION_SWATCH_DARK) &&
                                                                    named(10, 10, 40, VISION_SWATCH_DARK) &&
                                                                    named(0, 0, 0, VISION_SWATCH_DARK));
    check("names: the categories and mixed", strcmp(vision_swatch_name(VISION_SWATCH_RED), "red") == 0 &&
                                                 strcmp(vision_swatch_name(VISION_SWATCH_MIXED), "mixed") == 0 &&
                                                 strcmp(vision_swatch_name(99), "?") == 0);
}

static void test_read(void)
{
    struct vision_swatch_read r;
    int32_t half = vision_swatch_half(W, H);

    check("the square's half side is a sixteenth of the smaller side, bounded",
          half == 6 && vision_swatch_half(1000, 1000) == VISION_SWATCH_HALF_MAX && vision_swatch_half(20, 20) ==
                                                                                     VISION_SWATCH_HALF_MIN);
    fill(c565(220, 30, 30));
    r = read_mid();
    check("a red picture reads red, all of it", r.cat == VISION_SWATCH_RED && r.first_pm == 1000 &&
                                                    r.second == VISION_SWATCH_RED && r.second_pm == 0);
    check("the colour shown is the red's", r.mean.r > 200 && r.mean.g < 50 && r.mean.b < 50);
    /* A red patch filling 70 % of the square, white round it. */
    fill(c565(250, 250, 250));
    box(W / 2 - 5, H / 2 - 6, 11, 13, c565(220, 30, 30));
    r = read_mid();
    check("a red patch in a white margin reads red, the white second", r.cat == VISION_SWATCH_RED &&
                                                                          r.second == VISION_SWATCH_WHITE &&
                                                                          r.first_pm > 550 && r.first_pm < 1000);
    check("and is shown red, not pink: the mean is the red pixels' only", r.mean.r > 200 && r.mean.g < 50);
    /* Half blue, half yellow: no colour holds enough. */
    fill(c565(30, 60, 220));
    box(0, 0, W / 2, H, c565(230, 220, 40));
    r = read_mid();
    check("a square split between blue and yellow is mixed, naming both",
          r.cat == VISION_SWATCH_MIXED &&
              ((r.first == VISION_SWATCH_BLUE && r.second == VISION_SWATCH_YELLOW) ||
               (r.first == VISION_SWATCH_YELLOW && r.second == VISION_SWATCH_BLUE)) &&
              r.first_pm < VISION_SWATCH_MIXED_PM);
    fill(c565(20, 10, 10));
    r = read_mid();
    check("a dark square is dark, with its low value", r.cat == VISION_SWATCH_DARK && r.value < VISION_SWATCH_DARK_V);
    fill(c565(140, 140, 135));
    r = read_mid();
    check("a pale square is grey", r.cat == VISION_SWATCH_GREY);
    /* The square moves inside the picture rather than being cut by it. */
    fill(c565(30, 60, 220));
    box(0, 0, 2 * half + 1, 2 * half + 1, c565(40, 180, 60));
    check("a square asked for at a corner is moved inside: it reads what is there",
          vision_swatch_read(&pic[0][0], W, H, W, -50, -50, half, &r) == 0 && r.cat == VISION_SWATCH_GREEN &&
              r.first_pm == 1000);
    check("a picture this cannot read, or a square larger than it, is refused",
          vision_swatch_read(NULL, W, H, W, 0, 0, half, &r) == -EINVAL &&
              vision_swatch_read(&pic[0][0], W, H, W, 0, 0, H, &r) == -EINVAL &&
              vision_swatch_read(&pic[0][0], 2, H, W, 0, 0, 1, &r) == -EINVAL);
}

static struct vision_swatch_read reading(enum vision_swatch_cat cat, uint8_t r, uint8_t g, uint8_t b)
{
    struct vision_swatch_read x;

    memset(&x, 0, sizeof(x));
    x.cat = cat;
    x.first = cat == VISION_SWATCH_MIXED ? VISION_SWATCH_RED : cat;
    x.second = VISION_SWATCH_WHITE;
    x.first_pm = cat == VISION_SWATCH_MIXED ? 400 : 900;
    x.mean.r = r;
    x.mean.g = g;
    x.mean.b = b;
    return x;
}

static void test_filter(void)
{
    struct vision_swatch_filter f;
    struct vision_swatch_read red = reading(VISION_SWATCH_RED, 220, 30, 30);
    struct vision_swatch_read red2 = reading(VISION_SWATCH_RED, 200, 40, 40);
    struct vision_swatch_read blue = reading(VISION_SWATCH_BLUE, 30, 60, 220);
    struct vision_swatch_read mixed = reading(VISION_SWATCH_MIXED, 220, 30, 30);
    int i;
    int ok;

    vision_swatch_filter_reset(&f);
    check("empty: nothing shown", !f.have);
    check("the first reading is shown at once", vision_swatch_filter_add(&f, &red) && f.have &&
                                                    f.shown == VISION_SWATCH_RED && f.rgb.r == 220 && f.steady == 0);
    check("the same name again changes nothing", !vision_swatch_filter_add(&f, &red2) && f.shown == VISION_SWATCH_RED &&
                                                     f.steady == 1);
    check("the colour is the mean of the readings", f.rgb.r == 210 && f.rgb.g == 35);
    /* One odd frame: blue once, then red again. */
    check("a single blue frame does not flip the name", !vision_swatch_filter_add(&f, &blue) &&
                                                            f.shown == VISION_SWATCH_RED && f.rgb.r == 210);
    vision_swatch_filter_add(&f, &red);
    check("nor does it leak into the colour", f.shown == VISION_SWATCH_RED && f.rgb.b < 40);
    /* Blue for real: VISION_SWATCH_CONFIRM in a row. */
    ok = 1;
    for (i = 1; i < VISION_SWATCH_CONFIRM; i++) {
        ok &= !vision_swatch_filter_add(&f, &blue) && f.shown == VISION_SWATCH_RED;
    }
    check("blue is held off for one reading less than the confirmation", ok);
    check("and taken on the confirming one", vision_swatch_filter_add(&f, &blue) && f.shown == VISION_SWATCH_BLUE &&
                                                 f.steady == 0);
    check("the colour starts again with the name: blue, never a red-blue blend",
          f.rgb.r == 30 && f.rgb.g == 60 && f.rgb.b == 220);
    /* Alternating red and blue: neither ever holds three in a row. */
    ok = 1;
    for (i = 0; i < 20; i++) {
        vision_swatch_filter_add(&f, i % 2 ? &blue : &red);
        ok &= f.shown == VISION_SWATCH_BLUE;
    }
    check("a flicker between two colours keeps the name it had", ok);
    /* The mean is bounded: after many readings it is the last WINDOW's. */
    for (i = 0; i < 40; i++) {
        vision_swatch_filter_add(&f, &blue);
    }
    for (i = 0; i < VISION_SWATCH_WINDOW; i++) {
        struct vision_swatch_read b2 = reading(VISION_SWATCH_BLUE, 0, 0, 255);

        vision_swatch_filter_add(&f, &b2);
    }
    check("the colour is the mean of the last readings only", f.rgb.r == 0 && f.rgb.b == 255 &&
                                                                  f.n == VISION_SWATCH_WINDOW);
    /* Mixed is a name like any other, and carries its largest. */
    for (i = 0; i < VISION_SWATCH_CONFIRM; i++) {
        vision_swatch_filter_add(&f, &mixed);
    }
    check("mixed, confirmed, keeps its largest and second", f.shown == VISION_SWATCH_MIXED &&
                                                                f.shown_first == VISION_SWATCH_RED &&
                                                                f.shown_second == VISION_SWATCH_WHITE &&
                                                                f.shown_pm == 400);
    {
        /* Mixed across a red/blue edge, the largest flipping: the colour
         * is the largest's alone, never their purple average. */
        struct vision_swatch_read mb = reading(VISION_SWATCH_MIXED, 30, 60, 220);

        mb.first = VISION_SWATCH_BLUE;
        vision_swatch_filter_add(&f, &mb);
        check("mixed whose largest turns blue shows blue, not a blend",
              f.shown == VISION_SWATCH_MIXED && f.shown_first == VISION_SWATCH_BLUE && f.rgb.r == 30 &&
                  f.rgb.b == 220 && f.n == 1);
        vision_swatch_filter_add(&f, &mixed);
        check("and back to red shows red", f.shown_first == VISION_SWATCH_RED && f.rgb.r == 220 && f.rgb.b == 30);
    }
    vision_swatch_filter_reset(&f);
    check("reset: nothing shown", !f.have && f.n == 0);
}

int main(void)
{
    test_classify();
    test_read();
    test_filter();
    printf("vision_swatch_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
