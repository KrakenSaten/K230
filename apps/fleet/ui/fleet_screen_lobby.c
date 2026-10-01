/*
 * PocketFleet Lobby: multiplayer's front door (docs/apps/FLEET_MULTIPLAYER.md,
 * UX states). The players the mesh has heard, the invitation in hand either
 * way, and the match in progress - with the one or two actions each of those
 * allows.
 *
 * Built like Command and out of the same parts: panels in columns, a foot row
 * with the way back, every object made once and only relabelled or hidden
 * afterwards. Nothing on it transmits by itself; each packet it causes is a
 * button a player pressed (INVITE, CANCEL, ACCEPT, DECLINE, MAKE VISIBLE,
 * RESUME, FORFEIT) or the protocol answering for a match the player is in.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../fleet_app.h"

#include "../link/fleet_session.h"
#include "fleet_view_mp.h"
#include "fleet_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEER_ROWS 5
#define PEER_ROW_H 56

enum lobby_action {
    ACT_NONE = 0,
    ACT_INVITE,
    ACT_CANCEL,
    ACT_ACCEPT,
    ACT_DECLINE,
    ACT_RESUME,
    ACT_FORFEIT,
    ACT_RESULT,
    ACT_DISMISS,
};

struct fleet_lobby_ui {
    struct fleet_app *app;
    lv_obj_t *cols;
    lv_obj_t *left;
    lv_obj_t *right;
    lv_obj_t *foot;
    lv_obj_t *players;
    lv_obj_t *row[PEER_ROWS];
    lv_obj_t *row_label[PEER_ROWS];
    lv_obj_t *empty;
    lv_obj_t *visible;
    lv_obj_t *link_line;
    lv_obj_t *session_line;
    lv_obj_t *act;
    lv_obj_t *alt;
    lv_obj_t *back;
    struct fleet_link_peer peers[PEER_ROWS];
    int count;
    int selected;
    uint8_t act_kind;
    uint8_t alt_kind;
    char visible_note[64];
};

static struct fleet_session *session(struct fleet_lobby_ui *ui)
{
    return ui->app->mp;
}

static void set_button(lv_obj_t *btn, const char *text, int shown)
{
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    if (label && text) {
        lv_label_set_text(label, text);
    }
    if (shown) {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_row(lv_event_t *e)
{
    struct fleet_lobby_ui *ui = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target_obj(e);
    int i;

    for (i = 0; i < PEER_ROWS; i++) {
        if (ui->row[i] == row) {
            ui->selected = i;
        }
    }
    fleet_screen_lobby_refresh(ui->app);
}

static void run(struct fleet_lobby_ui *ui, uint8_t kind)
{
    struct fleet_app *app = ui->app;
    struct fleet_session *s = session(ui);
    int64_t now = fleet_app_now(app);

    if (!s) {
        return;
    }
    if (kind != ACT_FORFEIT) {
        app->mp_forfeit_armed = 0;
    }
    switch (kind) {
    case ACT_INVITE:
        if (ui->selected >= 0 && ui->selected < ui->count) {
            fleet_session_invite(s, ui->peers[ui->selected].key, ui->peers[ui->selected].name, now);
        }
        break;
    case ACT_CANCEL:
        fleet_session_cancel(s, now);
        break;
    case ACT_ACCEPT:
        fleet_session_accept(s, now);
        break;
    case ACT_DECLINE:
        fleet_session_decline(s, now);
        break;
    case ACT_RESUME:
        fleet_app_mp_resume(app);
        return;
    case ACT_RESULT:
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        return;
    case ACT_DISMISS:
        fleet_session_dismiss(s, now);
        break;
    case ACT_FORFEIT:
        /* Two presses, like the power actions: the first arms it. */
        if (!app->mp_forfeit_armed) {
            app->mp_forfeit_armed = 1;
        } else {
            app->mp_forfeit_armed = 0;
            fleet_session_forfeit(s, now);
        }
        break;
    default:
        break;
    }
    fleet_app_mp_changed(app);
    fleet_screen_lobby_refresh(app);
}

static void on_act(lv_event_t *e)
{
    struct fleet_lobby_ui *ui = lv_event_get_user_data(e);

    run(ui, ui->act_kind);
}

static void on_alt(lv_event_t *e)
{
    struct fleet_lobby_ui *ui = lv_event_get_user_data(e);

    run(ui, ui->alt_kind);
}

static void on_visible(lv_event_t *e)
{
    struct fleet_lobby_ui *ui = lv_event_get_user_data(e);
    struct fleet_link *link = ui->app->link;

    if (link && link->ops->advertise(link->ctx) == 0) {
        snprintf(ui->visible_note, sizeof(ui->visible_note),
                 "Announced to devices in direct range.");
    } else {
        snprintf(ui->visible_note, sizeof(ui->visible_note),
                 "Could not announce: the mesh service is not available.");
    }
    fleet_screen_lobby_refresh(ui->app);
}

