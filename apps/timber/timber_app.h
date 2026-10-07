/*
 * PocketTimber application state shared by the screen modules.
 *
 * The app owns one struct timber_run, a session-only best score, the view,
 * and one container per screen; only one container is visible at a time.
 * It also owns the only clock in the app: a single LVGL timer at
 * TIMBER_TICK_MS feeds the pull track's travel to the engine, steps the
 * engine, and repaints the table, so there is no second timebase.
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
    /* The pull track: the finger's travel since the last tick. */
    uint8_t pressing;
    int32_t last_x;
    int32_t pending_px;
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
/* One engine step, with whatever the finger did since the last one. */
void timber_screen_table_tick(struct timber_app *app);
/* What a tap on the table means: select a block, or clear the selection
 * on the felt. Also the entry point the headless states drive. */
void timber_screen_table_tap(struct timber_app *app, int id);
void timber_screen_table_test(struct timber_app *app);
void timber_screen_table_place(struct timber_app *app);
void timber_screen_table_choose(struct timber_app *app, int side);

lv_obj_t *timber_screen_result_create(struct timber_app *app, lv_obj_t *parent);
void timber_screen_result_refresh(struct timber_app *app);

#endif
