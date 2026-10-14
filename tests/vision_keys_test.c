/*
 * Vision's keyboard moves (apps/vision/vision_keys.h): the arrows over the
 * mode's buttons in both shapes and over a sheet's rows of choices, Tab and
 * Shift-Tab with their wrap, no focus yet, no buttons, and a move with
 * nothing that way.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_keys.h"

#include <stdio.h>

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

/* n buttons of w x h, cols to a row, gap g, from (x0, y0): the layout's grid. */
static void grid(struct vision_rect *r, int n, int cols, int32_t x0, int32_t y0, int32_t w, int32_t h, int32_t g)
{
    int i;

    for (i = 0; i < n; i++) {
        r[i].x = x0 + (i % cols) * (w + g);
        r[i].y = y0 + (i / cols) * (h + g);
        r[i].w = w;
        r[i].h = h;
    }
}

static void test_empty_and_unfocused(void)
{
    struct vision_rect r[3];

    grid(r, 3, 3, 0, 0, 100, 60, 8);
    check("no buttons: no focus", vision_keys_move(r, 0, 0, VISION_KEY_RIGHT) == -1);
    check("no focus yet: any arrow lands on the first", vision_keys_move(r, 3, -1, VISION_KEY_DOWN) == 0);
    check("no focus yet: Tab lands on the first", vision_keys_move(r, 3, -1, VISION_KEY_NEXT) == 0);
    check("a focus past the end lands on the first", vision_keys_move(r, 3, 7, VISION_KEY_LEFT) == 0);
    check("no move keeps the focus", vision_keys_move(r, 3, 2, VISION_KEY_NONE) == 2);
}

static void test_tall_rows(void)
{
    struct vision_rect r[5];

    /* Portrait: three to a row under the picture, the last row short. */
    grid(r, 5, 3, 16, 700, 170, 64, 8);
    check("tall: Right moves along the row", vision_keys_move(r, 5, 0, VISION_KEY_RIGHT) == 1);
    check("tall: Right at the row's end stays", vision_keys_move(r, 5, 2, VISION_KEY_RIGHT) == 2);
    check("tall: Down goes to the button below", vision_keys_move(r, 5, 1, VISION_KEY_DOWN) == 4);
    check("tall: Down under a short row goes to its nearest", vision_keys_move(r, 5, 2, VISION_KEY_DOWN) == 4);
    check("tall: Up comes back to the button above", vision_keys_move(r, 5, 4, VISION_KEY_UP) == 1);
    check("tall: Up from the first row stays", vision_keys_move(r, 5, 0, VISION_KEY_UP) == 0);
    check("tall: Left from the second row's first stays", vision_keys_move(r, 5, 3, VISION_KEY_LEFT) == 3);
}

static void test_wide_column(void)
{
    struct vision_rect r[6];

    /* Landscape: two to a row in the column beside the picture. */
    grid(r, 6, 2, 1300, 80, 260, 64, 8);
    check("wide: Down moves in the column", vision_keys_move(r, 6, 0, VISION_KEY_DOWN) == 2);
    check("wide: Right goes to the neighbour", vision_keys_move(r, 6, 2, VISION_KEY_RIGHT) == 3);
    check("wide: Down at the foot stays", vision_keys_move(r, 6, 5, VISION_KEY_DOWN) == 5);
    check("wide: Up from the second column stays in it", vision_keys_move(r, 6, 5, VISION_KEY_UP) == 3);
}

static void test_tab(void)
{
    struct vision_rect r[4];

    grid(r, 4, 2, 0, 0, 100, 60, 8);
    check("Tab goes to the next in order", vision_keys_move(r, 4, 1, VISION_KEY_NEXT) == 2);
    check("Tab after the last wraps to the first", vision_keys_move(r, 4, 3, VISION_KEY_NEXT) == 0);
    check("Shift-Tab goes to the previous", vision_keys_move(r, 4, 2, VISION_KEY_PREV) == 1);
    check("Shift-Tab before the first wraps to the last", vision_keys_move(r, 4, 0, VISION_KEY_PREV) == 3);
    check("one button: Tab stays on it", vision_keys_move(r, 1, 0, VISION_KEY_NEXT) == 0);
}

static void test_sheet(void)
{
    /* A sheet: a row of three choices, a row of two, a row of one, each
     * row's choices from the caption's right. */
    struct vision_rect r[6] = {
        { 200, 60, 120, 56 }, { 330, 60, 120, 56 }, { 460, 60, 120, 56 },
        { 200, 130, 120, 56 }, { 330, 130, 120, 56 },
        { 200, 200, 120, 56 },
    };

    check("sheet: Down from the row's last goes to the nearest below", vision_keys_move(r, 6, 2, VISION_KEY_DOWN) == 4);
    check("sheet: Down from the middle goes straight down", vision_keys_move(r, 6, 1, VISION_KEY_DOWN) == 4);
    check("sheet: Down to the one-choice row", vision_keys_move(r, 6, 4, VISION_KEY_DOWN) == 5);
    check("sheet: Up from the one-choice row goes to the one above it", vision_keys_move(r, 6, 5, VISION_KEY_UP) == 3);
    check("sheet: in line beats nearer but beside", vision_keys_move(r, 6, 0, VISION_KEY_DOWN) == 3);
}

int main(void)
{
    test_empty_and_unfocused();
    test_tall_rows();
    test_wide_column();
    test_tab();
    test_sheet();
    printf("vision_keys_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
