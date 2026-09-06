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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETRADAR_APP_H
#define POCKETRADAR_APP_H

#include "engine/radar_rules.h"
#include "radar_store.h"

#include "lvgl.h"

enum radar_screen {
    RADAR_SCREEN_SCAN = 0,   /* standby and the run itself */
    RADAR_SCREEN_RESULT,
    RADAR_SCREEN_COUNT
};

struct radar_scan_ui;
struct radar_result_ui;

struct radar_app {
    struct radar_run run;
    struct radar_record record;
    lv_obj_t *body;                         /* the root the shell handed us */
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

lv_obj_t *radar_screen_scan_create(struct radar_app *app, lv_obj_t *parent);
void radar_screen_scan_refresh(struct radar_app *app);
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

#endif
