/*
 * PocketRadar application state shared by the two screen modules.
 *
 * The app owns one struct radar_run, one lifetime record, and one container
 * per screen; only one container is visible at a time. It also owns the only
 * clock in the app: a single LVGL timer at RADAR_TICK_MS drives the engine
 * and repaints the scope, so there is no second timebase that could disagree
 * with the first.
 *
 * Two screens are enough. SCAN carries both the standby state and the run
 * itself, because the scope face is the same thing before and during a run
 * and swapping containers to change one button would be a screen change the
 * player could see. RESULT is separate because it shows different content.
 *
 * The engine below apps/radar/engine knows nothing about LVGL and does no
 * I/O; the screens read run state and call the referee.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETRADAR_APP_H
#define POCKETRADAR_APP_H

#include "engine/radar_rules.h"
#include "radar_store.h"

#include "lvgl.h"
#include "pocketui.h"
#include "pos_display.h"

enum radar_screen {
    RADAR_SCREEN_SCAN = 0,   /* standby and the run itself */
    RADAR_SCREEN_RESULT,
    RADAR_SCREEN_COUNT
};

struct radar_scan_ui;
struct radar_result_ui;

/* The shape both screens lay out in, chosen from the body alone (DS 29.1).
 * TALL is the portrait stack; WIDE puts the scope beside what is read off it.
 * Nothing here names an orientation: the app is given a box and reads it. */
enum radar_shape {
    RADAR_SHAPE_TALL = 0,
    RADAR_SHAPE_WIDE
};

/* The scope down the page: the panel's width less the body's padding. It is
 * also the largest the scope may ever be, so the wide shape can never cost
 * more per tick than the tall one already does. */
#define RADAR_SCOPE_TALL 520
/* The smallest scope worth drawing: below this the wide shape is refused and
 * the tall stack is kept whole and scrolled. */
#define RADAR_SCOPE_MIN 240
/* The narrowest useful region beside the scope: two cards abreast and a
 * 64 px action under them. */
#define RADAR_SIDE_MIN 360
/* Gutter between the columns of the wide shape (DS section 7). */
#define RADAR_COL_GAP POCKETUI_PAD

struct radar_app {
    struct radar_run run;
    struct radar_record record;
    lv_obj_t *body;                         /* the root the shell handed us */
    /* Exactly the body's content box, and the only thing measured: both
     * screens take their shape from this one rectangle, never from the size
     * of what a layout itself put in it. */
    lv_obj_t *frame;
    /* What the shape in force was chosen from: the box, and how far the
     * panel's unsafe area reaches into it. Both, because a panel with
     * different corners gives the same box a different amount of room.
     * PocketUI owns the comparison, and every responsive app makes it the
     * same way (pocketui_layout_begin). */
    struct pocketui_layout_guard layout_guard;
    /* How many times the layout has actually been worked out. A pass that
     * finds nothing changed does not count, which is what makes "only on a
     * change, never on a tick" something a test can hold the app to. */
    uint32_t layouts;
    uint8_t shape;                          /* enum radar_shape */
    int scope_size;                         /* the side in force */
    lv_obj_t *screen[RADAR_SCREEN_COUNT];
    uint8_t current;
    /* Persistence is best effort. It is switched off for the session after
     * the first failure and the game carries on without it. */
    uint8_t storage_ok;
    /* DS section 12, read once at start from the settings store. */
    uint8_t reduced_motion;
    uint8_t new_best;                       /* the finished run beat the record */
    lv_timer_t *clock;                      /* the engine tick; paused off-run */
    struct radar_scan_ui *scan;
    struct radar_result_ui *result;
};

/* Show a screen and refresh it. */
void radar_app_show(struct radar_app *app, enum radar_screen screen);
/* Start a fresh run from a seed taken at the app boundary. */
void radar_app_begin(struct radar_app *app);
/* Fold a finished run into the record, store it, and show the result. */
void radar_app_finish(struct radar_app *app);
/* Container for a screen: full width, vertical flow, Design System gap. */
lv_obj_t *radar_app_screen_container(lv_obj_t *parent);
/* A transparent box that holds part of a screen, so the same objects can be
 * stacked down the page and set side by side across it. */
lv_obj_t *radar_app_box(lv_obj_t *parent);
/* A splitter: its children stack down the page in the tall shape and stand
 * side by side in the wide one. */
void radar_app_box_split(lv_obj_t *box, int wide);
/* One column of a splitter: always down the page, full width and as high as
 * its content in the tall shape, an equal share of the row and the full
 * height in the wide one. */
void radar_app_box_column(lv_obj_t *box, int wide);
/* A screen container. across says whether the wide shape turns the screen
 * itself across the page - which the screen built round the scope does, while
 * the one built round cards keeps its column and turns a box inside it. */
void radar_app_screen_flow(lv_obj_t *screen, int wide, int across);

/* The side the wide shape would draw the scope at in a body this high, and
 * whether the wide shape fits at all. Pure arithmetic, so the rule is one
 * function and the tests can ask it directly. */
int radar_scope_for_height(int32_t h);
int radar_shape_is_wide(int32_t w, int32_t h, int *scope_out);

/* Screen modules. Each builds its objects once and then only moves and
 * resizes them: _relayout is called when the body's box changes and never
 * creates or deletes anything. */
lv_obj_t *radar_screen_scan_create(struct radar_app *app, lv_obj_t *parent);
void radar_screen_scan_refresh(struct radar_app *app);
void radar_screen_scan_relayout(struct radar_app *app, int wide, int scope);
/* One engine step and the repaint that follows it. */
void radar_screen_scan_tick(struct radar_app *app);
/* Select whatever a tap at these polar coordinates found, or clear the
 * selection when it found open water. Also the entry point the headless
 * test drives, so the interaction can be exercised without an input device. */
void radar_screen_scan_tap(struct radar_app *app, int bearing, int range);
/* Take the shot, exactly as the ENGAGE button does. */
void radar_screen_scan_engage(struct radar_app *app);

lv_obj_t *radar_screen_result_create(struct radar_app *app, lv_obj_t *parent);
void radar_screen_result_refresh(struct radar_app *app);
void radar_screen_result_relayout(struct radar_app *app, int wide);

#endif
