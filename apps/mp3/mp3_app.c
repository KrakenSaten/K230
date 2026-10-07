/*
 * MP3: local music, played from the device's own storage.
 *
 * One screen. The deck (on top in portrait, at the left in landscape) says
 * what plays - the title and artist from the file's tags, else its name -
 * with its progress, which a tap or a drag moves when the file can seek,
 * then PREV, PLAY/PAUSE and NEXT, STOP and the volume. The list beside it is
 * one folder: the places first (Music, Home, Recordings, removable storage),
 * then folders and audio files. A file plays from the list, and the rest of
 * its folder follows it.
 *
 * This file is the screen and nothing else: what it shows is mp3_view.c,
 * what happens is mp3_ctl.c, a track is mp3_player.c over one pos-mp3
 * helper process (mp3_session.c), and a folder is read on a thread
 * (mp3_library.c). An LVGL timer polls every 50 ms and makes only
 * non-blocking calls. The one wait on the LVGL thread is destroy() giving a
 * playing helper MP3_DESTROY_GRACE_MS to close the device.
 *
 * Leaving the app stops the music: nothing plays without its screen (the
 * audio ownership rule of ADR-010, which pos-mp3 follows). No text is typed
 * anywhere in the app, so the touch keyboard never comes up.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_ctl.h"
#include "mp3_view.h"

#include "app.h"
#include "pocketui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP3_POLL_MS 50
#define MP3_BTN_H 64
#define MP3_GAP 12
#define MP3_GAP_WIDE 8
#define MP3_BAR_H 16
#define MP3_VOL_BTN_W 112
#define MP3_UP_W 96
/* Two columns from this body width up (a landscape body is 1192 wide). */
#define MP3_WIDE_MIN_W 900
#define MP3_DECK_W 560

struct mp3_app {
    struct mp3_ctl ctl;
    struct mp3_view view;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    int wide;

    lv_obj_t *frame;
    lv_obj_t *body;
    lv_obj_t *deck;
    lv_obj_t *side;

    lv_obj_t *chip;
    lv_obj_t *counter;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *seek;
    lv_obj_t *bar;
    lv_obj_t *fill;
    lv_obj_t *elapsed;
    lv_obj_t *total;
    lv_obj_t *prev_btn;
    lv_obj_t *play_btn;
    lv_obj_t *next_btn;
    lv_obj_t *stop_btn;
    lv_obj_t *down_btn;
    lv_obj_t *volume;
    lv_obj_t *up_vol_btn;
    lv_obj_t *status;

    lv_obj_t *up_btn;
    lv_obj_t *caption;
    lv_obj_t *list;
    lv_obj_t *note;
    lv_obj_t *row[MP3_LIST_MAX];
    lv_obj_t *row_title[MP3_LIST_MAX];
    lv_obj_t *row_caption[MP3_LIST_MAX];
    int rows;                        /* rows built so far */

    int dragging;                    /* a finger is on the progress bar */
    int drag_permille;

    unsigned painted_changes;
    const struct mp3_list *painted_list;
    int painted_once;
    const char *hint_shown;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* ---- small builders ------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow, int gap)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Role styles are swapped only when they change: re-adding one makes LVGL
 * recompute and redraw the object even when nothing is different. The role
 * last applied is kept in the object's user data, offset by one. */
static int role_unchanged(lv_obj_t *obj, int role)
{
    if ((intptr_t)lv_obj_get_user_data(obj) == role + 1) {
        return 1;
    }
    lv_obj_set_user_data(obj, (void *)(intptr_t)(role + 1));
    return 0;
}

static void set_primary(lv_obj_t *b, int primary)
{
    if (role_unchanged(b, primary ? 1 : 0)) {
        return;
    }
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (primary) {
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, int primary)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    pos_style_add(b, POS_STYLE_BUTTON_DISABLED, LV_STATE_DISABLED);
    lv_obj_set_height(b, MP3_BTN_H);
    set_primary(b, primary);
    return b;
}

static lv_obj_t *label_of(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, 0);
}

