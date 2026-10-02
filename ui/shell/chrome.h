/*
 * Status chrome policy (DS §30, §36): whether an app's screen carries the
 * shell's status cluster - one compact capsule in the top-right corner
 * holding the radio chip and the clock - or no status at all.
 *
 * There is no full-width status bar any more (DS §36 retired §30's FULL and
 * COMPACT bars): no policy reserves a row along the top edge, so the content
 * area starts at the top edge under every chrome, and the cluster lies over
 * the right end of whatever row is there - an app's header, the launcher's
 * header band, Controls' header row. What a screen must keep clear of is the
 * cluster's box, and this file is the one place that box comes from.
 *
 * Pure C, no LVGL. The shell resolves what an app declares here, once,
 * before the app is created, and lays the content area and the app header
 * out from the result. The app tests build their frame from the same
 * functions, so a test lays an app out in exactly the frame the shell would
 * give it. An app never calls any of this: it declares a policy in its
 * struct pocketos_app (app.h) and lays out in whatever body it is given.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_CHROME_H
#define POCKETOS_CHROME_H

#include <stdbool.h>
#include <stdint.h>

/* What an app declares. DEFAULT is what an app that says nothing gets: the
 * status cluster. NONE is a fullscreen app (DS §30.8): no status drawn. */
enum pocketos_chrome {
    POCKETOS_CHROME_DEFAULT = 0,
    POCKETOS_CHROME_CLUSTER,
    POCKETOS_CHROME_NONE,
};

/* The app header row (app.h `header`): the shell's in both orientations,
 * or the app's own in landscape (DS §37.2). */
enum pocketos_header {
    POCKETOS_HEADER_DEFAULT = 0,
    POCKETOS_HEADER_NONE_LANDSCAPE,
};

/* Resolve a declaration into what the screen gets; never DEFAULT.
 *   home:      the shell's own screens (launcher, Controls), which always
 *              show the cluster (DS §36.2).
 *   landscape: this run's orientation. The cluster is the same in both, so
 *              no rule depends on it today; it stays in the signature so a
 *              rule that does is one line here and not a change of every
 *              caller and every app test.
 * NONE is honoured in both orientations; anything else is the cluster. */
enum pocketos_chrome chrome_resolve(enum pocketos_chrome declared, bool landscape, bool home);

/* Pixels the effective policy reserves as a full-width row along the top
 * edge: 0 under every policy. The full-width bar is gone (DS §36), and this
 * stays the one number the content area's top and every app test's frame
 * are built from, so nothing can reintroduce a reserved strip by a constant
 * of its own. */
int32_t chrome_height(enum pocketos_chrome effective);

/* "cluster", "none"; "default" for an unresolved declaration. */
const char *chrome_name(enum pocketos_chrome c);

/* The content area: where it starts and how tall it is on a display
 * display_h tall, with reserve pixels kept free at the foot (the keyboard
 * sheet while it is shown, 0 otherwise). Always y + height ==
 * display_h - reserve, so the box reaches the keyboard exactly while it is
 * up and reaches the foot again when it hides: no dead strip under any
 * policy. Never negative; a reserve below zero counts as none. */
struct chrome_box {
    int32_t y;
    int32_t height;
};

struct chrome_box chrome_content_box(enum pocketos_chrome effective, int32_t display_h,
                                     int32_t reserve);

/* ---- the status cluster (DS §36.1) ---------------------------------------- *
 *
 * A capsule anchored to the top-right corner: the radio chip, then the clock
 * when the screen shows one (the shell's own screens show the time large and
 * leave it out, DS §31.1). Its width is its content's - padding, chip, gap,
 * clock - and never the screen's. It sits CLUSTER_Y below the top edge, so
 * it is centred on the 72 px app header row (DS §7) and lines up with the
 * header's 56 px back slab, and its right edge keeps the side margin the
 * header keeps at that edge: POCKETUI_PAD, raised to the top edge's
 * rounded-corner inset where that is larger (DS §21.1: 30 px portrait, 50
 * landscape on the T-Display K230). */
#define POCKETOS_CHROME_CLUSTER_H 44
#define POCKETOS_CHROME_CLUSTER_Y 14
#define POCKETOS_CHROME_CLUSTER_PAD_L 6  /* before the chip */
#define POCKETOS_CHROME_CLUSTER_PAD_R 12 /* after the clock, or the chip */
#define POCKETOS_CHROME_CLUSTER_GAP 10   /* between the chip and the clock */
#define POCKETOS_CHROME_MARK_PAD 6       /* each side of the background mark's word (DS §51.4) */
#define POCKETOS_CHROME_EDGE_MIN 20      /* POCKETUI_PAD (shell.c checks the two agree) */
/* What a row under the cluster keeps free between its own content and the
 * cluster's left edge: the header's column gap (DS §7). */
#define POCKETOS_CHROME_CLUSTER_CLEAR 16

struct chrome_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

/* The cluster's width for a chip chip_w wide and a clock clock_w wide (0:
 * no clock shown). */
int32_t chrome_cluster_width(int32_t chip_w, int32_t clock_w);

/* The cluster's box on a display display_w wide whose top edge needs
 * inset_right at its right end (pos_display_bar_insets(POS_EDGE_TOP).right),
 * for a cluster width wide. Anchored right; never past either side of the
 * display: a cluster wider than the room it has starts at the top edge's
 * left margin and is as wide as that leaves, however wide it asked to be. */
struct chrome_rect chrome_cluster_box(int32_t display_w, int32_t inset_right, int32_t width);

/* The right padding a row lying along the top edge under the effective
 * policy needs so that nothing of it runs under the cluster: from reserve's
 * left edge to the display's right edge, plus CLUSTER_CLEAR. reserve is the
 * widest box the cluster can take on that screen (chrome_cluster_box of the
 * widest chip and clock), so the row does not move when the chip changes
 * state or the clock its digits. 0 under NONE: nothing is there, and the
 * row takes the top edge's corner insets as any bar would. */
int32_t chrome_row_reserve(enum pocketos_chrome effective, int32_t display_w,
                           const struct chrome_rect *reserve);

/* Whether two boxes share a pixel. */
bool chrome_rects_overlap(const struct chrome_rect *a, const struct chrome_rect *b);

/* The radio chip in the cluster (DS §9, §32.4, §36.1): 32 px tall with its
 * text centred in it. A label draws its text from the top of its content box
 * and clips whatever does not fit, so the chip's height and padding are
 * derived from the line height of the font it actually draws in (the
 * symbol font, 22 px), never from constants alone: a fixed padding that left
 * less than a line cut the tops off "RX" in the old 32 px bar. The chip
 * grows past its nominal height rather than clip, up to the cluster's height
 * less its own 2 px of air above and below; only a font taller than that
 * could still be clipped. The chip is the same under every policy (under
 * NONE it is not drawn, but keeps its size for the lock, which shows it). */
#define POCKETOS_CHROME_CHIP_H 32
#define POCKETOS_CHROME_CHIP_MIN_PAD 2
#define POCKETOS_CHROME_CHIP_AIR 2

struct chrome_chip {
    int32_t height;
    int32_t pad_top;
    int32_t pad_bottom;
};

struct chrome_chip chrome_chip_box(int32_t line_h);

#endif
