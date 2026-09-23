/*
 * The status chrome policy (ui/shell/chrome.h, DS §30): what a declaration
 * resolves to in each orientation and on the launcher, the bar's height
 * under each policy, and the content box below it with and without the
 * keyboard - in particular that the box always reaches the keyboard while
 * it is up and the foot of the display when it is not, so no chrome leaves
 * a dead strip behind a keyboard that hid.
 *
 * Pure C: built and run by the root Makefile (make test).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "chrome.h"

#include <stdio.h>
#include <string.h>

/* The reference panel (ui/shell/platform.h) and the keyboard sheet
 * (ui/pocketui/pos_keyboard.h), repeated here because both headers need
 * LVGL and this test does not. tests/chrome_shell_test.sh holds the shell
 * to the same numbers in the running simulator. */
#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define KB_H 296

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

static const enum pocketos_chrome all[] = {
    POCKETOS_CHROME_DEFAULT, POCKETOS_CHROME_FULL, POCKETOS_CHROME_COMPACT, POCKETOS_CHROME_NONE
};
#define ALL_COUNT (sizeof(all) / sizeof(all[0]))

static void test_resolve(void)
{
    size_t i;

    /* The launcher: FULL whatever is declared, in both orientations. */
    for (i = 0; i < ALL_COUNT; i++) {
        check("home is FULL in portrait, whatever was declared",
              chrome_resolve(all[i], false, true) == POCKETOS_CHROME_FULL);
        check("home is FULL in landscape, whatever was declared",
              chrome_resolve(all[i], true, true) == POCKETOS_CHROME_FULL);
    }

    /* Stage 1 (DS §30.4): portrait is FULL for every app, whatever it
     * declares. When this stage is lifted, these four checks change with
     * the one line in chrome_resolve() that holds them. */
    for (i = 0; i < ALL_COUNT; i++) {
        check("stage 1: an app in portrait is FULL, whatever it declared",
              chrome_resolve(all[i], false, false) == POCKETOS_CHROME_FULL);
    }

    /* Landscape: the default is COMPACT, and an explicit policy is honoured. */
    check("landscape: DEFAULT resolves to COMPACT",
          chrome_resolve(POCKETOS_CHROME_DEFAULT, true, false) == POCKETOS_CHROME_COMPACT);
    check("landscape: FULL stays FULL",
          chrome_resolve(POCKETOS_CHROME_FULL, true, false) == POCKETOS_CHROME_FULL);
    check("landscape: COMPACT stays COMPACT",
          chrome_resolve(POCKETOS_CHROME_COMPACT, true, false) == POCKETOS_CHROME_COMPACT);
    check("landscape: NONE stays NONE",
          chrome_resolve(POCKETOS_CHROME_NONE, true, false) == POCKETOS_CHROME_NONE);

    /* Never DEFAULT out of the resolver: the shell lays out from the answer. */
    for (i = 0; i < ALL_COUNT; i++) {
        check("the resolver never answers DEFAULT (portrait, app)",
              chrome_resolve(all[i], false, false) != POCKETOS_CHROME_DEFAULT);
        check("the resolver never answers DEFAULT (landscape, app)",
              chrome_resolve(all[i], true, false) != POCKETOS_CHROME_DEFAULT);
    }
    /* A value outside the enum (a corrupt field) lands on the default too. */
    check("an out-of-range declaration in landscape is treated as DEFAULT",
          chrome_resolve((enum pocketos_chrome)99, true, false) == POCKETOS_CHROME_COMPACT);
}