static void set_enabled(lv_obj_t *obj, int on)
{
    if (on == (int)lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        if (on) {
            lv_obj_remove_state(obj, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(obj, LV_STATE_DISABLED);
        }
    }
}

static void set_hidden(lv_obj_t *obj, int hidden)
{
    if (hidden != (int)lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_tone(lv_obj_t *lb, enum mp3_tone tone)
{
    static const enum pos_style_role roles[] = {
        POS_STYLE_TEXT_PRIMARY, POS_STYLE_TEXT_MUTED, POS_STYLE_STATUS_OK_TEXT,
        POS_STYLE_STATUS_WARN_TEXT, POS_STYLE_STATUS_ERROR_TEXT,
    };
    size_t i;

    if (role_unchanged(lb, (int)tone)) {
        return;
    }
    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        lv_obj_remove_style(lb, pos_style(roles[i]), 0);
    }
    pos_style_add(lb, roles[tone], 0);
}

static void set_chip(lv_obj_t *chip, enum mp3_chip kind)
{
    if (role_unchanged(chip, (int)kind)) {
        return;
    }
    lv_obj_remove_style(chip, pos_style(POS_STYLE_CHIP_OFF), 0);
    lv_obj_remove_style(chip, pos_style(POS_STYLE_CHIP_ACTIVE), 0);
    pos_style_add(chip, kind == MP3_CHIP_ACTIVE ? POS_STYLE_CHIP_ACTIVE : POS_STYLE_CHIP_OFF, 0);
}

static void set_selected(lv_obj_t *row, int selected)
{
    if (role_unchanged(row, selected ? 1 : 0)) {
        return;
    }
    if (selected) {
        pos_style_add(row, POS_STYLE_SELECTED, 0);
    } else {
        lv_obj_remove_style(row, pos_style(POS_STYLE_SELECTED), 0);
    }
}

/* One line, cut with dots: a fixed height, or LVGL wraps a long name. */
static lv_obj_t *line_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *l = pocketui_label(parent, text, role);

    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    return l;
}

/* ---- the list ------------------------------------------------------------ */

static void on_row(lv_event_t *e);

static void add_row(struct mp3_app *a)
{
    int i = a->rows;
    lv_obj_t *row = box(a->list, LV_FLEX_FLOW_COLUMN, 2);

    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, MP3_BTN_H, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    pos_style_add(row, POS_STYLE_SLAB, 0);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);
    a->row_title[i] = line_label(row, "", POS_STYLE_TEXT_PRIMARY);
    a->row_caption[i] = line_label(row, "", POS_STYLE_CAPTION);
    a->row[i] = row;
    a->rows++;
}

static void paint_list(struct mp3_app *a)
{
    const struct mp3_list *l = a->ctl.list;
    int n = l ? l->n : 0;
    int i;

    if (a->painted_once && a->painted_changes == a->ctl.changes) {
        return;
    }
    while (a->rows < n) {
        add_row(a);
    }
    for (i = 0; i < a->rows; i++) {
        if (i < n) {
            char title[MP3_NAME_MAX];
            char caption[MP3_PATH_MAX];

            mp3_view_row(&a->ctl, i, title, sizeof(title), caption, sizeof(caption));
            set_text(a->row_title[i], title);
            set_text(a->row_caption[i], caption);
            set_selected(a->row[i], mp3_ctl_row_is_current(&a->ctl, i));
        }
        set_hidden(a->row[i], i >= n);
    }
    if (l != a->painted_list) {
        /* A new folder starts at its top. */
        lv_obj_scroll_to_y(a->list, 0, LV_ANIM_OFF);
        a->painted_list = l;
    }
    a->painted_once = 1;
    a->painted_changes = a->ctl.changes;
}

/* ---- painting ------------------------------------------------------------ */

