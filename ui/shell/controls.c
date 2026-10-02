/*
 * DOORS Controls. See controls.h; the decisions are controls_model.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "controls.h"

#include "app.h"
#include "brightness.h"
#include "controls_model.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_glyphs.h"
#include "shell_ipc.h"
#include "volume.h"

#include <stdio.h>
#include <string.h>

#define WIFI_POLL_TICKS 2
/* system.status carries storage and network too, so Bluetooth and the
 * battery are asked for less often than the rest. */
#define SYSD_POLL_TICKS 5
/* The one Controls call with a longer deadline than the UI's 200 ms: the
 * radio switch, made only on a tap (and, to switch on, a confirmed one).
 * Switching the SX1262 on is its whole start-up path - power, init, the
 * profile, receive - and a reply cut at 200 ms would report a failure for a
 * radio that is coming up. Bounded all the same. */
#define RADIO_SWITCH_TIMEOUT_MS 2000
/* After a switch the reply's state is shown until the status bar's own poll
 * has caught up with it, so the tile does not flick back for a second. */
#define RADIO_HOLD_TICKS 2

static struct {
    lv_obj_t *root;
    bool landscape;
    struct controls_actions actions;
    struct controls_layout layout;
    lv_obj_t *value[CONTROLS_TILE_COUNT];
    lv_obj_t *dot[CONTROLS_TILE_COUNT];
    lv_obj_t *slider;
    lv_obj_t *slider_value;
    lv_obj_t *volume;
    lv_obj_t *volume_value;
    lv_obj_t *volume_mute;
    lv_obj_t *dialog;
    struct controls_radio_flow radio_flow;
    char radio_hold[16];
    int radio_hold_ticks;
    int radio_failed_ticks; /* a switch radiod refused or did not answer */
    int tick;
    int sysd_tick;
} ct;

/* ---- building blocks ------------------------------------------------------ */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *glyph(lv_obj_t *parent, const lv_image_dsc_t *g)
{
    lv_obj_t *o = lv_image_create(parent);

    lv_obj_remove_style_all(o);
    pos_style_add(o, POS_STYLE_ENV_GLYPH, 0);
    lv_image_set_src(o, g);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, enum pos_style_role role, const char *s)
{
    lv_obj_t *o = lv_label_create(parent);

    pos_style_add(o, role, 0);
    lv_label_set_text(o, s);
    return o;
}

