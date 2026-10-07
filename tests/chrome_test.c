/*
 * The status chrome policy (ui/shell/chrome.h, DS §30, §36): what a
 * declaration resolves to in each orientation and on the shell's own
 * screens; that no policy reserves a full-width row any more; the content
 * box with and without the keyboard - in particular that the box always
 * reaches the keyboard while it is up and the foot of the display when it
 * is not, so no chrome leaves a dead strip behind a keyboard that hid; and
 * the status cluster's box - anchored to the top-right corner, as wide as
 * its content and never the screen, inside the display and clear of the
 * rounded corners in both orientations - with the reserve a row under it
 * keeps and the radio chip inside it.
 *
 * Pure C: built and run by the root Makefile (make test).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chrome.h"

#include <stdio.h>
#include <string.h>

/* The reference panel (ui/shell/platform.h), its rounded corners at the top
 * edge in each orientation (DS §21.1: 30 px, and 50 at the top in
 * landscape), the app header row (ui/pocketui/pocketui.h) and the keyboard
 * sheet (ui/pocketui/pos_keyboard.h), repeated here because those headers
 * need LVGL and this test does not. tests/chrome_shell_test.sh holds the
 * shell to the same numbers in the running simulator. */
#define PANEL_W 568
#define PANEL_H 1232
#define CORNER_PORTRAIT 30
#define CORNER_LANDSCAPE_TOP 50
#define HEADER_H 72
#define BACK_SLAB_H 56
#define KB_H 296
/* The retired bars (DS §7, §30.1), for what their removal gives back. */
#define OLD_FULL_H 56
#define OLD_COMPACT_H 32

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static const enum pocketos_chrome all[] = { POCKETOS_CHROME_DEFAULT, POCKETOS_CHROME_CLUSTER,
                                            POCKETOS_CHROME_NONE };
#define ALL_COUNT (sizeof(all) / sizeof(all[0]))

static void test_resolve(void)
{
    size_t i;
    int land;

    for (land = 0; land <= 1; land++) {
        const char *o = land ? "landscape" : "portrait";
        char label[120];

        /* The shell's own screens: the cluster whatever is declared. */
        for (i = 0; i < ALL_COUNT; i++) {
            snprintf(label, sizeof(label), "%s: home shows the cluster, whatever was declared (%s)", o,
                     chrome_name(all[i]));
            check(label, chrome_resolve(all[i], land, true) == POCKETOS_CHROME_CLUSTER);
        }
        /* DS §36.2: an app shows the cluster unless it declares NONE, the
         * same in both orientations. These checks change with the one line
         * in chrome_resolve() that holds them. */
        snprintf(label, sizeof(label), "%s: DEFAULT is the cluster", o);
        check(label, chrome_resolve(POCKETOS_CHROME_DEFAULT, land, false) == POCKETOS_CHROME_CLUSTER);
        snprintf(label, sizeof(label), "%s: CLUSTER stays the cluster", o);
        check(label, chrome_resolve(POCKETOS_CHROME_CLUSTER, land, false) == POCKETOS_CHROME_CLUSTER);
        snprintf(label, sizeof(label), "%s: NONE stays NONE (the fullscreen apps, DS §30.8)", o);
        check(label, chrome_resolve(POCKETOS_CHROME_NONE, land, false) == POCKETOS_CHROME_NONE);
        /* A value outside the enum (a corrupt field) is the default. */
        snprintf(label, sizeof(label), "%s: an out-of-range declaration is treated as DEFAULT", o);
        check(label, chrome_resolve((enum pocketos_chrome)99, land, false) == POCKETOS_CHROME_CLUSTER);
        for (i = 0; i < ALL_COUNT; i++) {
            snprintf(label, sizeof(label), "%s: the resolver never answers DEFAULT (%s)", o,
                     chrome_name(all[i]));
            check(label, chrome_resolve(all[i], land, false) != POCKETOS_CHROME_DEFAULT);
        }
    }
    check("names: cluster", strcmp(chrome_name(POCKETOS_CHROME_CLUSTER), "cluster") == 0);
    check("names: none", strcmp(chrome_name(POCKETOS_CHROME_NONE), "none") == 0);
    check("names: default", strcmp(chrome_name(POCKETOS_CHROME_DEFAULT), "default") == 0);
}

