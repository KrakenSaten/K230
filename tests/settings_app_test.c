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
 * The app is hosted the way the shell hosts it (a header, then a padded
 * body), on the reference panel with its 30 px rounded corners, and the
 * keyboard's sheet takes its height off the content area when the app asks
 * for it, as the shell's does. Sections 13 on lay every screen out in
 * portrait and in landscape (DS 21, 22) and turn the display under the open
 * app.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/settings_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "brightness.h"
#include "pocketui.h"
#include "pos_keyboard.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* The status bar the shell gives this app in the display's orientation
 * (ui/shell/chrome.h, DS section 30), so the frame built here is the one
 * shell.c builds: 56 px in portrait, 32 px under an app in landscape. */
#define STATUS_H chrome_height(chrome_resolve(app_settings.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
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
    g_orient.applying = g_orient.next_landscape != g_orient.landscape;
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

/* The keyboard as the shell has it: one sheet at the foot of the screen, and
 * the content area, where the app's body lives, losing the sheet's height
 * while it is up (shell.c). Done is called by the test (kb_done). */
static lv_obj_t *g_content;
static lv_obj_t *g_keyboard;

static void content_size(void)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();

    lv_obj_set_size(g_content, g->width, g->height - STATUS_H - (kb_shown ? POS_KB_H : 0));
    lv_obj_set_pos(g_content, 0, STATUS_H);
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    kb_shown = 1;
    kb_done = on_done;
    kb_user = user;
    content_size();
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    kb_shown = 0;
    kb_done = NULL;
    pos_keyboard_hide(g_keyboard);
    content_size();
}

int pocketos_shell_keyboard_visible(void) { return kb_shown; }

/* brightness.c is not linked; the clamp is the one piece the stub needs. */
int brightness_clamp_percent(int pct)
{
    return pct < BRIGHTNESS_MIN_PCT ? BRIGHTNESS_MIN_PCT : pct > BRIGHTNESS_MAX_PCT ? BRIGHTNESS_MAX_PCT : pct;
}

/* ---- display, finger, keys ------------------------------------------------------ */

static uint8_t draw_buf[PANEL_H * 40 * 4]; /* the long side, either way up */
static lv_display_t *disp;
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
/* A full list: twelve networks, one with the longest SSID there is (32 bytes
 * of wide capitals) and one nearly as long, so the list is longer than any
 * screen and its names are longer than any row. */
#define LONG_SSID "ABCDEFGHIJKLMNOPQRSTUVWXYZ-12345"
#define LONG_SSID2 "Kjellerstua-5G-extender-2nd-flr"
#define NET_WPA2(name, hex, bars) \
    "{\"ssid\":\"" name "\",\"ssid_hex\":\"" hex "\",\"signal_bars\":" #bars ",\"security\":\"wpa2\",\"supported\":true," \
    "\"needs_passphrase\":true,\"saved\":false,\"connected\":false}"
#define NETS_MANY "{\"scanning\":false,\"age_s\":2,\"hidden_count\":1,\"networks\":[" \
    "{\"ssid\":\"Home\",\"ssid_hex\":\"486f6d65\",\"signal_bars\":4,\"security\":\"wpa2\",\"supported\":true,\"needs_passphrase\":true,\"saved\":true,\"connected\":true}," \
    NET_WPA2(LONG_SSID, "4142434445464748494a4b4c4d4e4f505152535455565758595a2d3132333435", 4) "," \
    NET_WPA2(LONG_SSID2, "4b6a656c6c6572737475612d35472d657874656e6465722d326e642d666c72", 3) "," \
    "{\"ssid\":\"Guest\",\"ssid_hex\":\"4775657374\",\"signal_bars\":3,\"security\":\"open\",\"supported\":true,\"needs_passphrase\":false,\"saved\":false,\"connected\":false}," \
    "{\"ssid\":\"W3\",\"ssid_hex\":\"5733\",\"signal_bars\":2,\"security\":\"wpa3\",\"supported\":false,\"needs_passphrase\":true,\"saved\":false,\"connected\":false}," \
    NET_WPA2("New", "4e6577", 2) "," NET_WPA2("Net 7", "4e65742037", 2) "," NET_WPA2("Net 8", "4e65742038", 2) "," \
    NET_WPA2("Net 9", "4e65742039", 1) "," NET_WPA2("Net 10", "4e6574203130", 1) "," \
    NET_WPA2("Net 11", "4e6574203131", 1) "," NET_WPA2("Net 12", "4e6574203132", 1) "]}"
