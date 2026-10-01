/*
 * Recorder: voice notes, field recordings and audio checks, kept as WAV
 * files in the owner's Recordings folder.
 *
 * One screen. The big button records and stops; PAUSE pauses and resumes a
 * recording or a playback; the list below (beside, in landscape) holds the
 * recordings, newest first, and PLAY and DELETE act on the selected one.
 * A timer, a level meter from the real samples, and a status line say what
 * is happening. The preset (VOICE 16 kHz, STANDARD 48 kHz) is remembered.
 *
 * This file is the screen and nothing else: what it shows is rec_view.c,
 * what happens is rec_ctl.c (the state machine in rec_state.c), and the
 * audio is not here at all - each recording, playback and repair is one
 * pos-record helper process, polled every 50 ms from an LVGL timer that only
 * makes non-blocking calls. The waits on the LVGL thread are reading the
 * list (a page per file) after something changed it, and destroy() giving a
 * running helper REC_DESTROY_GRACE_MS to finish its file.
 *
 * PRIVACY. Nothing records until RECORD is pressed. While the microphone may
 * be open the chip says RECORDING, the header hint says MIC ON, and the
 * button says STOP; leaving the app stops and saves the recording. Nothing
 * leaves the device and nothing is logged.
 *
 * No text is typed anywhere in the app, so the touch keyboard never comes up.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_ctl.h"
#include "rec_view.h"

#include "app.h"
#include "pocketui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REC_POLL_MS 50
#define REC_BTN_H 64
#define REC_GAP 12
#define REC_METER_H 24
#define REC_HOLD_W 4
#define REC_PAUSE_W 170
/* Rows on screen: the newest. The rest are in the folder (and in Files). */
#define REC_ROWS 30
/* Two columns from this body width up (a landscape body is 1232 wide). */
#define REC_WIDE_MIN_W 900
#define REC_DECK_W 520

struct rec_app {
    struct rec_ctl ctl;
    struct rec_view view;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    int wide;

    lv_obj_t *frame;
    lv_obj_t *body;
    lv_obj_t *deck;
    lv_obj_t *side;

    lv_obj_t *chip;
    lv_obj_t *status;
    lv_obj_t *clock;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *hold;
    lv_obj_t *meter_text;
    lv_obj_t *space;
    lv_obj_t *main_btn;
    lv_obj_t *pause_btn;
    lv_obj_t *preset_btn;

    lv_obj_t *caption;
    lv_obj_t *list;
    lv_obj_t *empty_note;
    lv_obj_t *row[REC_ROWS];
    lv_obj_t *row_title[REC_ROWS];
    lv_obj_t *row_caption[REC_ROWS];
    lv_obj_t *play_btn;
    lv_obj_t *delete_btn;

