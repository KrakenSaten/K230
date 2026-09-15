/*
 * Wave: short text messages sent and received as sound (ggwave).
 *
 * SEND turns a typed message into tones on the speaker; RECEIVE listens on
 * the microphone and shows what it decodes. One or the other, never both:
 * the K230 needs opposite audio routes for them (docs/hardware/
 * AUDIO_HARDWARE_MAP_2026-09-13.md), and half duplex is all a first version
 * needs.
 *
 * This file is the screen. What it shows and allows is wave_view.c, and the
 * audio is not here at all: each send or listen is one pos-wave helper
 * process (wave_session.c), polled from an LVGL timer that only ever makes
 * non-blocking calls. The only wait on the LVGL thread is destroy() giving a
 * running helper WAVE_DESTROY_GRACE_MS to put the device back before it is
 * killed.
 *
 * PRIVACY. While a listen runs, and until its helper has actually exited,
 * the panel says MICROPHONE ON above the received list, the status line says
 * so, and the status bar hint reads MIC ON. A listen stops by itself after
 * WAVE_LISTEN_SECONDS, and leaving the app stops it.
 *
 * INPUT. The message is an ordinary single-line text field in the shell's
 * one focus group, so the touch keyboard, the physical keyboard and the
 * simulator's keyboard all type into it the same way (DS 17.4). Enter - the
 * physical key or the touch keyboard's Done, which is the same key in the
 * stream - transmits when the message is sendable, and otherwise just puts
 * the touch keyboard away. No button takes focus from the field.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_view.h"
#include "wave_session.h"

#include "app.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>

#define WAVE_BTN_H 64
#define WAVE_POLL_MS 50
/* destroy(): time for a running helper to close the device cleanly. */
#define WAVE_DESTROY_GRACE_MS 300

struct wave_app {
    struct wave_view view;
    struct wave_session session;
    lv_timer_t *timer;

    lv_obj_t *mode_btn[2];
    lv_obj_t *status;

    lv_obj_t *send_panel;
    lv_obj_t *field;
    int field_enabled;
    lv_obj_t *counter;
    lv_obj_t *profile_btn[WAVE_PROFILE_COUNT];

    lv_obj_t *recv_panel;
    lv_obj_t *mic_banner;
    lv_obj_t *empty_note;
    lv_obj_t *rx_label[WAVE_VIEW_KEEP];

    lv_obj_t *action;
    lv_obj_t *action_label;

    const char *hint_shown;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* ---- small builders ---------------------------------------------------- */

static lv_obj_t *hrow(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), WAVE_BTN_H);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
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
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
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

static void repaint(struct wave_app *a)
{
    struct wave_view *v = &a->view;
    int i;

    wave_view_refresh(v, message(a), now_ms());

    for (i = 0; i < 2; i++) {
        set_primary(a->mode_btn[i], (int)v->mode == i);
        set_enabled(a->mode_btn[i], v->can_switch_mode || (int)v->mode == i);
    }
    set_hidden(a->send_panel, v->mode != WAVE_MODE_SEND);
    set_hidden(a->recv_panel, v->mode != WAVE_MODE_RECEIVE);

    set_text(a->status, v->status);
    set_tone(a->status, v->status_tone);

    set_text(a->counter, v->counter);
    set_tone(a->counter, v->counter_tone);
    /* Only on a change: enabling re-adds the field to the focus group, and
     * doing that on every poll would move focus around under the typist. */
    if (a->field_enabled != v->can_edit) {
        pocketui_text_field_set_enabled(a->field, v->can_edit);
        a->field_enabled = v->can_edit;
    }
    for (i = 0; i < WAVE_PROFILE_COUNT; i++) {
        set_primary(a->profile_btn[i], (int)v->profile == i);
        set_enabled(a->profile_btn[i], v->can_pick_profile || (int)v->profile == i);
    }

    set_hidden(a->mic_banner, !v->mic_on);
    set_hidden(a->empty_note, v->received_count > 0);
    for (i = 0; i < WAVE_VIEW_KEEP; i++) {
        if (i < v->received_count) {
            set_text(a->rx_label[i], v->received[i].shown);
            set_tone(a->rx_label[i], i == 0 ? WAVE_TONE_PRIMARY : WAVE_TONE_MUTED);
        }
        set_hidden(a->rx_label[i], i >= v->received_count);
    }

    set_text(a->action_label, v->action_label);
    set_primary(a->action, v->action_primary);
    set_enabled(a->action, v->action_enabled);

    if (a->hint_shown != v->status_hint) {
        pocketos_shell_set_status_hint(v->status_hint);
        a->hint_shown = v->status_hint;
    }
}

/* ---- doing ------------------------------------------------------------- */

static void start(struct wave_app *a, enum wave_action what)
{
    char err[160] = "";
    int rc;

    if (what == WAVE_DO_SEND) {
        const char *text = message(a);

        rc = wave_session_start_send(&a->session, wave_session_helper_path(),
                                     wave_view_profile_name(a->view.profile), WAVE_DEFAULT_VOLUME,
                                     text, strlen(text), err, sizeof(err));
    } else {
        rc = wave_session_start_listen(&a->session, wave_session_helper_path(), WAVE_LISTEN_SECONDS,
                                       err, sizeof(err));
    }
    if (rc == 0) {
        wave_view_started(&a->view, now_ms());
    } else {
        wave_view_start_failed(&a->view, err);
    }
}

