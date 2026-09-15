/*
 * Settings in the running app, under a real LVGL pointer device and the one
 * key stream: what a finger and a keyboard actually reach, and what the app
 * then sends.
 *
 * The shell and netd are not here, so this file plays both. shell_ipc's
 * shell_ipc_call_timeout() is implemented below over a scripted netd that
 * records every call and its parameters; the shell's brightness and keyboard
 * entry points are implemented over variables. That is what lets the test
 * assert the part that matters most: which request carried a passphrase,
 * and that no other one did.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/settings_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "brightness.h"
#include "pocketui.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H POCKETUI_STATUS_BAR_H
#define PASS "correct horse 9"

extern const struct pocketos_app app_settings;

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* ---- the scripted netd ---------------------------------------------------- */

#define CALLS_MAX 128
static struct {
    char method[32];
    char params[512];
} calls[CALLS_MAX];
static int call_count;
static int netd_down;
static const char *g_status;
static const char *g_networks;
static const char *g_connect_error;

static void calls_reset(void)
{
    call_count = 0;
}

static int called(const char *method, const char *params)
{
    int i;

    for (i = 0; i < call_count; i++) {
        if (strcmp(calls[i].method, method) == 0 && (!params || strcmp(calls[i].params, params) == 0)) {
            return 1;
        }
    }
    return 0;
}

cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params, int timeout_ms,
                              char *err, size_t errlen)
{
    const char *reply = NULL;

    (void)timeout_ms;
    if (err && errlen) {
        err[0] = '\0';
    }
    if (strcmp(service, "netd") != 0) {
        cJSON_Delete(params);
        return NULL;
    }
    if (call_count < CALLS_MAX) {
        char *text = params ? cJSON_PrintUnformatted(params) : NULL;

        snprintf(calls[call_count].method, sizeof(calls[0].method), "%s", method);
        snprintf(calls[call_count].params, sizeof(calls[0].params), "%s", text ? text : "null");
        free(text);
        call_count++;
    }
    cJSON_Delete(params);
    if (netd_down) {
        return NULL;
    }
    if (strcmp(method, "wifi.status") == 0 || strcmp(method, "wifi.set_enabled") == 0 ||
        strcmp(method, "wifi.disconnect") == 0) {
        reply = g_status;
    } else if (strcmp(method, "wifi.networks") == 0) {
        reply = g_networks;
    } else if (strcmp(method, "wifi.scan") == 0) {
        reply = "{\"scanning\":true}";
    } else if (strcmp(method, "wifi.connect") == 0) {
        if (g_connect_error) {
            snprintf(err, errlen, "%s", g_connect_error);
            return NULL;
        }
        reply = "{\"accepted\":true}";
    } else if (strcmp(method, "wifi.forget") == 0) {
        reply = "{\"forgotten\":true,\"persisted\":true,\"saved_count\":0}";
    }
    return reply ? cJSON_Parse(reply) : NULL;
}

cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params, char *err, size_t errlen)
{
    return shell_ipc_call_timeout(service, method, params, 0, err, errlen);
}

void shell_ipc_shutdown(void) { }

/* ---- the shell's side of app.h ------------------------------------------------ */

static int g_bright = 60;
static int g_bright_sets;
static int kb_shown;
static void (*kb_done)(void *user);
static void *kb_user;

int pocketos_shell_brightness_get(void)
{
    return g_bright;
}

int pocketos_shell_brightness_set(int percent)
{
    if (g_bright < 0) {
        return -1;
    }
    g_bright_sets++;
    g_bright = brightness_clamp_percent(percent);
    return g_bright;
}

static char g_theme_set[32];
static char g_mode_set[16];

/* The shell's appearance entry point, over the real theme engine so the
 * styles on screen really change. */
int pocketos_shell_set_appearance(const char *theme_id, const char *mode_name)
{
    char why[128];

    snprintf(g_theme_set, sizeof(g_theme_set), "%s", theme_id ? theme_id : "");
    snprintf(g_mode_set, sizeof(g_mode_set), "%s", mode_name ? mode_name : "");
    return pos_theme_apply(theme_id, mode_name, why, sizeof(why)) < 0 ? -1 : 0;
}

/* The shell's orientation, over a variable: stored mode, this run, the next. */
static struct pocketos_orientation g_orient = {
    .mode = POCKETOS_ROTATION_AUTOMATIC,
    .mode_valid = true,
    .keyboard = POCKETOS_KEYBOARD_UNKNOWN,
};
static int g_rot_sets;
static int g_rot_last = -1;

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    *out = g_orient;
}