/* DS §36: the full-width bar is gone, under every policy and on every
 * screen, and nothing takes its place as padding. */
static void test_no_bar(void)
{
    size_t i;

    for (i = 0; i < ALL_COUNT; i++) {
        char label[120];

        snprintf(label, sizeof(label), "%s reserves no full-width row along the top edge",
                 chrome_name(all[i]));
        check(label, chrome_height(all[i]) == 0);
    }
    /* What that gives back to an app's body, whose first row is the shell's
     * 72 px header: it now starts at the top edge, not under a bar. */
    check("portrait: an app's body starts 56 px higher than under the FULL bar",
          OLD_FULL_H + HEADER_H - (chrome_height(POCKETOS_CHROME_CLUSTER) + HEADER_H) == 56);
    check("landscape: 32 px higher than under the COMPACT bar",
          OLD_COMPACT_H + HEADER_H - (chrome_height(POCKETOS_CHROME_CLUSTER) + HEADER_H) == 32);
    check("the launcher and Controls: 56 px, in both orientations",
          OLD_FULL_H - chrome_height(chrome_resolve(POCKETOS_CHROME_DEFAULT, true, true)) == 56);
}

/* One box, checked against the two invariants every caller relies on: it
 * starts at the top edge and it ends where the reserve begins. */
static void box_is(const char *what, enum pocketos_chrome c, int32_t display_h, int32_t reserve,
                   int32_t want_y, int32_t want_h)
{
    struct chrome_box b = chrome_content_box(c, display_h, reserve);
    char label[160];

    snprintf(label, sizeof(label), "%s: y %d (want %d)", what, (int)b.y, (int)want_y);
    check(label, b.y == want_y);
    snprintf(label, sizeof(label), "%s: height %d (want %d)", what, (int)b.height, (int)want_h);
    check(label, b.height == want_h);
    snprintf(label, sizeof(label), "%s: the box ends where the reserve begins", what);
    check(label, b.y + b.height == display_h - (reserve > 0 ? reserve : 0));
}

static void test_boxes(void)
{
    box_is("portrait cluster", POCKETOS_CHROME_CLUSTER, PANEL_H, 0, 0, 1232);
    box_is("portrait cluster, keyboard up", POCKETOS_CHROME_CLUSTER, PANEL_H, KB_H, 0, 936);
    box_is("landscape cluster", POCKETOS_CHROME_CLUSTER, PANEL_W, 0, 0, 568);
    box_is("landscape cluster, keyboard up", POCKETOS_CHROME_CLUSTER, PANEL_W, KB_H, 0, 272);
    box_is("portrait NONE", POCKETOS_CHROME_NONE, PANEL_H, 0, 0, 1232);
    box_is("portrait NONE, keyboard up", POCKETOS_CHROME_NONE, PANEL_H, KB_H, 0, 936);
    box_is("landscape NONE", POCKETOS_CHROME_NONE, PANEL_W, 0, 0, 568);
    box_is("landscape NONE, keyboard up", POCKETOS_CHROME_NONE, PANEL_W, KB_H, 0, 272);
}

/* The keyboard's whole life under each chrome: shown, hidden, and the box
 * back to exactly the foot. A shell that subtracted a constant here would
 * leave a dead strip the height of that constant - that was the failure
 * this test exists to keep out. */
