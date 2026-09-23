/*
 * Status chrome policy (DS §30): whether an app's body sits under the shell's
 * full 56 px status bar, a 32 px compact one, or no bar at all.
 *
 * Pure C, no LVGL. The shell resolves what an app declares against this
 * run's orientation here, once, before the app is created, and lays the
 * content area out from the result. The app tests build their frame from
 * the same two functions, so a test lays an app out under exactly the bar
 * the shell would give it. An app never calls any of this: it declares a
 * policy in its struct pocketos_app (app.h) and lays out in whatever body it
 * is given.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_CHROME_H
#define POCKETOS_CHROME_H

#include <stdbool.h>
#include <stdint.h>

/* What an app declares. DEFAULT is what an app that says nothing gets: the
 * shell's choice for the orientation. */
enum pocketos_chrome {
    POCKETOS_CHROME_DEFAULT = 0,
    POCKETOS_CHROME_FULL,
    POCKETOS_CHROME_COMPACT,
    POCKETOS_CHROME_NONE,
};

/* The bar under each effective policy. FULL is the status bar of DS §7 and
 * §9 (POCKETUI_STATUS_BAR_H; shell.c checks that the two agree). COMPACT is
 * §30.1: taller than the 30 px corner squares of §21.1, so an app header
 * under it starts below the corner band and needs no inset of its own. */
#define POCKETOS_CHROME_FULL_H 56
#define POCKETOS_CHROME_COMPACT_H 32

/* Resolve a declaration into what the screen gets; never DEFAULT.
 *   home:      the launcher, which is FULL in every orientation (§30.2).
 *   landscape: this run's orientation.
 * The portrait rule is the staged rollout of §30.4: every screen is FULL in
 * portrait for now, whatever was declared. It is one line in chrome.c and
 * one group of checks in tests/chrome_test.c, so lifting it is one change.
 * In landscape DEFAULT is COMPACT and an explicit policy is honoured. */
enum pocketos_chrome chrome_resolve(enum pocketos_chrome declared, bool landscape, bool home);

/* Pixels the effective policy takes along the top edge: 56, 32 or 0.
 * DEFAULT, which never comes out of chrome_resolve(), counts as FULL. */
int32_t chrome_height(enum pocketos_chrome effective);

/* "full", "compact", "none"; "default" for an unresolved declaration. */
const char *chrome_name(enum pocketos_chrome c);

/* The content area below the chrome: where it starts and how tall it is on a
 * display display_h tall, with reserve pixels kept free at the foot (the
 * keyboard sheet while it is shown, 0 otherwise). Always y + height ==
 * display_h - reserve, so the box reaches the keyboard exactly while it is
 * up and reaches the foot again when it hides: no dead strip under any
 * policy. Never negative; a reserve below zero counts as none. */
struct chrome_box {
    int32_t y;
    int32_t height;
};

struct chrome_box chrome_content_box(enum pocketos_chrome effective, int32_t display_h,
                                     int32_t reserve);

/* The radio chip in the bar (DS §7, §30.1): 36 px tall under FULL, 26 under
 * COMPACT, and its text centred in it. A label draws its text from the top
 * of its content box and clips whatever does not fit, so the chip's height
 * and padding are derived from the line height of the font it actually
 * draws in (the symbol font, 22 px), never from constants alone: a fixed
 * padding that left less than a line cut the tops off "RX" in the 32 px bar.
 * The chip grows past its nominal height rather than clip, up to what the
 * bar leaves above its thickest hairline (2 px, Outdoor); only a font taller
 * than that could still be clipped. NONE draws no chip and gets FULL's. */
#define POCKETOS_CHROME_CHIP_FULL_H 36
#define POCKETOS_CHROME_CHIP_COMPACT_H 26
#define POCKETOS_CHROME_CHIP_MIN_PAD 2
#define POCKETOS_CHROME_HAIRLINE_MAX 2

struct chrome_chip {
    int32_t height;
    int32_t pad_top;
    int32_t pad_bottom;
};

struct chrome_chip chrome_chip_box(enum pocketos_chrome effective, int32_t line_h);

#endif