int pocketos_shell_set_rotation_mode(enum pocketos_rotation_mode mode)
{
    g_rot_sets++;
    g_rot_last = (int)mode;
    g_orient.mode = mode;
    g_orient.mode_valid = true;
    g_orient.next_landscape = mode == POCKETOS_ROTATION_LANDSCAPE ||
                              (mode == POCKETOS_ROTATION_AUTOMATIC && g_orient.keyboard == POCKETOS_KEYBOARD_PRESENT);
    g_orient.restart_required = g_orient.next_landscape != g_orient.landscape;
    return 0;
}

/* Whether a button is drawn as the accented (primary) one: its fill is the
 * primary role's accent_primary rather than the secondary role's surface. */
static int role_on(lv_obj_t *obj, enum pos_style_role role)
{
    lv_color_t accent = lv_color_hex(pos_theme_rgb(POS_COLOR_ACCENT_PRIMARY));
    lv_color_t fill;

    if (!obj || role != POS_STYLE_BUTTON_PRIMARY) {
        return 0;
    }
    fill = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    return lv_color_eq(fill, accent);
}

int64_t pocketos_shell_system_day(void) { return -1; }
void pocketos_shell_set_status_hint(const char *text) { (void)text; }
void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    kb_shown = 1;
    kb_done = on_done;
    kb_user = user;
}

void pocketos_shell_keyboard_hide(void)
{
    kb_shown = 0;
    kb_done = NULL;
}

int pocketos_shell_keyboard_visible(void) { return kb_shown; }

/* brightness.c is not linked; the clamp is the one piece the stub needs. */
int brightness_clamp_percent(int pct)
{
    return pct < BRIGHTNESS_MIN_PCT ? BRIGHTNESS_MIN_PCT : pct > BRIGHTNESS_MAX_PCT ? BRIGHTNESS_MAX_PCT : pct;
}

/* ---- display, finger, keys ------------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static lv_obj_t *app_body;
static void *app_priv;

static lv_obj_t *find_visible(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_visible(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static lv_obj_t *find_containing(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strstr(t, text)) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_containing(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int shows(const char *text)
{
    return find_visible(app_body, text) != NULL;
}

/* The clickable ancestor a finger on this label would hit. */
static lv_obj_t *target_of(const char *text)
{
    lv_obj_t *o = find_visible(app_body, text);

    while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

static void tap(const char *text)
{
    lv_obj_t *obj = target_of(text);
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on \"%s\": not on screen\n", text);
        failed++;
        checks++;
        return;
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    if (getenv("SETTINGS_TEST_DEBUG")) {
        lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &finger_point);

        printf("     tap \"%s\" at %d,%d (%d..%d x %d..%d) hits %s\n", text, (int)finger_point.x,
               (int)finger_point.y, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
               hit == obj ? "it" : "something else");
    }
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static int disabled(const char *text)
{
    lv_obj_t *o = target_of(text);

    return o && lv_obj_has_state(o, LV_STATE_DISABLED);
}

static void type(const char *s)
{
    int guard;

    for (; *s; s++) {
        pos_input_push_key((pos_key_t)(unsigned char)*s);
        for (guard = 0; guard < 40 && pos_input_queued() > 0; guard++) {
            pump(10);
        }
    }
    pump(40);
}

static const char *focused_text(void)
{
    lv_obj_t *f = pos_input_focused();

    return f && lv_obj_check_type(f, &lv_textarea_class) ? lv_textarea_get_text(f) : NULL;
}

static void tick(void)
{
    app_settings.tick(app_priv);
    pump(60);
}

