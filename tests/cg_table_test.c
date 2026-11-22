/*
 * Card games shared table pieces: the cloth renderer and the tween
 * (core/cards/cg_cloth.h, cg_tween.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_cloth.h"
#include "cg_tween.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static int luma(uint16_t p)
{
    return ((p >> 11) & 31) * 2 + ((p >> 5) & 63) + (p & 31) * 2;
}

#define W 142
#define H 308

static void test_cloth(void)
{
    static uint16_t a[W * H], b[W * H], guard[(W + 4) * 8];
    struct cg_cloth c = { 0x7d2130, 0x3a0d16, W / 2, H * 42 / 100, W * 95 / 100, H * 55 / 100, 3 };
    struct cg_cloth flat = c;
    int x, y, ok, bands, last;

    cg_cloth_render(a, W, H, W, &c);
    cg_cloth_render(b, W, H, W, &c);
    check("the same arguments give the same cloth", memcmp(a, b, sizeof(a)) == 0);

    check("the lamp's centre is the red of the lit cloth",
          ((a[c.cy * W + c.cx] >> 11) & 31) >= 14 && ((a[c.cy * W + c.cx] >> 5) & 63) <= 10);
    check("the cloth is darker at the lamp's radius than under it", luma(a[c.cy * W + c.cx]) > luma(a[c.cy * W + 2]) + 6);
    check("and falls to black far from the lamp", luma(a[(H - 1) * W]) <= 2 && luma(a[(H - 1) * W + W - 1]) <= 2);

    ok = 1;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint16_t p = a[y * W + x];

            /* Lit is 125, 33, 48: 15, 8, 6 in 5-6-5, plus grain and dither. */
            if (((p >> 11) & 31) > 17 || ((p >> 5) & 63) > 10 || (p & 31) > 8) {
                ok = 0;
            }
        }
    }
    check("no pixel is brighter than the lit cloth allows", ok);

    /* Without grain, a row through the centre never steps up going outward
     * by more than the dither's one level, and it does step down. */
    flat.grain = 0;
    cg_cloth_render(b, W, H, W, &flat);
    ok = 1;
    bands = 0;
    last = (b[c.cy * W + c.cx] >> 11) & 31;
    for (x = c.cx + 1; x < W; x++) {
        int r = (b[c.cy * W + x] >> 11) & 31;

        if (r > last + 1) {
            ok = 0;
        }
        if (r < last) {
            bands++;
            last = r;
        }
    }
    check("outward from the lamp the cloth only darkens, give or take the dither", ok && bands >= 2);

    ok = 0;
    for (x = 0; x < W * H; x++) {
        if (a[x] != b[x]) {
            ok = 1;
        }
    }
    check("grain changes the cloth", ok);

    memset(guard, 0xA5, sizeof(guard));
    cg_cloth_render(guard, W, 8, W + 4, &c);
    ok = 1;
    for (y = 0; y < 8; y++) {
        for (x = W; x < W + 4; x++) {
            if (guard[y * (W + 4) + x] != 0xA5A5) {
                ok = 0;
            }
        }
    }
    check("the stride's padding is left alone", ok);

    memset(b, 0x5A, sizeof(b));
    c.rx = 0;
    cg_cloth_render(b, W, H, W, &c);
    cg_cloth_render(NULL, W, H, W, &flat);
    cg_cloth_render(b, W, H, W - 1, &flat);
    check("a lamp with no radius, no buffer or a short stride draws nothing", b[0] == 0x5A5A && b[W * H - 1] == 0x5A5A);
}

static void test_tween(void)
{
    struct cg_tween t = { 10, 200, 310, -40, 1000, 300 };
    struct cg_tween now = { 5, 6, 7, 8, 50, 0 };
    int x = -1, y = -1, last = 0, ok = 1;
    uint32_t ms;

    check("before its start a tween has not moved", cg_tween_progress(&t, 0) == 0 && cg_tween_progress(&t, 999) == 0 &&
                                                        cg_tween_at(&t, 999, &x, &y) == 0 && x == 10 && y == 200);
    check("at its start it is at the start", cg_tween_at(&t, 1000, &x, &y) == 0 && x == 10 && y == 200);
    check("at its end it has arrived exactly", cg_tween_at(&t, 1300, &x, &y) == 1 && x == 310 && y == -40);
    check("and stays there", cg_tween_at(&t, 99999, &x, &y) == 1 && x == 310 && y == -40);
    for (ms = 1000; ms <= 1300; ms++) {
        int p = cg_tween_progress(&t, ms);

        if (p < last || p > 1024) {
            ok = 0;
        }
        last = p;
    }
    check("progress never goes back and never overshoots", ok && last == 1024);
    check("it eases out: past half way at half time", cg_tween_progress(&t, 1150) > 800);
    check("a tween with no duration moves at once", cg_tween_at(&now, 49, &x, &y) == 0 && x == 5 && y == 6 &&
                                                        cg_tween_at(&now, 50, &x, &y) == 1 && x == 7 && y == 8);
    check("no tween has arrived and writes nothing", cg_tween_at(NULL, 0, &x, &y) == 1 && x == 7 && y == 8 &&
                                                         cg_tween_progress(NULL, 5) == 0);
}

int main(void)
{
    test_cloth();
    test_tween();
    printf("cg_table_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