/* The longest refusal netd gives a join (services/netd), which wraps in
 * Outdoor type across a landscape field. */
#define LONG_REFUSAL "network not in the last scan; scan again, or pass hidden=true"

/* The shell's own frame (ui/shell/shell.c, app_open): a header row, then a
 * body that grows into what is left, padded the same way. */
static lv_obj_t *app_root;

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);

    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_settings.create(app_body);
    pump(80);
}

/* The shell's app_close(): hide the keyboard, destroy, delete the root. */
static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_settings.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
}

/* The display the shell would open: the reference panel with corner squares
 * of `corner` px, turned to `rotation`, the geometry handed to PocketUI and
 * the content area below the status bar sized to it, less the keyboard's
 * sheet when that is up. Called with the app open, it is the body changing
 * shape under a running app. */
static void use_display(enum pos_rotation rotation, int32_t corner)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { corner, corner, corner, corner },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    /* The lifted finger's last point could be off the turned display, which
     * LVGL warns about on every read. */
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    content_size();
    pump(60);
}

/* ---- layout: where things are, in either orientation ---------------------------------- */

/* NULL-safe, so that a layout that lost an object fails its checks and the
 * run goes on, rather than stopping in an LVGL assert that never returns. */
static void area_of(lv_obj_t *obj, lv_area_t *a)
{
    if (!obj) {
        lv_area_set(a, 0, 0, -1, -1);
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static int within(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* On the display and outside every rounded corner's square (pos_display.h). */
static int is_safe(const lv_area_t *a)
{
    return pos_display_rect_is_safe(pocketui_display_geometry(), a->x1, a->y1, a->x2, a->y2);
}

/* Where the app may put anything: the body's content box. */
static void body_box(lv_area_t *b)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, b);
}

/* How far the foot corner squares reach above the body's foot. */
static int32_t foot_inset(void)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    lv_area_t b;

    body_box(&b);
    return LV_MAX(0, b.y2 - (g->height - LV_MAX(g->corners.bottom_left, g->corners.bottom_right)) + 1);
}

static void check_rect(const char *what, lv_obj_t *obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a;

    checks++;
    area_of(obj, &a);
    if (!obj || a.x1 != x1 || a.y1 != y1 || a.x2 != x2 || a.y2 != y2) {
        failed++;
        printf("FAIL %s: is %d..%d x %d..%d, want %d..%d x %d..%d\n", what, (int)a.x1, (int)a.x2, (int)a.y1,
               (int)a.y2, (int)x1, (int)x2, (int)y1, (int)y2);
    } else {
        printf("ok   %s\n", what);
    }
}

/* The app's frame and the box that scrolls the screen on show inside it. */
static lv_obj_t *frame_obj(void)
{
    return app_body && lv_obj_get_child_count(app_body) ? lv_obj_get_child(app_body, 0) : NULL;
}

static lv_obj_t *screen_obj(void)
{
    lv_obj_t *f = frame_obj();

    return f && lv_obj_get_child_count(f) ? lv_obj_get_child(f, 0) : NULL;
}

/* The nearest ancestor that scrolls: what a finger drags to bring obj into
 * view, and what clips it. */
static lv_obj_t *scroller_of(lv_obj_t *obj)
{
    lv_obj_t *p = obj ? lv_obj_get_parent(obj) : NULL;

    while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
        p = lv_obj_get_parent(p);
    }
    return p;
}

/* Every visible thing a finger can press: clickable, with a handler. */
static int collect_targets(lv_obj_t *obj, lv_obj_t **out, int n, int max)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return n;
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(obj) > 0 && n < max) {
        out[n++] = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n = collect_targets(lv_obj_get_child(obj, i), out, n, max);
    }
    return n;
}

static int count_objects(lv_obj_t *obj)
{
    uint32_t i;
    int n = 1;

    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n += count_objects(lv_obj_get_child(obj, i));
    }
    return n;
}

/* Every visible label drawn whole across: inside its parent's width, so no
 * text runs past the box that holds it. */
static int labels_fit(lv_obj_t *obj)
{
    uint32_t i;
    int bad = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t l;
        lv_area_t p;

        /* A one-line field scrolls its own text sideways; that is its job. */
        if (lv_obj_check_type(lv_obj_get_parent(obj), &lv_textarea_class)) {
            return 0;
        }
        area_of(obj, &l);
        area_of(lv_obj_get_parent(obj), &p);
        if (l.x1 < p.x1 || l.x2 > p.x2) {
            printf("     label \"%.40s\" %d..%d outside its box %d..%d\n", lv_label_get_text(obj), (int)l.x1,
                   (int)l.x2, (int)p.x1, (int)p.x2);
            bad++;
        }
        return bad;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        bad += labels_fit(lv_obj_get_child(obj, i));
    }
    return bad;
}