static void repaint(struct mp3_app *a)
{
    struct mp3_view *v = &a->view;
    int progress;

    mp3_view_refresh(&a->ctl, v);

    set_text(a->chip, v->chip);
    set_chip(a->chip, v->chip_kind);
    set_text(a->counter, v->counter);
    set_text(a->title, v->title);
    set_text(a->subtitle, v->subtitle);

    if (a->dragging && !v->seek_enabled) {
        a->dragging = 0;
    }
    progress = a->dragging ? a->drag_permille : v->progress;
    lv_obj_set_width(a->fill, lv_pct(progress / 10));
    set_hidden(a->fill, progress <= 0);
    if (a->dragging) {
        char t[16];

        mp3_view_time(a->ctl.player.total_ms * a->drag_permille / 1000, t, sizeof(t));
        set_text(a->elapsed, t);
    } else {
        set_text(a->elapsed, v->elapsed);
    }
    set_text(a->total, v->total);

    set_text(label_of(a->play_btn), v->play_label);
    set_enabled(a->play_btn, v->play_enabled);
    set_enabled(a->prev_btn, v->prev_enabled);
    set_enabled(a->next_btn, v->next_enabled);
    set_enabled(a->stop_btn, v->stop_enabled);
    set_text(a->volume, v->volume);
    set_enabled(a->down_btn, v->vol_down_enabled);
    set_enabled(a->up_vol_btn, v->vol_up_enabled);

    set_text(a->status, v->status);
    set_tone(a->status, v->status_tone);

    set_text(a->caption, v->caption);
    set_enabled(a->up_btn, v->up_enabled);
    set_text(a->note, v->note);
    set_hidden(a->note, !v->note[0]);

    paint_list(a);

    if (a->hint_shown != v->hint) {
        pocketos_shell_set_status_hint(v->hint ? v->hint : "");
        a->hint_shown = v->hint;
    }
}

/* ---- layout -------------------------------------------------------------- */

static void shape(struct mp3_app *a)
{
    int gap = a->wide ? MP3_GAP_WIDE : MP3_GAP;

    lv_obj_set_flex_flow(a->body, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->deck, gap, 0);
    if (a->wide) {
        lv_obj_set_size(a->deck, MP3_DECK_W, LV_PCT(100));
        lv_obj_set_height(a->side, LV_PCT(100));
        lv_obj_set_width(a->side, 0);
    } else {
        lv_obj_set_size(a->deck, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_width(a->side, LV_PCT(100));
        lv_obj_set_height(a->side, 0);
    }
    lv_obj_set_flex_grow(a->side, 1);
}

static void layout(struct mp3_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    int32_t w;
    int32_t h;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(area) - in.left - in.right;
    h = lv_area_get_height(area) - in.top - in.bottom;
    /* The room decides the shape, never the orientation itself. */
    a->wide = w > h && w >= MP3_WIDE_MIN_W;
    shape(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- events -------------------------------------------------------------- */

static void on_poll(lv_timer_t *t)
{
    struct mp3_app *a = lv_timer_get_user_data(t);
    /* Busy before the poll: the poll that delivers the exit must paint. */
    int busy = mp3_player_running(&a->ctl.player) || a->ctl.loading;
    int changed = mp3_ctl_poll(&a->ctl, now_ms());

    if (busy || changed) {
        repaint(a);
    }
}

static void on_play(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_play_pause(&a->ctl, now_ms());
    repaint(a);
}

static void on_stop(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_stop(&a->ctl, now_ms());
    repaint(a);
}

static void on_prev(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_prev(&a->ctl, now_ms());
    repaint(a);
}

static void on_next(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_next(&a->ctl, now_ms());
    repaint(a);
}

static void on_vol_down(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_volume_step(&a->ctl, -1, now_ms());
    repaint(a);
}

static void on_vol_up(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_volume_step(&a->ctl, 1, now_ms());
    repaint(a);
}

static void on_up(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);

    mp3_ctl_up(&a->ctl);
    repaint(a);
}

static void on_row(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < a->rows; i++) {
        if (row == a->row[i]) {
            mp3_ctl_open_entry(&a->ctl, i, now_ms());
            break;
        }
    }
    repaint(a);
}

/* The progress bar: a finger on it previews the place, lifting it seeks
 * there. The bar's own area was laid out before the touch; nothing is
 * measured that a layout pass could still move. */
static void on_seek(lv_event_t *e)
{
    struct mp3_app *a = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESS_LOST) {
        a->dragging = 0;
        repaint(a);
        return;
    }
    if (!a->view.seek_enabled) {
        return;
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_area_t bar;
        lv_point_t p;
        int32_t w;

        if (!indev) {
            return;
        }
        lv_indev_get_point(indev, &p);
        lv_obj_get_coords(a->bar, &bar);
        w = lv_area_get_width(&bar);
        if (w <= 0) {
            return;
        }
        a->drag_permille = (int)((int64_t)(p.x - bar.x1) * 1000 / w);
        a->drag_permille = a->drag_permille < 0 ? 0 : a->drag_permille > 1000 ? 1000 : a->drag_permille;
        a->dragging = 1;
        repaint(a);
    } else if (code == LV_EVENT_RELEASED && a->dragging) {
        a->dragging = 0;
        mp3_ctl_seek_permille(&a->ctl, a->drag_permille);
        repaint(a);
    }
}

