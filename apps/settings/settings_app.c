/*
 * Settings: Wi-Fi, display brightness and rotation, and appearance. Everything
 * it decides is in settings_view.c; this builds the panels and turns taps into
 * calls.
 *
 * It owns no hardware and reads nothing from the machine: Wi-Fi is netd's
 * (wifi.* over pocketipc, docs/api/network.md) and brightness is the shell's
 * (pocketos_shell_brightness_*, app.h). Every IPC call carries the UI
 * deadline, and netd answers every request at once, so nothing here waits on
 * a scan or a join - it polls wifi.status once a second and shows what it
 * says.
 *
 * A passphrase exists only in the text field while it is typed and in the
 * one wifi.connect request it is sent with. It is cleared from the field
 * before the sheet closes, never logged, and never shown unless the person
 * taps Show.
 *
 * LAYOUT. Each of the two screens in two shapes, chosen from the body the app
 * is given and chosen again whenever that body changes size (see "the layout"
 * below): the panels one above the other when the body is tall, side by side
 * when it is wide. The orientation is the system's (DS section 21.2); nothing
 * here asks what it is, only how much room there is.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "app.h"
#include "brightness.h"
#include "pocketui.h"
#include "settings_view.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_BTN_H 64
#define SETTINGS_TOGGLE_W 120
#define SETTINGS_STEP_W 96
/* wifi.networks every this many ticks while nothing is changing; every tick
 * while a scan runs. */
#define SETTINGS_LIST_POLL_TICKS 3
/* The Design System has six themes; room for a few more. */
#define SETTINGS_THEMES_MAX 8
/* DS section 7: the gap between panels, whichever way they lie. */
#define SETTINGS_PANEL_GAP 22
/* The portrait body's width on the reference panel (568 less the 20 px side
 * padding of DS section 7). The wide shape is only chosen when each of its
 * two columns keeps at least this much, so no panel is narrower there than
 * in portrait. */
#define SETTINGS_COLUMN_W 528

enum settings_phase {
    PHASE_MAIN = 0,
    PHASE_NETWORK,
};

struct settings_app {
    lv_obj_t *root;
    lv_obj_t *frame;      /* the app's own box in the body; built once */
    lv_obj_t *body;       /* the screen on show, rebuilt with it */
    /* What the layout in force was chosen from, and the one place that
     * decides whether a pass is needed at all (pocketui.h). */
    struct pocketui_layout_guard layout_guard;
    bool wide;
    struct sv_wifi wifi;
    struct sv_brightness bright;
    enum settings_phase phase;
    struct sv_network sel;
    enum sv_join_kind sel_kind;
    int tick;

    /* main phase, repainted in place */
    lv_obj_t *column[2];  /* Wi-Fi | Display and Appearance */
    lv_obj_t *toggle;
    lv_obj_t *toggle_label;
    lv_obj_t *headline;
    lv_obj_t *detail;
    lv_obj_t *store_note;
    lv_obj_t *scan_btn;
    lv_obj_t *disc_btn;
    lv_obj_t *list;
    lv_obj_t *list_note;
    lv_obj_t *bright_value;
    lv_obj_t *bright_down;
    lv_obj_t *bright_up;
    lv_obj_t *bright_note;
    lv_obj_t *theme_chip[SETTINGS_THEMES_MAX];
    lv_obj_t *mode_btn[POS_MODE_COUNT];
    lv_obj_t *rot_btn[SV_ROTATION_MODES];
    lv_obj_t *rot_note;
    struct sv_rotation rot;

    /* network sheet */
    lv_obj_t *sheet;
    lv_obj_t *sheet_side[2]; /* the network described | the field and the actions */
    lv_obj_t *field;
    lv_obj_t *show_label;
    lv_obj_t *sheet_error;

    /* what the list was built from, so it is rebuilt only when it changes */
    char list_sig[SV_MAX_NETWORKS * 120];
    int built_list_visible;
};

static void rebuild(struct settings_app *a);
static void repaint(struct settings_app *a);
static void shape(struct settings_app *a);

/* ---- small builders ---------------------------------------------------------- */

