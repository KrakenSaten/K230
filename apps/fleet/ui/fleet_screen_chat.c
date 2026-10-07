/*
 * PocketFleet chat: a few short lines with the opponent while the match is
 * open (docs/apps/FLEET_MULTIPLAYER.md, "Chat").
 *
 * Reached from the CHAT button beside FIRE, or from Result, and left with
 * BOARD for whatever screen the match's phase belongs to by then. The match
 * goes on while it is open: the header keeps saying whose turn it is, and so
 * does the first line here.
 *
 *   status        YOUR TURN · SHOT 12
 *   the lines     the last FLEET_CHAT_HISTORY, oldest at the top; the list
 *                 scrolls inside its panel, never the screen
 *   composer      the field, SEND, and KEYS where the body is wider than
 *                 tall (the physical keyboard's shape, DS §21)
 *   BOARD         back to the match
 *
 * The keyboard follows Wave (apps/wave/wave_layout.h): a finger on the field
 * brings the touch keyboard up in a tall body; in a wide one the physical
 * keyboard types here and KEYS brings the touch keyboard up when there is
 * none. Enter sends from either. With the touch keyboard up in a wide body
 * there are 156 px left, so only the composer row is shown.
 *
 * Every object is made once; refreshing only relabels, hides and shows them.
 * The lines are a fixed pool of labels, as many as the history can hold.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../fleet_app.h"

#include "../link/fleet_session.h"
#include "app.h"
#include "fleet_view_mp.h"
#include "fleet_widgets.h"
#include "pos_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEND_W 132
#define KEYS_W 112
#define COMPOSER_GAP 12
/* Below this body height only the composer row is shown (Wave's strip). */
#define CHAT_STRIP_MAX_H 240

struct fleet_chat_ui {
    struct fleet_app *app;
    lv_obj_t *screen;
    lv_obj_t *panel;
    lv_obj_t *status;
    lv_obj_t *list;
    lv_obj_t *line[FLEET_CHAT_HISTORY];
    int8_t line_mine[FLEET_CHAT_HISTORY];       /* the style each label has */
    lv_obj_t *empty;
    lv_obj_t *composer;
    lv_obj_t *field;
    lv_obj_t *send;
    lv_obj_t *keys;
    lv_obj_t *board;
    int field_enabled;
    int send_enabled;
    int keys_shown;             /* KEYS is offered: the body is wider than tall */
    int shown_count;            /* lines on show, and the newest of them, so a */
    int shown_last;             /* new one scrolls the list to it once */
    uint8_t scroll_pending;
    uint8_t focus_pending;
};

static struct fleet_match *match_of(struct fleet_chat_ui *ui)
{
    struct fleet_app *app = ui->app;

    return app->mode == FLEET_MODE_MULTI && app->mp && app->mp->ready ? &app->mp->m : NULL;
}

static const char *field_text(struct fleet_chat_ui *ui)
{
    return ui->field ? lv_textarea_get_text(ui->field) : "";
}

/* Characters somebody could have meant (a text area takes Esc and Tab in as
 * characters before any callback sees them). */
static int has_words(const char *s)
{
    for (; s && *s; s++) {
        if ((unsigned char)*s > 0x20 && (unsigned char)*s != 0x7F) {
            return 1;
        }
    }
    return 0;
}

static void set_field_enabled(struct fleet_chat_ui *ui, int on)
{
    /* Only on a change: re-enabling a field re-adds it to the focus group,
     * which moves the focus off it. */
    if (on != ui->field_enabled) {
        ui->field_enabled = on;
        pocketui_text_field_set_enabled(ui->field, on != 0);
    }
}

static void set_send_enabled(struct fleet_chat_ui *ui, int on)
{
    if (on != ui->send_enabled) {
        ui->send_enabled = on;
        fleet_button_set_enabled(ui->send, on);
    }
}

