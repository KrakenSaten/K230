/*
 * DOORS Controls. See controls.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "controls.h"

#include "app.h"
#include "brightness.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_glyphs.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <string.h>

#define TILE_H_PORTRAIT 120
#define TILE_H_LANDSCAPE 112
#define GAP 16
#define ROW_H 64
#define WIFI_POLL_TICKS 2

enum tile { TILE_RADIO, TILE_WIFI, TILE_ROTATION, TILE_MODE, TILE_COUNT };

static struct {
    lv_obj_t *root;
    bool landscape;
    struct controls_actions actions;
    lv_obj_t *value[TILE_COUNT];
    lv_obj_t *dot[TILE_COUNT];
    lv_obj_t *slider;
    lv_obj_t *slider_value;
    int tick;
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

static lv_obj_t *glass(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, bool pressable)
{
    lv_obj_t *o = plain(parent);

    pos_style_add(o, POS_STYLE_ENV_PANEL, 0);
    if (pressable) {
        pos_style_add(o, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

/* ---- what each tile says ------------------------------------------------- */

static void set_text(lv_obj_t *label, const char *s)
{
    if (label && strcmp(lv_label_get_text(label), s) != 0) {
        lv_label_set_text(label, s);
    }
}

static void set_dot(enum tile t, bool on)
{
    if (on) {
        lv_obj_remove_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_radio(void)
{
    const char *s = pocketos_shell_radio_state();

    if (!s) {
        set_text(ct.value[TILE_RADIO], "Not answering");
    } else if (strcmp(s, "rx") == 0) {
        set_text(ct.value[TILE_RADIO], "Receiving");
    } else if (strcmp(s, "tx") == 0) {
        set_text(ct.value[TILE_RADIO], "Transmitting");
    } else {
        set_text(ct.value[TILE_RADIO], "Idle");
    }
    set_dot(TILE_RADIO, s && (strcmp(s, "rx") == 0 || strcmp(s, "tx") == 0));
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
    set_text(ct.value[TILE_WIFI], line);
    set_dot(TILE_WIFI, on);
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
    set_text(ct.value[TILE_ROTATION], line);
    set_dot(TILE_ROTATION, false);
}

static void refresh_mode(void)
{
    static const char *const names[] = { "Normal", "Outdoor", "Night" };
    enum pos_mode m = pos_theme_current_mode();

    set_text(ct.value[TILE_MODE], names[m < POS_MODE_COUNT ? m : 0]);
    set_dot(TILE_MODE, m != POS_MODE_NORMAL);
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

/* ---- layout -------------------------------------------------------------- */

static void tile(lv_obj_t *parent, enum tile t, int32_t x, int32_t y, int32_t w, int32_t h,
                 const lv_image_dsc_t *g, const char *name, lv_event_cb_t cb, void *user)
{
    lv_obj_t *o = glass(parent, x, y, w, h, true);
    lv_obj_t *l;

    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    l = glyph(o, g);
    lv_obj_set_pos(l, 20, 22);
    l = text(o, POS_STYLE_ENV_TEXT, name);
    lv_obj_set_pos(l, 66, 20);
    ct.value[t] = text(o, POS_STYLE_ENV_TEXT_SMALL, "");
    lv_obj_set_pos(ct.value[t], 66, 52);
    lv_label_set_long_mode(ct.value[t], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(ct.value[t], w - 66 - 16);
    ct.dot[t] = plain(o);
    pos_style_add(ct.dot[t], POS_STYLE_ENV_DOT, 0);
    lv_obj_set_size(ct.dot[t], 8, 8);
    lv_obj_set_pos(ct.dot[t], w - 22, 14);
    lv_obj_add_flag(ct.dot[t], LV_OBJ_FLAG_HIDDEN);
}

static void row(lv_obj_t *panel, int i, int32_t w, const lv_image_dsc_t *g, const char *name, const char *app)
{
    lv_obj_t *r = plain(panel);
    lv_obj_t *o;

    lv_obj_set_pos(r, 0, i * ROW_H);
    lv_obj_set_size(r, w - 2, ROW_H);
    pos_style_add(r, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, on_open_app, LV_EVENT_CLICKED, (void *)app);
    o = glyph(r, g);
    lv_obj_set_pos(o, 18, (ROW_H - 32) / 2);
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

static void action(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_image_dsc_t *g, const char *name,
                   lv_event_cb_t cb, void *user)
{
    lv_obj_t *o = glass(parent, x, y, w, 64, true);
    lv_obj_t *l;

    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 12, 0);
    glyph(o, g);
    l = text(o, POS_STYLE_ENV_TEXT, name);
    (void)l;
}

static void brightness_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *p = glass(parent, x, y, w, 104, false);
    lv_obj_t *o;

    o = glyph(p, &pos_glyph_sun);
    lv_obj_set_pos(o, 18, 16);
    o = text(p, POS_STYLE_ENV_TEXT, "Brightness");
    lv_obj_set_pos(o, 66, 18);
    ct.slider_value = text(p, POS_STYLE_ENV_TEXT_SMALL, "");
    lv_obj_align(ct.slider_value, LV_ALIGN_TOP_RIGHT, -20, 22);
    ct.slider = lv_slider_create(p);
    lv_obj_remove_style_all(ct.slider);
    pos_style_add(ct.slider, POS_STYLE_ENV_SLIDER, 0);
    pos_style_add(ct.slider, POS_STYLE_ENV_SLIDER_FILL, LV_PART_INDICATOR);
    pos_style_add(ct.slider, POS_STYLE_ENV_SLIDER_KNOB, LV_PART_KNOB);
    lv_obj_set_style_opa(ct.slider, LV_OPA_40, LV_STATE_DISABLED);
    lv_slider_set_range(ct.slider, BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT);
    lv_obj_set_size(ct.slider, w - 66 - 36, 4);
    lv_obj_set_pos(ct.slider, 66, 70);
    /* The track is 4 px; the touch target is the DS minimum around it. */
    lv_obj_set_ext_click_area(ct.slider, 30);
    lv_obj_add_event_cb(ct.slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ct.slider, on_slider, LV_EVENT_RELEASED, NULL);
}

lv_obj_t *controls_create(lv_obj_t *parent, bool landscape, const struct controls_actions *actions)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    int32_t w = g->width;
    int32_t m = 36;
    int32_t y;
    int32_t col;
    int32_t th = landscape ? TILE_H_LANDSCAPE : TILE_H_PORTRAIT;
    int32_t tw;
    lv_obj_t *o;
    lv_obj_t *back;

    memset(&ct, 0, sizeof(ct));
    ct.landscape = landscape;
    ct.actions = *actions;
    ct.root = plain(parent);
    lv_obj_set_size(ct.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(ct.root, LV_OBJ_FLAG_HIDDEN);

    /* Header: back, the title, what this is. */
    back = glass(ct.root, m, 16, 72, 56, true);
    lv_obj_add_event_cb(back, on_close, LV_EVENT_CLICKED, NULL);
    o = text(back, POS_STYLE_ENV_TEXT, LV_SYMBOL_LEFT);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_center(o);
    o = text(ct.root, POS_STYLE_ENV_TITLE, "Controls");
    lv_obj_set_pos(o, m + 72 + 20, 12);
    o = text(ct.root, POS_STYLE_ENV_TEXT_SECONDARY, "Quick state of this device");
    lv_obj_set_pos(o, m + 72 + 22, landscape ? 60 : 64);

    col = landscape ? 520 : w - 2 * m;          /* the tile block's width */
    tw = (col - GAP) / 2;
    y = landscape ? 104 : 120;
    tile(ct.root, TILE_RADIO, m, y, tw, th, &pos_glyph_radio, "Radio", on_open_app, (void *)"radio");
    tile(ct.root, TILE_WIFI, m + tw + GAP, y, tw, th, &pos_glyph_wifi, "Wi-Fi", on_open_app, (void *)"settings");
    tile(ct.root, TILE_ROTATION, m, y + th + GAP, tw, th, &pos_glyph_display, "Rotation", on_rotation, NULL);
    tile(ct.root, TILE_MODE, m + tw + GAP, y + th + GAP, tw, th, &pos_glyph_sun, "Display", on_mode, NULL);

    if (landscape) {
        int32_t rx = m + col + 32;
        int32_t rw = w - m - rx;

        action(ct.root, m, y + 2 * (th + GAP), tw, &pos_glyph_lock, "Lock", on_lock, NULL);
        action(ct.root, m + tw + GAP, y + 2 * (th + GAP), tw, &pos_glyph_power, "Power", on_open_app,
               (void *)"system");
        brightness_panel(ct.root, rx, y, rw);
        o = glass(ct.root, rx, y + 104 + GAP, rw, 3 * ROW_H, false);
        row(o, 0, rw, &pos_glyph_settings, "Settings", "settings");
        row(o, 1, rw, &pos_glyph_mesh, "Mesh messages", "rift");
        row(o, 2, rw, &pos_glyph_info, "About DOORS", "system");
    } else {
        int32_t py = y + 2 * (th + GAP) + 8;
        int32_t ah = 64;
        int32_t foot = lv_obj_get_height(parent) > 0 ? lv_obj_get_height(parent) : g->height - 56;

        brightness_panel(ct.root, m, py, col);
        o = glass(ct.root, m, py + 104 + GAP, col, 3 * ROW_H, false);
        row(o, 0, col, &pos_glyph_settings, "Settings", "settings");
        row(o, 1, col, &pos_glyph_mesh, "Mesh messages", "rift");
        row(o, 2, col, &pos_glyph_info, "About DOORS", "system");
        py = foot - 48 - ah;
        action(ct.root, m, py, tw, &pos_glyph_lock, "Lock", on_lock, NULL);
        action(ct.root, m + tw + GAP, py, tw, &pos_glyph_power, "Power", on_open_app, (void *)"system");
    }
    return ct.root;
}

void controls_tick(void)
{
    if (!controls_visible()) {
        return;
    }
    refresh_radio();
    refresh_rotation();
    refresh_mode();
    refresh_brightness();
    if (++ct.tick >= WIFI_POLL_TICKS) {
        ct.tick = 0;
        refresh_wifi();
    }
}

void controls_show(void)
{
    if (!ct.root) {
        return;
    }
    lv_obj_remove_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
    ct.tick = 0;
    refresh_radio();
    refresh_wifi();
    refresh_rotation();
    refresh_mode();
    refresh_brightness();
    LOG_INFO("controls: shown");
}

void controls_hide(void)
{
    if (ct.root && !lv_obj_has_flag(ct.root, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
        LOG_INFO("controls: hidden");
    }
}

bool controls_visible(void)
{
    return ct.root && !lv_obj_has_flag(ct.root, LV_OBJ_FLAG_HIDDEN);
}