static void test_keyboard_round_trip(void)
{
    static const int32_t displays[] = { PANEL_H, PANEL_W };
    size_t i;
    size_t d;

    for (d = 0; d < sizeof(displays) / sizeof(displays[0]); d++) {
        for (i = 1; i < ALL_COUNT; i++) { /* the effective policies */
            struct chrome_box up = chrome_content_box(all[i], displays[d], KB_H);
            struct chrome_box down = chrome_content_box(all[i], displays[d], 0);
            char label[160];

            snprintf(label, sizeof(label), "%s on %d: the keyboard takes exactly its height",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.height - up.height == KB_H && up.y == down.y);
            snprintf(label, sizeof(label), "%s on %d: hidden again, the box reaches the foot",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.y + down.height == displays[d]);
            snprintf(label, sizeof(label), "%s on %d: the box starts at the top edge",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.y == 0 && up.y == 0);
        }
    }
    /* Degenerate inputs never go negative. */
    {
        struct chrome_box b = chrome_content_box(POCKETOS_CHROME_CLUSTER, 40, KB_H);

        check("a display shorter than the keyboard gives an empty box, not a negative one",
              b.height == 0 && b.y == 0);
        b = chrome_content_box(POCKETOS_CHROME_CLUSTER, PANEL_W, -10);
        check("a negative reserve counts as none", b.height == PANEL_W && b.y == 0);
    }
}

/* ---- the status cluster (DS §36.1) ---------------------------------------- */

/* What the shell measures in the simulator: the RX chip (12 px padding each
 * side around the Wi-Fi glyph and "OFF", the widest state) and the clock at
 * its widest in mono 14 - near enough; the checks hold for any width. */
#define CHIP_W 99
#define CLOCK_W 47

static void cluster_in_frame(const char *what, int32_t display_w, int32_t corner, int32_t width)
{
    struct chrome_rect r = chrome_cluster_box(display_w, corner, width);
    struct chrome_rect corner_sq = { display_w - corner, 0, corner, corner };
    char label[200];

    snprintf(label, sizeof(label), "%s: inside the display (x %d..%d of %d)", what, (int)r.x,
             (int)(r.x + r.w), (int)display_w);
    check(label, r.x >= 0 && r.x + r.w <= display_w && r.y >= 0);
    snprintf(label, sizeof(label), "%s: clear of the %d px top-right corner square", what, (int)corner);
    check(label, !chrome_rects_overlap(&r, &corner_sq));
    snprintf(label, sizeof(label), "%s: its right edge keeps the header's side margin (%d)", what,
             (int)(display_w - (r.x + r.w)));
    check(label, r.w == 0 || display_w - (r.x + r.w) == (corner > POCKETOS_CHROME_EDGE_MIN
                                                                 ? corner
                                                                 : POCKETOS_CHROME_EDGE_MIN));
    snprintf(label, sizeof(label), "%s: inside the %d px header row, centred on it like the back slab",
             what, HEADER_H);
    check(label, r.y + r.h <= HEADER_H && r.y + r.h / 2 == HEADER_H / 2 &&
                     r.h <= BACK_SLAB_H && r.y >= (HEADER_H - BACK_SLAB_H) / 2);
}

