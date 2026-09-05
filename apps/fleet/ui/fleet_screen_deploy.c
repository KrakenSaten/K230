/*
 * PocketFleet Deploy screen: place the fleet before the engagement starts.
 *
 * Placing is reversible, so a tap on the grid places the selected ship
 * directly. The aim-then-confirm rule that governs the dense grid applies to
 * the irreversible action, which on this screen is CONFIRM DEPLOYMENT.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_grid.h"
#include "fleet_widgets.h"

#include <stdio.h>
#include <stdlib.h>

#define DEPLOY_CELL 48
#define ROSTER_ROW 56

struct fleet_deploy_ui {
    struct fleet_app *app;
    struct fleet_rng rng;              /* auto-deploy, kept off the match streams */
    lv_obj_t *grid;
    lv_obj_t *name[FLEET_SHIP_COUNT];
    lv_obj_t *state[FLEET_SHIP_COUNT];
    lv_obj_t *message;
    lv_obj_t *confirm;
    uint8_t selected;
    uint8_t vertical;
};

static struct fleet_board *player_board(struct fleet_app *app)
{
    return &app->game.board[FLEET_SIDE_PLAYER];
}

/* Why a placement was refused, in the player's terms. */
static const char *place_reason(const struct fleet_board *board, enum fleet_ship ship,
                                int row, int col, int vertical, char *buf, size_t n)
{
    int length = fleet_ship_length(ship);
    int i;

    for (i = 0; i < length; i++) {
        int r = row + (vertical ? i : 0);
        int c = col + (vertical ? 0 : i);
        int idx = fleet_index(r, c);
        uint8_t occupant;

        if (idx < 0) {
            snprintf(buf, n, "%s does not fit there", fleet_ship_name(ship));
            return buf;
        }
        occupant = board->ship_at[idx];
        if (occupant != FLEET_NO_SHIP && occupant != (uint8_t)ship) {
            snprintf(buf, n, "Overlaps the %s", fleet_ship_name((enum fleet_ship)occupant));
            return buf;
        }
    }
    snprintf(buf, n, "That position is not available");
    return buf;
}

static void message(struct fleet_deploy_ui *ui, const char *text)
{
    lv_label_set_text(ui->message, text ? text : "");
}

/* Move the selection to the first ship still waiting for a position. */
static void select_next_unplaced(struct fleet_deploy_ui *ui)
{
    const struct fleet_board *board = player_board(ui->app);
    int i;

    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        int candidate = (ui->selected + 1 + i) % FLEET_SHIP_COUNT;

        if (!board->ships[candidate].placed) {
            ui->selected = (uint8_t)candidate;
            return;
        }
    }
}

static void on_cell(void *user, int row, int col)
{
    struct fleet_deploy_ui *ui = user;
    struct fleet_board *board = player_board(ui->app);
    enum fleet_ship ship = (enum fleet_ship)ui->selected;
    char reason[64];

    if (fleet_board_place(board, ship, row, col, ui->vertical ? FLEET_VERTICAL
                                                              : FLEET_HORIZONTAL) != 0) {
        message(ui, place_reason(board, ship, row, col, ui->vertical, reason, sizeof(reason)));
        fleet_screen_deploy_refresh(ui->app);
        return;
    }
    message(ui, "");
    select_next_unplaced(ui);
    fleet_screen_deploy_refresh(ui->app);
}

static void on_roster(lv_event_t *e)
{
    struct fleet_deploy_ui *ui = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_obj_t *panel = lv_obj_get_parent(row);
    uint32_t count = lv_obj_get_child_count(panel);
    uint32_t i;

    for (i = 0; i < count && i < FLEET_SHIP_COUNT; i++) {
        if (lv_obj_get_child(panel, (int32_t)i) == row) {
            ui->selected = (uint8_t)i;
            break;
        }
    }
    message(ui, "");
    fleet_screen_deploy_refresh(ui->app);
}

static void on_rotate(lv_event_t *e)
{
    struct fleet_deploy_ui *ui = lv_event_get_user_data(e);
    struct fleet_board *board = player_board(ui->app);
    const struct fleet_ship_state *s = &board->ships[ui->selected];

    ui->vertical = !ui->vertical;
    if (s->placed &&
        fleet_board_place(board, (enum fleet_ship)ui->selected, s->row, s->col,
                          ui->vertical ? FLEET_VERTICAL : FLEET_HORIZONTAL) != 0) {
        message(ui, "No room to turn it there");
    } else {
        message(ui, "");
    }
    fleet_screen_deploy_refresh(ui->app);
}

