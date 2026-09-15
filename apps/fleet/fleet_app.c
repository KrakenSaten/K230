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
#include "engine/fleet_store.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "ui/fleet_view.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gap between panels (DS §7). */
#define FLEET_PANEL_GAP 22

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
    if (app->current == FLEET_SCREEN_BATTLE && screen != FLEET_SCREEN_BATTLE) {
        fleet_screen_battle_leave(app);
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

void fleet_app_autosave(struct fleet_app *app)
{
    if (!app || !app->storage_ok) {
        return;
    }
    if (fleet_game_is_over(&app->game)) {
        /* A finished match is not worth resuming, so the slot is freed
         * rather than filled with something Resume would refuse anyway. */
        fleet_store_clear();
        app->resumable = 0;
        return;
    }
    if (fleet_store_save(&app->game) != 0) {
        /* One failure is enough: retrying every turn would spend the whole
         * match writing to a filesystem that has already said no. */
        app->storage_ok = 0;
        app->resumable = 0;
        LOG_WARN("fleet: cannot write %s, continuing without persistence",
                 fleet_store_path());
        fleet_screen_command_refresh(app);
    }
}

void fleet_app_resume(struct fleet_app *app)
{
    if (!app || !app->resumable) {
        return;
    }
    app->resumable = 0;
    app->difficulty = app->game.difficulty;
    switch (app->game.phase) {
    case FLEET_PHASE_DEPLOY:
        fleet_screen_deploy_enter(app);
        fleet_app_show(app, FLEET_SCREEN_DEPLOY);
        break;
    case FLEET_PHASE_OVER:
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        break;
    default:
        fleet_screen_battle_enter(app);
        fleet_app_show(app, FLEET_SCREEN_BATTLE);
        break;
    }
}

void fleet_app_new_match(struct fleet_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    if (app->storage_ok) {
        fleet_store_clear();
    }
    app->resumable = 0;
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
    /* Same save point as a turn played by hand, so the persistence path is
     * exercised by the headless tests too. */
    fleet_app_autosave(app);
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
    if (strcmp(want, "battle") != 0 && strcmp(want, "battle_paced") != 0 &&
        strcmp(want, "result") != 0) {
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
    if (strcmp(want, "battle_paced") == 0) {
        /* Take the shot through the button's own path, so a headless run
         * exercises the paced reply, its timer and the teardown that has to
         * settle it. */
        fleet_screen_battle_fire(app);
    }
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
    app->storage_ok = 1;
    app->reduced_motion = (uint8_t)(pocketos_shell_reduced_motion() != 0);
    /* A match exists from the start so every screen has something to read. */
    fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);
    /* Appearance of a stored match must never delay or prevent the app from
     * opening: an absent, unreadable, damaged or impossible save simply
     * means there is nothing to resume. */
    {
        int loaded = fleet_store_load(&app->game);

        if (loaded == 0 && !fleet_game_is_over(&app->game)) {
            app->resumable = 1;
            app->difficulty = app->game.difficulty;
            LOG_INFO("fleet: resumable match from %s, %s turn %u", fleet_store_path(),
                     fleet_difficulty_name((enum fleet_difficulty)app->game.difficulty),
                     (unsigned)app->game.turn);
        } else {
            if (loaded < 0) {
                LOG_WARN("fleet: stored match at %s rejected, starting fresh",
                         fleet_store_path());
            }
            fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);
        }
    }

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
    /* Settle a paced turn and stop every timer before the objects they refer
     * to go away with the shell's root. */
    fleet_screen_battle_leave(app);
    /* The screen containers are children of the shell's root and are deleted
     * with it; only the private blocks are ours to release. */
    free(app->command);
    free(app->deploy);
    free(app->battle);
    free(app->result);
    free(app);
}

LV_IMAGE_DECLARE(pos_app_icon_fleet);

const struct pocketos_app app_fleet = {
    .id = "fleet",
    .name = "Fleet",
    /* The launcher draws the Doors icon (DS §20); the glyph stays as the
     * app's text icon, the closest LV_SYMBOL. */
    .icon = LV_SYMBOL_GPS,
    .icon_mask = &pos_app_icon_fleet,
    .create = fleet_create,
    .tick = NULL,
    .destroy = fleet_destroy,
};
