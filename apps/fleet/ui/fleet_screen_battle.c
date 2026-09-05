/*
 * PocketFleet Battle screen: the target grid, the turn loop and your waters.
 *
 * Aim-then-confirm (approved deviation for dense grids): a tap on the target
 * grid only moves the crosshair, which is harmless and correctable. The shot
 * is committed by the 64 px FIRE button and nothing else.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_grid.h"
#include "fleet_view.h"
#include "fleet_widgets.h"

#include <stdio.h>
#include <stdlib.h>

#define TARGET_CELL 48
#define OWN_CELL 20
#define PIP_H 14
#define PIP_UNIT 7
#define PIP_GAP 6
/* Explicit, because LV_SIZE_CONTENT on a flex row inside a space-between row
 * measured short and clipped the leading bars. */
#define PIP_BAR_W (FLEET_HULL_CELLS * PIP_UNIT + (FLEET_SHIP_COUNT - 1) * PIP_GAP)

struct fleet_battle_ui {
    struct fleet_app *app;
    lv_obj_t *target;
    lv_obj_t *own;
    lv_obj_t *cell_value;
    lv_obj_t *pip[FLEET_SHIP_COUNT];
    lv_obj_t *fire;
    lv_obj_t *note;
    lv_obj_t *log;
    uint8_t exchanged;   /* an exchange has been reported in the log line */
};

static struct fleet_game *game_of(struct fleet_battle_ui *ui)
{
    return &ui->app->game;
}

void fleet_screen_battle_aim(struct fleet_app *app, int row, int col)
{
    if (!app || !app->battle) {
        return;
    }
    fleet_grid_set_cursor(app->battle->target, row, col);
    fleet_screen_battle_refresh(app);
}

/* A tap moves the crosshair and nothing else (aim-then-confirm). */
static void on_target_cell(void *user, int row, int col)
{
    struct fleet_battle_ui *ui = user;

    fleet_screen_battle_aim(ui->app, row, col);
}

static void on_fire(lv_event_t *e)
{
    struct fleet_battle_ui *ui = lv_event_get_user_data(e);
    struct fleet_game *game = game_of(ui);
    char own[40] = "";
    char enemy[40] = "";
    char line[96];
    int row = 0;
    int col = 0;
    int sunk = -1;
    enum fleet_shot_result result;

    if (fleet_grid_get_cursor(ui->target, &row, &col) != 0) {
        return;
    }
    result = fleet_game_fire(game, FLEET_SIDE_PLAYER, row, col, &sunk);
    if (result == FLEET_SHOT_INVALID) {
        return;
    }
    fleet_view_shot(row, col, result, sunk, own, sizeof(own));
    if (!fleet_game_is_over(game)) {
        int erow = 0;
        int ecol = 0;
        int esunk = -1;
        enum fleet_shot_result reply = fleet_game_opponent_turn(game, &erow, &ecol, &esunk);

        if (reply != FLEET_SHOT_INVALID) {
            fleet_view_shot(erow, ecol, reply, esunk, enemy, sizeof(enemy));
        }
    }
    /* Both halves of the exchange are reported together; pacing and motion
     * between them is a later step. */
    fleet_view_exchange(own, enemy[0] ? enemy : NULL, line, sizeof(line));
    lv_label_set_text(ui->log, line);
    ui->exchanged = 1;
    fleet_grid_set_cursor(ui->target, -1, -1);
    fleet_screen_battle_refresh(ui->app);
    if (fleet_game_is_over(game)) {
        fleet_app_show(ui->app, FLEET_SCREEN_RESULT);
    }
}

/* One bar per enemy ship, as long as its hull: filled once it has been sunk,
 * which is the only thing the player is told about the enemy fleet. */
static lv_obj_t *fleet_pips(lv_obj_t *parent, lv_obj_t **out)
{
    lv_obj_t *box = fleet_hbox(parent, PIP_H, PIP_GAP);
    int i;

    lv_obj_set_width(box, PIP_BAR_W);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_obj_t *pip = lv_obj_create(box);

        lv_obj_remove_style_all(pip);
        /* The secondary button role is the only one that pairs a surface fill
         * with a hairline border; without the border a surviving hull is
         * almost invisible against the panel. The local size beats the 64 px
         * height that role carries. */
        pos_style_add(pip, POS_STYLE_BUTTON_SECONDARY, 0);
        lv_obj_set_size(pip, fleet_ship_length((enum fleet_ship)i) * PIP_UNIT, PIP_H);
        lv_obj_set_style_radius(pip, 2, 0);
        lv_obj_remove_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
        out[i] = pip;
    }
    return box;
}