static lv_obj_t *hrow(lv_obj_t *parent, int height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    /* A layout box, not a target: a clickable one would swallow a tap that
     * misses its children and do nothing with it. */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* A box that stacks what is put in it, gap px apart, as tall as its content:
 * a column of panels, or a side of one. Which way it lies, and how wide it
 * is, is the layout's (shape). */
static lv_obj_t *stack(lv_obj_t *parent, int gap)
{
    lv_obj_t *s = lv_obj_create(parent);

    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s, gap, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* A button that does not take focus from a text field when tapped (the
 * keyboard's lesson from M4), with the DS disabled treatment. */
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, int primary)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    pos_style_add(b, POS_STYLE_BUTTON_DISABLED, LV_STATE_DISABLED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_height(b, SETTINGS_BTN_H);
    return b;
}

static void set_enabled(lv_obj_t *obj, int on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void set_tone(lv_obj_t *lb, enum sv_tone tone)
{
    static const enum pos_style_role roles[] = {
        POS_STYLE_TEXT_PRIMARY, POS_STYLE_TEXT_MUTED, POS_STYLE_STATUS_OK_TEXT,
        POS_STYLE_STATUS_WARN_TEXT, POS_STYLE_STATUS_ERROR_TEXT,
    };
    size_t i;

    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        lv_obj_remove_style(lb, pos_style(roles[i]), 0);
    }
    pos_style_add(lb, roles[tone], 0);
}

static void set_hidden(lv_obj_t *obj, int hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- polling ------------------------------------------------------------------- */

static cJSON *call(const char *method, cJSON *params, char *err, size_t n)
{
    err[0] = '\0';
    return shell_ipc_call_timeout("netd", method, params, SHELL_IPC_UI_TIMEOUT_MS, err, n);
}

static void poll_status(struct settings_app *a)
{
    char err[96];
    cJSON *st = call("wifi.status", NULL, err, sizeof(err));

    sv_wifi_apply_status(&a->wifi, st);
    cJSON_Delete(st);
}

static void poll_networks(struct settings_app *a)
{
    char err[96];
    cJSON *res;

    if (!a->wifi.list_visible) {
        return;
    }
    res = call("wifi.networks", NULL, err, sizeof(err));
    sv_wifi_apply_networks(&a->wifi, res);
    cJSON_Delete(res);
}

static void poll_brightness(struct settings_app *a)
{
    sv_brightness_apply(&a->bright, pocketos_shell_brightness_get(), BRIGHTNESS_MIN_PCT,
                        BRIGHTNESS_MAX_PCT);
}

/* ---- Wi-Fi actions --------------------------------------------------------------- */

static void on_toggle(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    cJSON *params = cJSON_CreateObject();
    char err[96];
    cJSON *res;

    cJSON_AddBoolToObject(params, "enabled", !a->wifi.enabled);
    res = call("wifi.set_enabled", params, err, sizeof(err));
    if (res) {
        sv_wifi_apply_status(&a->wifi, res);
        cJSON_Delete(res);
    } else {
        poll_status(a);
    }
    poll_networks(a);
    rebuild(a);
}

static void on_scan(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[96];
    cJSON *res = call("wifi.scan", NULL, err, sizeof(err));

    if (res) {
        a->wifi.scanning = 1;
        a->wifi.can_scan = 0;
        cJSON_Delete(res);
    }
    poll_networks(a);
    repaint(a);
}

static void on_disconnect(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[96];
    cJSON *res;

    pocketos_shell_keyboard_hide();
    res = call("wifi.disconnect", NULL, err, sizeof(err));
    if (res) {
        sv_wifi_apply_status(&a->wifi, res);
        cJSON_Delete(res);
    }
    a->phase = PHASE_MAIN;
    poll_networks(a);
    rebuild(a);
}

static void on_row(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(row);

    if (i < 0 || i >= a->wifi.net_count) {
        return;
    }
    a->sel = a->wifi.nets[i];
    a->sel_kind = sv_join_kind(&a->sel);
    a->phase = PHASE_NETWORK;
    rebuild(a);
}

/* ---- the network sheet --------------------------------------------------------------- */

static void clear_field(struct settings_app *a)
{
    if (a->field) {
        lv_textarea_set_text(a->field, "");
    }
}

static void close_sheet(struct settings_app *a)
{
    clear_field(a);
    pocketos_shell_keyboard_hide();
    explicit_bzero(&a->sel, sizeof(a->sel));
    a->phase = PHASE_MAIN;
    rebuild(a);
}

static void on_cancel(lv_event_t *e)
{
    close_sheet(lv_event_get_user_data(e));
}

/* The field and its error caption, brought into view in the sheet. Above the
 * keyboard in landscape the body is 100 px tall, and a caption that appears
 * under the field lands below it: the sheet scrolls just far enough for the
 * caption to be read, which leaves the field in view with it. Anywhere the
 * sheet already fits, nothing moves. */
static void reveal_field(struct settings_app *a)
{
    if (a->phase != PHASE_NETWORK || !a->field) {
        return;
    }
    lv_obj_update_layout(a->body);
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(a->field), LV_ANIM_OFF);
}

/* The same once the body has changed size. That is learned in the middle of
 * LVGL's layout pass, before the sheet inside has been laid out for the new
 * size, so the field is brought into view straight after the pass instead. */
static void reveal_field_later(void *user)
{
    reveal_field(user);
}

static void show_sheet_error(struct settings_app *a, const char *msg)
{
    if (a->field && a->sel_kind == SV_JOIN_PASSPHRASE) {
        pocketui_text_field_set_error(a->field, msg);
        reveal_field(a);
    } else if (a->sheet_error) {
        lv_label_set_text(a->sheet_error, msg);
        set_hidden(a->sheet_error, 0);
    }
}

static void do_join(struct settings_app *a)
{
    const char *pass = NULL;
    char why[96];
    char err[160];
    cJSON *params;
    cJSON *res;

    if (a->sel_kind == SV_JOIN_PASSPHRASE) {
        pass = a->field ? lv_textarea_get_text(a->field) : NULL;
        if (sv_passphrase_check(pass, why, sizeof(why)) < 0) {
            show_sheet_error(a, why);
            return;
        }
    }
    params = sv_connect_params(&a->sel, a->sel_kind, pass);
    if (!params) {
        return;
    }
    res = call("wifi.connect", params, err, sizeof(err));
    if (!res) {
        show_sheet_error(a, err[0] ? err : "Wi-Fi did not answer");
        return;
    }
    cJSON_Delete(res);
    poll_status(a);
    close_sheet(a);
}

static void on_join(lv_event_t *e)
{
    do_join(lv_event_get_user_data(e));
}

static void on_keyboard_done(void *user)
{
    do_join(user);
}

static void on_forget(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[160];
    cJSON *res = call("wifi.forget", sv_ssid_params(&a->sel), err, sizeof(err));

    if (!res) {
        show_sheet_error(a, err[0] ? err : "Wi-Fi did not answer");
        return;
    }
    cJSON_Delete(res);
    poll_status(a);
    poll_networks(a);
    close_sheet(a);
}

static void on_show(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    bool hidden;

    if (!a->field) {
        return;
    }
    hidden = !lv_textarea_get_password_mode(a->field);
    lv_textarea_set_password_mode(a->field, hidden);
    lv_label_set_text(a->show_label, hidden ? "SHOW" : "HIDE");
}

/* One panel in two sides: what the network is, then the field and what can be
 * done - one above the other when the body is tall, side by side when it is
 * wide (shape_sheet). Tall, the sides are invisible: the panel's own 12 px
 * gap between them is the gap there always was between the text and the
 * field. */
static void build_sheet(struct settings_app *a)
{
    lv_obj_t *p = pocketui_card(a->body);
    lv_obj_t *buttons;
    lv_obj_t *b;
    char body[256];
    int i;

    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_style_pad_column(p, POCKETUI_PAD, 0);
    a->sheet = p;
    for (i = 0; i < 2; i++) {
        a->sheet_side[i] = stack(p, 12);
        /* Layout boxes, not targets: a tap between the controls reaches the
         * panel as it always did, and never takes focus off the field. */
        lv_obj_clear_flag(a->sheet_side[i], LV_OBJ_FLAG_CLICKABLE);
    }
    p = a->sheet_side[0];
    pocketui_label(p, "NETWORK", POS_STYLE_CAPTION);
    b = wrap_label(p, a->sel.ssid, POS_STYLE_TITLE);
    (void)b;
    sv_join_text(&a->sel, body, sizeof(body));
    wrap_label(p, body, a->sel_kind == SV_JOIN_OPEN ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_TEXT_SECONDARY);

    p = a->sheet_side[1];
    if (a->sel_kind == SV_JOIN_PASSPHRASE) {
        lv_obj_t *r;

        a->field = pocketui_text_field(p, "Passphrase", true);
        lv_textarea_set_password_mode(a->field, true);
        lv_textarea_set_max_length(a->field, 63);
        r = hrow(p, SETTINGS_BTN_H);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        b = button(r, "SHOW", on_show, a, 0);
        lv_obj_set_width(b, SETTINGS_TOGGLE_W);
        a->show_label = lv_obj_get_child(b, 0);
    }
    a->sheet_error = wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);
    set_hidden(a->sheet_error, 1);

    buttons = hrow(p, SETTINGS_BTN_H);
    /* Cancel first, and the accent on the action a person came here for -
     * except on an open network, where joining is the one to think twice
     * about (DS §17.5 dialog rule). */
    switch (a->sel_kind) {
    case SV_JOIN_PASSPHRASE:
        b = button(buttons, "CANCEL", on_cancel, a, 0);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "JOIN", on_join, a, 1);
        lv_obj_set_flex_grow(b, 1);
        break;
    case SV_JOIN_SAVED:
        b = button(buttons, "CANCEL", on_cancel, a, 0);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "FORGET", on_forget, a, 0);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "JOIN", on_join, a, 1);
        lv_obj_set_flex_grow(b, 1);
        break;
    case SV_JOIN_OPEN:
        b = button(buttons, "CANCEL", on_cancel, a, 1);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "JOIN ANYWAY", on_join, a, 0);
        lv_obj_set_flex_grow(b, 1);
        break;
    case SV_JOIN_CONNECTED:
        b = button(buttons, "CANCEL", on_cancel, a, 0);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "FORGET", on_forget, a, 0);
        lv_obj_set_flex_grow(b, 1);
        b = button(buttons, "DISCONNECT", on_disconnect, a, 1);
        lv_obj_set_flex_grow(b, 1);
        break;
    case SV_JOIN_UNSUPPORTED:
    default:
        b = button(buttons, "BACK", on_cancel, a, 1);
        lv_obj_set_flex_grow(b, 1);
        break;
    }

    /* Shaped before the field takes focus, so that anything focus scrolls
     * into view is where it will be seen. */
    shape(a);
    if (a->field) {
        pos_input_focus(a->field);
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, on_keyboard_done, a);
    }
}