static void on_back(lv_event_t *e)
{
    struct fleet_app *app = lv_event_get_user_data(e);

    app->mp_forfeit_armed = 0;
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
}

lv_obj_t *fleet_screen_lobby_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_lobby_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *panel;
    int i;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    ui->selected = -1;
    app->lobby = ui;
    screen = fleet_app_screen_container(parent);
    ui->cols = fleet_app_box(screen);
    ui->left = fleet_app_box(ui->cols);
    ui->right = fleet_app_box(ui->cols);
    ui->foot = fleet_app_box(screen);

    ui->players = fleet_list_panel(ui->left, "PLAYERS IN RANGE");
    for (i = 0; i < PEER_ROWS; i++) {
        ui->row[i] = fleet_row(ui->players, PEER_ROW_H, i < PEER_ROWS - 1);
        lv_obj_add_flag(ui->row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ui->row[i], on_row, LV_EVENT_CLICKED, ui);
        ui->row_label[i] = pocketui_label(ui->row[i], "", POS_STYLE_ROW_TITLE);
        lv_label_set_long_mode(ui->row_label[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(ui->row_label[i], LV_PCT(100));
    }
    ui->empty = pocketui_label(ui->players, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->empty, LV_PCT(100));
    /* A list panel has no padding of its own down the page - its rows run
     * edge to edge - so a line of text standing in for the rows brings its
     * own, or it sits on the caption. */
    lv_obj_set_style_pad_ver(ui->empty, 16, 0);
    ui->visible = fleet_button_secondary(ui->left, "MAKE VISIBLE", on_visible, ui);

    panel = fleet_panel(ui->right, "ENGAGEMENT");
    ui->link_line = pocketui_label(panel, "", POS_STYLE_CAPTION);
    lv_label_set_long_mode(ui->link_line, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->link_line, LV_PCT(100));
    ui->session_line = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->session_line, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->session_line, LV_PCT(100));
    ui->act = pocketui_button(ui->right, "INVITE", on_act, ui);
    ui->alt = fleet_button_secondary(ui->right, "DECLINE", on_alt, ui);

    ui->back = fleet_button_secondary(ui->foot, "COMMAND", on_back, app);
    return screen;
}