static void act(struct wave_app *a)
{
    enum wave_action what = wave_view_action(&a->view, message(a));

    switch (what) {
    case WAVE_DO_SEND:
    case WAVE_DO_LISTEN:
        pocketos_shell_keyboard_hide();
        start(a, what);
        break;
    case WAVE_DO_STOP:
        wave_session_stop(&a->session, now_ms());
        wave_view_stopping(&a->view, now_ms());
        break;
    case WAVE_DO_NOTHING:
    default:
        break;
    }
    repaint(a);
}

static void on_poll(lv_timer_t *t)
{
    struct wave_app *a = lv_timer_get_user_data(t);
    struct wave_event ev;
    int64_t now = now_ms();
    /* Busy before this poll, not after it: the poll that delivers the exit is
     * the one whose result has to reach the screen. */
    int busy = wave_session_active(&a->session) || a->view.phase != WAVE_PHASE_IDLE;
    int events = 0;

    while (wave_session_poll(&a->session, &ev, now)) {
        wave_view_apply(&a->view, &ev, now);
        events++;
    }
    if (busy || events) {
        repaint(a);
    }
}

/* ---- events ------------------------------------------------------------ */

static void on_action(lv_event_t *e)
{
    act(lv_event_get_user_data(e));
}

static void on_mode(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    enum wave_mode m = b == a->mode_btn[WAVE_MODE_RECEIVE] ? WAVE_MODE_RECEIVE : WAVE_MODE_SEND;

    if (wave_view_set_mode(&a->view, m) && m == WAVE_MODE_RECEIVE) {
        pocketos_shell_keyboard_hide();
    }
    repaint(a);
}

static void on_profile(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < WAVE_PROFILE_COUNT; i++) {
        if (b == a->profile_btn[i]) {
            wave_view_set_profile(&a->view, (enum wave_profile)i);
        }
    }
    repaint(a);
}

static void on_field_clicked(lv_event_t *e)
{
    struct wave_app *a = lv_event_get_user_data(e);

    if (a->view.can_edit) {
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

    if (wave_view_action(&a->view, message(a)) == WAVE_DO_SEND) {
        act(a);
    } else {
        pocketos_shell_keyboard_hide();
    }
}

/* ---- building ---------------------------------------------------------- */

static void build(struct wave_app *a, lv_obj_t *root)
{
    lv_obj_t *r;
    lv_obj_t *p;
    int i;

    r = hrow(root);
    a->mode_btn[WAVE_MODE_SEND] = button(r, "SEND", on_mode, a);
    a->mode_btn[WAVE_MODE_RECEIVE] = button(r, "RECEIVE", on_mode, a);
    lv_obj_set_flex_grow(a->mode_btn[WAVE_MODE_SEND], 1);
    lv_obj_set_flex_grow(a->mode_btn[WAVE_MODE_RECEIVE], 1);

    p = pocketui_card(root);
    lv_obj_set_style_pad_row(p, 8, 0);
    pocketui_label(p, "STATUS", POS_STYLE_CAPTION);
    a->status = wrap_label(p, "", POS_STYLE_TEXT_MUTED);

    a->send_panel = pocketui_card(root);
    lv_obj_set_style_pad_row(a->send_panel, 12, 0);
    pocketui_label(a->send_panel, "MESSAGE", POS_STYLE_CAPTION);
    a->field = pocketui_text_field(a->send_panel, "Type a short message", true);
    lv_textarea_set_max_length(a->field, WAVE_MAX_MESSAGE_BYTES);
    lv_obj_add_event_cb(a->field, on_field_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->field, on_field_changed, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->field, on_field_ready, LV_EVENT_READY, a);
    a->counter = pocketui_label(a->send_panel, "", POS_STYLE_TEXT_MUTED);
    pocketui_label(a->send_panel, "SPEED", POS_STYLE_CAPTION);
    r = hrow(a->send_panel);
    for (i = 0; i < WAVE_PROFILE_COUNT; i++) {
        a->profile_btn[i] = button(r, wave_view_profile_label((enum wave_profile)i), on_profile, a);
        lv_obj_set_flex_grow(a->profile_btn[i], 1);
    }

    a->recv_panel = pocketui_card(root);
    lv_obj_set_style_pad_row(a->recv_panel, 12, 0);
    /* Words, not only a colour (DS 2): the banner says what is on. */
    a->mic_banner = pocketui_label(a->recv_panel, "MICROPHONE ON", POS_STYLE_TITLE);
    set_tone(a->mic_banner, WAVE_TONE_WARN);
    pocketui_label(a->recv_panel, "RECEIVED", POS_STYLE_CAPTION);
    a->empty_note = wrap_label(a->recv_panel, "Nothing received yet", POS_STYLE_TEXT_MUTED);
    for (i = 0; i < WAVE_VIEW_KEEP; i++) {
        a->rx_label[i] = wrap_label(a->recv_panel, "", POS_STYLE_TEXT_PRIMARY);
    }

    a->action = button(root, "", on_action, a);
    a->action_label = lv_obj_get_child(a->action, 0);
}

/* ---- the app ----------------------------------------------------------- */

static void *wave_create(lv_obj_t *root)
{
    struct wave_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    wave_view_init(&a->view);
    wave_session_init(&a->session);
    /* The hint is only ever written on a change of the app's own, so opening
     * Wave does not wipe whatever the status bar was showing. */
    a->hint_shown = a->view.status_hint;
    build(a, root);
    a->field_enabled = 1; /* pocketui_text_field() creates it enabled */
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
    /* The timer is ours; the helper is ours. Leaving the app ends whatever
     * it was doing: the microphone does not outlive the screen that says it
     * is on. */
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    wave_session_abandon(&a->session, WAVE_DESTROY_GRACE_MS);
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
};