/* ---- building ------------------------------------------------------------ */

static void build_deck(struct mp3_app *a)
{
    lv_obj_t *r;

    a->deck = box(a->body, LV_FLEX_FLOW_COLUMN, MP3_GAP);

    /* 0: the state and the place in the queue. */
    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->chip = pocketui_label(r, "READY", POS_STYLE_CHIP);
    a->counter = pocketui_label(r, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->counter, 0);
    lv_obj_set_flex_grow(a->counter, 1);
    lv_obj_set_style_text_align(a->counter, LV_TEXT_ALIGN_RIGHT, 0);

    /* 1, 2: the title and the artist. */
    a->title = line_label(a->deck, "", POS_STYLE_TITLE);
    a->subtitle = line_label(a->deck, "", POS_STYLE_TEXT_SECONDARY);

    /* 3: the progress, a touch target as tall as a button around a thin bar.
     * Plain objects, no animation. */
    a->seek = lv_obj_create(a->deck);
    lv_obj_remove_style_all(a->seek);
    lv_obj_set_size(a->seek, LV_PCT(100), MP3_BTN_H);
    lv_obj_clear_flag(a->seek, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->seek, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->seek, on_seek, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->seek, on_seek, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->seek, on_seek, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->seek, on_seek, LV_EVENT_PRESS_LOST, a);
    a->bar = lv_obj_create(a->seek);
    lv_obj_remove_style_all(a->bar);
    pos_style_add(a->bar, POS_STYLE_SLAB, 0);
    lv_obj_set_size(a->bar, LV_PCT(100), MP3_BAR_H);
    lv_obj_align(a->bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(a->bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->fill = lv_obj_create(a->bar);
    lv_obj_remove_style_all(a->fill);
    pos_style_add(a->fill, POS_STYLE_CHIP_ACTIVE, 0);
    lv_obj_set_style_bg_opa(a->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->fill, 6, 0);
    lv_obj_set_size(a->fill, 0, LV_PCT(100));
    lv_obj_clear_flag(a->fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->fill, LV_OBJ_FLAG_HIDDEN);

    /* 4: elapsed and length. */
    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    a->elapsed = pocketui_label(r, "0:00", POS_STYLE_VALUE);
    a->total = pocketui_label(r, "--:--", POS_STYLE_VALUE);
    lv_obj_set_width(a->total, 0);
    lv_obj_set_flex_grow(a->total, 1);
    lv_obj_set_style_text_align(a->total, LV_TEXT_ALIGN_RIGHT, 0);

    /* 5: the transport. */
    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), MP3_BTN_H);
    a->prev_btn = button(r, "PREV", on_prev, a, 0);
    a->play_btn = button(r, "PLAY", on_play, a, 1);
    a->next_btn = button(r, "NEXT", on_next, a, 0);
    lv_obj_set_width(a->prev_btn, 0);
    lv_obj_set_flex_grow(a->prev_btn, 1);
    lv_obj_set_width(a->play_btn, 0);
    lv_obj_set_flex_grow(a->play_btn, 1);
    lv_obj_set_width(a->next_btn, 0);
    lv_obj_set_flex_grow(a->next_btn, 1);

    /* 6: stop and the volume. */
    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), MP3_BTN_H);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->stop_btn = button(r, "STOP", on_stop, a, 0);
    lv_obj_set_width(a->stop_btn, 0);
    lv_obj_set_flex_grow(a->stop_btn, 1);
    a->down_btn = button(r, "VOL -", on_vol_down, a, 0);
    lv_obj_set_width(a->down_btn, MP3_VOL_BTN_W);
    a->volume = pocketui_label(r, "", POS_STYLE_VALUE);
    /* As wide as its widest word, never narrower than it always was: at a
     * larger text size "NO AUDIO" would otherwise wrap out of the row. */
    lv_obj_set_width(a->volume, LV_MAX(104, pocketui_text_width(a->volume, "NO AUDIO")));
    lv_obj_set_style_text_align(a->volume, LV_TEXT_ALIGN_CENTER, 0);
    a->up_vol_btn = button(r, "VOL +", on_vol_up, a, 0);
    lv_obj_set_width(a->up_vol_btn, MP3_VOL_BTN_W);

    /* 7: what happened. */
    a->status = line_label(a->deck, "", POS_STYLE_TEXT_MUTED);
}