lv_obj_t *fleet_screen_battle_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_battle_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *panel;
    lv_obj_t *row;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->battle = ui;
    screen = fleet_app_screen_container(parent);

    ui->target = fleet_grid_create(screen, FLEET_GRID_TARGET, TARGET_CELL, 1);
    fleet_grid_bind(ui->target, &app->game.board[FLEET_SIDE_OPPONENT]);
    fleet_grid_set_tap_cb(ui->target, on_target_cell, ui);

    panel = fleet_panel(screen, "TARGET");
    row = fleet_row(panel, 40, 0);
    ui->cell_value = pocketui_label(row, "\xe2\x80\x94", POS_STYLE_VALUE);
    fleet_pips(row, ui->pip);
    ui->note = pocketui_label(panel, "Tap a square, then fire.", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->note, LV_PCT(100));

    ui->fire = pocketui_button(screen, "FIRE", on_fire, ui);

    panel = fleet_panel(screen, "YOUR WATERS");
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui->own = fleet_grid_create(panel, FLEET_GRID_OWN, OWN_CELL, 0);
    fleet_grid_bind(ui->own, &app->game.board[FLEET_SIDE_PLAYER]);
    ui->log = pocketui_label(panel, "", POS_STYLE_CAPTION);
    lv_label_set_long_mode(ui->log, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->log, LV_PCT(100));
    return screen;
}

void fleet_screen_battle_refresh(struct fleet_app *app)
{
    struct fleet_battle_ui *ui;
    struct fleet_game *game;
    char name[FLEET_CELL_NAME_MAX];
    char note[64];
    int row = 0;
    int col = 0;
    int aimed;
    int ready = 0;
    int i;

    if (!app || !app->battle) {
        return;
    }
    ui = app->battle;
    game = &app->game;
    aimed = fleet_grid_get_cursor(ui->target, &row, &col) == 0;
    if (aimed && fleet_cell_name(row, col, name, sizeof(name)) == 0) {
        lv_label_set_text(ui->cell_value, name);
        if (game->board[FLEET_SIDE_OPPONENT].shot[fleet_index(row, col)]) {
            snprintf(note, sizeof(note), "%s has already been fired at.", name);
            lv_label_set_text(ui->note, note);
        } else {
            lv_label_set_text(ui->note, "Ready to fire.");
            ready = 1;
        }
    } else {
        lv_label_set_text(ui->cell_value, "\xe2\x80\x94");
        lv_label_set_text(ui->note, "Tap a square, then fire.");
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        int sunk = fleet_board_ship_sunk(&game->board[FLEET_SIDE_OPPONENT],
                                         (enum fleet_ship)i);

        lv_obj_remove_style(ui->pip[i], pos_style(POS_STYLE_CHIP_TX), 0);
        if (sunk) {
            pos_style_add(ui->pip[i], POS_STYLE_CHIP_TX, 0);
        }
    }
    if (!ui->exchanged) {
        /* Until the first exchange the log line carries the standing report
         * rather than sitting empty. */
        char afloat[40];

        fleet_view_afloat(&game->board[FLEET_SIDE_PLAYER], afloat, sizeof(afloat));
        snprintf(note, sizeof(note), "YOUR FLEET %s", afloat);
        lv_label_set_text(ui->log, note);
    }
    fleet_button_set_enabled(ui->fire, ready);
    fleet_grid_refresh(ui->target);
    fleet_grid_refresh(ui->own);
}

void fleet_screen_battle_enter(struct fleet_app *app)
{
    struct fleet_battle_ui *ui = app ? app->battle : NULL;

    if (!ui) {
        return;
    }
    fleet_grid_bind(ui->target, &app->game.board[FLEET_SIDE_OPPONENT]);
    fleet_grid_bind(ui->own, &app->game.board[FLEET_SIDE_PLAYER]);
    fleet_grid_set_cursor(ui->target, -1, -1);
    ui->exchanged = 0;
    lv_label_set_text(ui->log, "");
}