    unsigned painted_changes;
    int painted_selected;
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

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    pos_style_add(b, POS_STYLE_BUTTON_DISABLED, LV_STATE_DISABLED);
    lv_obj_set_height(b, REC_BTN_H);
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

static void set_tone(lv_obj_t *lb, enum rec_tone tone)
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

static void set_chip(lv_obj_t *chip, enum rec_chip kind)
{
    static const enum pos_style_role roles[] = {
        POS_STYLE_CHIP_OFF, POS_STYLE_CHIP_TX, POS_STYLE_CHIP_RX, POS_STYLE_CHIP_ACTIVE,
    };
    size_t i;

    if (role_unchanged(chip, (int)kind)) {
        return;
    }
    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        lv_obj_remove_style(chip, pos_style(roles[i]), 0);
    }
    pos_style_add(chip, roles[kind], 0);
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

/* ---- painting ------------------------------------------------------------ */

static void paint_list(struct rec_app *a)
{
    const struct rec_list *l = &a->ctl.list;
    int sel = rec_ctl_selected_index(&a->ctl);
    int i;

    if (a->painted_once && a->painted_changes == a->ctl.changes && a->painted_selected == sel) {
        return;
    }
    if (!a->painted_once || a->painted_changes != a->ctl.changes) {
        for (i = 0; i < REC_ROWS; i++) {
            if (i < l->n) {
                char title[REC_FILE_NAME_MAX];
                char caption[160];

                rec_view_row(&l->e[i], title, sizeof(title), caption, sizeof(caption));
                set_text(a->row_title[i], title);
                set_text(a->row_caption[i], caption);
            }
            set_hidden(a->row[i], i >= l->n);
        }
        set_hidden(a->empty_note, l->n > 0);
    }
    for (i = 0; i < REC_ROWS && i < l->n; i++) {
        set_selected(a->row[i], i == sel);
    }
    a->painted_once = 1;
    a->painted_changes = a->ctl.changes;
    a->painted_selected = sel;
}

static void repaint(struct rec_app *a)
{
    struct rec_view *v = &a->view;

    rec_view_refresh(&a->ctl, now_ms(), v);

    set_text(a->chip, v->chip);
    set_chip(a->chip, v->chip_kind);
    set_text(a->status, v->status);
    set_tone(a->status, v->status_tone);
    set_text(a->clock, v->timer);

    lv_obj_set_width(a->fill, lv_pct(v->meter_pct));
    set_hidden(a->fill, v->meter_pct <= 0);
    set_hidden(a->hold, v->hold_pct <= 0);
    if (v->hold_pct > 0) {
        lv_obj_align(a->hold, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_x(a->hold, (lv_obj_get_content_width(a->track) - REC_HOLD_W) * v->hold_pct / 100);
    }
    set_text(a->meter_text, v->meter_text);
    set_tone(a->meter_text, v->meter_tone);
    set_text(a->space, v->space);

    set_text(label_of(a->main_btn), v->main_label);
    set_primary(a->main_btn, v->main_primary);
    set_enabled(a->main_btn, v->main_enabled);
    set_text(label_of(a->pause_btn), v->pause_label);
    set_enabled(a->pause_btn, v->pause_enabled);
    set_text(label_of(a->preset_btn), v->preset_label);
    set_enabled(a->preset_btn, v->preset_enabled);

    set_text(a->caption, v->list_caption);
    set_text(label_of(a->play_btn), v->play_label);
    set_enabled(a->play_btn, v->play_enabled);
    set_text(label_of(a->delete_btn), v->delete_label);
    set_enabled(a->delete_btn, v->delete_enabled);

    paint_list(a);

    if (a->hint_shown != v->hint) {
        pocketos_shell_set_status_hint(v->hint ? v->hint : "");
        a->hint_shown = v->hint;
    }
}

/* ---- layout -------------------------------------------------------------- */

static void shape(struct rec_app *a)
{
    lv_obj_set_flex_flow(a->body, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    if (a->wide) {
        lv_obj_set_size(a->deck, REC_DECK_W, LV_PCT(100));
        lv_obj_set_height(a->side, LV_PCT(100));
        lv_obj_set_width(a->side, 0);
    } else {
        lv_obj_set_size(a->deck, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_width(a->side, LV_PCT(100));
        lv_obj_set_height(a->side, 0);
    }
    lv_obj_set_flex_grow(a->side, 1);
}

static void layout(struct rec_app *a)
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
    a->wide = w > h && w >= REC_WIDE_MIN_W;
    shape(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- events -------------------------------------------------------------- */

static void on_poll(lv_timer_t *t)
{
    struct rec_app *a = lv_timer_get_user_data(t);
    /* Busy before the poll: the poll that delivers the exit must paint. */
    int busy = rec_machine_busy(&a->ctl.m);
    int events = rec_ctl_poll(&a->ctl, now_ms());

    if (busy || events || rec_ctl_delete_armed(&a->ctl, now_ms())) {
        repaint(a);
    }
}

static void on_main(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);
    int valid = pocketos_shell_system_day() >= 0;

    rec_ctl_record(&a->ctl, now_ms(), valid ? (int64_t)time(NULL) : 0, valid);
    repaint(a);
}

static void on_pause(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);

    rec_ctl_pause(&a->ctl, now_ms());
    repaint(a);
}

static void on_preset(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);

    rec_ctl_next_preset(&a->ctl);
    repaint(a);
}

static void on_play(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);

    rec_ctl_play(&a->ctl, now_ms());
    repaint(a);
}

static void on_delete(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);

