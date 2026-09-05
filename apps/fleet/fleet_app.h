/*
 * PocketFleet application state shared by the screen modules.
 *
 * The app owns one struct fleet_game and one container per screen; only one
 * container is visible at a time. The engine below apps/fleet/engine knows
 * nothing about LVGL, and the screens never reach into the engine's rules:
 * they call the referee and read the state.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_APP_H
#define POCKETFLEET_APP_H

#include "engine/fleet_rules.h"

#include "lvgl.h"

enum fleet_screen {
    FLEET_SCREEN_COMMAND = 0,
    FLEET_SCREEN_DEPLOY,
    FLEET_SCREEN_BATTLE,
    FLEET_SCREEN_RESULT,
    FLEET_SCREEN_COUNT
};

struct fleet_command_ui;
struct fleet_deploy_ui;
struct fleet_battle_ui;
struct fleet_result_ui;

struct fleet_app {
    struct fleet_game game;
    uint8_t difficulty;                       /* the Command screen selection */
    lv_obj_t *body;                           /* the root the shell handed us */
    lv_obj_t *screen[FLEET_SCREEN_COUNT];     /* NULL until that screen exists */
    uint8_t current;
    struct fleet_command_ui *command;
    struct fleet_deploy_ui *deploy;
    struct fleet_battle_ui *battle;
    struct fleet_result_ui *result;
};

/* Show a screen and refresh it. A screen that has not been built yet is
 * ignored, so the phases can land one screen at a time. */
void fleet_app_show(struct fleet_app *app, enum fleet_screen screen);
/* Start a fresh match at the selected difficulty and go to deployment. */
void fleet_app_new_match(struct fleet_app *app);
/* Container for a screen: full width, vertical flow, Design System gap. */
lv_obj_t *fleet_app_screen_container(lv_obj_t *parent);

/* Screen modules. */
lv_obj_t *fleet_screen_command_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_command_refresh(struct fleet_app *app);
lv_obj_t *fleet_screen_deploy_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_deploy_refresh(struct fleet_app *app);
/* Reset the deployment screen for a freshly created match. */
void fleet_screen_deploy_enter(struct fleet_app *app);
lv_obj_t *fleet_screen_battle_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_battle_refresh(struct fleet_app *app);
/* Rebind the grids and clear the crosshair for a freshly started match. */
void fleet_screen_battle_enter(struct fleet_app *app);
/* Move the crosshair. It never fires: only the FIRE button does. */
void fleet_screen_battle_aim(struct fleet_app *app, int row, int col);
lv_obj_t *fleet_screen_result_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_result_refresh(struct fleet_app *app);

#endif