static void test_cluster(void)
{
    int32_t app = chrome_cluster_width(CHIP_W, CLOCK_W);
    int32_t env = chrome_cluster_width(CHIP_W, 0);
    struct chrome_rect p = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, app);
    struct chrome_rect l = chrome_cluster_box(PANEL_H, CORNER_LANDSCAPE_TOP, app);
    int32_t w;
    int ok;

    /* Its width is its content's. */
    check("the width is the padding, the chip, the gap and the clock",
          app == POCKETOS_CHROME_CLUSTER_PAD_L + CHIP_W + POCKETOS_CHROME_CLUSTER_GAP + CLOCK_W +
                     POCKETOS_CHROME_CLUSTER_PAD_R);
    check("with no clock, the chip with the same air on both sides",
          env == 2 * POCKETOS_CHROME_CLUSTER_PAD_L + CHIP_W);
    check("hiding the clock narrows it", env < app);
    check("the box is as wide as asked, not as the screen: portrait", p.w == app && p.w < PANEL_W / 2);
    check("the box is as wide as asked, not as the screen: landscape", l.w == app && l.w < PANEL_H / 4);
    ok = 1;
    for (w = 1; w < 300; w++) {
        struct chrome_rect a = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, w);
        struct chrome_rect b = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, w + 1);

        ok &= a.w == w && b.w == a.w + 1 && a.x + a.w == b.x + b.w && b.x == a.x - 1;
    }
    check("wider content grows it to the left: the right edge never moves", ok);

    /* Top-right, in both orientations of the reference panel. */
    check("portrait: in the top-right corner, 30 px from the right edge",
          p.x + p.w == PANEL_W - CORNER_PORTRAIT && p.y == POCKETOS_CHROME_CLUSTER_Y);
    check("landscape: in the top-right corner, 50 px from the right edge",
          l.x + l.w == PANEL_H - CORNER_LANDSCAPE_TOP && l.y == POCKETOS_CHROME_CLUSTER_Y);
    check("portrait and landscape: the same height and the same top",
          p.y == l.y && p.h == l.h && p.h == POCKETOS_CHROME_CLUSTER_H);
    cluster_in_frame("portrait, app", PANEL_W, CORNER_PORTRAIT, app);
    cluster_in_frame("portrait, shell screen", PANEL_W, CORNER_PORTRAIT, env);
    cluster_in_frame("landscape, app", PANEL_H, CORNER_LANDSCAPE_TOP, app);
    cluster_in_frame("landscape, shell screen", PANEL_H, CORNER_LANDSCAPE_TOP, env);
    cluster_in_frame("a rectangular panel", PANEL_W, 0, app);

    /* Never outside the display, however wide it asks to be. */
    ok = 1;
    for (w = 0; w < 3 * PANEL_H; w += 7) {
        struct chrome_rect a = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, w);
        struct chrome_rect b = chrome_cluster_box(PANEL_H, CORNER_LANDSCAPE_TOP, w);

        ok &= a.x >= CORNER_PORTRAIT && a.x + a.w <= PANEL_W - CORNER_PORTRAIT && a.w >= 0;
        ok &= b.x >= CORNER_LANDSCAPE_TOP && b.x + b.w <= PANEL_H - CORNER_LANDSCAPE_TOP && b.w >= 0;
    }
    check("a cluster wider than the screen is held inside both margins", ok);
    {
        struct chrome_rect a = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, -40);

        check("a negative width counts as none", a.w == 0 && a.x == PANEL_W - CORNER_PORTRAIT);
        a = chrome_cluster_box(30, CORNER_PORTRAIT, 20);
        check("a display narrower than its margins gives an empty box, not a negative one",
              a.w == 0 && a.x + a.w <= 30);
    }
}

/* A row along the top edge under the cluster (an app header) stops short of
 * it; under NONE it takes no reserve at all. */
static void test_reserve(void)
{
    struct chrome_rect p = chrome_cluster_box(PANEL_W, CORNER_PORTRAIT, chrome_cluster_width(CHIP_W, CLOCK_W));
    struct chrome_rect l = chrome_cluster_box(PANEL_H, CORNER_LANDSCAPE_TOP, chrome_cluster_width(CHIP_W, CLOCK_W));
    int32_t rp = chrome_row_reserve(POCKETOS_CHROME_CLUSTER, PANEL_W, &p);
    int32_t rl = chrome_row_reserve(POCKETOS_CHROME_CLUSTER, PANEL_H, &l);
    struct chrome_rect row_p = { 0, 0, PANEL_W - rp, HEADER_H };
    struct chrome_rect row_l = { 0, 0, PANEL_H - rl, HEADER_H };

    check("portrait: the row's content ends CLUSTER_CLEAR short of the cluster",
          PANEL_W - rp == p.x - POCKETOS_CHROME_CLUSTER_CLEAR);
    check("landscape: the same", PANEL_H - rl == l.x - POCKETOS_CHROME_CLUSTER_CLEAR);
    check("portrait: the row and the cluster do not overlap", !chrome_rects_overlap(&row_p, &p));
    check("landscape: the row and the cluster do not overlap", !chrome_rects_overlap(&row_l, &l));
    check("the reserve is more than the corner inset the row takes anyway",
          rp > CORNER_PORTRAIT && rl > CORNER_LANDSCAPE_TOP);
    check("portrait: the row keeps more than half the width for the back slab, title and hint",
          PANEL_W - rp - CORNER_PORTRAIT > PANEL_W / 2);
    check("under NONE the row reserves nothing (it takes the corner insets as any bar does)",
          chrome_row_reserve(POCKETOS_CHROME_NONE, PANEL_W, &p) == 0 &&
              chrome_row_reserve(POCKETOS_CHROME_NONE, PANEL_H, &l) == 0);
    check("no reserve box, no reserve", chrome_row_reserve(POCKETOS_CHROME_CLUSTER, PANEL_W, NULL) == 0);
    {
        struct chrome_rect a = { 0, 0, 10, 10 };
        struct chrome_rect b = { 10, 0, 10, 10 };
        struct chrome_rect c = { 9, 9, 10, 10 };
        struct chrome_rect e = { 5, 5, 0, 10 };

        check("boxes that only touch do not overlap", !chrome_rects_overlap(&a, &b));
        check("boxes that share a pixel do", chrome_rects_overlap(&a, &c));
        check("an empty box overlaps nothing", !chrome_rects_overlap(&a, &e));
    }
}