void fleet_screen_lobby_relayout(struct fleet_app *app, int wide)
{
    struct fleet_lobby_ui *ui = app ? app->lobby : NULL;
    lv_obj_t *col[2];
    int i;

    if (!ui) {
        return;
    }
    /* Command's arrangement: panels across the page in columns that scroll
     * if they must, and the way back on a foot row that never does. */
    fleet_app_screen_flow(app->screen[FLEET_SCREEN_LOBBY], wide, 0);
    fleet_app_box_split(ui->cols, wide);
    fleet_app_box_column(ui->left, wide);
    fleet_app_box_column(ui->right, wide);
    fleet_app_box_split(ui->foot, wide);
    lv_obj_set_flex_grow(ui->foot, 0);
    lv_obj_set_height(ui->foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(ui->foot, wide ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(ui->back, wide ? LV_PCT(33) : LV_PCT(100));
    lv_obj_set_flex_grow(ui->cols, wide ? 1 : 0);
    lv_obj_set_height(ui->cols, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(ui->cols, wide ? 0 : FLEET_CAPTION_RISE, 0);
    col[0] = ui->left;
    col[1] = ui->right;
    for (i = 0; i < 2; i++) {
        lv_obj_set_style_pad_top(col[i], wide ? FLEET_CAPTION_RISE : 0, 0);
        if (wide) {
            lv_obj_add_flag(col[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(col[i], LV_DIR_VER);
            lv_obj_remove_flag(col[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        } else {
            lv_obj_scroll_to_y(col[i], 0, LV_ANIM_OFF);
            lv_obj_remove_flag(col[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(col[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        }
    }
}

static void refresh_players(struct fleet_lobby_ui *ui, int can_choose)
{
    struct fleet_link *link = ui->app->link;
    int64_t now = fleet_app_now(ui->app);
    char text[96];
    int i;

    ui->count = 0;
    if (link && ui->app->mp && ui->app->mp->engaged) {
        ui->count = link->ops->peers(link->ctx, ui->peers, PEER_ROWS);
    }
    if (ui->selected >= ui->count) {
        ui->selected = -1;
    }
    for (i = 0; i < PEER_ROWS; i++) {
        if (i >= ui->count) {
            lv_obj_add_flag(ui->row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(ui->row[i], LV_OBJ_FLAG_HIDDEN);
        fleet_view_mp_peer_row(&ui->peers[i], now, text, sizeof(text));
        lv_label_set_text(ui->row_label[i], text);
        lv_obj_remove_style(ui->row_label[i], pos_style(POS_STYLE_ACCENT_TEXT), 0);
        if (i == ui->selected && can_choose) {
            pos_style_add(ui->row_label[i], POS_STYLE_ACCENT_TEXT, 0);
        }
    }
    if (ui->count == 0) {
        lv_label_set_text(ui->empty, ui->visible_note[0]
                                         ? ui->visible_note
                                         : "No players heard yet. Both devices need Fleet "
                                           "open; MAKE VISIBLE announces this one.");
        lv_obj_remove_flag(ui->empty, LV_OBJ_FLAG_HIDDEN);
    } else if (ui->visible_note[0]) {
        lv_label_set_text(ui->empty, ui->visible_note);
        lv_obj_remove_flag(ui->empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lobby_refresh(struct fleet_app *app)
{
    struct fleet_lobby_ui *ui = app ? app->lobby : NULL;
    struct fleet_session *s;
    const struct fleet_match *m;
    char peer[40];
    char text[160];
    int idle;

    if (!ui) {
        return;
    }
    s = app->mp;
    if (!s) {
        lv_label_set_text(ui->link_line, "Multiplayer is not available on this device.");
        lv_label_set_text(ui->session_line, "");
        set_button(ui->act, NULL, 0);
        set_button(ui->alt, NULL, 0);
        refresh_players(ui, 0);
        return;
    }
    lv_label_set_text(ui->link_line,
                      fleet_link_state_text(app->link->ops->state(app->link->ctx)));
    if (!s->ready) {
        lv_label_set_text(ui->session_line, "");
        set_button(ui->act, NULL, 0);
        set_button(ui->alt, NULL, 0);
        refresh_players(ui, 0);
        return;
    }
    m = &s->m;
    fleet_app_peer(app, peer, sizeof(peer));
    idle = m->phase == FLEET_MP_IDLE;
    refresh_players(ui, idle || m->phase == FLEET_MP_DONE);
    fleet_view_mp_lobby(m, idle && m->notice == FLEET_NOTICE_NONE ? NULL : peer, text, sizeof(text));
    if (m->phase == FLEET_MP_DONE) {
        snprintf(text, sizeof(text), "The engagement with %s is over: %s.", peer,
                 fleet_view_mp_outcome(m));
    }
    if (s->save_failed) {
        size_t len = strlen(text);

        snprintf(text + len, sizeof(text) - len, " Storage is unavailable: this match "
                                                 "cannot be resumed if Fleet closes.");
    }
    lv_label_set_text(ui->session_line, text);

    ui->act_kind = ACT_NONE;
    ui->alt_kind = ACT_NONE;
    switch (m->phase) {
    case FLEET_MP_IDLE:
        ui->act_kind = ACT_INVITE;
        break;
    case FLEET_MP_INVITING:
        ui->act_kind = ACT_CANCEL;
        break;
    case FLEET_MP_INVITED:
        ui->act_kind = ACT_ACCEPT;
        ui->alt_kind = ACT_DECLINE;
        break;
    case FLEET_MP_ACCEPTING:
        break;
    case FLEET_MP_DONE:
        ui->act_kind = ACT_RESULT;
        ui->alt_kind = ACT_DISMISS;
        break;
    default:
        ui->act_kind = ACT_RESUME;
        ui->alt_kind = m->phase <= FLEET_MP_BATTLE ? ACT_FORFEIT : ACT_NONE;
        break;
    }
    {
        static const char *const names[] = {
            "", "INVITE", "CANCEL", "ACCEPT", "DECLINE", "RESUME", "FORFEIT", "RESULT", "CLOSE",
        };

        set_button(ui->act, names[ui->act_kind], ui->act_kind != ACT_NONE);
        set_button(ui->alt, ui->alt_kind == ACT_FORFEIT && app->mp_forfeit_armed
                                ? "CONFIRM FORFEIT" : names[ui->alt_kind],
                   ui->alt_kind != ACT_NONE);
        fleet_button_set_enabled(ui->act, ui->act_kind != ACT_INVITE ||
                                              (ui->selected >= 0 && ui->selected < ui->count));
    }
}

/* Every refresh ends with the type held at Small (fleet_app_hold_type): a
 * refresh may put back a role that carries a font. */
void fleet_screen_lobby_refresh(struct fleet_app *app)
{
    lobby_refresh(app);
    fleet_app_hold_type(app);
}