/* ---- brightness ------------------------------------------------------------------ */

static void brightness_step(struct settings_app *a, int direction)
{
    int want;

    if (!a->bright.supported) {
        return;
    }
    want = sv_brightness_step(a->bright.percent, direction, BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT,
                              BRIGHTNESS_STEP_PCT);
    pocketos_shell_brightness_set(want);
    poll_brightness(a);
    repaint(a);
}

static void on_bright_down(lv_event_t *e)
{
    brightness_step(lv_event_get_user_data(e), -1);
}

static void on_bright_up(lv_event_t *e)
{
    brightness_step(lv_event_get_user_data(e), 1);
}

/* ---- rotation ------------------------------------------------------------------------ */

/* The orientation is the shell's: this only stores a mode through it and
 * shows what the shell says - including that a change takes effect when the
 * Doors shell restarts. Nothing here rotates anything. */
static void poll_rotation(struct settings_app *a)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    sv_rotation_apply(&a->rot, (int)o.mode, o.mode_valid, o.landscape, o.next_landscape, o.applying,
                      o.keyboard == POCKETOS_KEYBOARD_PRESENT);
}

static void on_rotation(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < SV_ROTATION_MODES) {
        pocketos_shell_set_rotation_mode((enum pocketos_rotation_mode)i);
    }
    poll_rotation(a);
    repaint(a);
}