/* The screen on show, checked whole: every box that scrolls lies in the body
 * and the safe area, so nothing scrolled is ever drawn into a rounded corner
 * or under the keyboard; every target is a finger's size, on no other target,
 * and can be brought wholly into view inside its scroller; every label fits
 * its box; and the body itself never scrolls (the app scrolls inside it). */
static void check_screen(const char *what)
{
    static lv_obj_t *t[96];
    char msg[200];
    lv_area_t box;
    lv_area_t sheet;
    int n;
    int i;
    int j;
    int small = 0;
    int overlap = 0;
    int unreachable = 0;
    int unsafe = 0;

    body_box(&box);
    area_of(g_keyboard, &sheet);
    n = collect_targets(screen_obj(), t, 0, 96);
    snprintf(msg, sizeof(msg), "%s: has targets", what);
    check(msg, n > 0);
    for (i = 0; i < n; i++) {
        lv_obj_t *s = scroller_of(t[i]);
        lv_area_t a;
        lv_area_t sa;

        area_of(t[i], &a);
        if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN) {
            printf("     small target %dx%d\n", (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
            small++;
        }
        for (j = i + 1; j < n; j++) {
            lv_area_t b;

            area_of(t[j], &b);
            /* a row holds no other target; a field's wrapper is not one */
            if (overlaps(&a, &b)) {
                printf("     targets overlap: %d..%d x %d..%d and %d..%d x %d..%d\n", (int)a.x1, (int)a.x2,
                       (int)a.y1, (int)a.y2, (int)b.x1, (int)b.x2, (int)b.y1, (int)b.y2);
                overlap++;
            }
        }
        area_of(s, &sa);
        if (!s || !within(&sa, &box) || !is_safe(&sa) || (kb_shown && overlaps(&sa, &sheet))) {
            printf("     scroller %d..%d x %d..%d not inside the body %d..%d x %d..%d and the safe area\n",
                   (int)sa.x1, (int)sa.x2, (int)sa.y1, (int)sa.y2, (int)box.x1, (int)box.x2, (int)box.y1,
                   (int)box.y2);
            unsafe++;
        }
    }
    for (i = 0; i < n; i++) {
        lv_obj_t *s = scroller_of(t[i]);
        lv_area_t a;
        lv_area_t sa;
        int32_t sy = lv_obj_get_scroll_y(s);

        lv_obj_scroll_to_view_recursive(t[i], LV_ANIM_OFF);
        area_of(t[i], &a);
        area_of(s, &sa);
        if (!within(&a, &sa)) {
            printf("     target %d..%d x %d..%d cannot be scrolled wholly into %d..%d x %d..%d\n", (int)a.x1,
                   (int)a.x2, (int)a.y1, (int)a.y2, (int)sa.x1, (int)sa.x2, (int)sa.y1, (int)sa.y2);
            unreachable++;
        }
        lv_obj_scroll_to_y(s, sy, LV_ANIM_OFF);
    }
    pump(20);
    snprintf(msg, sizeof(msg), "%s: every target is at least 64 x 64 px", what);
    check(msg, small == 0);
    snprintf(msg, sizeof(msg), "%s: no target lies on another", what);
    check(msg, overlap == 0);
    snprintf(msg, sizeof(msg), "%s: everything scrolls inside the body, the safe area and clear of the keyboard",
             what);
    check(msg, unsafe == 0);
    snprintf(msg, sizeof(msg), "%s: every target can be scrolled wholly into view", what);
    check(msg, unreachable == 0);
    snprintf(msg, sizeof(msg), "%s: every label fits its box", what);
    check(msg, labels_fit(screen_obj()) == 0);
    snprintf(msg, sizeof(msg), "%s: the body itself does not scroll", what);
    check(msg, lv_obj_get_scroll_top(app_body) <= 0 && lv_obj_get_scroll_bottom(app_body) <= 0);
}

/* Whether obj is wholly on show: inside its scroller's box as it is scrolled
 * now, and inside the body. */
static int in_view(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t s;
    lv_area_t b;

    if (!obj) {
        return 0;
    }
    area_of(obj, &a);
    area_of(scroller_of(obj), &s);
    body_box(&b);
    return lv_area_get_height(&a) > 0 && within(&a, &s) && within(&a, &b);
}

/* A finger drawn dy pixels across obj in small moves, the way a scroll
 * reaches LVGL from the panel, then time for the scroll to come to rest. */
static void drag(lv_obj_t *obj, int32_t dy)
{
    lv_area_t a;
    int32_t y0;
    int step;

    if (!obj) {
        return;
    }
    area_of(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    y0 = a.y1 + lv_area_get_height(&a) / 2;
    finger_point.y = y0;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (step = 1; step <= 20; step++) {
        finger_point.y = y0 + dy * step / 20;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(1500);
}

/* ---- every screen, laid out ------------------------------------------------------------- */

/* The panel holding the label with this text. */
static lv_obj_t *panel_of(const char *text)
{
    lv_obj_t *o = find_visible(app_body, text);

    while (o && lv_obj_get_parent(o) && !lv_obj_has_flag(lv_obj_get_parent(o), LV_OBJ_FLAG_SCROLLABLE)) {
        if (lv_obj_get_style_border_width(o, LV_PART_MAIN) > 0) {
            return o;
        }
        o = lv_obj_get_parent(o);
    }
    return o;
}

/* Fingers drag the scroller holding obj until obj is wholly in view. */
static int scroll_to(lv_obj_t *obj, int32_t dy)
{
    int i;

    for (i = 0; i < 12 && obj && !in_view(obj); i++) {
        drag(scroller_of(obj), dy);
    }
    return in_view(obj);
}

/* How much of obj is on show in its scroller, in pixels of height. */
static int32_t shown_h(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t s;
    int32_t y1;
    int32_t y2;

    area_of(obj, &a);
    area_of(scroller_of(obj), &s);
    y1 = LV_MAX(a.y1, s.y1);
    y2 = LV_MIN(a.y2, s.y2);
    return obj && y2 >= y1 ? y2 - y1 + 1 : 0;
}

static void open_sheet(const char *row)
{
    tap(row);
}

/* A sheet without a field: its actions reachable, beside the text when wide
 * and under it when tall. */
static void check_plain_sheet(const char *what, const char *row, const char *first_action, int wide)
{
    char msg[200];
    lv_area_t t;
    lv_area_t b;

    open_sheet(row);
    snprintf(msg, sizeof(msg), "%s: opens without the keyboard", what);
    check(msg, shows(first_action) && !kb_shown && focused_text() == NULL);
    check_screen(what);
    area_of(find_visible(app_body, "NETWORK"), &t);
    area_of(target_of(first_action), &b);
    snprintf(msg, sizeof(msg), "%s: %s is %s the network's name", what, first_action, wide ? "beside" : "below");
    check(msg, wide ? b.x1 > t.x2 && b.y1 < t.y2 + 64 : b.y1 > t.y2 && b.x1 == t.x1);
    snprintf(msg, sizeof(msg), "%s: the whole sheet is in view without scrolling", what);
    check(msg, in_view(target_of(first_action)) && in_view(find_visible(app_body, "NETWORK")));
}

/* One display, one mode: the main screen with a long list, scrolled by a
 * finger; the passphrase sheet above the keyboard with a short passphrase
 * refused and a long refusal from netd; and the sheets without a field. */
static void check_orientation(const char *name, enum pos_rotation rotation, int32_t corner, const char *mode)
{
    char what[160];
    char why[128];
    int wide = rotation == POS_ROTATION_270;
    lv_area_t box;
    lv_area_t w;
    lv_area_t d;
    lv_area_t ap;
    lv_area_t a;
    lv_area_t b;
    lv_obj_t *field;
    lv_obj_t *caption;

    use_display(rotation, corner);
    pos_theme_apply(NULL, mode, why, sizeof(why));
    netd_down = 0;
    g_connect_error = NULL;
    g_status = STATUS_CONN;
    g_networks = NETS_MANY;
    g_bright = 60;
    app_start();
    tick();

    /* ---- the main screen */
    body_box(&box);
    area_of(panel_of("WI-FI"), &w);
    area_of(panel_of("DISPLAY"), &d);
    area_of(panel_of("APPEARANCE"), &ap);
    snprintf(what, sizeof(what), "[%s] main: Display is %s Wi-Fi", name, wide ? "beside" : "below");
    check(what, wide ? d.x1 > w.x2 && d.y1 == w.y1 : d.y1 > w.y2 && d.x1 == w.x1 && d.x2 == w.x2);
    snprintf(what, sizeof(what), "[%s] main: Appearance is below Display, in line with it", name);
    check(what, ap.y1 == d.y2 + 1 + 22 && ap.x1 == d.x1 && ap.x2 == d.x2);
    if (wide) {
        snprintf(what, sizeof(what), "[%s] main: two columns across the body, 22 px apart, none under 528 px",
                 name);
        check(what, w.x1 == box.x1 && d.x2 == box.x2 && d.x1 - w.x2 - 1 == 22 && lv_area_get_width(&w) >= 528 &&
                        lv_area_get_width(&d) >= 528);
        area_of(scroller_of(panel_of("WI-FI")), &a);
        area_of(scroller_of(panel_of("DISPLAY")), &b);
        snprintf(what, sizeof(what), "[%s] main: each column scrolls itself, to the foot less the corners", name);
        check(what, scroller_of(panel_of("WI-FI")) != scroller_of(panel_of("DISPLAY")) && a.y1 == box.y1 &&
                        b.y1 == box.y1 && a.y2 == box.y2 - foot_inset() && b.y2 == box.y2 - foot_inset());
    } else {
        snprintf(what, sizeof(what), "[%s] main: one column the body's width", name);
        check(what, w.x1 == box.x1 && w.x2 == box.x2 && d.y1 - w.y2 - 1 == 22);
        area_of(screen_obj(), &a);
        snprintf(what, sizeof(what), "[%s] main: it scrolls to the foot less the corners", name);
        check(what, scroller_of(panel_of("WI-FI")) == screen_obj() && a.y1 == box.y1 &&
                        a.y2 == box.y2 - foot_inset() && a.x1 == box.x1 && a.x2 == box.x2);
    }
    snprintf(what, sizeof(what), "[%s] main", name);
    check_screen(what);

    /* long names: cut short inside their rows, never onto a badge */
    area_of(target_of("Home"), &a);
    area_of(find_visible(app_body, "CONNECTED"), &b);
    area_of(find_visible(app_body, "Home"), &d);
    snprintf(what, sizeof(what), "[%s] main: the connected row's name and badge are apart, both in the row", name);
    check(what, within(&b, &a) && within(&d, &a) && d.x2 < b.x1);
    area_of(target_of(LONG_SSID), &a);
    area_of(find_visible(app_body, LONG_SSID), &b);
    snprintf(what, sizeof(what), "[%s] main: the longest SSID stays inside its row", name);
    check(what, within(&b, &a) && lv_area_get_height(&a) == POCKETUI_ROW_H + 8);

    /* a finger scrolls the list to its end; in the wide shape the other
     * column stays where it was */
    snprintf(what, sizeof(what), "[%s] main: a finger scrolls to the last network", name);
    check(what, scroll_to(target_of("Net 12"), wide ? -250 : -500));
    snprintf(what, sizeof(what), "[%s] main: %s", name,
             wide ? "the Display column did not move" : "the body itself did not scroll");
    check(what, wide ? lv_obj_get_scroll_y(scroller_of(panel_of("DISPLAY"))) == 0
                     : lv_obj_get_scroll_top(app_body) <= 0);
    snprintf(what, sizeof(what), "[%s] main: a finger scrolls to the last control", name);
    check(what, scroll_to(target_of("NIGHT"), wide ? -250 : -500));
    tap("NIGHT");
    snprintf(what, sizeof(what), "[%s] main: and it works there", name);
    check(what, pos_theme_current_mode() == POS_MODE_NIGHT);
    pos_theme_apply(NULL, mode, why, sizeof(why));
    snprintf(what, sizeof(what), "[%s] main, scrolled", name);
    check_screen(what);
    tick();

    /* ---- the passphrase sheet above the keyboard */
    open_sheet("New");
    field = pos_input_focused();
    snprintf(what, sizeof(what), "[%s] sheet: opens with the keyboard up and the field focused", name);
    check(what, kb_shown && focused_text() != NULL && shows("JOIN"));
    pump(60);
    snprintf(what, sizeof(what), "[%s] sheet: the field is in view above the keyboard", name);
    check(what, in_view(field));
    area_of(field, &a);
    area_of(find_visible(app_body, "New"), &b);
    if (wide) {
        snprintf(what, sizeof(what), "[%s] sheet: beside the network's name, which is in view too", name);
        check(what, a.x1 > b.x2 && in_view(find_visible(app_body, "New")) && in_view(find_visible(app_body, "NETWORK")));
    } else {
        snprintf(what, sizeof(what), "[%s] sheet: below the network's name", name);
        check(what, a.y1 > b.y2);
    }
    snprintf(what, sizeof(what), "[%s] sheet: the field is no narrower than in portrait", name);
    check(what, lv_area_get_width(&a) >= 484);
    snprintf(what, sizeof(what), "[%s] sheet above the keyboard", name);
    check_screen(what);

    type("short");
    if (kb_done) {
        kb_done(kb_user);
        pump(60);
    }
    caption = find_visible(app_body, "At least 8 characters");
    area_of(caption, &b);
    area_of(field, &a);
    snprintf(what, sizeof(what), "[%s] sheet: a refused passphrase's caption is read whole above the keyboard", name);
    check(what, caption && in_view(caption) && b.y1 > a.y2);
    snprintf(what, sizeof(what), "[%s] sheet: with the field in view, still focused", name);
    check(what, in_view(field) && pos_input_focused() == field && kb_shown);

    g_connect_error = LONG_REFUSAL;
    type("\b\b\b\b\b" PASS);
    if (kb_done) {
        kb_done(kb_user);
        pump(60);
    }
    g_connect_error = NULL;
    caption = find_visible(app_body, LONG_REFUSAL);
    snprintf(what, sizeof(what), "[%s] sheet: netd's longest refusal is read whole above the keyboard", name);
    check(what, caption && in_view(caption));
    /* It fits one line across the field in either shape and mode but portrait
     * Outdoor, where it takes two and there is room for them. */
    snprintf(what, sizeof(what), "[%s] sheet: and the field stays in view with it, still holding the passphrase",
             name);
    check(what, in_view(field) && shown_h(field) == POCKETUI_ROW_H && focused_text() &&
                    strcmp(focused_text(), PASS) == 0);
    snprintf(what, sizeof(what), "[%s] sheet with an error above the keyboard", name);
    check_screen(what);
    tap("CANCEL");
    snprintf(what, sizeof(what), "[%s] sheet: CANCEL is reachable and closes it", name);
    check(what, !shows("JOIN") && !kb_shown && shows("WI-FI"));

    /* ---- the sheets without a field */
    snprintf(what, sizeof(what), "[%s] connected sheet", name);
    check_plain_sheet(what, "Home", "CANCEL", wide);
    netd_down = 1;
    tap("FORGET");
    netd_down = 0;
    snprintf(what, sizeof(what), "[%s] connected sheet: a failure is shown in view", name);
    check(what, in_view(find_visible(app_body, "Wi-Fi did not answer")) && shows("DISCONNECT"));
    snprintf(what, sizeof(what), "[%s] connected sheet with a failure", name);
    check_screen(what);
    tap("CANCEL");
    snprintf(what, sizeof(what), "[%s] open sheet", name);
    check_plain_sheet(what, "Guest", "CANCEL", wide);
    tap("CANCEL");
    snprintf(what, sizeof(what), "[%s] unsupported sheet", name);
    check_plain_sheet(what, "W3", "BACK", wide);
    tap("BACK");
    snprintf(what, sizeof(what), "[%s] back on the main screen", name);
    check(what, shows("WI-FI") && !kb_shown);
    app_stop();
    pos_theme_apply(NULL, "normal", why, sizeof(why));
}

int main(void)
{
    lv_indev_t *finger;

    /* Line by line, so a run that stops says where. */
    setvbuf(stdout, NULL, _IOLBF, 0);
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
    g_keyboard = pos_keyboard_create(lv_screen_active());
    /* The unit's panel, as the shell opens it in portrait. */
    use_display(POS_ROTATION_0, PANEL_CORNER);

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
    check("and says it is being applied, and that Doors opens on the launcher",
          find_containing(app_body, "Turning to landscape now. The screen goes dark for a moment and Doors opens "
                                    "on the launcher.") != NULL);
    check("and Landscape is now the accented mode", role_on(target_of("LANDSCAPE"), POS_STYLE_BUTTON_PRIMARY) &&
                                                        !role_on(target_of("AUTOMATIC"), POS_STYLE_BUTTON_PRIMARY));
    check("rotation: every target is at least 64 px", small_targets(app_body) == 0);
    tap("AUTOMATIC");
    check("back to Automatic: nothing pending", g_rot_last == POCKETOS_ROTATION_AUTOMATIC &&
                                                    find_containing(app_body, "Turning to") == NULL);
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
    g_orient.mode = POCKETOS_ROTATION_AUTOMATIC;
    g_orient.mode_valid = true;
    g_orient.keyboard = POCKETOS_KEYBOARD_UNKNOWN;
    g_orient.landscape = g_orient.next_landscape = g_orient.applying = false;

    /* ---- 13. every screen in both orientations and both modes (DS 21, 22) --------------------- */
    check_orientation("portrait", POS_ROTATION_0, PANEL_CORNER, "normal");
    check_orientation("landscape", POS_ROTATION_270, PANEL_CORNER, "normal");
    check_orientation("portrait, Outdoor", POS_ROTATION_0, PANEL_CORNER, "outdoor");
    check_orientation("landscape, Outdoor", POS_ROTATION_270, PANEL_CORNER, "outdoor");
    check_orientation("portrait, square corners", POS_ROTATION_0, 0, "normal");
    check_orientation("landscape, square corners", POS_ROTATION_270, 0, "normal");

    /* ---- 14. where things are, to the pixel ----------------------------------------------------- */
    /* Portrait is the v0.0.10 layout: the body's 528 px column from 152 to
     * the foot, which the corner squares of the unit's panel (DS 21.1) bring
     * up by 10 px and square corners do not. Landscape: two columns of 585 px
     * with the 22 px panel gap, and the sheet's field at the top of its right
     * half above the keyboard. */
    g_status = STATUS_CONN;
    g_networks = NETS_MANY;
    {
        static const int32_t corners[] = { PANEL_CORNER, 0 };
        size_t k;

        for (k = 0; k < 2; k++) {
            int32_t c = corners[k];
            int32_t lift = c > 20 ? c - 20 : 0;
            char what[120];

            use_display(POS_ROTATION_0, c);
            app_start();
            tick();
            snprintf(what, sizeof(what), "portrait %d px corners: the screen scrolls in the body's column", (int)c);
            check_rect(what, screen_obj(), 20, 152, 547, 1211 - lift);
            snprintf(what, sizeof(what), "portrait %d px corners: Wi-Fi's panel is the column's width at its top",
                     (int)c);
            {
                lv_area_t a;

                area_of(panel_of("WI-FI"), &a);
                check(what, a.x1 == 20 && a.x2 == 547 && a.y1 == 152);
            }
            snprintf(what, sizeof(what), "portrait %d px corners: the switch", (int)c);
            check_rect(what, target_of("ON"), 407, 199, 526, 262);
            tap("New");
            snprintf(what, sizeof(what), "portrait %d px corners: the field above the keyboard", (int)c);
            check_rect(what, pos_input_focused(), 41, 277, 526, 340);
            snprintf(what, sizeof(what), "portrait %d px corners: the sheet scrolls in the body above the keyboard",
                     (int)c);
            check_rect(what, screen_obj(), 20, 152, 547, 1231 - POS_KB_H - 20);
            app_stop();

            use_display(POS_ROTATION_270, c);
            app_start();
            tick();
            /* From row 128, not 152: the 32 px COMPACT bar of DS section 30
             * above the same header and padding; the keyboard's edge at the
             * foot does not move. */
            snprintf(what, sizeof(what), "landscape %d px corners: Wi-Fi's column", (int)c);
            check_rect(what, scroller_of(panel_of("WI-FI")), 20, 128, 604, 547 - lift);
            snprintf(what, sizeof(what), "landscape %d px corners: Display and Appearance's column", (int)c);
            check_rect(what, scroller_of(panel_of("DISPLAY")), 627, 128, 1211, 547 - lift);
            snprintf(what, sizeof(what), "landscape %d px corners: the switch", (int)c);
            check_rect(what, target_of("ON"), 464, 175, 583, 238);
            tap("New");
            pump(60);
            snprintf(what, sizeof(what), "landscape %d px corners: the sheet scrolls in the body above the keyboard",
                     (int)c);
            check_rect(what, screen_obj(), 20, 128, 1211, 567 - POS_KB_H - 20);
            snprintf(what, sizeof(what), "landscape %d px corners: the field at the top of the sheet's right half",
                     (int)c);
            check_rect(what, pos_input_focused(), 626, 149, 1190, 212);
            app_stop();
        }
    }

    /* ---- 15. the display turning under the open app ------------------------------------------ */
    use_display(POS_ROTATION_0, PANEL_CORNER);
    g_bright = 60;
    app_start();
    tick();
    {
        static lv_obj_t *t[96];
        int objects = count_objects(app_body);
        int targets = collect_targets(screen_obj(), t, 0, 96);
        lv_obj_t *mark;
        int i;
        int same = 1;

        for (i = 0; i < 6; i++) {
            lv_area_t w;
            lv_area_t d;

            use_display(i % 2 == 0 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            area_of(panel_of("WI-FI"), &w);
            area_of(panel_of("DISPLAY"), &d);
            mark = find_visible(app_body, "SELECTED");
            if (count_objects(app_body) != objects || collect_targets(screen_obj(), t, 0, 96) != targets ||
                !shows("60 %") || !shows("Connected to Home") || !shows(LONG_SSID) || !mark ||
                lv_obj_get_parent(mark) != target_of(pos_theme_at(0)->name) ||
                !role_on(target_of("AUTOMATIC"), POS_STYLE_BUTTON_PRIMARY) ||
                (i % 2 == 0 ? !(d.x1 > w.x2) : !(d.y1 > w.y2))) {
                printf("     turn %d: objects %d/%d targets %d\n", i, count_objects(app_body), objects,
                       collect_targets(screen_obj(), t, 0, 96));
                same = 0;
            }
        }
        check("turning six times keeps every object once, every value, and reshapes each time", same);
    }
    tap("New");
    type("correct");
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(60);
    check("a sheet open while the display turns stays open, keyboard up",
          shows("JOIN") && kb_shown && lv_obj_get_child_count(screen_obj()) == 1u);
    check("with what was typed, and the focus, where they were",
          focused_text() && strcmp(focused_text(), "correct") == 0);
    check("and the field in view above the keyboard", in_view(pos_input_focused()));
    type(" horse 9");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(60);
    check("turned back: still the field, in view", in_view(pos_input_focused()) && focused_text() &&
                                                       strcmp(focused_text(), PASS) == 0);
    calls_reset();
    tap("JOIN");
    check("what was typed across both turns is what JOIN sends",
          called("wifi.connect", "{\"ssid_hex\":\"4e6577\",\"passphrase\":\"" PASS "\"}"));
    check("and nothing else carried it", passphrase_only_in_connect() && !shows("JOIN") && !kb_shown);

    tap("New");
    type("short");
    if (kb_done) {
        kb_done(kb_user);
        pump(60);
    }
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(60);
    check("an error shown while the display turns is still read whole",
          in_view(find_visible(app_body, "At least 8 characters")) && in_view(pos_input_focused()));
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(60);
    check("and turned back", in_view(find_visible(app_body, "At least 8 characters")));

    /* The shell takes the keyboard away while an alert is up (shell_alarm.c):
     * the body grows under the sheet, and nothing on it moves or is lost. */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pocketos_shell_keyboard_hide();
    pump(60);
    check("the keyboard taken away: the field and its caption stay in view",
          in_view(pos_input_focused()) && in_view(find_visible(app_body, "At least 8 characters")));
    type("er!");
    check("and typing still reaches the field", focused_text() && strcmp(focused_text(), "shorter!") == 0);
    check("the whole sheet fits the landscape body without the keyboard",
          in_view(target_of("JOIN")) && in_view(find_visible(app_body, "NETWORK")) &&
              lv_obj_get_scroll_y(screen_obj()) == 0);
    calls_reset();
    tap("CANCEL");
    check("CANCEL in landscape: back to the main screen in two columns, nothing sent",
          shows("WI-FI") && call_count == 0 &&
              scroller_of(panel_of("WI-FI")) != scroller_of(panel_of("DISPLAY")));

    /* Back from the shell's header with a passphrase typed: the app closes,
     * the keyboard goes, and nothing is sent. */
    tap("New");
    type("abandoned");
    calls_reset();
    app_stop();
    check("closed from the sheet: nothing was sent", !called("wifi.connect", NULL) && !kb_shown);

    /* ---- 16. closed and opened again, both ways up ------------------------------------------ */
    /* Settings keeps nothing: what it shows on opening is what the shell and
     * netd hold, whichever way up it opens. */
    {
        char why[128];
        int i;

        use_display(POS_ROTATION_270, PANEL_CORNER);
        app_start();
        tick();
        tap("+");
        tap(pos_theme_at(2)->name);
        tap("LANDSCAPE");
        app_stop();
        for (i = 0; i < 4; i++) {
            lv_obj_t *mark;

            use_display(i % 2 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            app_start();
            tick();
            mark = find_visible(app_body, "SELECTED");
            check(i % 2 ? "reopened in landscape: brightness, theme and rotation as they were set"
                        : "reopened in portrait: brightness, theme and rotation as they were set",
                  shows("70 %") && mark && lv_obj_get_parent(mark) == target_of(pos_theme_at(2)->name) &&
                      role_on(target_of("LANDSCAPE"), POS_STYLE_BUTTON_PRIMARY) && shows("Connected to Home"));
            app_stop();
        }
        pos_theme_apply(pos_theme_at(0)->id, "normal", why, sizeof(why));
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("every round leaves nothing behind", lv_obj_get_child_count(g_content) == 0u && !kb_shown);

    printf("settings_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