    rec_ctl_delete(&a->ctl, now_ms());
    repaint(a);
}

static void on_row(lv_event_t *e)
{
    struct rec_app *a = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < REC_ROWS; i++) {
        if (row == a->row[i]) {
            rec_ctl_select(&a->ctl, i);
            break;
        }
    }
    repaint(a);
}

/* ---- building ------------------------------------------------------------ */

static void build_deck(struct rec_app *a)
{
    lv_obj_t *r;
    lv_obj_t *meter_row;

    a->deck = box(a->body, LV_FLEX_FLOW_COLUMN, REC_GAP);

    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->chip = pocketui_label(r, "READY", POS_STYLE_CHIP);
    a->clock = pocketui_label(r, "0:00", POS_STYLE_HERO_48);
    lv_obj_set_width(a->clock, 0);
    lv_obj_set_flex_grow(a->clock, 1);
    lv_obj_set_style_text_align(a->clock, LV_TEXT_ALIGN_RIGHT, 0);

    /* The meter: a track and a fill proportional to the peak on a dB scale,
     * with the recent peak marked. Plain objects, no animation. */
    a->track = lv_obj_create(a->deck);
    lv_obj_remove_style_all(a->track);
    pos_style_add(a->track, POS_STYLE_SLAB, 0);
    lv_obj_set_size(a->track, LV_PCT(100), REC_METER_H);
    lv_obj_clear_flag(a->track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->fill = lv_obj_create(a->track);
    lv_obj_remove_style_all(a->fill);
    pos_style_add(a->fill, POS_STYLE_CHIP_ACTIVE, 0);
    lv_obj_set_style_bg_opa(a->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->fill, 6, 0);
    lv_obj_set_size(a->fill, 0, LV_PCT(100));
    lv_obj_clear_flag(a->fill, LV_OBJ_FLAG_CLICKABLE);
    a->hold = lv_obj_create(a->track);
    lv_obj_remove_style_all(a->hold);
    pos_style_add(a->hold, POS_STYLE_CHIP_TX, 0);
    lv_obj_set_style_bg_opa(a->hold, LV_OPA_COVER, 0);
    lv_obj_set_size(a->hold, REC_HOLD_W, LV_PCT(100));
    lv_obj_clear_flag(a->hold, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->hold, LV_OBJ_FLAG_HIDDEN);

    meter_row = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(meter_row, LV_PCT(100), LV_SIZE_CONTENT);
    a->meter_text = pocketui_label(meter_row, "-", POS_STYLE_TEXT_MUTED);
    a->space = pocketui_label(meter_row, "", POS_STYLE_TEXT_MUTED);
    lv_obj_set_width(a->space, 0);
    lv_obj_set_flex_grow(a->space, 1);
    lv_obj_set_style_text_align(a->space, LV_TEXT_ALIGN_RIGHT, 0);

    r = box(a->deck, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), REC_BTN_H);
    a->main_btn = button(r, "RECORD", on_main, a);
    lv_obj_set_width(a->main_btn, 0);
    lv_obj_set_flex_grow(a->main_btn, 1);
    a->pause_btn = button(r, "PAUSE", on_pause, a);
    lv_obj_set_width(a->pause_btn, REC_PAUSE_W);
    set_primary(a->pause_btn, 0);

    a->preset_btn = button(a->deck, "", on_preset, a);
    lv_obj_set_width(a->preset_btn, LV_PCT(100));
    set_primary(a->preset_btn, 0);

    a->status = pocketui_label(a->deck, "", POS_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->status, LV_PCT(100));
}