/* Every visible clickable object is a finger's size (DS §7, 64 px). */
static int small_targets(lv_obj_t *obj)
{
    uint32_t i;
    int bad = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && obj != app_body) {
        lv_area_t a;

        lv_obj_get_coords(obj, &a);
        if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN) {
            printf("     small target %dx%d\n", (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
            bad++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        bad += small_targets(lv_obj_get_child(obj, i));
    }
    return bad;
}

/* No request other than wifi.connect carries the passphrase. */
static int passphrase_only_in_connect(void)
{
    int i;

    for (i = 0; i < call_count; i++) {
        if (strstr(calls[i].params, PASS) && strcmp(calls[i].method, "wifi.connect") != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- fixtures ---------------------------------------------------------------------- */

#define STATUS_OFF "{\"available\":true,\"enabled\":false,\"state\":\"off\",\"reason\":null,\"scanning\":false,\"store\":\"ok\"}"
#define STATUS_DISC "{\"available\":true,\"enabled\":true,\"state\":\"disconnected\",\"reason\":null,\"scanning\":false,\"store\":\"ok\"}"
#define STATUS_CONN "{\"available\":true,\"enabled\":true,\"state\":\"connected\",\"reason\":null,\"ssid\":\"Home\"," \
                    "\"ipv4\":\"192.168.1.20\",\"signal_bars\":4,\"scanning\":false,\"store\":\"ok\"}"
#define NETS "{\"scanning\":false,\"age_s\":2,\"hidden_count\":1,\"networks\":[" \
    "{\"ssid\":\"Home\",\"ssid_hex\":\"486f6d65\",\"signal_bars\":4,\"security\":\"wpa2\",\"supported\":true,\"needs_passphrase\":true,\"saved\":true,\"connected\":false}," \
    "{\"ssid\":\"Guest\",\"ssid_hex\":\"4775657374\",\"signal_bars\":3,\"security\":\"open\",\"supported\":true,\"needs_passphrase\":false,\"saved\":false,\"connected\":false}," \
    "{\"ssid\":\"W3\",\"ssid_hex\":\"5733\",\"signal_bars\":2,\"security\":\"wpa3\",\"supported\":false,\"needs_passphrase\":true,\"saved\":false,\"connected\":false}," \
    "{\"ssid\":\"New\",\"ssid_hex\":\"4e6577\",\"signal_bars\":1,\"security\":\"wpa2\",\"supported\":true,\"needs_passphrase\":true,\"saved\":false,\"connected\":false}]}"
#define NETS_CONN "{\"scanning\":false,\"age_s\":2,\"hidden_count\":0,\"networks\":[" \
    "{\"ssid\":\"Home\",\"ssid_hex\":\"486f6d65\",\"signal_bars\":4,\"security\":\"wpa2\",\"supported\":true,\"needs_passphrase\":true,\"saved\":true,\"connected\":true}]}"

static lv_obj_t *g_content;

static void app_start(void)
{
    app_body = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_size(app_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    app_priv = app_settings.create(app_body);
    pump(80);
}

static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_settings.destroy(app_priv);
    lv_obj_delete(app_body);
    app_body = NULL;
    pump(20);
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H - POCKETUI_HEADER_H);
    lv_obj_set_pos(g_content, 0, STATUS_H + POCKETUI_HEADER_H);
    pump(60);

    /* ---- 1. netd not running ------------------------------------------------------- */
    netd_down = 1;
    app_start();
    check("netd down: says the service is not running", shows("Wi-Fi service is not running"));
    check("netd down: the switch cannot be used", disabled("OFF"));
    check("netd down: brightness still works", shows("60 %"));
    app_stop();
    netd_down = 0;

    /* ---- 2. off, turned on -------------------------------------------------------------- */
    g_status = STATUS_OFF;
    g_networks = NETS;
    calls_reset();
    app_start();
    check("off: says so", shows("Wi-Fi is off"));
    check("off: switch reads OFF and is usable", shows("OFF") && !disabled("OFF"));
    check("off: no network list", !shows("Home") && !shows("SCAN"));
    check("off: netd was not asked for networks", !called("wifi.networks", NULL));
    g_status = STATUS_DISC;
    tap("OFF");
    check("tap OFF asks netd to turn Wi-Fi on", called("wifi.set_enabled", "{\"enabled\":true}"));
    tick();
    check("on: switch reads ON", shows("ON"));
    check("on: not connected", shows("Not connected"));

    /* ---- 3. the list ------------------------------------------------------------------------ */
    check("list: networks are listed", shows("Home") && shows("Guest") && shows("New") && shows("W3"));
    check("list: saved badge", shows("SAVED"));
    check("list: unsupported is marked", find_containing(app_body, "not supported") != NULL);
    check("list: hidden networks are mentioned", find_containing(app_body, "hidden") != NULL);
    check("list: every target is at least 64 px", small_targets(app_body) == 0);
    calls_reset();
    tap("SCAN");
    check("SCAN asks netd to scan", called("wifi.scan", NULL));

    /* ---- 4. a new network: passphrase, validation, join -------------------------------------- */
    calls_reset();
    tap("New");
    check("new network: a sheet with its name", shows("New") && shows("JOIN") && shows("CANCEL"));
    check("new network: the keyboard is asked for", kb_shown);
    check("new network: the passphrase field has focus", focused_text() != NULL);
    check("new network: every target is at least 64 px", small_targets(app_body) == 0);
    type("short");
    tap("JOIN");
    check("a short passphrase is refused before any request", !called("wifi.connect", NULL));
    check("and the field says why", find_visible(app_body, "At least 8 characters") != NULL);
    check("the field is masked", lv_textarea_get_password_mode(pos_input_focused()));
    tap("SHOW");
    check("SHOW unmasks it", !lv_textarea_get_password_mode(pos_input_focused()) && shows("HIDE"));
    tap("HIDE");
    check("HIDE masks it again", lv_textarea_get_password_mode(pos_input_focused()));
    check("focus stayed on the field through the taps", focused_text() != NULL);
    tap("CANCEL");
    check("CANCEL closes the sheet", shows("Not connected") && !shows("JOIN"));
    check("and hides the keyboard", !kb_shown);

    calls_reset();
    tap("New");
    type(PASS);
    check("the typed passphrase is in the field", focused_text() && strcmp(focused_text(), PASS) == 0);
    tap("JOIN");
    check("JOIN sends ssid_hex and the passphrase",
          called("wifi.connect", "{\"ssid_hex\":\"4e6577\",\"passphrase\":\"" PASS "\"}"));
    check("and closes the sheet", !shows("JOIN") && !kb_shown);
    check("no other request carried the passphrase", passphrase_only_in_connect());

    /* Done on the keyboard joins too */
    calls_reset();
    tap("New");
    type(PASS);
    check("keyboard Done is wired", kb_done != NULL);
    if (kb_done) {
        kb_done(kb_user);
        pump(60);
    }
    check("keyboard Done joins", called("wifi.connect", "{\"ssid_hex\":\"4e6577\",\"passphrase\":\"" PASS "\"}"));

    /* netd refusing is shown, and the sheet stays */
    calls_reset();
    g_connect_error = "passphrase must be 8..63 printable ASCII characters";
    tap("New");
    type(PASS);
    tap("JOIN");
    check("a refusal from netd is shown on the sheet",
          find_visible(app_body, "passphrase must be 8..63 printable ASCII characters") != NULL);
    check("and the sheet stays open", shows("JOIN"));
    g_connect_error = NULL;
    tap("CANCEL");

    /* ---- 5. an open network ---------------------------------------------------------------- */
    calls_reset();
    tap("Guest");
    check("open network: warns about encryption", find_containing(app_body, "not encrypted") != NULL);
    check("open network: no passphrase field", focused_text() == NULL && !kb_shown);
    tap("JOIN ANYWAY");
    check("JOIN ANYWAY sends allow_open and no passphrase",
          called("wifi.connect", "{\"ssid_hex\":\"4775657374\",\"allow_open\":true}"));

    /* ---- 6. a saved network ----------------------------------------------------------------- */
    calls_reset();
    tap("Home");
    check("saved network: join or forget", shows("JOIN") && shows("FORGET"));
    check("saved network: no keyboard", !kb_shown);
    tap("JOIN");
    check("saved network joins without a passphrase", called("wifi.connect", "{\"ssid_hex\":\"486f6d65\"}"));
    calls_reset();
    tap("Home");
    tap("FORGET");
    check("FORGET sends the exact SSID bytes", called("wifi.forget", "{\"ssid_hex\":\"486f6d65\"}"));

    /* ---- 7. unsupported ----------------------------------------------------------------------- */
    calls_reset();
    tap("W3");
    check("WPA3-only: explained", find_containing(app_body, "WPA3-only") != NULL);
    check("WPA3-only: nothing to join", !shows("JOIN") && shows("BACK"));
    tap("BACK");
    check("BACK returns", shows("Not connected"));
    check("nothing was sent for it", !called("wifi.connect", NULL));

    /* ---- 8. connected ------------------------------------------------------------------------- */
    g_status = STATUS_CONN;
    g_networks = NETS_CONN;
    tick();
    tick();
    tick();
    check("connected: headline", shows("Connected to Home"));
    check("connected: address and signal", shows("192.168.1.20 \xc2\xb7 Signal 4/4"));
    check("connected: badge", shows("CONNECTED"));
    check("connected: disconnect is usable", !disabled("DISCONNECT"));
    calls_reset();
    tap("DISCONNECT");
    check("DISCONNECT asks netd", called("wifi.disconnect", NULL));

    /* ---- 9. failure ---------------------------------------------------------------------------- */
    g_status = "{\"available\":true,\"enabled\":true,\"state\":\"failed\",\"reason\":\"auth_failed\","
               "\"ssid\":\"New\",\"scanning\":false,\"store\":\"ok\"}";
    tick();
    check("a wrong passphrase is shown as such", shows("Wrong passphrase for New"));

    /* ---- 10. brightness ------------------------------------------------------------------------- */
    g_bright = 60;
    tick();
    check("brightness shows 60 %", shows("60 %"));
    tap("+");
    check("+ raises to 70 through the shell", g_bright == 70 && shows("70 %"));
    tap("-");
    tap("-");
    check("- lowers by a step each time", g_bright == 50 && shows("50 %"));
    g_bright = 100;
    tick();
    check("at 100 the + is disabled", disabled("+") && !disabled("-"));
    g_bright = 10;
    tick();
    check("at the floor the - is disabled", disabled("-") && !disabled("+"));
    {
        int before = g_bright_sets;

        tap("-");
        check("a disabled - does nothing", g_bright_sets == before && g_bright == 10);
    }
    g_bright = -1;
    tick();
    check("no brightness control: both disabled", disabled("-") && disabled("+"));
    check("no brightness control: says so", find_containing(app_body, "no brightness control") != NULL);

    /* ---- 11. appearance ------------------------------------------------------------------------ */
    {
        const struct pos_theme_def *second = pos_theme_at(1);
        int marks = 0;
        lv_obj_t *mark;

        check("appearance: every theme is listed", pos_theme_count() >= 2 && shows(pos_theme_at(0)->name) &&
                                                       shows(second->name));
        mark = find_visible(app_body, "SELECTED");
        marks = mark != NULL;
        check("appearance: the current theme is marked", marks == 1 &&
                                                            lv_obj_get_parent(mark) == target_of(pos_theme_current_def()->name));
        tap(second->name);
        check("tapping a theme asks the shell for it", strcmp(g_theme_set, second->id) == 0 && g_mode_set[0] == '\0');
        check("and it is live", pos_theme_current_def() == second);
        mark = find_visible(app_body, "SELECTED");
        check("and the mark moved", mark && lv_obj_get_parent(mark) == target_of(second->name));
        tap("NIGHT");
        check("NIGHT asks the shell for the mode only", strcmp(g_mode_set, "night") == 0 && g_theme_set[0] == '\0');
        check("and it is live", pos_theme_current_mode() == POS_MODE_NIGHT);
        tap("OUTDOOR");
        check("OUTDOOR", pos_theme_current_mode() == POS_MODE_OUTDOOR);
        check("appearance: every target is at least 64 px in Outdoor", small_targets(app_body) == 0);
        tap("NORMAL");
        tap(pos_theme_at(0)->name);
        check("back to the first theme in Normal", pos_theme_current_def() == pos_theme_at(0) &&
                                                       pos_theme_current_mode() == POS_MODE_NORMAL);
    }

    /* ---- 12. rotation --------------------------------------------------------------------------- */
    tick();
    check("rotation: three modes under Display", shows("Rotation") && shows("AUTOMATIC") && shows("PORTRAIT") &&
                                                     shows("LANDSCAPE"));
    check("rotation: Automatic with no keyboard says portrait, and why",
          find_containing(app_body, "Portrait: no keyboard detected") != NULL);
    check("rotation: Automatic is the accented mode", role_on(target_of("AUTOMATIC"), POS_STYLE_BUTTON_PRIMARY) &&
                                                         !role_on(target_of("LANDSCAPE"), POS_STYLE_BUTTON_PRIMARY));
    tap("LANDSCAPE");
    check("tapping LANDSCAPE stores the mode through the shell",
          g_rot_sets == 1 && g_rot_last == POCKETOS_ROTATION_LANDSCAPE);
    check("and says it takes effect when the Doors shell restarts",
          find_containing(app_body, "Showing portrait now. Landscape takes effect when the Doors shell restarts.") != NULL);
    check("and Landscape is now the accented mode", role_on(target_of("LANDSCAPE"), POS_STYLE_BUTTON_PRIMARY) &&
                                                        !role_on(target_of("AUTOMATIC"), POS_STYLE_BUTTON_PRIMARY));
    check("rotation: every target is at least 64 px", small_targets(app_body) == 0);
    tap("AUTOMATIC");
    check("back to Automatic: nothing pending", g_rot_last == POCKETOS_ROTATION_AUTOMATIC &&
                                                    find_containing(app_body, "takes effect") == NULL);
    g_orient.keyboard = POCKETOS_KEYBOARD_PRESENT;
    g_orient.landscape = g_orient.next_landscape = true;
    tick();
    check("a shell started landscape with a keyboard says so",
          find_containing(app_body, "Landscape, because a keyboard is attached.") != NULL);
    g_orient.mode_valid = false;
    tick();
    check("a stored value that is not a mode is explained",
          find_containing(app_body, "The stored rotation was not recognised, so Automatic is used.") != NULL);
    app_stop();

    printf("settings_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
