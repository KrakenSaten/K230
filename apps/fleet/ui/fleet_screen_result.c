/*
 * PocketFleet Result screen: how the engagement ended.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_view.h"
#include "fleet_view_mp.h"
#include "fleet_widgets.h"

#include "../link/fleet_session.h"

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
    /* The outcome stays at the top in both shapes. Across the page the two
     * accounts of it stand side by side and the two ways on stand under
     * them, so nothing has to be scrolled to read how it ended. */
    lv_obj_t *panels;
    lv_obj_t *engagement;
    lv_obj_t *gunnery;
    lv_obj_t *foot;
    lv_obj_t *again;
    lv_obj_t *command;
    /* Multiplayer only, last in the foot: the chat outlasts the match until
     * it is put away, for the word after the last shot. */
    lv_obj_t *chat;
    /* Multiplayer only, at the end of ENGAGEMENT so the rows above keep their
     * places: whether the opponent's fleet checked out. In multiplayer the
     * Rounds row says how the match ended instead - Shots fired already
     * counts the rounds. */
    lv_obj_t *verify_row;
    lv_obj_t *verify;
    lv_obj_t *rounds_key;
};

static int multi(struct fleet_app *app)
{
    return app->mode == FLEET_MODE_MULTI && app->mp && app->mp->ready;
}

static void on_again(lv_event_t *e)
{
    struct fleet_app *app = lv_event_get_user_data(e);

    if (multi(app)) {
        if (app->mp->m.phase == FLEET_MP_DONE) {
            fleet_session_dismiss(app->mp, fleet_app_now(app));
        }
        fleet_app_multiplayer(app);
        return;
    }
    fleet_app_new_match(app);
}

static void on_command(lv_event_t *e)
{
    struct fleet_app *app = lv_event_get_user_data(e);

    if (multi(app) && app->mp->m.phase == FLEET_MP_DONE) {
        fleet_session_dismiss(app->mp, fleet_app_now(app));
    }
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
}

static void on_chat(lv_event_t *e)
{
    fleet_app_chat(lv_event_get_user_data(e));
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

    /* DS §3 hero-40, the size the outcome of an engagement calls for. */
    ui->heading = pocketui_label(screen, "", POS_STYLE_HERO_40);
    lv_label_set_long_mode(ui->heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->heading, LV_PCT(100));

    ui->panels = fleet_app_box(screen);
    panel = fleet_list_panel(ui->panels, "ENGAGEMENT");
    ui->engagement = panel;
    ui->opponent = pocketui_kv_row(panel, "Opponent", "-");
    ui->rounds = pocketui_kv_row(panel, "Rounds", "-");
    ui->survivors = pocketui_kv_row(panel, "Your fleet", "-");
    ui->verify = pocketui_kv_row(panel, "Their fleet", "-");
    ui->verify_row = lv_obj_get_parent(ui->verify);
    ui->rounds_key = lv_obj_get_child(lv_obj_get_parent(ui->rounds), 0);

    panel = fleet_list_panel(ui->panels, "GUNNERY");
    ui->gunnery = panel;
    ui->shots = pocketui_kv_row(panel, "Shots fired", "-");
    ui->accuracy = pocketui_kv_row(panel, "Your accuracy", "-");
    ui->enemy_accuracy = pocketui_kv_row(panel, "Enemy accuracy", "-");

    ui->foot = fleet_app_box(screen);
    ui->again = pocketui_button(ui->foot, "NEW ENGAGEMENT", on_again, app);
    ui->command = fleet_button_secondary(ui->foot, "COMMAND", on_command, app);
    ui->chat = fleet_button_secondary(ui->foot, "CHAT", on_chat, app);
    lv_obj_add_flag(ui->chat, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

void fleet_screen_result_relayout(struct fleet_app *app, int wide)
{
    struct fleet_result_ui *ui = app ? app->result : NULL;

    if (!ui) {
        return;
    }
    /* The screen keeps its column: the heading stays over everything. */
    fleet_app_screen_flow(app->screen[FLEET_SCREEN_RESULT], wide, 0);
    fleet_app_box_split(ui->panels, wide);
    /* Side by side, the two panels start on one line whatever their heights
     * (multiplayer's ENGAGEMENT has a row more than GUNNERY). */
    lv_obj_set_flex_align(ui->panels, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    fleet_app_box_split(ui->foot, wide);
    lv_obj_set_flex_grow(ui->panels, wide ? 1 : 0);
    lv_obj_set_height(ui->panels, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->foot, 0);
    lv_obj_set_height(ui->foot, LV_SIZE_CONTENT);
    /* Side by side across the page, each half of what the row holds. */
    lv_obj_set_flex_grow(ui->engagement, wide ? 1 : 0);
    lv_obj_set_flex_grow(ui->gunnery, wide ? 1 : 0);
    lv_obj_set_width(ui->engagement, wide ? LV_PCT(50) : LV_PCT(100));
    lv_obj_set_width(ui->gunnery, wide ? LV_PCT(50) : LV_PCT(100));
    lv_obj_set_flex_grow(ui->again, wide ? 1 : 0);
    lv_obj_set_flex_grow(ui->command, wide ? 1 : 0);
    lv_obj_set_width(ui->again, wide ? LV_PCT(50) : LV_PCT(100));
    lv_obj_set_width(ui->command, wide ? LV_PCT(50) : LV_PCT(100));
    /* Across the page the row's buttons share it equally, two or three. */
    lv_obj_set_flex_grow(ui->chat, wide ? 1 : 0);
    lv_obj_set_width(ui->chat, wide ? LV_PCT(33) : LV_PCT(100));
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
    if (multi(app)) {
        const struct fleet_match *m = &app->mp->m;
        char peer[40];

        fleet_app_peer(app, peer, sizeof(peer));
        lv_label_set_text(ui->heading, fleet_view_mp_outcome(m));
        lv_label_set_text(ui->opponent, peer);
        lv_label_set_text(ui->rounds_key, "Ended");
        lv_label_set_text(ui->rounds, fleet_view_mp_ended(m));
        fleet_view_afloat(&m->own, text, sizeof(text));
        lv_label_set_text(ui->survivors, text);
        lv_label_set_text(ui->verify, fleet_view_mp_verify(m));
        lv_obj_remove_flag(ui->verify_row, LV_OBJ_FLAG_HIDDEN);
        snprintf(text, sizeof(text), "%d", fleet_match_shots_by(m, (enum fleet_mp_role)m->role));
        lv_label_set_text(ui->shots, text);
        fleet_view_mp_accuracy(m, 1, text, sizeof(text));
        lv_label_set_text(ui->accuracy, text);
        fleet_view_mp_accuracy(m, 0, text, sizeof(text));
        lv_label_set_text(ui->enemy_accuracy, text);
        lv_label_set_text(lv_obj_get_child(ui->again, 0), "MULTIPLAYER");
        if (fleet_match_chat_open(m) || m->chat.count) {
            char title[32];
            char preview[FLEET_CHAT_TEXT_MAX + 64];

            fleet_view_mp_chat_tile(m, peer, title, sizeof(title), preview, sizeof(preview));
            lv_label_set_text(lv_obj_get_child(ui->chat, 0), title);
            lv_obj_remove_flag(ui->chat, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->chat, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_add_flag(ui->chat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->verify_row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ui->rounds_key, "Rounds");
    lv_label_set_text(lv_obj_get_child(ui->again, 0), "NEW ENGAGEMENT");

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