/* Whether SEND can go, and if not, why - said under the field. */
static void judge(struct fleet_chat_ui *ui)
{
    struct fleet_match *m = match_of(ui);
    const char *text = field_text(ui);
    size_t len = strlen(text);
    char why[80] = "";
    int ok = 0;

    if (!m || !fleet_match_chat_open(m)) {
        snprintf(why, sizeof(why), "The match is over; the chat has closed.");
    } else if (len > FLEET_CHAT_TEXT_MAX) {
        snprintf(why, sizeof(why), "Too long: %zu of %d bytes.", len, FLEET_CHAT_TEXT_MAX);
    } else if (fleet_chat_pending(&m->chat) >= FLEET_CHAT_OUTGOING) {
        snprintf(why, sizeof(why), "Wait: %d lines are still on their way.",
                 FLEET_CHAT_OUTGOING);
    } else {
        ok = has_words(text);
    }
    pocketui_text_field_set_error(ui->field, why[0] ? why : NULL);
    set_send_enabled(ui, ok);
    /* In the focus group only while the chat is on the screen: a hidden field
     * would take the physical keyboard's keys - and its Enter - on Battle. */
    set_field_enabled(ui, m && fleet_match_chat_open(m) &&
                              ui->app->current == FLEET_SCREEN_CHAT);
}

static void send_now(struct fleet_chat_ui *ui)
{
    struct fleet_app *app = ui->app;

    judge(ui);
    if (!ui->send_enabled || !app->mp) {
        return;
    }
    if (fleet_session_chat_send(app->mp, field_text(ui), fleet_app_now(app)) == 0) {
        /* The line is in the history now; the field is free for the next,
         * and the keyboard goes so the answer can be seen. */
        lv_textarea_set_text(ui->field, "");
        if (pocketos_shell_keyboard_visible()) {
            pocketos_shell_keyboard_hide();
        }
    }
    fleet_app_mp_changed(app);
}

static void on_send(lv_event_t *e)
{
    send_now(lv_event_get_user_data(e));
}

static void on_keys(lv_event_t *e)
{
    struct fleet_chat_ui *ui = lv_event_get_user_data(e);

    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    } else {
        ui->focus_pending = 1;
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    }
}

static void on_board(lv_event_t *e)
{
    fleet_app_chat_back(lv_event_get_user_data(e));
}

static void on_field_clicked(lv_event_t *e)
{
    struct fleet_chat_ui *ui = lv_event_get_user_data(e);
    lv_indev_t *src = lv_indev_active();

    /* A finger only: Enter - the touch keyboard's Done included - reaches
     * the focused field as a click too, after it has sent and put the
     * keyboard away, and must not bring it back. */
    if (!src || lv_indev_get_type(src) != LV_INDEV_TYPE_POINTER || ui->keys_shown) {
        return;
    }
    pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
}

static void on_field_changed(lv_event_t *e)
{
    judge(lv_event_get_user_data(e));
}

/* Enter from any source (DS 17.4). */
static void on_field_ready(lv_event_t *e)
{
    struct fleet_chat_ui *ui = lv_event_get_user_data(e);

    judge(ui);
    if (ui->send_enabled) {
        send_now(ui);
    } else if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
}