static void build_side(struct mp3_app *a)
{
    lv_obj_t *r;

    a->side = box(a->body, LV_FLEX_FLOW_COLUMN, 8);

    r = box(a->side, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), MP3_BTN_H);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->up_btn = button(r, "UP", on_up, a, 0);
    lv_obj_set_width(a->up_btn, MP3_UP_W);
    a->caption = pocketui_label(r, "LIBRARY", POS_STYLE_CAPTION);
    lv_label_set_long_mode(a->caption, LV_LABEL_LONG_DOT);
    lv_obj_set_height(a->caption, lv_font_get_line_height(lv_obj_get_style_text_font(a->caption, 0)));
    lv_obj_set_width(a->caption, 0);
    lv_obj_set_flex_grow(a->caption, 1);

    a->list = box(a->side, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_width(a->list, LV_PCT(100));
    lv_obj_set_height(a->list, 0);
    lv_obj_set_flex_grow(a->list, 1);
    /* The one part whose length is unbounded scrolls inside itself (DS 17.1). */
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->list, LV_SCROLLBAR_MODE_AUTO);

    a->note = pocketui_label(a->list, "", POS_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(a->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->note, LV_PCT(100));
    lv_obj_add_flag(a->note, LV_OBJ_FLAG_HIDDEN);
}

static void build(struct mp3_app *a, lv_obj_t *root)
{
    a->frame = box(root, LV_FLEX_FLOW_COLUMN, MP3_GAP);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    a->body = box(a->frame, LV_FLEX_FLOW_COLUMN, MP3_GAP * 2);
    lv_obj_set_width(a->body, LV_PCT(100));
    lv_obj_set_height(a->body, 0);
    lv_obj_set_flex_grow(a->body, 1);
    build_deck(a);
    build_side(a);
}

/* ---- the app ------------------------------------------------------------- */

static void *mp3_create(lv_obj_t *root)
{
    static const struct mp3_volume_ops vol = {
        .get = pocketos_shell_volume_get,
        .muted = pocketos_shell_volume_muted,
        .available = pocketos_shell_volume_available,
        .set = pocketos_shell_volume_set,
        .set_muted = pocketos_shell_volume_set_muted,
    };
    struct mp3_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    mp3_ctl_open(&a->ctl, &vol, NULL);
    mp3_view_refresh(&a->ctl, &a->view);
    /* The hint is only written on a change of the app's own. */
    a->hint_shown = a->view.hint;
    build(a, root);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    shape(a);
    layout(a);
    a->timer = lv_timer_create(on_poll, MP3_POLL_MS, a);
    repaint(a);
    return a;
}

/* Every callback under obj that was given the app, removed: the shell deletes
 * the objects after destroy() has freed the app, and an event on the way
 * out (a press lost, a size change) must not reach it. */
static void detach(lv_obj_t *obj, void *app)
{
    uint32_t i;

    lv_obj_remove_event_cb_with_user_data(obj, NULL, app);
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        detach(lv_obj_get_child(obj, i), app);
    }
}

static void mp3_destroy(void *priv)
{
    struct mp3_app *a = priv;

    if (!a) {
        return;
    }
    detach(a->frame, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* Leaving the app stops the music; no helper outlives the screen. */
    mp3_ctl_close(&a->ctl);
    if (a->hint_shown) {
        pocketos_shell_set_status_hint("");
    }
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_mp3);

const struct pocketos_app app_mp3 = {
    .id = "mp3",
    .name = "MP3",
    /* The launcher draws the Doors icon (DS section 20); the glyph is the
     * text fallback. */
    .icon = LV_SYMBOL_AUDIO,
    .icon_mask = &pos_app_icon_mp3,
    .create = mp3_create,
    .tick = NULL,
    .destroy = mp3_destroy,
    /* Fullscreen like Wave and Recorder, the other audio apps: the body from
     * the header down, and the PLAYING hint in the header. */
    .chrome = POCKETOS_CHROME_NONE,
};