/* The radio chip in the cluster (DS §32.4, §36.1). The chip is a label, and
 * a label clips what does not fit its content box: the 24 px chip of the
 * old COMPACT bar with 5 px of padding left 14 px for the 22 px line of the
 * symbol font it draws in, and "RX" lost its top on unit A. Every line
 * height a status font could have: the text gets a whole line, it is
 * centred, and the chip stays inside the cluster with air above and below. */
#define SYMBOL_FONT_LINE_H 22 /* lv_font_montserrat_20, the chip's font (POS_STYLE_SYMBOL) */
#define SMALL_LINE_H 17

static void test_chip(void)
{
    int32_t lh;
    struct chrome_chip c;

    for (lh = 10; lh <= 38; lh++) {
        char label[160];

        c = chrome_chip_box(lh);
        snprintf(label, sizeof(label), "chip, %d px line: the text gets the whole line (%d)", (int)lh,
                 (int)(c.height - c.pad_top - c.pad_bottom));
        check(label, c.height - c.pad_top - c.pad_bottom >= lh);
        snprintf(label, sizeof(label), "chip, %d px line: centred, no negative padding", (int)lh);
        check(label, c.pad_top >= 0 && c.pad_bottom >= 0 && c.pad_bottom - c.pad_top >= 0 &&
                         c.pad_bottom - c.pad_top <= 1);
        snprintf(label, sizeof(label), "chip, %d px line: %d px, inside the %d px cluster with air", (int)lh,
                 (int)c.height, POCKETOS_CHROME_CLUSTER_H);
        check(label, c.height <= POCKETOS_CHROME_CLUSTER_H - 2 * POCKETOS_CHROME_CHIP_AIR);
    }
    c = chrome_chip_box(SYMBOL_FONT_LINE_H);
    check("symbol font: 32 px, text centred by 5 px", c.height == 32 && c.pad_top == 5 && c.pad_bottom == 5);
    check("the old defect: 24 px less 2 x 5 px of padding leaves less than the symbol font's line",
          24 - 2 * 5 < SYMBOL_FONT_LINE_H);
    c = chrome_chip_box(SMALL_LINE_H);
    check("a smaller font: the nominal 32 px, centred", c.height == 32 && c.pad_top == 7);
    c = chrome_chip_box(60);
    check("a font taller than the cluster: the chip is held inside it",
          c.height == POCKETOS_CHROME_CLUSTER_H - 2 * POCKETOS_CHROME_CHIP_AIR && c.pad_top == 0);
    c = chrome_chip_box(-3);
    check("a negative line height counts as none", c.height == 32 && c.pad_top == 16);
}

int main(void)
{
    test_resolve();
    test_no_bar();
    test_boxes();
    test_keyboard_round_trip();
    test_cluster();
    test_reserve();
    test_chip();
    printf("chrome_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
