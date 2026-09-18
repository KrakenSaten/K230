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
#include "pos_display.h"

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

/* The shape every screen lays out in, chosen from the body alone (DS §28.1).
 * TALL is the portrait stack; WIDE puts the board beside what is said about
 * it. Nothing here names an orientation: the app is given a box and reads it. */
enum fleet_shape {
    FLEET_SHAPE_TALL = 0,
    FLEET_SHAPE_WIDE
};

/* The target and deployment boards in the tall shape: ten 48 px cells, the
 * approved deviation D1 (docs/apps/POCKETFLEET.md). */
#define FLEET_CELL_TALL 48
/* The finest board the wide shape will draw. Below this the wide shape is
 * refused and the tall stack is kept whole and scrolled, rather than a board
 * nobody can aim at. */
#define FLEET_CELL_MIN 32
/* The narrowest useful column beside the board: a 64 px button, a value and a
 * wrapped line of text. Two of them and the board are what the wide shape
 * needs across. */
#define FLEET_COL_MIN 280

struct fleet_app {
    struct fleet_game game;
    uint8_t difficulty;                       /* the Command screen selection */
    lv_obj_t *body;                           /* the root the shell handed us */
    /* Exactly the body's content box, and the only thing measured: every
     * screen's shape is chosen from this one rectangle, never from the size
     * of what a layout itself put in it. */
    lv_obj_t *frame;
    /* What the shape in force was chosen from: the box, and how far the
     * panel's unsafe area reaches into it. Both, because a panel with
     * different corners gives the same box a different amount of room. */
    lv_area_t laid_out;
    struct pos_insets laid_out_insets;
    uint8_t laid_out_valid;
    /* How many times the layout has actually been worked out. A pass that
     * finds nothing changed does not count, which is what makes "only on a
     * change" something a test can hold the app to. */
    uint32_t layouts;
    uint8_t shape;                            /* enum fleet_shape */
    int cell;                                 /* board cell size in force */
    lv_obj_t *screen[FLEET_SCREEN_COUNT];     /* NULL until that screen exists */
    uint8_t current;
    /* Persistence is best effort. It is switched off for the session after
     * the first failure, and the game carries on without it. */
    uint8_t storage_ok;
    uint8_t resumable;                        /* a stored match is waiting */
    /* DS §12: with reduced motion every animated row becomes an instant
     * state change. Read once at start from the settings store. */
    uint8_t reduced_motion;
    struct fleet_command_ui *command;
    struct fleet_deploy_ui *deploy;
    struct fleet_battle_ui *battle;
    struct fleet_result_ui *result;
};

/* Show a screen and refresh it. A screen that has not been built yet is
 * ignored, so the phases can land one screen at a time. */
void fleet_app_show(struct fleet_app *app, enum fleet_screen screen);
/* Start a fresh match at the selected difficulty and go to deployment. Any
 * stored match is superseded and removed. */
void fleet_app_new_match(struct fleet_app *app);
/* Continue the match that was loaded at start, on the screen it left off. */
void fleet_app_resume(struct fleet_app *app);
/* Store the match after a resolved turn. Never fails loudly: the first
 * failure switches persistence off for the session and is logged. */
void fleet_app_autosave(struct fleet_app *app);
/* Container for a screen: full width, vertical flow, Design System gap. */
lv_obj_t *fleet_app_screen_container(lv_obj_t *parent);
/* A transparent box that holds part of a screen, so the same objects can be
 * stacked in the tall shape and set side by side in the wide one. It draws
 * nothing and does not clip, so a panel caption straddling its top border is
 * still drawn; only a scroller and the frame ever clip. */
lv_obj_t *fleet_app_box(lv_obj_t *parent);
/* A splitter: its children stack down the page in the tall shape and stand
 * side by side in the wide one. */
void fleet_app_box_split(lv_obj_t *box, int wide);
/* One column of a splitter: always down the page, full width and as high as
 * its content in the tall shape, an equal share of the row and the full
 * height in the wide one. A caller wanting a column sized to what it holds
 * rather than to its share says so afterwards, as Battle does. */
void fleet_app_box_column(lv_obj_t *box, int wide);
/* The same for a screen container. across says whether the wide shape turns
 * the screen itself across the page - which the two screens built round a
 * board do, while the two built round panels keep their column and turn a box
 * inside it instead. In the tall shape both are the stack they always were,
 * children centred across it; in the wide shape the screen fills the body. */
void fleet_app_screen_flow(lv_obj_t *screen, int wide, int across);

/* The cell size the wide shape would draw a labelled board at in a body this
 * high, and whether the wide shape fits at all. Pure arithmetic, so the rule
 * is one function and the tests can ask it directly. */
int fleet_cell_for_height(int32_t h);
int fleet_shape_is_wide(int32_t w, int32_t h, int *cell_out);

/* Screen modules. Each builds its objects once and then only moves and
 * resizes them: _relayout is called when the body's box changes and never
 * creates or deletes anything. */
lv_obj_t *fleet_screen_command_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_command_refresh(struct fleet_app *app);
void fleet_screen_command_relayout(struct fleet_app *app, int wide);
lv_obj_t *fleet_screen_deploy_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_deploy_refresh(struct fleet_app *app);
void fleet_screen_deploy_relayout(struct fleet_app *app, int wide, int cell);
/* Reset the deployment screen for a freshly created match. */
void fleet_screen_deploy_enter(struct fleet_app *app);
lv_obj_t *fleet_screen_battle_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_battle_refresh(struct fleet_app *app);
/* Rebind the grids and clear the crosshair for a freshly started match. */
void fleet_screen_battle_enter(struct fleet_app *app);
/* Move the crosshair. It never fires: only the FIRE button does. */
void fleet_screen_battle_aim(struct fleet_app *app, int row, int col);
/* Stop the grids animating and settle any turn still being paced out, so the
 * match is never left half played. */
void fleet_screen_battle_leave(struct fleet_app *app);
/* Commit the aimed shot, exactly as the FIRE button does. */
void fleet_screen_battle_fire(struct fleet_app *app);
void fleet_screen_battle_relayout(struct fleet_app *app, int wide, int cell);
lv_obj_t *fleet_screen_result_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_result_refresh(struct fleet_app *app);
void fleet_screen_result_relayout(struct fleet_app *app, int wide);

#endif
