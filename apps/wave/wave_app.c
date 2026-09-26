/*
 * Wave: short text messages sent and received as sound (ggwave).
 *
 * One screen. The person picks a preset and stays in it: LISTEN is a toggle
 * that keeps the microphone decoding; SEND plays the typed message with the
 * same preset at any time, pausing the listen for the length of the send and
 * resuming it afterwards (the K230 needs opposite audio routes for the two,
 * so this is still half duplex underneath); CAPTURE records the preset's
 * capture length and decodes the recording afterwards. Everything sent and
 * heard goes into a bounded history that survives a restart.
 *
 * This file is the screen and nothing else. What it shows and allows is
 * wave_view.c; the loop that starts and stops helpers, keeps the history and
 * writes the files is wave_ctl.c; which shape the screen takes and when the
 * touch keyboard comes up is wave_layout.c. The audio is not here at all:
 * each send, listen, capture and decode is one pos-wave helper process,
 * polled from an LVGL timer that only ever makes non-blocking calls. The only
 * wait on the LVGL thread is destroy() giving a running helper
 * WAVE_DESTROY_GRACE_MS to put the device back before it is killed.
 *
 * PRIVACY. While a listen or a capture runs, and until its helper has
 * actually exited, the panel says MICROPHONE ON, the chip says LISTENING or
 * CAPTURING, the status line says "Microphone on", and the header hint reads
 * MIC ON. A listen stops by itself after the preset's listen length, leaving
 * the app stops it, and opening the app never starts one. The history holds
 * what was sent and heard (wave_store.h) and CLEAR removes it; audio is never
 * stored.
 *
 * INPUT. The message is an ordinary single-line text field in the shell's one
 * focus group, so the touch keyboard, a physical keyboard and the simulator's
 * keyboard all type into it the same way (DS 17.4). Enter - the physical key
 * or the touch keyboard's Done - sends. In portrait a tap on the field brings
 * the touch keyboard up; in landscape it does not, and KEYS does (wave_layout.h
 * says why). No button takes focus from the field. A tap on a history entry
 * copies its text into the field, to answer or resend it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_ctl.h"
#include "wave_layout.h"
#include "wave_view.h"

#include "app.h"
#include "pocketui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WAVE_BTN_H 64
#define WAVE_POLL_MS 50
#define WAVE_GAP 12
/* History entries on screen: the newest. The rest stay in the file. */
#define WAVE_ROWS 20
#define WAVE_CLEAR_W 140

struct wave_app {
    struct wave_ctl ctl;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    struct wave_layout layout;
    int landscape;

    lv_obj_t *frame;
    lv_obj_t *top;          /* the history column and the rail */
    lv_obj_t *main;         /* state, status, history */
    lv_obj_t *rail;         /* preset, LISTEN, CAPTURE, CLEAR, counter */
    lv_obj_t *composer;     /* field, SEND, KEYS */

    lv_obj_t *chip;
    lv_obj_t *status;
    lv_obj_t *mic_banner;
    lv_obj_t *list;
    lv_obj_t *empty_note;
    lv_obj_t *row[WAVE_ROWS];
    lv_obj_t *row_meta[WAVE_ROWS];
    lv_obj_t *row_text[WAVE_ROWS];
    unsigned painted_changes;
    int painted_once;

    lv_obj_t *preset_btn;
    lv_obj_t *preset_label;
    lv_obj_t *summary;
    lv_obj_t *listen_btn;
    lv_obj_t *capture_btn;
    lv_obj_t *clear_btn;
    lv_obj_t *counter;

    lv_obj_t *field;
    lv_obj_t *send_btn;
    lv_obj_t *keys_btn;

    const char *hint_shown;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* Wall-clock seconds for history entries, or 0 while the board's clock is not
 * set: the shell's rule for whether it is (app.h), so a history entry is
 * never dated 1970. */
static int64_t wall_s(void)
{
    return pocketos_shell_system_day() >= 0 ? (int64_t)time(NULL) : 0;
}

/* ---- small builders ---------------------------------------------------- */

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

static lv_obj_t *hrow(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent, LV_FLEX_FLOW_ROW, 8);