static void build_side(struct rec_app *a)
{
    lv_obj_t *r;
    int i;

    a->side = box(a->body, LV_FLEX_FLOW_COLUMN, 8);
    a->caption = pocketui_label(a->side, "RECORDINGS", POS_STYLE_CAPTION);

    a->list = box(a->side, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_width(a->list, LV_PCT(100));
    lv_obj_set_height(a->list, 0);
    lv_obj_set_flex_grow(a->list, 1);
    /* The one part whose length is unbounded scrolls inside itself (DS 17.1). */
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->list, LV_SCROLLBAR_MODE_AUTO);

    a->empty_note = pocketui_label(a->list, "No recordings yet. They are saved in Recordings, "
                                            "which Files can open.",
                                   POS_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(a->empty_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->empty_note, LV_PCT(100));
    for (i = 0; i < REC_ROWS; i++) {
        lv_obj_t *row = box(a->list, LV_FLEX_FLOW_COLUMN, 4);

        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, REC_BTN_H, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        pos_style_add(row, POS_STYLE_SLAB, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);
        a->row_title[i] = pocketui_label(row, "", POS_STYLE_TEXT_PRIMARY);
        lv_label_set_long_mode(a->row_title[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(a->row_title[i], LV_PCT(100));
        a->row_caption[i] = pocketui_label(row, "", POS_STYLE_CAPTION);
        lv_label_set_long_mode(a->row_caption[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->row_caption[i], LV_PCT(100));
        a->row[i] = row;
        set_hidden(row, 1);
    }

    r = box(a->side, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), REC_BTN_H);
    a->play_btn = button(r, "PLAY", on_play, a);
    lv_obj_set_width(a->play_btn, 0);
    lv_obj_set_flex_grow(a->play_btn, 1);
    set_primary(a->play_btn, 0);
    a->delete_btn = button(r, "DELETE", on_delete, a);
    lv_obj_set_width(a->delete_btn, 0);
    lv_obj_set_flex_grow(a->delete_btn, 1);
    set_primary(a->delete_btn, 0);
}

static void build(struct rec_app *a, lv_obj_t *root)
{
    a->frame = box(root, LV_FLEX_FLOW_COLUMN, REC_GAP);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    a->body = box(a->frame, LV_FLEX_FLOW_COLUMN, REC_GAP * 2);
    lv_obj_set_width(a->body, LV_PCT(100));
    lv_obj_set_height(a->body, 0);
    lv_obj_set_flex_grow(a->body, 1);
    build_deck(a);
    build_side(a);
}

/* ---- the app ------------------------------------------------------------- */

static void *rec_create(lv_obj_t *root)
{
    struct rec_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    rec_ctl_open(&a->ctl, pocketos_shell_volume_effective);
    rec_view_refresh(&a->ctl, now_ms(), &a->view);
    /* The hint is only written on a change of the app's own. */
    a->hint_shown = a->view.hint;
    a->painted_selected = -2;
    build(a, root);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    shape(a);
    layout(a);
    a->timer = lv_timer_create(on_poll, REC_POLL_MS, a);
    repaint(a);
    return a;
}

static void rec_destroy(void *priv)
{
    struct rec_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* Leaving the app ends what it was doing: a recording is stopped and
     * saved, a playback stopped. No helper outlives the screen. */
    rec_ctl_close(&a->ctl);
    if (a->hint_shown) {
        pocketos_shell_set_status_hint("");
    }
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_recorder);

const struct pocketos_app app_recorder = {
    .id = "recorder",
    .name = "Recorder",
    /* The launcher draws the Doors icon (DS section 20); the glyph is the
     * text fallback: LVGL's symbol font has no microphone. */
    .icon = LV_SYMBOL_AUDIO,
    .icon_mask = &pos_app_icon_recorder,
    .create = rec_create,
    .tick = NULL,
    .destroy = rec_destroy,
    /* Fullscreen like Wave: the body from the header down, and the MIC ON
     * hint in the header. */
    .chrome = POCKETOS_CHROME_NONE,
};