static void on_auto(lv_event_t *e)
{
    struct fleet_deploy_ui *ui = lv_event_get_user_data(e);
    struct fleet_board *board = player_board(ui->app);

    fleet_board_clear(board);
    fleet_board_autoplace(board, &ui->rng);
    message(ui, "");
    fleet_screen_deploy_refresh(ui->app);
}

static void on_clear(lv_event_t *e)
{
    struct fleet_deploy_ui *ui = lv_event_get_user_data(e);

    fleet_board_clear(player_board(ui->app));
    ui->selected = 0;
    message(ui, "");
    fleet_screen_deploy_refresh(ui->app);
}

static void on_confirm(lv_event_t *e)
{
    struct fleet_deploy_ui *ui = lv_event_get_user_data(e);

    if (fleet_game_start(&ui->app->game) != 0) {
        message(ui, "Place every ship first");
        return;
    }
    fleet_app_show(ui->app, FLEET_SCREEN_BATTLE);
}

lv_obj_t *fleet_screen_deploy_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_deploy_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *panel;
    lv_obj_t *row;
    lv_obj_t *box;
    int i;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->deploy = ui;
    screen = fleet_app_screen_container(parent);

    ui->grid = fleet_grid_create(screen, FLEET_GRID_DEPLOY, DEPLOY_CELL, 1);
    fleet_grid_bind(ui->grid, player_board(app));
    fleet_grid_set_tap_cb(ui->grid, on_cell, ui);

    panel = fleet_list_panel(screen, "YOUR FLEET");
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        row = fleet_row(panel, ROSTER_ROW, i < FLEET_SHIP_COUNT - 1);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_roster, LV_EVENT_CLICKED, ui);
        ui->name[i] = pocketui_label(row, fleet_ship_name((enum fleet_ship)i),
                                     POS_STYLE_ROW_TITLE);
        ui->state[i] = pocketui_label(row, "PENDING", POS_STYLE_CAPTION);
    }

    box = fleet_hbox(screen, 56, 8);
    fleet_button_paired(box, "TURN", on_rotate, ui);
    fleet_button_paired(box, "AUTO", on_auto, ui);
    fleet_button_paired(box, "CLEAR", on_clear, ui);

    ui->message = pocketui_label(screen, "", POS_STYLE_STATUS_WARN_TEXT);
    lv_label_set_long_mode(ui->message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->message, LV_PCT(100));

    ui->confirm = pocketui_button(screen, "CONFIRM DEPLOYMENT", on_confirm, ui);
    return screen;
}

void fleet_screen_deploy_refresh(struct fleet_app *app)
{
    struct fleet_deploy_ui *ui;
    const struct fleet_board *board;
    int i;

    if (!app || !app->deploy) {
        return;
    }
    ui = app->deploy;
    board = player_board(app);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        int placed = board->ships[i].placed;

        lv_obj_remove_style(ui->name[i], pos_style(POS_STYLE_ACCENT_TEXT), 0);
        if (i == ui->selected) {
            pos_style_add(ui->name[i], POS_STYLE_ACCENT_TEXT, 0);
        }
        lv_obj_remove_style(ui->state[i], pos_style(POS_STYLE_STATUS_OK_TEXT), 0);
        lv_obj_remove_style(ui->state[i], pos_style(POS_STYLE_TEXT_MUTED), 0);
        pos_style_add(ui->state[i], placed ? POS_STYLE_STATUS_OK_TEXT : POS_STYLE_TEXT_MUTED, 0);
        lv_label_set_text(ui->state[i], placed ? "PLACED" : "PENDING");
    }
    /* Outline where the selected ship would go if it is still waiting. */
    if (!board->ships[ui->selected].placed) {
        fleet_grid_clear_preview(ui->grid);
    } else {
        const struct fleet_ship_state *s = &board->ships[ui->selected];

        fleet_grid_set_preview(ui->grid, s->row, s->col,
                               fleet_ship_length((enum fleet_ship)ui->selected),
                               s->orient == FLEET_VERTICAL, 1);
    }
    fleet_grid_refresh(ui->grid);
    fleet_button_set_enabled(ui->confirm, fleet_board_complete(board));
}

/* Called when the screen is entered: a new match means an empty board and a
 * fresh auto-deploy stream derived from the match seed. */
void fleet_screen_deploy_enter(struct fleet_app *app)
{
    struct fleet_deploy_ui *ui = app ? app->deploy : NULL;

    if (!ui) {
        return;
    }
    fleet_rng_seed(&ui->rng, app->game.seed ^ 0x5A5A5A5Au);
    ui->selected = 0;
    ui->vertical = 0;
    message(ui, "");
    fleet_grid_bind(ui->grid, player_board(app));
}