static void test_heights(void)
{
    check("FULL is the DS §7 status bar, 56 px", chrome_height(POCKETOS_CHROME_FULL) == 56);
    check("FULL is POCKETOS_CHROME_FULL_H", chrome_height(POCKETOS_CHROME_FULL) == POCKETOS_CHROME_FULL_H);
    check("COMPACT is 32 px", chrome_height(POCKETOS_CHROME_COMPACT) == 32);
    check("COMPACT is POCKETOS_CHROME_COMPACT_H",
          chrome_height(POCKETOS_CHROME_COMPACT) == POCKETOS_CHROME_COMPACT_H);
    check("NONE takes no height at all", chrome_height(POCKETOS_CHROME_NONE) == 0);
    check("DEFAULT, should it ever reach a height, counts as FULL",
          chrome_height(POCKETOS_CHROME_DEFAULT) == POCKETOS_CHROME_FULL_H);
    /* DS §30.1: a COMPACT bar still covers the corner band, so an app header
     * under it never enters a rounded corner and needs no inset of its own.
     * NONE is the only chrome under which the header takes the insets. */
    check("COMPACT is taller than the 30 px corner squares of DS §21.1",
          POCKETOS_CHROME_COMPACT_H > PANEL_CORNER);
    check("landscape gains 24 px of body under COMPACT",
          chrome_height(POCKETOS_CHROME_FULL) - chrome_height(POCKETOS_CHROME_COMPACT) == 24);
    check("and 56 px under NONE",
          chrome_height(POCKETOS_CHROME_FULL) - chrome_height(POCKETOS_CHROME_NONE) == 56);

    check("names: full", strcmp(chrome_name(POCKETOS_CHROME_FULL), "full") == 0);
    check("names: compact", strcmp(chrome_name(POCKETOS_CHROME_COMPACT), "compact") == 0);
    check("names: none", strcmp(chrome_name(POCKETOS_CHROME_NONE), "none") == 0);
    check("names: default", strcmp(chrome_name(POCKETOS_CHROME_DEFAULT), "default") == 0);
}

/* One box, checked against the two invariants every caller relies on: it
 * starts where the chrome ends and it ends where the reserve begins. */
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
    /* Portrait, the FULL every screen has: what shell.c always laid out. */
    box_is("portrait FULL", POCKETOS_CHROME_FULL, PANEL_H, 0, 56, 1176);
    box_is("portrait FULL, keyboard up", POCKETOS_CHROME_FULL, PANEL_H, KB_H, 56, 880);
    /* Landscape under each chrome. */
    box_is("landscape FULL", POCKETOS_CHROME_FULL, PANEL_W, 0, 56, 512);
    box_is("landscape FULL, keyboard up", POCKETOS_CHROME_FULL, PANEL_W, KB_H, 56, 216);
    box_is("landscape COMPACT", POCKETOS_CHROME_COMPACT, PANEL_W, 0, 32, 536);
    box_is("landscape COMPACT, keyboard up", POCKETOS_CHROME_COMPACT, PANEL_W, KB_H, 32, 240);
    box_is("landscape NONE", POCKETOS_CHROME_NONE, PANEL_W, 0, 0, 568);
    box_is("landscape NONE, keyboard up", POCKETOS_CHROME_NONE, PANEL_W, KB_H, 0, 272);
    /* Portrait under the chromes no portrait screen gets yet (stage 1), so
     * that lifting the stage changes no geometry, only the resolver. */
    box_is("portrait COMPACT", POCKETOS_CHROME_COMPACT, PANEL_H, 0, 32, 1200);
    box_is("portrait NONE", POCKETOS_CHROME_NONE, PANEL_H, 0, 0, 1232);
}

/* The keyboard's whole life under each chrome: shown, hidden, and the box
 * back to exactly the foot. A shell that subtracted a constant bar here
 * would leave a dead strip the height of the difference under every chrome
 * but FULL - that was the failure this test exists to keep out. */
static void test_keyboard_round_trip(void)
{
    static const int32_t displays[] = { PANEL_H, PANEL_W };
    size_t i;
    size_t d;

    for (d = 0; d < sizeof(displays) / sizeof(displays[0]); d++) {
        for (i = 1; i < ALL_COUNT; i++) { /* the three effective policies */
            struct chrome_box up = chrome_content_box(all[i], displays[d], KB_H);
            struct chrome_box down = chrome_content_box(all[i], displays[d], 0);
            char label[160];

            snprintf(label, sizeof(label), "%s on %d: the keyboard takes exactly its height",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.height - up.height == KB_H && up.y == down.y);
            snprintf(label, sizeof(label), "%s on %d: hidden again, the box reaches the foot",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.y + down.height == displays[d]);
            snprintf(label, sizeof(label), "%s on %d: the box starts where the chrome ends",
                     chrome_name(all[i]), (int)displays[d]);
            check(label, down.y == chrome_height(all[i]) && up.y == chrome_height(all[i]));
        }
    }
    /* Degenerate inputs never go negative. */
    {
        struct chrome_box b = chrome_content_box(POCKETOS_CHROME_FULL, 40, KB_H);

        check("a display shorter than chrome and keyboard gives an empty box, not a negative one",
              b.height == 0 && b.y == 56);
        b = chrome_content_box(POCKETOS_CHROME_COMPACT, PANEL_W, -10);
        check("a negative reserve counts as none", b.height == PANEL_W - 32 && b.y == 32);
    }
}