/* ---- appearance ---------------------------------------------------------------------- */

/* The selection is the shell's: it applies it live, stores it and announces
 * it, exactly as shell.theme does. Everything on screen follows through the
 * shared styles; only the selected marks are repainted here. */
static void on_theme(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    const struct pos_theme_def *d = pos_theme_at((int)i);

    if (d) {
        pocketos_shell_set_appearance(d->id, NULL);
    }
    repaint(a);
}

static void on_mode(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < POS_MODE_COUNT) {
        pocketos_shell_set_appearance(NULL, pos_mode_name((enum pos_mode)i));
    }
    repaint(a);
}

static void build_appearance(struct settings_app *a, lv_obj_t *body)
{
    static const char *const mode_labels[POS_MODE_COUNT] = { "NORMAL", "OUTDOOR", "NIGHT" };
    lv_obj_t *p = pocketui_card(body);
    lv_obj_t *r;
    int i;

    lv_obj_set_style_pad_row(p, 8, 0);
    pocketui_label(p, "APPEARANCE", POS_STYLE_CAPTION);
    for (i = 0; i < pos_theme_count() && i < SETTINGS_THEMES_MAX; i++) {
        const struct pos_theme_def *d = pos_theme_at(i);
        lv_obj_t *row = hrow(p, POCKETUI_ROW_H + 8);

        pos_style_add(row, POS_STYLE_DIVIDER, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_theme, LV_EVENT_CLICKED, a);
        pocketui_label(row, d->name, POS_STYLE_ROW_TITLE);
        a->theme_chip[i] = lv_label_create(row);
        pos_style_add(a->theme_chip[i], POS_STYLE_CHIP, 0);
        pos_style_add(a->theme_chip[i], POS_STYLE_CHIP_ACTIVE, 0);
        lv_label_set_text(a->theme_chip[i], "SELECTED");
    }
    pocketui_label(p, "Display mode", POS_STYLE_TEXT_SECONDARY);
    r = hrow(p, SETTINGS_BTN_H);
    for (i = 0; i < POS_MODE_COUNT; i++) {
        a->mode_btn[i] = button(r, mode_labels[i], on_mode, a, 0);
        lv_obj_set_flex_grow(a->mode_btn[i], 1);
        lv_obj_set_user_data(a->mode_btn[i], (void *)(intptr_t)i);
    }
}

