/*
 * PocketTimber application state shared by the screen modules.
 *
 * The app owns one struct timber_run, a session-only best score, the view,
 * and one container per screen; only one container is visible at a time.
 * It also owns the only clock in the app: a single LVGL timer at
 * TIMBER_TICK_MS feeds the pull track's travel to the engine, steps the
 * engine, and repaints the table, so there is no second timebase. The
 * engine's time follows the LVGL tick, not the number of callbacks: a late
 * callback runs every tick that fell due, within TIMBER_CATCHUP_TICKS.
 *
 * Two screens. TABLE carries standby, the run and the collapse, because
 * the tower is the same thing before, during and after; RESULT is separate
 * because it shows different content.
 *
 * The records (best score and lifetime counters) come from the record file
 * through timber_store when the app opens and go back to it when a run
 * finishes (D2); a store that fails leaves the app session-only. No audio,
 * no shake.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETTIMBER_APP_H
#define POCKETTIMBER_APP_H

#include "engine/timber_rules.h"
#include "ui/timber_view.h"
#include "timber_store.h"

#include "lvgl.h"

enum timber_screen {
    TIMBER_SCREEN_TABLE = 0,    /* standby, the run and the collapse */
    TIMBER_SCREEN_RESULT,
    TIMBER_SCREEN_COUNT
};

/* A clock callback that comes late runs the engine ticks it missed, up to
 * this many (320 ms at 25 Hz), so a delayed callback costs no game time and
 * a finger's speed is judged over the time it really took. A longer stall
 * is a pause: the ticks past the bound are skipped, never run in a burst,
 * and whatever the finger did in them is dropped (timber_screens.c). */
#define TIMBER_CATCHUP_TICKS 8
/* The pull track's travel is binned by the tick it happened in: the ticks a
 * late clock still owes, and the one in progress. */
#define TIMBER_DRAG_BINS (TIMBER_CATCHUP_TICKS + 1)

struct timber_table_ui;
struct timber_result_ui;

struct timber_app {
    struct timber_run run;
    struct timber_records records;          /* the record file's contents (D2) */
    uint8_t store_ok;                       /* the file can still be written */
    struct timber_view view;
    lv_obj_t *body;
    lv_obj_t *screen[TIMBER_SCREEN_COUNT];
    uint8_t current;
    uint8_t reduced_motion;
    uint8_t new_best;
    lv_timer_t *clock;
    /* The engine's time: where its last tick ended, in LVGL ticks. */
    uint32_t clock_ms;
    /* The pull track: the finger's last reading, and the travel it has
     * made since the engine last took some, binned by tick: bin 0 is the
     * tick that ends TIMBER_TICK_MS after clock_ms. The travel belongs to
     * the block selected when the finger went down, in that turn, and is
     * dropped if either changes. */
    uint8_t pressing;
    int8_t drag_block;
    uint16_t drag_turn;
    int32_t last_x;
    uint32_t last_ms;
    int32_t pending_px[TIMBER_DRAG_BINS];
    /* The slot the block in hand would go to. */
    int8_t ghost;
    struct timber_table_ui *table;
    struct timber_result_ui *result;
};

void timber_app_show(struct timber_app *app, enum timber_screen screen);
void timber_app_begin(struct timber_app *app);
void timber_app_finish(struct timber_app *app);
lv_obj_t *timber_app_screen_container(lv_obj_t *parent);

lv_obj_t *timber_screen_table_create(struct timber_app *app, lv_obj_t *parent);
void timber_screen_table_refresh(struct timber_app *app);
/* Run the engine up to `now` (LVGL ticks): every whole TIMBER_TICK_MS since
 * the last tick, each with the travel the finger made in it, at most
 * TIMBER_CATCHUP_TICKS of them. Stops at the end of the run. */
void timber_screen_table_clock(struct timber_app *app, uint32_t now);
/* Forget the pull track's travel and start the engine's time at `now`. */
void timber_screen_table_drag_reset(struct timber_app *app, uint32_t now);
/* What a tap on the table means: select a block, or clear the selection
 * on the felt. Also the entry point the headless states drive. */
void timber_screen_table_tap(struct timber_app *app, int id);
void timber_screen_table_test(struct timber_app *app);
void timber_screen_table_place(struct timber_app *app);
void timber_screen_table_choose(struct timber_app *app, int side);

lv_obj_t *timber_screen_result_create(struct timber_app *app, lv_obj_t *parent);
void timber_screen_result_refresh(struct timber_app *app);

#endif