/* The radio chip (DS §30.1). The chip is a label, and a label clips what
 * does not fit its content box: the 24 px COMPACT chip with 5 px of padding
 * left 14 px for the 22 px line of the symbol font it draws in, and "RX"
 * lost its top on unit A. Every line height a status font could have, under
 * every chrome: the text gets a whole line, it is centred, and the chip
 * stays inside the bar above its thickest hairline. */
#define SYMBOL_FONT_LINE_H 22 /* lv_font_montserrat_20, the chip's font (POS_STYLE_SYMBOL) */
#define SMALL_LINE_H 17

static void test_chip(void)
{
    size_t i;
    int32_t lh;
    struct chrome_chip c;

    for (i = 1; i < ALL_COUNT; i++) {
        for (lh = 10; lh <= 26; lh++) {
            char label[160];

            c = chrome_chip_box(all[i], lh);
            snprintf(label, sizeof(label), "%s chip, %d px line: the text gets the whole line (%d)",
                     chrome_name(all[i]), (int)lh, (int)(c.height - c.pad_top - c.pad_bottom));
            check(label, c.height - c.pad_top - c.pad_bottom >= lh);
            snprintf(label, sizeof(label), "%s chip, %d px line: centred, no negative padding",
                     chrome_name(all[i]), (int)lh);
            check(label, c.pad_top >= 0 && c.pad_bottom >= 0 && c.pad_bottom - c.pad_top >= 0 &&
                             c.pad_bottom - c.pad_top <= 1);
            if (all[i] != POCKETOS_CHROME_NONE) {
                snprintf(label, sizeof(label), "%s chip, %d px line: %d px fits the %d px bar over a 2 px hairline",
                         chrome_name(all[i]), (int)lh, (int)c.height, (int)chrome_height(all[i]));
                check(label, c.height <= chrome_height(all[i]) - POCKETOS_CHROME_HAIRLINE_MAX);
            }
        }
    }
    /* The fonts the bar has: the numbers DS §7 and §30.1 give. */
    c = chrome_chip_box(POCKETOS_CHROME_FULL, SYMBOL_FONT_LINE_H);
    check("FULL, symbol font: the §7 chip, 36 px, text centred by 7 px",
          c.height == 36 && c.pad_top == 7 && c.pad_bottom == 7);
    c = chrome_chip_box(POCKETOS_CHROME_COMPACT, SYMBOL_FONT_LINE_H);
    check("COMPACT, symbol font: 26 px, 2 px above and below the 22 px line",
          c.height == 26 && c.pad_top == 2 && c.pad_bottom == 2);
    check("the defect: 24 px less 2 x 5 px of padding leaves less than the symbol font's line",
          24 - 2 * 5 < SYMBOL_FONT_LINE_H);
    c = chrome_chip_box(POCKETOS_CHROME_COMPACT, SMALL_LINE_H);
    check("COMPACT, a smaller font: the nominal 26 px, centred", c.height == 26 && c.pad_top == 4);
    c = chrome_chip_box(POCKETOS_CHROME_NONE, SYMBOL_FONT_LINE_H);
    check("NONE: no chip is drawn; it keeps FULL's box for the next bar that shows it",
          c.height == 36 && c.pad_top == 7);
    c = chrome_chip_box(POCKETOS_CHROME_COMPACT, 40);
    check("a font taller than the bar: the chip is held inside the bar, never over its rule",
          c.height == POCKETOS_CHROME_COMPACT_H - POCKETOS_CHROME_HAIRLINE_MAX && c.pad_top == 0);
    c = chrome_chip_box(POCKETOS_CHROME_COMPACT, -3);
    check("a negative line height counts as none", c.height == 26 && c.pad_top == 13);
}

int main(void)
{
    test_resolve();
    test_heights();
    test_boxes();
    test_keyboard_round_trip();
    test_chip();
    printf("chrome_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