static void repaint_appearance(struct settings_app *a)
{
    const struct pos_theme_def *cur = pos_theme_current_def();
    enum pos_mode mode = pos_theme_current_mode();
    int i;

    for (i = 0; i < pos_theme_count() && i < SETTINGS_THEMES_MAX; i++) {
        set_hidden(a->theme_chip[i], !cur || pos_theme_at(i) != cur);
    }
    for (i = 0; i < POS_MODE_COUNT; i++) {
        if (!a->mode_btn[i]) {
            continue;
        }
        /* The current mode is the accented one; the words say it too. */
        lv_obj_remove_style(a->mode_btn[i], pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(a->mode_btn[i], pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
        pos_style_add(a->mode_btn[i], i == (int)mode ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
    }
}

/* ---- the main screen ----------------------------------------------------------------- */

static void build_list(struct settings_app *a)
{
    int i;

    lv_obj_clean(a->list);
    for (i = 0; i < a->wifi.net_count; i++) {
        const struct sv_network *n = &a->wifi.nets[i];
        lv_obj_t *row = lv_obj_create(a->list);
        lv_obj_t *left;
        lv_obj_t *lb;

        lv_obj_remove_style_all(row);
        pos_style_add(row, POS_STYLE_DIVIDER, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, POCKETUI_ROW_H + 8);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);

        left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lb = pocketui_label(left, n->ssid, POS_STYLE_ROW_TITLE);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, LV_PCT(100));
        /* "not supported" is in the words; a muted role would also change
         * the font, and colour must not carry it alone (DS §2). */
        pocketui_label(left, n->detail, POS_STYLE_CAPTION);
        if (n->badge[0]) {
            lb = lv_label_create(row);
            pos_style_add(lb, POS_STYLE_CHIP, 0);
            pos_style_add(lb, n->connected ? POS_STYLE_CHIP_ACTIVE : POS_STYLE_CHIP_OFF, 0);
            lv_label_set_text(lb, n->badge);
        }
    }
}

static void list_signature(const struct settings_app *a, char *out, size_t n)
{
    size_t o = 0;
    int i;

    out[0] = '\0';
    for (i = 0; i < a->wifi.net_count && o < n; i++) {
        const struct sv_network *net = &a->wifi.nets[i];

        o += (size_t)snprintf(out + o, n - o, "%s|%s|%s;", net->ssid_hex, net->detail, net->badge);
    }
}

/* The three panels in two columns: Wi-Fi, whose list is the long one, and
 * then Display and Appearance. Stacked when the body is tall they are the one
 * column there always was, the same gap apart (shape_main). */
static void build_main(struct settings_app *a)
{
    lv_obj_t *p;
    lv_obj_t *r;
    int i;

    for (i = 0; i < 2; i++) {
        a->column[i] = stack(a->body, SETTINGS_PANEL_GAP);
        lv_obj_set_scroll_dir(a->column[i], LV_DIR_VER);
        lv_obj_set_scrollbar_mode(a->column[i], LV_SCROLLBAR_MODE_AUTO);
    }

    /* Wi-Fi */
    p = pocketui_card(a->column[0]);
    lv_obj_set_style_pad_row(p, 8, 0);
    pocketui_label(p, "WI-FI", POS_STYLE_CAPTION);
    r = hrow(p, SETTINGS_BTN_H);
    pocketui_label(r, "Wi-Fi", POS_STYLE_ROW_TITLE);
    a->toggle = button(r, "OFF", on_toggle, a, 0);
    lv_obj_set_width(a->toggle, SETTINGS_TOGGLE_W);
    a->toggle_label = lv_obj_get_child(a->toggle, 0);
    a->headline = wrap_label(p, "", POS_STYLE_TEXT_PRIMARY);
    a->detail = wrap_label(p, "", POS_STYLE_CAPTION);
    a->store_note = wrap_label(p, "", POS_STYLE_STATUS_WARN_TEXT);

    r = hrow(p, SETTINGS_BTN_H);
    a->scan_btn = button(r, "SCAN", on_scan, a, 1);
    lv_obj_set_flex_grow(a->scan_btn, 1);
    a->disc_btn = button(r, "DISCONNECT", on_disconnect, a, 0);
    lv_obj_set_flex_grow(a->disc_btn, 1);
    a->built_list_visible = a->wifi.list_visible;
    set_hidden(r, !a->wifi.list_visible);

    a->list = lv_obj_create(p);
    lv_obj_remove_style_all(a->list);
    lv_obj_set_width(a->list, LV_PCT(100));
    lv_obj_set_height(a->list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(a->list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    set_hidden(a->list, !a->wifi.list_visible);
    list_signature(a, a->list_sig, sizeof(a->list_sig));
    build_list(a);
    a->list_note = wrap_label(p, "", POS_STYLE_TEXT_MUTED);

    /* Display */
    p = pocketui_card(a->column[1]);
    lv_obj_set_style_pad_row(p, 8, 0);
    pocketui_label(p, "DISPLAY", POS_STYLE_CAPTION);
    /* One flat row - title, minus, value, plus - rather than a content-sized
     * box inside it, which LVGL left unlaid-out with the buttons on top of
     * each other (found by tests/settings_app_test.c). */
    r = hrow(p, SETTINGS_BTN_H);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(pocketui_label(r, "Brightness", POS_STYLE_ROW_TITLE), 1);
    a->bright_down = button(r, "-", on_bright_down, a, 0);
    lv_obj_set_width(a->bright_down, SETTINGS_STEP_W);
    a->bright_value = pocketui_label(r, "", POS_STYLE_VALUE);
    lv_obj_set_width(a->bright_value, 88);
    lv_obj_set_style_text_align(a->bright_value, LV_TEXT_ALIGN_CENTER, 0);
    a->bright_up = button(r, "+", on_bright_up, a, 0);
    lv_obj_set_width(a->bright_up, SETTINGS_STEP_W);
    a->bright_note = wrap_label(p, "", POS_STYLE_TEXT_MUTED);

    /* Rotation: three modes, the stored one accented, like the display mode
     * buttons below; the note carries the words (DS §2). */
    pocketui_label(p, "Rotation", POS_STYLE_TEXT_SECONDARY);
    r = hrow(p, SETTINGS_BTN_H);
    {
        static const char *const labels[SV_ROTATION_MODES] = { "AUTOMATIC", "PORTRAIT", "LANDSCAPE" };
        int i;

        for (i = 0; i < SV_ROTATION_MODES; i++) {
            a->rot_btn[i] = button(r, labels[i], on_rotation, a, 0);
            lv_obj_set_flex_grow(a->rot_btn[i], 1);
            lv_obj_set_user_data(a->rot_btn[i], (void *)(intptr_t)i);
        }
    }
    a->rot_note = wrap_label(p, "", POS_STYLE_TEXT_SECONDARY);

    build_appearance(a, a->column[1]);
}

static void repaint(struct settings_app *a)
{
    const struct sv_wifi *w = &a->wifi;
    char sig[sizeof(a->list_sig)];

    if (a->phase != PHASE_MAIN) {
        return;
    }
    if (w->list_visible != a->built_list_visible) {
        rebuild(a);
        return;
    }
    list_signature(a, sig, sizeof(sig));
    if (strcmp(sig, a->list_sig) != 0) {
        memcpy(a->list_sig, sig, sizeof(sig));
        build_list(a);
    }
    lv_label_set_text(a->toggle_label, w->enabled ? "ON" : "OFF");
    lv_obj_remove_style(a->toggle, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(a->toggle, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    pos_style_add(a->toggle, w->enabled ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
    set_enabled(a->toggle, w->toggle_enabled);
    lv_label_set_text(a->headline, w->headline);
    set_tone(a->headline, w->tone);
    lv_label_set_text(a->detail, w->detail);
    set_hidden(a->detail, w->detail[0] == '\0');
    lv_label_set_text(a->store_note, w->store_note);
    set_hidden(a->store_note, w->store_note[0] == '\0');
    set_enabled(a->scan_btn, w->can_scan);
    lv_label_set_text(lv_obj_get_child(a->scan_btn, 0), w->scanning ? "SCANNING" : "SCAN");
    set_enabled(a->disc_btn, w->can_disconnect);
    lv_label_set_text(a->list_note, w->list_visible ? w->list_note : "");
    set_hidden(a->list_note, !w->list_visible || w->list_note[0] == '\0');

    lv_label_set_text(a->bright_value, a->bright.value);
    set_enabled(a->bright_down, a->bright.can_down);
    set_enabled(a->bright_up, a->bright.can_up);
    lv_label_set_text(a->bright_note, a->bright.note);
    set_hidden(a->bright_note, a->bright.note[0] == '\0');
    {
        int i;

        for (i = 0; i < SV_ROTATION_MODES; i++) {
            lv_obj_remove_style(a->rot_btn[i], pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
            lv_obj_remove_style(a->rot_btn[i], pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
            pos_style_add(a->rot_btn[i], i == a->rot.selected ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY,
                          0);
        }
        lv_label_set_text(a->rot_note, a->rot.note);
    }
    repaint_appearance(a);
}

/* ---- the layout ---------------------------------------------------------------------- *
 *
 * The screen on show sits in one frame that is exactly the body's content box
 * - the whole of the room the shell gives the app - and is shaped from the
 * size of that box alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel, 528 x 764 with the
 *   keyboard up). The panels one above the other, the body scrolling: Wi-Fi,
 *   Display, Appearance; the network sheet's text above its field and
 *   buttons. The v0.0.10 layout, except that nothing scrolls into the corners
 *   at the foot (below).
 *
 *   WIDE (landscape: 1192 x 396, and 1192 x 100 with the keyboard up). A
 *   column of panels 1192 px wide is the portrait screen stretched, with a
 *   network list whose names sit a screen's width from their badges. So the
 *   panels go side by side instead, in two columns that share the width with
 *   the DS 7 panel gap between them and each scroll on their own: Wi-Fi on the
 *   left, Display and Appearance on the right. The network sheet is one panel
 *   across the body with the network described on the left and the field and
 *   the buttons on the right - the actions beside the content rather than
 *   under it, as Notes does, because height is what landscape lacks: above
 *   the keyboard the body is 100 px tall, and a field under the text would be
 *   out of sight while it is typed into. Beside the text it is at the top of
 *   the panel, in view with the network's name. Chosen only when each column
 *   keeps SETTINGS_COLUMN_W, so no panel, row or field is narrower than in
 *   portrait. No control's size is taken from the height, so no body can
 *   bring one under 64 px.
 *
 * A screen is built when it is shown, as it always was - the network sheet
 * when a network is tapped, the main screen again when it closes or the list
 * comes or goes - and shaped when it is built. A change of the body's size
 * only shapes: flow, sizes and which box scrolls. The objects, their values,
 * the typed passphrase, the focus and the keyboard are not touched by it.
 *
 * Whatever the shape, nothing is drawn into the panel's unsafe area (DS 21.1,
 * 22.2): scrolled panels pass the foot of the body, which reaches 10 px into
 * the 30 px corner squares of the reference panel, so the frame pads its foot
 * by however far a corner square reaches into the body, from the platform's
 * description (pos_display_rect_insets, the rule Calculator and Notes use).
 * With the keyboard up the foot is far from the corners and the pad is 0; on
 * a panel with square corners it is always 0. */

/* Tall: the body scrolls the one column the two make. Wide: the body lays the
 * two side by side and each scrolls itself. */
static void shape_main(struct settings_app *a)
{
    int i;

    if (!a->column[0] || !a->column[1]) {
        return;
    }
    lv_obj_set_flex_flow(a->body, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    if (a->wide) {
        lv_obj_scroll_to_y(a->body, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(a->body, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_add_flag(a->body, LV_OBJ_FLAG_SCROLLABLE);
    }
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(a->column[i], a->wide ? 1 : 0);
        lv_obj_set_size(a->column[i], LV_PCT(100), a->wide ? LV_PCT(100) : LV_SIZE_CONTENT);
        if (a->wide) {
            lv_obj_add_flag(a->column[i], LV_OBJ_FLAG_SCROLLABLE);
        } else {
            lv_obj_scroll_to_y(a->column[i], 0, LV_ANIM_OFF);
            lv_obj_clear_flag(a->column[i], LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

/* The sheet's two sides: stacked when tall, equal halves side by side when
 * wide, both from the top of the panel. The body scrolls in either shape. */
static void shape_sheet(struct settings_app *a)
{
    int i;

    if (!a->sheet || !a->sheet_side[0] || !a->sheet_side[1]) {
        return;
    }
    lv_obj_set_flex_flow(a->sheet, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(a->sheet_side[i], a->wide ? 1 : 0);
    }
}

static void shape(struct settings_app *a)
{
    if (!a->body) {
        return;
    }
    if (a->phase == PHASE_NETWORK) {
        shape_sheet(a);
    } else {
        shape_main(a);
    }
}

static void layout(struct settings_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;

    /* Nothing to lay out in, or nothing the layout is chosen from has
     * changed: PocketUI owns that decision for every responsive app, and
     * hands back the corner clearance the platform rule gives this box. */
    if (!pocketui_layout_begin(&a->layout_guard, a->frame, &in)) {
        return;
    }
    box = &a->layout_guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    a->wide = w > h && w >= 2 * SETTINGS_COLUMN_W + SETTINGS_PANEL_GAP;
    shape(a);
    if (a->phase == PHASE_NETWORK && a->field) {
        lv_async_call_cancel(reveal_field_later, a);
        lv_async_call(reveal_field_later, a);
    }
}

/* The frame is the body's content box, so this is the body changing size:
 * the keyboard came up or went down, or this is the first layout pass after
 * the app was built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct settings_app *a)
{
    lv_obj_t *frame = lv_obj_create(a->root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the shape itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    a->frame = frame;
}

static void rebuild(struct settings_app *a)
{
    if (a->body) {
        lv_obj_delete(a->body);
    }
    memset(a->column, 0, sizeof(a->column));
    a->sheet = NULL;
    memset(a->sheet_side, 0, sizeof(a->sheet_side));
    a->toggle = a->toggle_label = a->headline = a->detail = a->store_note = NULL;
    a->scan_btn = a->disc_btn = a->list = a->list_note = NULL;
    a->bright_value = a->bright_down = a->bright_up = a->bright_note = NULL;
    a->field = a->show_label = a->sheet_error = NULL;
    memset(a->theme_chip, 0, sizeof(a->theme_chip));
    memset(a->mode_btn, 0, sizeof(a->mode_btn));
    memset(a->rot_btn, 0, sizeof(a->rot_btn));
    a->rot_note = NULL;

    a->body = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->body);
    lv_obj_set_width(a->body, LV_PCT(100));
    lv_obj_set_height(a->body, LV_PCT(100));
    lv_obj_set_flex_flow(a->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->body, SETTINGS_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(a->body, SETTINGS_PANEL_GAP, 0);
    lv_obj_set_scroll_dir(a->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->body, LV_SCROLLBAR_MODE_AUTO);

    if (a->phase == PHASE_NETWORK) {
        build_sheet(a);
    } else {
        build_main(a);
        shape(a);
        repaint(a);
    }
}

/* ---- app lifecycle ------------------------------------------------------------------- */

static void *settings_create(lv_obj_t *root)
{
    struct settings_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->root = root;
    sv_wifi_init(&a->wifi);
    poll_status(a);
    poll_networks(a);
    poll_brightness(a);
    poll_rotation(a);
    build_frame(a);
    rebuild(a);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

static void settings_tick(void *priv)
{
    struct settings_app *a = priv;

    poll_status(a);
    if (a->phase != PHASE_MAIN) {
        return;
    }
    /* An empty list is asked again every second, so networks appear as soon
     * as Wi-Fi has any to report. */
    if (a->wifi.scanning || a->wifi.net_count == 0 || ++a->tick >= SETTINGS_LIST_POLL_TICKS) {
        a->tick = 0;
        poll_networks(a);
    }
    poll_brightness(a);
    poll_rotation(a);
    repaint(a);
}

static void settings_destroy(void *priv)
{
    struct settings_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    lv_async_call_cancel(reveal_field_later, a);
    /* The shell deletes the objects after this; the passphrase goes first. */
    clear_field(a);
    explicit_bzero(&a->sel, sizeof(a->sel));
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_settings);

/* The Back action (app.h `back`, hw_actions.h): an open sheet (join, forget,
 * disconnect) closes as its CANCEL or BACK button closes it, passphrase
 * wiped; the main page is the top level. */
static int settings_back(void *priv)
{
    struct settings_app *a = priv;

    if (!a || a->phase == PHASE_MAIN) {
        return 0;
    }
    close_sheet(a);
    return 1;
}

const struct pocketos_app app_settings = {
    .id = "settings",
    .name = "Settings",
    /* The launcher draws the Doors icon (DS §20). For the text icon: the
     * gear is System's and the list glyph is Timber's; the pencil reads as
     * "change things" without claiming either. */
    .icon = LV_SYMBOL_EDIT,
    .icon_mask = &pos_app_icon_settings,
    .create = settings_create,
    .tick = settings_tick,
    .destroy = settings_destroy,
    .back = settings_back,
};
