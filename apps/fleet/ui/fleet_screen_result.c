/*
 * PocketFleet Result screen: how the engagement ended.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_view.h"
#include "fleet_widgets.h"

#include <stdio.h>
#include <stdlib.h>

struct fleet_result_ui {
    struct fleet_app *app;
    lv_obj_t *heading;
    lv_obj_t *opponent;
    lv_obj_t *rounds;
    lv_obj_t *shots;
    lv_obj_t *accuracy;
    lv_obj_t *enemy_accuracy;
    lv_obj_t *survivors;
};

static void on_again(lv_event_t *e)
{
    fleet_app_new_match(lv_event_get_user_data(e));
}

static void on_command(lv_event_t *e)
{
    fleet_app_show(lv_event_get_user_data(e), FLEET_SCREEN_COMMAND);
}

lv_obj_t *fleet_screen_result_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_result_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *panel;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->result = ui;
    screen = fleet_app_screen_container(parent);

    /* PocketUI has no hero type role yet (DS §3 hero-40/48 is a later step),
     * so the outcome uses the app-title role. */
    ui->heading = pocketui_label(screen, "", POS_STYLE_TITLE);
    lv_label_set_long_mode(ui->heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->heading, LV_PCT(100));

    panel = fleet_list_panel(screen, "ENGAGEMENT");
    ui->opponent = pocketui_kv_row(panel, "Opponent", "-");
    ui->rounds = pocketui_kv_row(panel, "Rounds", "-");
    ui->survivors = pocketui_kv_row(panel, "Your fleet", "-");

    panel = fleet_list_panel(screen, "GUNNERY");
    ui->shots = pocketui_kv_row(panel, "Shots fired", "-");
    ui->accuracy = pocketui_kv_row(panel, "Your accuracy", "-");
    ui->enemy_accuracy = pocketui_kv_row(panel, "Enemy accuracy", "-");

    pocketui_button(screen, "NEW ENGAGEMENT", on_again, app);
    fleet_button_secondary(screen, "COMMAND", on_command, app);
    return screen;
}

void fleet_screen_result_refresh(struct fleet_app *app)
{
    struct fleet_result_ui *ui;
    const struct fleet_game *game;
    char text[48];

    if (!app || !app->result) {
        return;
    }
    ui = app->result;
    game = &app->game;

    lv_label_set_text(ui->heading, fleet_view_outcome(game));
    lv_label_set_text(ui->opponent,
                      fleet_difficulty_name((enum fleet_difficulty)game->difficulty));
    snprintf(text, sizeof(text), "%u", (unsigned)game->turn);
    lv_label_set_text(ui->rounds, text);
    fleet_view_afloat(&game->board[FLEET_SIDE_PLAYER], text, sizeof(text));
    lv_label_set_text(ui->survivors, text);
    snprintf(text, sizeof(text), "%u", (unsigned)game->stats[FLEET_SIDE_PLAYER].shots);
    lv_label_set_text(ui->shots, text);
    fleet_view_accuracy(&game->stats[FLEET_SIDE_PLAYER], text, sizeof(text));
    lv_label_set_text(ui->accuracy, text);
    fleet_view_accuracy(&game->stats[FLEET_SIDE_OPPONENT], text, sizeof(text));
    lv_label_set_text(ui->enemy_accuracy, text);
}