    lv_obj_set_size(r, LV_PCT(100), WAVE_BTN_H);
    return r;
}

/* Role styles are swapped only when they change: the screen is repainted
 * every poll while a helper runs, and re-adding a style makes LVGL recompute
 * and redraw the object even when nothing is different - work the single
 * hart would do twenty times a second beside the decoder. The role last
 * applied is kept in the object's user data (none of these objects uses it
 * for anything else), offset by one so that 0 means "not yet". */
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
    /* A tap must not take focus from the message field (DS 17.2). */
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_height(b, WAVE_BTN_H);
    return b;
}

static lv_obj_t *label_of(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, 0);
}

static void set_enabled(lv_obj_t *obj, int on)
{
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
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

static void set_tone(lv_obj_t *lb, enum wave_tone tone)
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

/* The chip's look: words first, the colour only repeats them (DS 2). */
static void set_chip(lv_obj_t *chip, enum wave_chip kind)
{
    static const enum pos_style_role roles[] = {
        POS_STYLE_CHIP_OFF, POS_STYLE_CHIP_RX, POS_STYLE_CHIP_TX, POS_STYLE_CHIP_ACTIVE,
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

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* ---- painting ---------------------------------------------------------- */

static const char *message(const struct wave_app *a)
{
    return a->field ? lv_textarea_get_text(a->field) : "";
}

static void paint_history(struct wave_app *a)
{
    const struct wave_history *h = &a->ctl.view.history;
    int64_t today = wall_s();
    int n = wave_history_count(h);
    int i;

    if (a->painted_once && a->painted_changes == h->changes) {
        return;
    }
    a->painted_once = 1;
    a->painted_changes = h->changes;
    for (i = 0; i < WAVE_ROWS; i++) {
        const struct wave_history_entry *e = wave_history_at(h, i);

        if (e) {
            char meta[96];
            char text[2 * WAVE_HISTORY_DATA_MAX + 16];

            wave_view_format_entry(e, today, meta, sizeof(meta), text, sizeof(text));
            set_text(a->row_meta[i], meta);
            set_text(a->row_text[i], text);
            set_tone(a->row_text[i], e->result == WAVE_RESULT_OK ? WAVE_TONE_PRIMARY : WAVE_TONE_MUTED);
        }
        set_hidden(a->row[i], e == NULL);
    }
    set_hidden(a->empty_note, n > 0);
    /* The newest is at the top: show it. */
    lv_obj_scroll_to_y(a->list, 0, LV_ANIM_OFF);
}

static void repaint(struct wave_app *a)
{
    struct wave_view *v = &a->ctl.view;
    int kb = pocketos_shell_keyboard_visible();

    wave_view_refresh(v, message(a), now_ms());

    set_text(a->chip, v->chip);
    set_chip(a->chip, v->chip_kind);
    set_text(a->status, v->status);
    set_tone(a->status, v->status_tone);
    set_hidden(a->mic_banner, !v->mic_on);

    set_text(a->preset_label, v->preset_label);
    set_enabled(a->preset_btn, v->can_pick_preset);
    set_text(a->summary, v->preset_summary);

    set_text(label_of(a->listen_btn), v->listen_label);
    set_primary(a->listen_btn, v->listen_primary);
    set_enabled(a->listen_btn, v->listen_enabled);
    set_text(label_of(a->capture_btn), v->capture_label);
    set_enabled(a->capture_btn, v->capture_enabled);
    set_text(label_of(a->clear_btn), v->clear_label);
    set_enabled(a->clear_btn, v->clear_enabled);

    set_text(a->counter, v->counter);
    set_tone(a->counter, v->counter_tone);

    set_text(label_of(a->send_btn), v->send_label);
    set_primary(a->send_btn, v->send_primary);
    set_enabled(a->send_btn, v->send_enabled);
    set_text(label_of(a->keys_btn), kb ? "HIDE" : "KEYS");

    paint_history(a);

    if (a->hint_shown != v->status_hint) {
        pocketos_shell_set_status_hint(v->status_hint);
        a->hint_shown = v->status_hint;
    }
}

/* ---- layout ------------------------------------------------------------ */

static void shape(struct wave_app *a)
{
    const struct wave_layout *l = &a->layout;
    int wide = l->shape == WAVE_SHAPE_WIDE;

    set_hidden(a->top, !l->show_history);
    set_hidden(a->keys_btn, !l->show_keys);
    lv_obj_set_flex_flow(a->top, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->rail, wide ? WAVE_RAIL_W : LV_PCT(100));
    lv_obj_set_height(a->rail, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    if (wide) {
        lv_obj_set_height(a->main, LV_PCT(100));
        lv_obj_set_width(a->main, 0);
    } else {
        lv_obj_set_width(a->main, LV_PCT(100));
        lv_obj_set_height(a->main, 0);
    }
    lv_obj_set_flex_grow(a->main, 1);
}

static void layout(struct wave_app *a)
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
    wave_layout_choose(&a->layout, (int)w, (int)h, a->landscape,
                       pocketos_shell_keyboard_visible());
    shape(a);
}

/* The frame is the body's content box, so this is the body changing size:
 * the keyboard came up or went down, or this is the first layout pass. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- doing ------------------------------------------------------------- */

static void send_now(struct wave_app *a)
{
    if (wave_ctl_send(&a->ctl, message(a), now_ms(), wall_s()) == 0) {
        /* The text is in the send (and the history); the field is free for
         * the next one, and the keyboard goes so the answer can be seen. */
        lv_textarea_set_text(a->field, "");
        pocketos_shell_keyboard_hide();
    }
    repaint(a);
}

static void on_poll(lv_timer_t *t)
{
    struct wave_app *a = lv_timer_get_user_data(t);
    /* Busy before this poll, not after it: the poll that delivers the exit is
     * the one whose result has to reach the screen. */
    int busy = wave_session_active(&a->ctl.session) || a->ctl.view.phase != WAVE_PHASE_IDLE;
    int events = wave_ctl_poll(&a->ctl, now_ms(), wall_s());

    if (busy || events || a->ctl.view.clear_armed_ms) {
        repaint(a);
    }
}

/* ---- events ------------------------------------------------------------ */

static void on_send(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    if (a->ctl.view.op == WAVE_OP_SEND && a->ctl.view.phase == WAVE_PHASE_RUNNING) {
        wave_ctl_stop(&a->ctl, now_ms(), wall_s());
        repaint(a);
        return;
    }
    send_now(a);
}

static void on_listen(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    wave_ctl_listen(&a->ctl, now_ms(), wall_s());
    repaint(a);
}

static void on_capture(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    wave_ctl_capture(&a->ctl, now_ms(), wall_s());
    repaint(a);
}

static void on_clear(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    wave_ctl_clear(&a->ctl, now_ms());
    repaint(a);
}

static void on_preset(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    wave_ctl_set_preset(&a->ctl, wave_preset_next(a->ctl.view.preset));
    repaint(a);
}

static void on_keys(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    if (wave_layout_keys_shows(&a->layout, pocketos_shell_keyboard_visible())) {
        pos_input_focus(a->field);
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    } else {
        pocketos_shell_keyboard_hide();
    }
    repaint(a);
}

/* A history entry: its text into the field, to answer or send it again. */
static void on_row(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < WAVE_ROWS; i++) {
        const struct wave_history_entry *h;

        if (row != a->row[i]) {
            continue;
        }
        h = wave_history_at(&a->ctl.view.history, i);
        if (h && h->len > 0 && wave_view_check_message(h->data, NULL, (char[8]){ 0 }, 8) == 0) {
            lv_textarea_set_text(a->field, h->data);
            pos_input_focus(a->field);
        }
        break;
    }
    repaint(a);
}

static void on_field_clicked(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    /* Portrait only: in landscape the physical keyboard types here, and KEYS
     * brings the touch keyboard up when there is none (wave_layout.h). */
    if (a->layout.field_tap_shows_keyboard) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    }
}

static void on_field_changed(lv_event_t *e)
{
    repaint(lv_event_get_user_data(e));
}

/* Enter from any source (DS 17.4). */
static void on_field_ready(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    wave_view_refresh(&a->ctl.view, message(a), now_ms());
    if (a->ctl.view.send_enabled && strcmp(a->ctl.view.send_label, "SEND") == 0) {
        send_now(a);
    } else {
        pocketos_shell_keyboard_hide();
    }
}

/* ---- building ---------------------------------------------------------- */

static void build_main(struct wave_app *a)
{
    lv_obj_t *state;
    int i;

    a->main = box(a->top, LV_FLEX_FLOW_COLUMN, 8);

    state = box(a->main, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(state, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(state, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->chip = pocketui_label(state, "READY", POS_STYLE_CHIP);
    a->status = pocketui_label(state, "", POS_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->status, 0);
    lv_obj_set_flex_grow(a->status, 1);

    /* Words, not only a colour (DS 2): the banner says what is on. */
    a->mic_banner = pocketui_label(a->main, "MICROPHONE ON", POS_STYLE_TITLE);
    set_tone(a->mic_banner, WAVE_TONE_WARN);

    pocketui_label(a->main, "HISTORY", POS_STYLE_CAPTION);
    a->list = box(a->main, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_width(a->list, LV_PCT(100));
    lv_obj_set_height(a->list, 0);
    lv_obj_set_flex_grow(a->list, 1);
    /* The one part whose length is unbounded scrolls inside itself; the body
     * never does (DS 17.1). */
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->list, LV_SCROLLBAR_MODE_AUTO);

    a->empty_note = wrap_label(a->list,
                               "Nothing yet. LISTEN to receive, or type below and SEND. "
                               "Tap an entry to copy its text into the message.",
                               POS_STYLE_TEXT_MUTED);
    for (i = 0; i < WAVE_ROWS; i++) {
        lv_obj_t *row = box(a->list, LV_FLEX_FLOW_COLUMN, 4);

        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, WAVE_BTN_H, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        pos_style_add(row, POS_STYLE_SLAB, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);
        a->row_meta[i] = pocketui_label(row, "", POS_STYLE_CAPTION);
        a->row_text[i] = wrap_label(row, "", POS_STYLE_TEXT_PRIMARY);
        a->row[i] = row;
        set_hidden(row, 1);
    }
}

static void build_rail(struct wave_app *a)
{
    lv_obj_t *r;

    a->rail = box(a->top, LV_FLEX_FLOW_COLUMN, 8);

    a->preset_btn = button(a->rail, "", on_preset, a);
    a->preset_label = label_of(a->preset_btn);
    set_primary(a->preset_btn, 0);
    a->summary = pocketui_label(a->rail, "", POS_STYLE_TEXT_MUTED);

    r = hrow(a->rail);
    a->listen_btn = button(r, "LISTEN", on_listen, a);
    a->capture_btn = button(r, "CAPTURE", on_capture, a);
    lv_obj_set_width(a->listen_btn, 0);
    lv_obj_set_width(a->capture_btn, 0);
    lv_obj_set_flex_grow(a->listen_btn, 1);
    lv_obj_set_flex_grow(a->capture_btn, 1);
    set_primary(a->capture_btn, 0);

    r = hrow(a->rail);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->clear_btn = button(r, "CLEAR", on_clear, a);
    lv_obj_set_width(a->clear_btn, WAVE_CLEAR_W);
    set_primary(a->clear_btn, 0);
    a->counter = pocketui_label(r, "", POS_STYLE_TEXT_MUTED);
    lv_obj_set_width(a->counter, 0);
    lv_obj_set_flex_grow(a->counter, 1);
    lv_obj_set_style_text_align(a->counter, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_composer(struct wave_app *a)
{
    lv_obj_t *wrap;

    a->composer = hrow(a->frame);
    lv_obj_set_flex_align(a->composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    a->field = pocketui_text_field(a->composer, "Type a short message", true);
    wrap = lv_obj_get_parent(a->field);
    lv_obj_set_width(wrap, 0);
    lv_obj_set_flex_grow(wrap, 1);
    lv_textarea_set_max_length(a->field, WAVE_MAX_MESSAGE_BYTES);
    lv_obj_add_event_cb(a->field, on_field_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->field, on_field_changed, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->field, on_field_ready, LV_EVENT_READY, a);

    a->send_btn = button(a->composer, "SEND", on_send, a);
    lv_obj_set_width(a->send_btn, WAVE_SEND_W);
    a->keys_btn = button(a->composer, "KEYS", on_keys, a);
    lv_obj_set_width(a->keys_btn, WAVE_KEYS_W);
    set_primary(a->keys_btn, 0);
}

static void build(struct wave_app *a, lv_obj_t *root)
{
    a->frame = box(root, LV_FLEX_FLOW_COLUMN, WAVE_GAP);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives. */
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));

    a->top = box(a->frame, LV_FLEX_FLOW_COLUMN, WAVE_GAP);
    lv_obj_set_width(a->top, LV_PCT(100));
    lv_obj_set_height(a->top, 0);
    lv_obj_set_flex_grow(a->top, 1);
    build_main(a);
    build_rail(a);
    build_composer(a);
}

/* ---- the app ----------------------------------------------------------- */

static void *wave_create(lv_obj_t *root)
{
    struct wave_app *a = calloc(1, sizeof(*a));
    struct pocketos_orientation o;

    if (!a) {
        return NULL;
    }
    pocketos_shell_orientation(&o);
    a->landscape = o.landscape;
    wave_ctl_open(&a->ctl, pocketos_shell_volume_effective);
    /* The hint is only ever written on a change of the app's own, so opening
     * Wave does not wipe whatever the header was showing. */
    wave_view_refresh(&a->ctl.view, "", now_ms());
    a->hint_shown = a->ctl.view.status_hint;
    build(a, root);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    wave_layout_choose(&a->layout, 0, 0, a->landscape, pocketos_shell_keyboard_visible());
    shape(a);
    layout(a);
    a->timer = lv_timer_create(on_poll, WAVE_POLL_MS, a);
    repaint(a);
    return a;
}

static void wave_destroy(void *priv)
{
    struct wave_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    /* The timer is ours; the helper is ours. Leaving the app ends whatever
     * it was doing: the microphone does not outlive the screen that says it
     * is on, and a recording does not outlive the app. */
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    wave_ctl_close(&a->ctl, wall_s());
    pocketos_shell_keyboard_hide();
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_wave);

const struct pocketos_app app_wave = {
    .id = "wave",
    .name = "Wave",
    /* The launcher draws the Doors icon (DS section 20). The glyph stays as
     * the app's text icon: LVGL's symbol font has no microphone or waveform,
     * and the volume glyph is the nearest thing to "sound goes out". */
    .icon = LV_SYMBOL_VOLUME_MAX,
    .icon_mask = &pos_app_icon_wave,
    .create = wave_create,
    .tick = NULL,
    .destroy = wave_destroy,
    /* Fullscreen (DS §30.4 stage 2): no status bar in either orientation,
     * the body from the header down. Whatever this app writes to the hint
     * the shell shows in its header instead. */
    .chrome = POCKETOS_CHROME_NONE,
};
