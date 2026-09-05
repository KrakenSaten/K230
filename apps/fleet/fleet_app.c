/*
 * PocketFleet: a tactical naval game for PocketOS. Application entry point,
 * screen ownership and navigation. See fleet_app.h.
 *
 * Phase 1 is local single player. The app talks to no service and opens no
 * device; it needs the shell's app API and PocketUI, nothing else.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_app.h"

#include "app.h"
#include "pocketui.h"
#include "ui/fleet_view.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gap between panels (DS §7). */
#define FLEET_PANEL_GAP 22
/* The shell's body padding plus this is the DS body top padding of 24, and
 * it keeps the first panel's caption inside the scroll area. */
#define FLEET_CAPTION_ROOM 8

lv_obj_t *fleet_app_screen_container(lv_obj_t *parent)
{
    lv_obj_t *screen = lv_obj_create(parent);

    lv_obj_remove_style_all(screen);
    lv_obj_set_width(screen, LV_PCT(100));
    lv_obj_set_height(screen, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    /* Panels fill the width; the grids, which are a few pixels narrower, are
     * centred rather than left-aligned. */
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, FLEET_PANEL_GAP, 0);
    /* Room for the first panel's caption, which straddles its top border. */
    lv_obj_set_style_pad_top(screen, FLEET_CAPTION_ROOM, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

void fleet_app_show(struct fleet_app *app, enum fleet_screen screen)
{
    char status[48];
    int i;

    if (!app || (unsigned)screen >= FLEET_SCREEN_COUNT || !app->screen[screen]) {
        return;
    }
    for (i = 0; i < FLEET_SCREEN_COUNT; i++) {
        if (app->screen[i]) {
            lv_obj_add_flag(app->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(app->screen[screen], LV_OBJ_FLAG_HIDDEN);
    app->current = (uint8_t)screen;
    lv_obj_scroll_to_y(app->body, 0, LV_ANIM_OFF);

    switch (screen) {
    case FLEET_SCREEN_COMMAND:
        fleet_screen_command_refresh(app);
        pocketos_shell_set_status_hint("COMMAND");
        return;
    case FLEET_SCREEN_DEPLOY:
        fleet_screen_deploy_refresh(app);
        break;
    case FLEET_SCREEN_BATTLE:
        fleet_screen_battle_refresh(app);
        break;
    case FLEET_SCREEN_RESULT:
        fleet_screen_result_refresh(app);
        break;
    default:
        break;
    }
    if (fleet_view_status(&app->game, status, sizeof(status)) == 0) {
        pocketos_shell_set_status_hint(status);
    }
}

void fleet_app_new_match(struct fleet_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    /* The engine is deterministic given a seed; the seed itself is taken from
     * the clock so successive engagements differ, and it is stored in the
     * match so one can be replayed exactly. */
    seed = (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get();
    fleet_game_new(&app->game, seed, (enum fleet_difficulty)app->difficulty);
    fleet_screen_deploy_enter(app);
    fleet_app_show(app, FLEET_SCREEN_DEPLOY);
}

/* ---- development aid --------------------------------------------------- */

/* $POCKETFLEET_SCREEN opens the app on a given screen with a fixed seed, so
 * the simulator can render every screen for design review the way the shell's
 * own --screenshot does. It does nothing unless the variable is set. */
#define FLEET_DEBUG_SEED 20260905u

/* Fire at a cell for the player and let the opponent answer. */
static void debug_exchange(struct fleet_app *app, int row, int col)
{
    if (fleet_game_fire(&app->game, FLEET_SIDE_PLAYER, row, col, NULL) == FLEET_SHOT_INVALID) {
        return;
    }
    if (!fleet_game_is_over(&app->game)) {
        fleet_game_opponent_turn(&app->game, NULL, NULL, NULL);
    }
}

static void debug_start_match(struct fleet_app *app)
{
    struct fleet_rng deploy;

    fleet_game_new(&app->game, FLEET_DEBUG_SEED, (enum fleet_difficulty)app->difficulty);
    fleet_screen_deploy_enter(app);
    fleet_rng_seed(&deploy, FLEET_DEBUG_SEED ^ 0x5A5A5A5Au);
    fleet_board_autoplace(&app->game.board[FLEET_SIDE_PLAYER], &deploy);
}

static void debug_open(struct fleet_app *app)
{
    const char *want = getenv("POCKETFLEET_SCREEN");
    const struct fleet_board *enemy;
    int i;

    if (!want) {
        return;
    }
    if (strcmp(want, "deploy") == 0) {
        debug_start_match(app);
        /* Two ships left waiting, so both roster states are visible. */
        fleet_board_unplace(&app->game.board[FLEET_SIDE_PLAYER], FLEET_SHIP_CRUISER);
        fleet_board_unplace(&app->game.board[FLEET_SIDE_PLAYER], FLEET_SHIP_DESTROYER);
        fleet_app_show(app, FLEET_SCREEN_DEPLOY);
        return;
    }
    if (strcmp(want, "battle") != 0 && strcmp(want, "result") != 0) {
        return;
    }
    debug_start_match(app);
    fleet_game_start(&app->game);
    fleet_screen_battle_enter(app);
    enemy = &app->game.board[FLEET_SIDE_OPPONENT];
    if (strcmp(want, "result") == 0) {
        for (i = 0; i < FLEET_CELLS && !fleet_game_is_over(&app->game); i++) {
            if (enemy->ship_at[i] != FLEET_NO_SHIP) {
                debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
            }
        }
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        return;
    }
    /* A readable mid-game: one ship sunk, a wounded one, and some water. */
    for (i = 0; i < FLEET_CELLS; i++) {
        if (enemy->ship_at[i] == FLEET_SHIP_DESTROYER) {
            debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
        }
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        if (enemy->ship_at[i] == FLEET_SHIP_CARRIER &&
            app->game.board[FLEET_SIDE_OPPONENT].ships[FLEET_SHIP_CARRIER].hits < 2) {
            debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
        }
    }
    for (i = 3; i < FLEET_CELLS; i += 11) {
        debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
    }
    fleet_app_show(app, FLEET_SCREEN_BATTLE);
    /* Aimed but not fired, which is the state the FIRE button acts on. */
    fleet_screen_battle_aim(app, 2, 6);
}

/* ---- shell app API ---------------------------------------------------- */

static void *fleet_create(lv_obj_t *root)
{
    struct fleet_app *app = calloc(1, sizeof(*app));

    if (!app) {
        return NULL;
    }
    app->body = root;
    app->difficulty = FLEET_OFFICER;
    /* A match exists from the start so every screen has something to read. */
    fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);

    app->screen[FLEET_SCREEN_COMMAND] = fleet_screen_command_create(app, root);
    app->screen[FLEET_SCREEN_DEPLOY] = fleet_screen_deploy_create(app, root);
    app->screen[FLEET_SCREEN_BATTLE] = fleet_screen_battle_create(app, root);
    app->screen[FLEET_SCREEN_RESULT] = fleet_screen_result_create(app, root);
    fleet_screen_deploy_enter(app);
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
    debug_open(app);
    return app;
}

static void fleet_destroy(void *priv)
{
    struct fleet_app *app = priv;

    if (!app) {
        return;
    }
    /* The screen containers are children of the shell's root and are deleted
     * with it; only the private blocks are ours to release. */
    free(app->command);
    free(app->deploy);
    free(app->battle);
    free(app->result);
    free(app);
}

const struct pocketos_app app_fleet = {
    .id = "fleet",
    .name = "Fleet",
    /* Placeholder: the Design System stroke icon set does not exist yet, so
     * the launcher uses the closest LV_SYMBOL glyph (DS §11, step 5+). */
    .icon = LV_SYMBOL_GPS,
    .create = fleet_create,
    .tick = NULL,
    .destroy = fleet_destroy,
};