lv_obj_t *fleet_screen_chat_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_chat_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *wrap;
    int i;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->chat = ui;
    screen = fleet_app_screen_container(parent);
    ui->screen = screen;

    ui->panel = fleet_panel(screen, "CHAT");
    lv_obj_set_width(ui->panel, LV_PCT(100));
    lv_obj_set_flex_flow(ui->panel, LV_FLEX_FLOW_COLUMN);
    ui->status = pocketui_label(ui->panel, "", POS_STYLE_VALUE);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui->status, LV_PCT(100));

    ui->list = lv_obj_create(ui->panel);
    lv_obj_remove_style_all(ui->list);
    lv_obj_set_width(ui->list, LV_PCT(100));
    lv_obj_set_flex_grow(ui->list, 1);
    lv_obj_set_flex_flow(ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->list, 8, 0);
    lv_obj_set_scroll_dir(ui->list, LV_DIR_VER);
    for (i = 0; i < FLEET_CHAT_HISTORY; i++) {
        ui->line[i] = pocketui_label(ui->list, "", POS_STYLE_TEXT_PRIMARY);
        ui->line_mine[i] = 0;
        lv_label_set_long_mode(ui->line[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ui->line[i], LV_PCT(100));
        lv_obj_add_flag(ui->line[i], LV_OBJ_FLAG_HIDDEN);
    }
    ui->empty = pocketui_label(ui->list, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->empty, LV_PCT(100));

    ui->composer = fleet_hbox(screen, POCKETUI_TOUCH_MIN, COMPOSER_GAP);
    lv_obj_set_height(ui->composer, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(ui->composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    ui->field = pocketui_text_field(ui->composer, "A short line", true);
    wrap = lv_obj_get_parent(ui->field);
    lv_obj_set_width(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(wrap, 1);
    /* Characters, not bytes: judge() says when an accented line runs over
     * the byte budget before the character count does. */
    lv_textarea_set_max_length(ui->field, FLEET_CHAT_TEXT_MAX);
    lv_obj_add_event_cb(ui->field, on_field_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->field, on_field_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->field, on_field_ready, LV_EVENT_READY, ui);
    /* Out of the focus group until the chat is shown (judge()). */
    ui->field_enabled = 1;
    set_field_enabled(ui, 0);
    ui->send = pocketui_button(ui->composer, "SEND", on_send, ui);
    lv_obj_set_size(ui->send, SEND_W, POCKETUI_TOUCH_MIN);
    ui->send_enabled = 1;
    set_send_enabled(ui, 0);
    ui->keys = fleet_button_secondary(ui->composer, "KEYS", on_keys, ui);
    lv_obj_set_size(ui->keys, KEYS_W, POCKETUI_TOUCH_MIN);

    ui->board = fleet_button_secondary(screen, "BOARD", on_board, app);
    /* A field joining the focus group is scrolled into view, which lays the
     * frame out while this screen is half built; relayout waits for BOARD,
     * the last thing made, so do now what that pass could not. */
    if (app->layouts > 0) {
        fleet_screen_chat_relayout(app, app->shape == FLEET_SHAPE_WIDE);
    }
    return screen;
}

void fleet_screen_chat_relayout(struct fleet_app *app, int wide)
{
    struct fleet_chat_ui *ui = app ? app->chat : NULL;
    const lv_area_t *box;
    int32_t w;
    int32_t h;
    int strip;

    if (!ui || !ui->board) {
        return;         /* still being built */
    }
    (void)wide;
    box = &app->layout_guard.area;
    w = lv_area_get_width(box);
    h = lv_area_get_height(box);
    /* One column in every shape, as tall as the body: the lines take what the
     * composer and BOARD leave, and scroll inside their panel. */
    fleet_app_screen_flow(ui->screen, 0, 0);
    lv_obj_set_height(ui->screen, LV_PCT(100));
    lv_obj_set_style_pad_top(ui->screen, FLEET_CAPTION_RISE, 0);
    lv_obj_set_flex_grow(ui->panel, 1);
    lv_obj_set_width(ui->composer, LV_PCT(100));
    lv_obj_set_width(ui->board, LV_PCT(100));
    strip = h > 0 && h < CHAT_STRIP_MAX_H;
    if (strip) {
        lv_obj_add_flag(ui->panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->board, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_top(ui->screen, 0, 0);
    } else {
        lv_obj_remove_flag(ui->panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->board, LV_OBJ_FLAG_HIDDEN);
    }
    ui->keys_shown = w > h;
    if (ui->keys_shown) {
        lv_obj_remove_flag(ui->keys, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->keys, LV_OBJ_FLAG_HIDDEN);
    }
    ui->scroll_pending = 1;
}

static void chat_refresh(struct fleet_app *app)
{
    struct fleet_chat_ui *ui = app ? app->chat : NULL;
    struct fleet_match *m;
    char peer[40];
    char text[FLEET_CHAT_TEXT_MAX + 80];
    int last = -1;
    int i;

    if (!ui) {
        return;
    }
    m = match_of(ui);
    if (fleet_app_mp_status(app, text, sizeof(text)) == 0) {
        lv_label_set_text(ui->status, text);
    }
    fleet_app_peer(app, peer, sizeof(peer));
    for (i = 0; i < FLEET_CHAT_HISTORY; i++) {
        const struct fleet_chat_line *l = m && i < m->chat.count ? &m->chat.line[i] : NULL;

        if (!l) {
            lv_obj_add_flag(ui->line[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (ui->line_mine[i] != (int8_t)l->mine) {
            ui->line_mine[i] = (int8_t)l->mine;
            lv_obj_remove_style(ui->line[i], pos_style(POS_STYLE_TEXT_PRIMARY), 0);
            lv_obj_remove_style(ui->line[i], pos_style(POS_STYLE_TEXT_SECONDARY), 0);
            /* Theirs in the text colour, ours a step quieter. */
            pos_style_add(ui->line[i], l->mine ? POS_STYLE_TEXT_SECONDARY : POS_STYLE_TEXT_PRIMARY, 0);
        }
        fleet_view_mp_chat_line(l, peer, text, sizeof(text));
        lv_label_set_text(ui->line[i], text);
        lv_obj_remove_flag(ui->line[i], LV_OBJ_FLAG_HIDDEN);
        last = l->id | (l->mine ? 0x100 : 0);
    }
    if (m && m->chat.count) {
        lv_obj_add_flag(ui->empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(text, sizeof(text),
                 "Nothing said yet. Lines are short - %d bytes - and go when the game can "
                 "spare the air.", FLEET_CHAT_TEXT_MAX);
        lv_label_set_text(ui->empty, text);
        lv_obj_remove_flag(ui->empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (m && ((int)m->chat.count != ui->shown_count || last != ui->shown_last)) {
        ui->shown_count = m->chat.count;
        ui->shown_last = last;
        ui->scroll_pending = 1;
    }
    lv_label_set_text(lv_obj_get_child(ui->keys, 0),
                      pocketos_shell_keyboard_visible() ? "HIDE" : "KEYS");
    judge(ui);
    /* On the screen, every line has been seen. */
    if (m && app->current == FLEET_SCREEN_CHAT) {
        fleet_match_chat_seen(m);
    }
}

void fleet_screen_chat_tick(struct fleet_app *app)
{
    struct fleet_chat_ui *ui = app ? app->chat : NULL;
    int i;

    if (!ui) {
        return;
    }
    /* Outside any LVGL event, so the focus sticks and the layout measured
     * is a finished one (DS: never measure or move focus inside an event). */
    if (ui->focus_pending) {
        ui->focus_pending = 0;
        if (ui->field_enabled) {
            pos_input_focus(ui->field);
        }
    }
    if (ui->scroll_pending) {
        ui->scroll_pending = 0;
        lv_obj_update_layout(ui->list);
        for (i = FLEET_CHAT_HISTORY - 1; i >= 0; i--) {
            if (!lv_obj_has_flag(ui->line[i], LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_scroll_to_view(ui->line[i], LV_ANIM_OFF);
                break;
            }
        }
    }
}

void fleet_screen_chat_leave(struct fleet_app *app)
{
    struct fleet_chat_ui *ui = app ? app->chat : NULL;

    if (!ui) {
        return;
    }
    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    /* Out of the focus group with the screen. A half-typed line stays in the
     * field for when the chat is opened again. */
    set_field_enabled(ui, 0);
    ui->focus_pending = 0;
}

void fleet_screen_chat_enter(struct fleet_app *app)
{
    if (app && app->chat) {
        app->chat->focus_pending = 1;
        app->chat->scroll_pending = 1;
    }
}

/* Every refresh ends with the type held at Small (fleet_app_hold_type): a
 * refresh may put back a role that carries a font. */
void fleet_screen_chat_refresh(struct fleet_app *app)
{
    chat_refresh(app);
    fleet_app_hold_type(app);
}

lv_obj_t *fleet_screen_chat_lines(struct fleet_app *app)
{
    return app && app->chat ? app->chat->list : NULL;
}