static lv_obj_t *glass(lv_obj_t *parent, const struct controls_rect *r, bool pressable)
{
    lv_obj_t *o = plain(parent);

    pos_style_add(o, POS_STYLE_ENV_PANEL, 0);
    if (pressable) {
        pos_style_add(o, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    return o;
}

/* ---- what each tile says ------------------------------------------------- */

static void set_text(lv_obj_t *label, const char *s)
{
    if (label && strcmp(lv_label_get_text(label), s) != 0) {
        lv_label_set_text(label, s);
    }
}

static void set_dot(enum controls_tile t, bool on)
{
    if (on) {
        lv_obj_remove_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
    }
}

/* The radio's state: the status bar's poll, or for a moment after a switch
 * the state radiod answered the switch with. */
static const char *radio_state(void)
{
    if (ct.radio_hold_ticks > 0) {
        return ct.radio_hold;
    }
    return pocketos_shell_radio_state();
}

static void refresh_radio(void)
{
    const char *s = radio_state();

    if (ct.radio_failed_ticks > 0) {
        set_text(ct.value[CONTROLS_TILE_RADIO], "Could not switch");
        set_dot(CONTROLS_TILE_RADIO, false);
        return;
    }
    set_text(ct.value[CONTROLS_TILE_RADIO], controls_radio_text(s));
    set_dot(CONTROLS_TILE_RADIO, controls_radio_dot(s));
}

static void refresh_wifi(void)
{
    char err[96];
    char line[48];
    cJSON *st = shell_ipc_call_timeout("netd", "wifi.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    const cJSON *state = st ? cJSON_GetObjectItemCaseSensitive(st, "state") : NULL;
    const cJSON *ssid = st ? cJSON_GetObjectItemCaseSensitive(st, "ssid") : NULL;
    const char *s = cJSON_IsString(state) ? state->valuestring : NULL;
    bool on = false;

    if (!s || strcmp(s, "unavailable") == 0) {
        snprintf(line, sizeof(line), "Unavailable");
    } else if (strcmp(s, "connected") == 0) {
        snprintf(line, sizeof(line), "%s", cJSON_IsString(ssid) ? ssid->valuestring : "Connected");
        on = true;
    } else if (strcmp(s, "off") == 0) {
        snprintf(line, sizeof(line), "Off");
    } else if (strcmp(s, "disconnected") == 0) {
        snprintf(line, sizeof(line), "Not connected");
    } else if (strcmp(s, "failed") == 0) {
        snprintf(line, sizeof(line), "Failed");
    } else {
        snprintf(line, sizeof(line), "Connecting");
    }
    cJSON_Delete(st);
    set_text(ct.value[CONTROLS_TILE_WIFI], line);
    set_dot(CONTROLS_TILE_WIFI, on);
}

/* Bluetooth and the battery, both from sysd's system.status. */
static void refresh_sysd(void)
{
    char err[96];
    char line[CONTROLS_LINE_MAX];
    cJSON *st = shell_ipc_call_timeout("sysd", "system.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                       sizeof(err));

    controls_bluetooth_text(st, line, sizeof(line));
    set_text(ct.value[CONTROLS_TILE_BLUETOOTH], line);
    set_dot(CONTROLS_TILE_BLUETOOTH, false);
    controls_battery_text(st, line, sizeof(line));
    set_text(ct.value[CONTROLS_TILE_BATTERY], line);
    set_dot(CONTROLS_TILE_BATTERY, controls_battery_dot(st));
    cJSON_Delete(st);
}

static const char *const rotation_names[] = { "Automatic", "Portrait", "Landscape" };

static void refresh_rotation(void)
{
    struct pocketos_orientation o;
    char line[40];

    pocketos_shell_orientation(&o);
    /* The mode, not what the screen is doing: a bench override (--rotation,
     * POCKETOS_DRM_ROTATION) can hold the screen away from it indefinitely. */
    snprintf(line, sizeof(line), "%s", rotation_names[o.mode % 3]);
    set_text(ct.value[CONTROLS_TILE_ROTATION], line);
    set_dot(CONTROLS_TILE_ROTATION, false);
}

static void refresh_mode(void)
{
    static const char *const names[] = { "Normal", "Outdoor", "Night" };
    enum pos_mode m = pos_theme_current_mode();

    set_text(ct.value[CONTROLS_TILE_MODE], names[m < POS_MODE_COUNT ? m : 0]);
    set_dot(CONTROLS_TILE_MODE, m != POS_MODE_NORMAL);
}

static void refresh_brightness(void)
{
    int pct = pocketos_shell_brightness_get();
    char line[16];

    if (pct < 0) {
        lv_obj_add_state(ct.slider, LV_STATE_DISABLED);
        lv_slider_set_value(ct.slider, BRIGHTNESS_MIN_PCT, LV_ANIM_OFF);
        set_text(ct.slider_value, "Not available");
        return;
    }
    lv_obj_remove_state(ct.slider, LV_STATE_DISABLED);
    if (!lv_obj_has_state(ct.slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(ct.slider, pct, LV_ANIM_OFF);
        snprintf(line, sizeof(line), "%d %%", pct);
        set_text(ct.slider_value, line);
    }
}

static void refresh_volume(void)
{
    bool available = pocketos_shell_volume_available() != 0;
    bool muted = pocketos_shell_volume_muted() != 0;
    char line[CONTROLS_LINE_MAX];

    if (!available || muted) {
        lv_obj_add_state(ct.volume, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ct.volume, LV_STATE_DISABLED);
    }
    if (!lv_obj_has_state(ct.volume, LV_STATE_PRESSED)) {
        lv_slider_set_value(ct.volume, pocketos_shell_volume_get(), LV_ANIM_OFF);
        controls_volume_text(available, muted, pocketos_shell_volume_get(), line, sizeof(line));
        set_text(ct.volume_value, line);
    }
}

/* ---- the antenna question -------------------------------------------------- */

static void dialog_show(bool show)
{
    if (!ct.dialog) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(ct.dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ct.dialog);
    } else {
        lv_obj_add_flag(ct.dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

/* radio.set_enabled, and whatever radiod answered shown at once. */
static void radio_switch(bool on)
{
    char err[160] = "";
    cJSON *params = cJSON_CreateObject();
    cJSON *r;
    const cJSON *state;

    cJSON_AddBoolToObject(params, "enabled", on);
    r = shell_ipc_call_timeout("radiod", "radio.set_enabled", params, RADIO_SWITCH_TIMEOUT_MS, err,
                               sizeof(err));
    state = r ? cJSON_GetObjectItemCaseSensitive(r, "state") : NULL;
    if (cJSON_IsString(state)) {
        snprintf(ct.radio_hold, sizeof(ct.radio_hold), "%s", state->valuestring);
        ct.radio_hold_ticks = RADIO_HOLD_TICKS;
        LOG_INFO("controls: radio switched %s (%s)", on ? "on" : "off", state->valuestring);
    } else {
        LOG_WARN("controls: radio not switched %s: %s", on ? "on" : "off",
                 err[0] ? err : "radiod did not answer");
        ct.radio_failed_ticks = RADIO_HOLD_TICKS;
    }
    cJSON_Delete(r);
    refresh_radio();
}

static void on_radio(lv_event_t *e)
{
    (void)e;
    switch (controls_radio_tapped(&ct.radio_flow, radio_state())) {
    case CONTROLS_TAP_ASK_ANTENNA:
        dialog_show(true);
        break;
    case CONTROLS_TAP_SWITCH_OFF:
        radio_switch(false);
        break;
    case CONTROLS_TAP_NOTHING:
    default:
        break;
    }
}

static void on_antenna_answer(lv_event_t *e)
{
    bool enable = lv_event_get_user_data(e) != NULL;

    dialog_show(false);
    if (controls_radio_answer(&ct.radio_flow, enable)) {
        radio_switch(true);
    } else {
        LOG_INFO("controls: radio left off (antenna question cancelled)");
    }
}

/* Taps on the scrim around the question go nowhere: it is answered with one
 * of its two buttons or by leaving Controls, never by a stray touch. */
static void on_scrim(lv_event_t *e)
{
    (void)e;
}

static void build_dialog(void)
{
    const struct controls_rect *d = &ct.layout.dialog;
    struct controls_rect btn;
    int32_t bw = (d->w - 3 * 24) / 2;
    lv_obj_t *panel;
    lv_obj_t *o;
    lv_obj_t *b;

    ct.dialog = plain(ct.root);
    lv_obj_set_size(ct.dialog, LV_PCT(100), LV_PCT(100));
    pos_style_add(ct.dialog, POS_STYLE_ENV_PANEL, 0);
    lv_obj_add_flag(ct.dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ct.dialog, on_scrim, LV_EVENT_CLICKED, NULL);

    panel = glass(ct.dialog, d, false);
    o = text(panel, POS_STYLE_ENV_TEXT, CONTROLS_ANTENNA_TITLE);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(o, d->w - 48);
    lv_obj_set_pos(o, 24, 24);
    o = text(panel, POS_STYLE_ENV_TEXT_SECONDARY, CONTROLS_ANTENNA_BODY);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(o, d->w - 48);
    lv_obj_set_pos(o, 24, 104);

    btn.x = 24;
    btn.y = d->h - 24 - 64;
    btn.w = bw;
    btn.h = 64;
    b = glass(panel, &btn, true);
    lv_obj_add_event_cb(b, on_antenna_answer, LV_EVENT_CLICKED, NULL);
    o = text(b, POS_STYLE_ENV_TEXT, CONTROLS_ANTENNA_CANCEL);
    lv_obj_center(o);
    btn.x = 24 + bw + 24;
    b = glass(panel, &btn, true);
    lv_obj_add_event_cb(b, on_antenna_answer, LV_EVENT_CLICKED, (void *)1);
    o = text(b, POS_STYLE_ENV_TEXT, CONTROLS_ANTENNA_ENABLE);
    lv_obj_center(o);
    lv_obj_add_flag(ct.dialog, LV_OBJ_FLAG_HIDDEN);
}

/* ---- taps ------------------------------------------------------------------ */

static void on_open_app(lv_event_t *e)
{
    if (ct.actions.open_app) {
        ct.actions.open_app((const char *)lv_event_get_user_data(e));
    }
}

static void on_lock(lv_event_t *e)
{
    (void)e;
    if (ct.actions.lock) {
        ct.actions.lock();
    }
}

static void on_close(lv_event_t *e)
{
    (void)e;
    if (ct.actions.close) {
        ct.actions.close();
    }
}

static void on_rotation(lv_event_t *e)
{
    struct pocketos_orientation o;

    (void)e;
    pocketos_shell_orientation(&o);
    if (pocketos_shell_set_rotation_mode((enum pocketos_rotation_mode)((o.mode + 1) % 3)) < 0) {
        LOG_WARN("controls: rotation mode not stored");
    }
    refresh_rotation();
}

static void on_mode(lv_event_t *e)
{
    static const char *const next[] = { "night", "normal", "outdoor" }; /* from normal, outdoor, night */
    enum pos_mode m = pos_theme_current_mode();

    (void)e;
    pocketos_shell_set_appearance(NULL, next[m < POS_MODE_COUNT ? m : 0]);
    refresh_mode();
}

static void on_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = (int)lv_slider_get_value(ct.slider);
    char line[16];

    v = (v + BRIGHTNESS_STEP_PCT / 2) / BRIGHTNESS_STEP_PCT * BRIGHTNESS_STEP_PCT;
    if (v < BRIGHTNESS_MIN_PCT) {
        v = BRIGHTNESS_MIN_PCT;
    }
    snprintf(line, sizeof(line), "%d %%", v);
    set_text(ct.slider_value, line);
    /* Written on release, not on every step of the drag: one sysfs write
     * and one stored value per gesture. A write the panel refused shows the
     * level it actually has, not the one asked for. */
    if (code == LV_EVENT_RELEASED) {
        if (pocketos_shell_brightness_set(v) < 0) {
            LOG_WARN("controls: brightness %d%% not applied", v);
        }
        refresh_brightness();
    }
}

static void on_volume(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = volume_round((int)lv_slider_get_value(ct.volume));
    char line[16];

    snprintf(line, sizeof(line), "%d %%", v);
    set_text(ct.volume_value, line);
    /* As the brightness: stored once per gesture, on release. The level is
     * what the next sound is played at (pos-wave reads it when it starts);
     * nothing is playing from Controls itself. */
    if (code == LV_EVENT_RELEASED) {
        if (pocketos_shell_volume_set(v) < 0) {
            LOG_WARN("controls: volume %d%% not stored", v);
        }
        refresh_volume();
    }
}

static void on_mute(lv_event_t *e)
{
    (void)e;
    if (!pocketos_shell_volume_available()) {
        return;
    }
    if (pocketos_shell_volume_set_muted(pocketos_shell_volume_muted() ? 0 : 1) < 0) {
        LOG_WARN("controls: mute not stored");
    }
    refresh_volume();
}

/* ---- layout -------------------------------------------------------------- */

static void tile(lv_obj_t *parent, enum controls_tile t, const lv_image_dsc_t *g, const char *name,
                 lv_event_cb_t cb, void *user)
{
    const struct controls_rect *r = &ct.layout.tile[t];
    lv_obj_t *o = glass(parent, r, cb != NULL);
    lv_obj_t *l;

    if (cb) {
        lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    }
    l = glyph(o, g);
    lv_obj_set_pos(l, 20, 22);
    l = text(o, POS_STYLE_ENV_TEXT, name);
    lv_obj_set_pos(l, 66, 20);
    ct.value[t] = text(o, POS_STYLE_ENV_TEXT_SMALL, "");
    lv_obj_set_pos(ct.value[t], 66, 52);
    lv_label_set_long_mode(ct.value[t], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(ct.value[t], r->w - 66 - 16);
    ct.dot[t] = plain(o);
    pos_style_add(ct.dot[t], POS_STYLE_ENV_DOT, 0);
    lv_obj_set_size(ct.dot[t], 8, 8);
    lv_obj_set_pos(ct.dot[t], r->w - 22, 14);
    lv_obj_add_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
}

static void row(lv_obj_t *panel, int i, int32_t w, int32_t h, const lv_image_dsc_t *g, const char *name,
                const char *app)
{
    lv_obj_t *r = plain(panel);
    lv_obj_t *o;

    lv_obj_set_pos(r, 0, i * h);
    lv_obj_set_size(r, w - 2, h);
    pos_style_add(r, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, on_open_app, LV_EVENT_CLICKED, (void *)app);
    o = glyph(r, g);
    lv_obj_set_pos(o, 18, (h - 32) / 2);
    o = text(r, POS_STYLE_ENV_TEXT, name);
    lv_obj_align(o, LV_ALIGN_LEFT_MID, 66, 0);
    o = text(r, POS_STYLE_ENV_TEXT, LV_SYMBOL_RIGHT);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_align(o, LV_ALIGN_RIGHT_MID, -18, 0);
    if (i > 0) {
        o = plain(r);
        pos_style_add(o, POS_STYLE_ENV_DIVIDER, 0);
        lv_obj_set_size(o, w - 2, 1);
        lv_obj_set_pos(o, 0, 0);
    }
}

static void action(lv_obj_t *parent, const struct controls_rect *r, const lv_image_dsc_t *g,
                   const char *name, lv_event_cb_t cb, void *user)
{
    lv_obj_t *o = glass(parent, r, true);

    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 12, 0);
    glyph(o, g);
    text(o, POS_STYLE_ENV_TEXT, name);
}

/* A panel with a glyph, a title, the value at the right and a slider under
 * them: Brightness and Volume. Returns the slider. */
static lv_obj_t *slider_panel(lv_obj_t *parent, const struct controls_rect *r, const lv_image_dsc_t *g,
                              const char *name, lv_obj_t **value, int32_t min, int32_t max,
                              lv_event_cb_t cb)
{
    lv_obj_t *p = glass(parent, r, false);
    lv_obj_t *o;
    lv_obj_t *s;

    o = glyph(p, g);
    lv_obj_set_pos(o, 18, 16);
    o = text(p, POS_STYLE_ENV_TEXT, name);
    lv_obj_set_pos(o, 66, 18);
    *value = text(p, POS_STYLE_ENV_TEXT_SMALL, "");
    lv_obj_align(*value, LV_ALIGN_TOP_RIGHT, -20, 22);
    s = lv_slider_create(p);
    lv_obj_remove_style_all(s);
    pos_style_add(s, POS_STYLE_ENV_SLIDER, 0);
    pos_style_add(s, POS_STYLE_ENV_SLIDER_FILL, LV_PART_INDICATOR);
    pos_style_add(s, POS_STYLE_ENV_SLIDER_KNOB, LV_PART_KNOB);
    lv_obj_set_style_opa(s, LV_OPA_40, LV_STATE_DISABLED);
    lv_slider_set_range(s, min, max);
    lv_obj_set_size(s, r->w - 66 - 36, 4);
    /* 70 of 104 in portrait; the same distance from the foot in the 88 px
     * landscape panel. */
    lv_obj_set_pos(s, 66, r->h - 34);
    /* The track is 4 px; the touch target is the DS minimum around it. */
    lv_obj_set_ext_click_area(s, 30);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

lv_obj_t *controls_create(lv_obj_t *parent, bool landscape, const struct controls_rect *keepout,
                          const struct controls_actions *actions)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    const struct controls_layout *L = &ct.layout;
    struct pos_insets top = pos_display_bar_insets(g, POS_EDGE_TOP);
    struct controls_frame f;
    lv_obj_t *o;
    lv_obj_t *back;

    memset(&ct, 0, sizeof(ct));
    memset(&f, 0, sizeof(f));
    ct.landscape = landscape;
    ct.actions = *actions;
    f.landscape = landscape;
    f.width = g->width;
    f.height = lv_obj_get_height(parent) > 0 ? lv_obj_get_height(parent) : g->height;
    f.inset_top_left = top.left;
    f.inset_top_right = top.right;
    if (keepout) {
        f.keepout = *keepout;
    }
    if (controls_layout(&f, &ct.layout) < 0) {
        LOG_WARN("controls: %dx%d is too small for the layout; panels may overlap", (int)f.width,
                 (int)f.height);
    }
    ct.root = plain(parent);
    lv_obj_set_size(ct.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(ct.root, LV_OBJ_FLAG_HIDDEN);

    /* Header: back, the title, what this is. */
    back = plain(ct.root);
    lv_obj_set_pos(back, L->back.x, L->back.y);
    lv_obj_set_size(back, L->back.w, L->back.h);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_close, LV_EVENT_CLICKED, NULL);
    o = text(back, POS_STYLE_ENV_TEXT, LV_SYMBOL_LEFT);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_center(o);
    /* The screen's top-left corner in glass, as the app header's (DS §48);
     * Controls starts at the top edge. */
    pocketui_back_corner(back, L->back.x, L->back.y, POS_STYLE_ENV_PANEL, POS_STYLE_ENV_PANEL_PRESSED);
    o = text(ct.root, POS_STYLE_ENV_TITLE, "Controls");
    lv_obj_set_pos(o, L->header.x, 12);
    o = text(ct.root, POS_STYLE_ENV_TEXT_SECONDARY, "Quick state of this device");
    lv_obj_set_pos(o, L->header.x + 2, landscape ? 60 : 64);

    /* Radios first, then state, then the display. Bluetooth and the battery
     * are read-only: nothing on this board switches either (docs/api/
     * system.md), so their tiles are not pressable. Bluetooth and the
     * battery borrow existing glyphs until the DS draws their own. */
    tile(ct.root, CONTROLS_TILE_WIFI, &pos_glyph_wifi, "Wi-Fi", on_open_app, (void *)"settings");
    tile(ct.root, CONTROLS_TILE_BLUETOOTH, &pos_glyph_network, "Bluetooth", NULL, NULL);
    tile(ct.root, CONTROLS_TILE_RADIO, &pos_glyph_radio, "LoRa radio", on_radio, NULL);
    tile(ct.root, CONTROLS_TILE_BATTERY, &pos_glyph_power, "Battery", NULL, NULL);
    tile(ct.root, CONTROLS_TILE_ROTATION, &pos_glyph_display, "Rotation", on_rotation, NULL);
    tile(ct.root, CONTROLS_TILE_MODE, &pos_glyph_mode, "Display", on_mode, NULL);

    ct.slider = slider_panel(ct.root, &L->brightness, &pos_glyph_sun, "Brightness", &ct.slider_value,
                             BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT, on_slider);
    ct.volume = slider_panel(ct.root, &L->volume, &pos_glyph_sound, "Volume", &ct.volume_value,
                             VOLUME_MIN_PCT, VOLUME_MAX_PCT, on_volume);
    /* The speaker glyph is the mute switch: a DS-minimum target around it. */
    ct.volume_mute = plain(lv_obj_get_parent(ct.volume));
    lv_obj_set_pos(ct.volume_mute, 6, 4);
    lv_obj_set_size(ct.volume_mute, 56, 56);
    lv_obj_add_flag(ct.volume_mute, LV_OBJ_FLAG_CLICKABLE);
    pos_style_add(ct.volume_mute, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ct.volume_mute, on_mute, LV_EVENT_CLICKED, NULL);

    o = glass(ct.root, &L->list, false);
    row(o, 0, L->list.w, L->list_row_h, &pos_glyph_settings, "Settings", "settings");
    row(o, 1, L->list.w, L->list_row_h, &pos_glyph_mesh, "Mesh messages", "rift");
    row(o, 2, L->list.w, L->list_row_h, &pos_glyph_info, "About DOORS", "system");
    action(ct.root, &L->lock, &pos_glyph_lock, "Lock", on_lock, NULL);
    action(ct.root, &L->power, &pos_glyph_power, "Power", on_open_app, (void *)"system");

    build_dialog();
    return ct.root;
}

void controls_tick(void)
{
    if (!controls_visible()) {
        return;
    }
    if (ct.radio_hold_ticks > 0) {
        ct.radio_hold_ticks--;
    }
    if (ct.radio_failed_ticks > 0) {
        ct.radio_failed_ticks--;
    }
    refresh_radio();
    refresh_rotation();
    refresh_mode();
    refresh_brightness();
    refresh_volume();
    if (++ct.tick >= WIFI_POLL_TICKS) {
        ct.tick = 0;
        refresh_wifi();
    }
    if (++ct.sysd_tick >= SYSD_POLL_TICKS) {
        ct.sysd_tick = 0;
        refresh_sysd();
    }
}

void controls_show(void)
{
    if (!ct.root) {
        return;
    }
    lv_obj_remove_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
    ct.tick = 0;
    ct.sysd_tick = 0;
    refresh_radio();
    refresh_wifi();
    refresh_sysd();
    refresh_rotation();
    refresh_mode();
    refresh_brightness();
    refresh_volume();
    LOG_INFO("controls: shown");
}

void controls_hide(void)
{
    /* Leaving Controls with the antenna question open is a Cancel: the radio
     * stays off. */
    controls_radio_dismiss(&ct.radio_flow);
    dialog_show(false);
    if (ct.root && !lv_obj_has_flag(ct.root, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
        LOG_INFO("controls: hidden");
    }
}

void controls_destroy(void)
{
    if (!ct.root) {
        return;
    }
    controls_radio_dismiss(&ct.radio_flow);
    lv_obj_delete(ct.root);
    memset(&ct, 0, sizeof(ct));
}

bool controls_visible(void)
{
    return ct.root && !lv_obj_has_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
}
