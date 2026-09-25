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
#include "pocketui.h"
#include "pos_display.h"

enum fleet_screen {
    FLEET_SCREEN_COMMAND = 0,
    FLEET_SCREEN_DEPLOY,
    FLEET_SCREEN_BATTLE,
    FLEET_SCREEN_RESULT,
    /* Multiplayer: choosing an opponent, invitations, the match in hand
     * (docs/apps/FLEET_MULTIPLAYER.md). Last, so the four screens above keep
     * their places. */
    FLEET_SCREEN_LOBBY,
    FLEET_SCREEN_COUNT
};

/* Whom Deploy, Battle and Result are about: the AI (fleet_game), or another
 * device (the multiplayer session). */
enum fleet_mode {
    FLEET_MODE_SOLO = 0,
    FLEET_MODE_MULTI
};

struct fleet_command_ui;
struct fleet_deploy_ui;
struct fleet_battle_ui;
struct fleet_result_ui;
struct fleet_lobby_ui;
struct fleet_session;
struct fleet_link;

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
/* The widest a cell across the wide board is drawn: 51 px, the §28 cell of 34
 * down made half as wide again, which is what the two columns beside the board
 * were measured against (DS §28.6). A taller body - the fullscreen one, with
 * no status bar - grows the cells down the board, not across it, so the
 * readout column, its four nudges and the log line keep the width they were
 * validated at. */
#define FLEET_CELL_ACROSS_MAX 51

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
     * different corners gives the same box a different amount of room.
     * PocketUI owns the comparison, and every responsive app makes it the
     * same way (pocketui_layout_begin). */
    struct pocketui_layout_guard layout_guard;
    /* How many times the layout has actually been worked out. A pass that
     * finds nothing changed does not count, which is what makes "only on a
     * change" something a test can hold the app to. */
    uint32_t layouts;
    uint8_t shape;                            /* enum fleet_shape */
    int cell;                                 /* board cell height in force */
    int cell_across;                          /* and its width, which may be more */
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
    struct fleet_lobby_ui *lobby;

    /* ---- multiplayer (fleet_mp.c) ---------------------------------------- */
    uint8_t mode;                             /* enum fleet_mode */
    struct fleet_link *link;                  /* NULL when there is none */
    struct fleet_session *mp;                 /* NULL when there is no link */
    lv_timer_t *mp_timer;
    unsigned mp_revision;                     /* what the screens last showed */
    uint8_t mp_phase;                         /* the phase they were steered by */
    uint8_t mp_saved;                         /* a saved match is waiting to resume */
    char mp_saved_peer[32];
    uint8_t mp_forfeit_armed;                 /* FORFEIT pressed once */
    int64_t mp_clock_offset;                  /* development aid: time skipped ahead */
    struct fleet_board mp_fleet;              /* the fleet being placed for a match */
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

/* The cell height the wide shape would draw a labelled board at in a body this
 * high; the width it would draw it at, given that height, in a body this wide;
 * and whether the wide shape fits at all. Pure arithmetic, so the rule is
 * three functions and the tests can ask them directly. */
int fleet_cell_for_height(int32_t h);
int fleet_cell_across(int32_t w, int cell_down);
int fleet_shape_is_wide(int32_t w, int32_t h, int *cell_w_out, int *cell_h_out);

/* Screen modules. Each builds its objects once and then only moves and
 * resizes them: _relayout is called when the body's box changes and never
 * creates or deletes anything. */
lv_obj_t *fleet_screen_command_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_command_refresh(struct fleet_app *app);
void fleet_screen_command_relayout(struct fleet_app *app, int wide);
lv_obj_t *fleet_screen_deploy_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_deploy_refresh(struct fleet_app *app);
void fleet_screen_deploy_relayout(struct fleet_app *app, int wide, int cell_w, int cell_h);
/* Reset the deployment screen for a freshly created match. */
void fleet_screen_deploy_enter(struct fleet_app *app);
lv_obj_t *fleet_screen_battle_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_battle_refresh(struct fleet_app *app);
/* Rebind the grids and clear the crosshair for a freshly started match. */
void fleet_screen_battle_enter(struct fleet_app *app);
/* Move the crosshair. It never fires: only the FIRE button does. */
void fleet_screen_battle_aim(struct fleet_app *app, int row, int col);
/* Move the crosshair one square, clamped at the edges of the board. With no
 * crosshair set it starts at the middle. This is the path that does not ask
 * the player to hit anything small: the buttons that call it are a finger's
 * size, whatever the board's cells are. */
void fleet_screen_battle_nudge(struct fleet_app *app, int drow, int dcol);
/* Stop the grids animating and settle any turn still being paced out, so the
 * match is never left half played. */
void fleet_screen_battle_leave(struct fleet_app *app);
/* Commit the aimed shot, exactly as the FIRE button does. */
void fleet_screen_battle_fire(struct fleet_app *app);
void fleet_screen_battle_relayout(struct fleet_app *app, int wide, int cell_w, int cell_h);
lv_obj_t *fleet_screen_result_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_result_refresh(struct fleet_app *app);
void fleet_screen_result_relayout(struct fleet_app *app, int wide);
lv_obj_t *fleet_screen_lobby_create(struct fleet_app *app, lv_obj_t *parent);
void fleet_screen_lobby_refresh(struct fleet_app *app);
void fleet_screen_lobby_relayout(struct fleet_app *app, int wide);

/* ---- multiplayer (fleet_mp.c) ------------------------------------------------ */

/* Make multiplayer available: pick the link and read any saved match. Opens
 * no connection and sends nothing. */
void fleet_mp_create(struct fleet_app *app);
void fleet_mp_destroy(struct fleet_app *app);
/* Open the lobby. This is the player choosing multiplayer: from here the
 * session is engaged and packets move. */
void fleet_app_multiplayer(struct fleet_app *app);
/* Continue the match in hand on the screen its phase belongs to. */
void fleet_app_mp_resume(struct fleet_app *app);
/* After anything that may have changed the match: steer and refresh. */
void fleet_app_mp_changed(struct fleet_app *app);
/* CLOCK_MONOTONIC in ms, as the session sees it. */
int64_t fleet_app_now(struct fleet_app *app);
/* The opponent's name as the screens write it. */
const char *fleet_app_peer(struct fleet_app *app, char *buf, size_t n);
/* Development aid: with POCKETFLEET_MP_FAKE set, drive a match against the
 * virtual opponent into the named state on a skipped clock, for screenshots
 * (POCKETFLEET_SCREEN=lobby|mp_invited|mp_deploy|mp_battle|mp_waiting|mp_lost|
 * mp_result). Returns 1 when it recognised the name. */
int fleet_mp_debug(struct fleet_app *app, const char *want);
/* The board Deploy works on: the AI match's, or the one for a multiplayer
 * match. */
struct fleet_board *fleet_app_deploy_board(struct fleet_app *app);

#endif
