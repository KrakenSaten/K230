/*
 * PacketShark in the running app, driven by a real LVGL pointer device, with
 * the real helper (tools/capture/pos-capture) behind it on the synthetic
 * source, and sysd's interface list scripted.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px corners, portrait and landscape, the body
 * under the 72 px header. Time is real: the helper runs on the wall clock.
 *
 * What is checked (PRODUCT_SPEC F1, S3, I2-I4 on the host; M3 acceptance):
 *   - setup with a link: READY, eth0 chosen, START enabled; without one: NO
 *     LINK, START disabled;
 *   - Wi-Fi (M10): off, not connected and connected (its network's name
 *     from netd, asked only while connected); Ethernet stays the default; a
 *     tap chooses Wi-Fi, a drop falls back and a rejoin chooses it again;
 *     Wi-Fi alone is READY and START asks the helper for wlan0;
 *   - START on a live interface this build cannot open: the reason on the
 *     setup banner, no capture, the helper gone;
 *   - START on the synthetic source: CAPTURING within the hello deadline,
 *     the header says so, counters rise; HOLD and RESUME; STOP (BY YOU);
 *     PACKETS says how many packets the helper holds; DISCARD asks first,
 *     then the setup returns and no helper is left;
 *   - FULL and NO LIMIT each ask first;
 *   - a crashing helper and a helper killed with SIGKILL: the error panel
 *     within a second, the reason with its signal, the shell usable, NEW
 *     CAPTURE back to setup; a missing helper is said on setup;
 *   - Back: closes a sheet, returns from PACKETS, asks before leaving a
 *     capture, and LEAVE goes home;
 *   - the capture bar's buttons are full targets inside the safe area in
 *     both orientations; the tabs are 56 px navigation rows;
 *   - twenty opens and closes with a capture running leave no helper, no
 *     timer and no object behind (I4).
 * PACKETSHARK_SHOTS=<dir> keeps a picture of each state (mockups 04, 05, 07).
 *
 * Needs: PACKETSHARK_HELPER, the path of tools/capture/pos-capture.
 * Built by ui/shell/CMakeLists.txt, run by tests/packetshark_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "app.h"
#include "chrome.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pocketui_audit.h"
#include "pos_theme.h"
#include "psk_app.h"
#include "psk_model.h"
#include "shell_ipc.h"

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define STATUS_H                                                                                  \
    chrome_height(chrome_resolve(app_packetshark.chrome,                                          \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_packetshark;
/* apps/packetshark/psk_app.h test seams */
extern lv_obj_t *psk_app_part(void *priv, const char *name);
extern void psk_app_cost(void *priv, int64_t *worst_poll_us, int64_t *worst_frame_us, int reset);

static int failed;
static int checks;
static char root[128];

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

/* ---- the shell's side --------------------------------------------------------------- */

static char g_hint[64];
static int g_home;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void)
{
    g_home++;
}

/* The header's hint, the mark that a companion is reading aside (it is
 * there for five seconds after any capture.* request: remote_request()
 * holds when). */
static bool hint_is(const char *want)
{
    size_t n = strlen(want);

    return strncmp(g_hint, want, n) == 0 && (g_hint[n] == '\0' || strcmp(g_hint + n, " · COMPANION") == 0);
}

/* The events the app raises (capture.state, capture.stats), kept in order as
 * the shell would pass them on; g_ev_front false stands for a unit that is
 * locked, where the shell passes nothing on and says so. */
#define EV_MAX 512
static struct {
    bool stats;
    char state[16];
    char reason[20];
    double seen;
    int keys;
    int64_t ms;
} g_ev[EV_MAX];
static int g_evn;
static int g_ev_refused;
static int g_ev_other;
static bool g_ev_front = true;

static int64_t ev_clock(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool pocketos_shell_app_in_front(void)
{
    return g_ev_front;
}

bool pocketos_shell_app_event(const char *event, struct cJSON *data)
{
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(data, "state");
    const cJSON *why = cJSON_GetObjectItemCaseSensitive(data, "reason");
    const cJSON *seen = cJSON_GetObjectItemCaseSensitive(data, "seen");

    if (!g_ev_front) {
        g_ev_refused++;
        cJSON_Delete(data);
        return false;
    }
    if (strcmp(event, "capture.state") != 0 && strcmp(event, "capture.stats") != 0) {
        g_ev_other++;
    } else if (g_evn < EV_MAX) {
        g_ev[g_evn].stats = strcmp(event, "capture.stats") == 0;
        snprintf(g_ev[g_evn].state, sizeof(g_ev[g_evn].state), "%s", cJSON_IsString(st) ? st->valuestring : "?");
        snprintf(g_ev[g_evn].reason, sizeof(g_ev[g_evn].reason), "%s", cJSON_IsString(why) ? why->valuestring : "?");
        g_ev[g_evn].seen = cJSON_IsNumber(seen) ? seen->valuedouble : -1;
        g_ev[g_evn].keys = cJSON_GetArraySize(data);
        g_ev[g_evn].ms = ev_clock();
        g_evn++;
    }
    cJSON_Delete(data);
    return true;
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
}

void pocketos_shell_keyboard_hide(void)
{
}

int pocketos_shell_keyboard_visible(void)
{
    return 0;
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = pocketui_display_geometry()->width > pocketui_display_geometry()->height;
    out->next_landscape = out->landscape;
}

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

/* sysd's interface list and netd's Wi-Fi, as the test says they are. */
static int g_link = 1;
static int g_wifi; /* 0 off, 1 up and not associated, 2 connected */
static int g_sysd_calls;
static int g_netd_calls;

cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params, int timeout_ms,
                              char *err, size_t errlen)
{
    cJSON *st;
    cJSON *net;
    cJSON *e;

    (void)timeout_ms;
    (void)err;
    (void)errlen;
    cJSON_Delete(params);
    if (strcmp(service, "netd") == 0 && strcmp(method, "wifi.status") == 0) {
        g_netd_calls++;
        st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "interface", "wlan0");
        cJSON_AddStringToObject(st, "state", g_wifi == 2 ? "connected" : g_wifi ? "disconnected" : "off");
        if (g_wifi == 2) {
            cJSON_AddStringToObject(st, "ssid", "Example Net");
        } else {
            cJSON_AddNullToObject(st, "ssid");
        }
        return st;
    }
    g_sysd_calls++;
    if (strcmp(service, "sysd") != 0 || strcmp(method, "system.status") != 0) {
        return NULL;
    }
    st = cJSON_CreateObject();
    net = cJSON_AddArrayToObject(st, "network");
    e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", "eth0");
    cJSON_AddStringToObject(e, "operstate", g_link ? "up" : "down");
    if (g_link) {
        cJSON_AddTrueToObject(e, "carrier");
        cJSON_AddStringToObject(e, "ipv4", "192.0.2.44");
    } else {
        cJSON_AddFalseToObject(e, "carrier");
        cJSON_AddNullToObject(e, "ipv4");
    }
    cJSON_AddItemToArray(net, e);
    e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", "wlan0");
    if (g_wifi == 2) {
        cJSON_AddStringToObject(e, "operstate", "up");
        cJSON_AddTrueToObject(e, "carrier");
        cJSON_AddStringToObject(e, "ipv4", "192.0.2.45");
    } else if (g_wifi == 1) {
        cJSON_AddStringToObject(e, "operstate", "dormant");
        cJSON_AddFalseToObject(e, "carrier");
        cJSON_AddNullToObject(e, "ipv4");
    } else {
        cJSON_AddStringToObject(e, "operstate", "down");
        cJSON_AddNullToObject(e, "carrier");
        cJSON_AddNullToObject(e, "ipv4");
    }
    cJSON_AddItemToArray(net, e);
    /* The driver's second, concurrent-mode interface: always down. */
    e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", "wlan1");
    cJSON_AddStringToObject(e, "operstate", "down");
    cJSON_AddNullToObject(e, "carrier");
    cJSON_AddNullToObject(e, "ipv4");
    cJSON_AddItemToArray(net, e);
    e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", "sit0");
    cJSON_AddStringToObject(e, "operstate", "down");
    cJSON_AddItemToArray(net, e);
    return st;
}

/* ---- display and finger ------------------------------------------------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 4];
static lv_display_t *disp;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

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

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pump(int ms)
{
    int64_t end = mono_ms() + ms;
    int64_t last = mono_ms();

    while (mono_ms() < end) {
        struct timespec d = { 0, 5 * 1000000L };
        int64_t now;

        nanosleep(&d, NULL);
        now = mono_ms();
        lv_tick_inc((uint32_t)(now - last));
        last = now;
        lv_timer_handler();
    }
}

/* Visible means: not hidden itself or through any parent inside the body. */
static lv_obj_t *find_label_in(lv_obj_t *obj, const char *text, bool prefix)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (prefix ? strncmp(t, text, strlen(text)) == 0 : strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = find_label_in(lv_obj_get_child(obj, i), text, prefix);

        if (f) {
            return f;
        }
    }
    return NULL;
}

/* The last match: a sheet's button over a bar's of the same word. */
static lv_obj_t *find_last_in(lv_obj_t *obj, const char *text)
{
    lv_obj_t *found = NULL;
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
        found = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = find_last_in(lv_obj_get_child(obj, i), text);

        if (f) {
            found = f;
        }
    }
    return found;
}

static lv_obj_t *shown(const char *prefix)
{
    return app_body ? find_label_in(app_body, prefix, true) : NULL;
}

static lv_obj_t *clickable_of(lv_obj_t *label)
{
    lv_obj_t *o = label;

    while (o && o != app_body) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_check_type(o, &lv_label_class)) {
            return o;
        }
        o = lv_obj_get_parent(o);
    }
    return NULL;
}

static bool enabled(const char *label)
{
    lv_obj_t *l = shown(label);
    lv_obj_t *o = l ? lv_obj_get_parent(l) : NULL;

    return o && lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(o, LV_STATE_DISABLED);
}

static lv_obj_t *containing_in(lv_obj_t *obj, const char *needle)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), needle)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = containing_in(lv_obj_get_child(obj, i), needle);

        if (f) {
            return f;
        }
    }
    return NULL;
}

/* For a failure: every visible label. */
static void dump_in(lv_obj_t *obj)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && lv_label_get_text(obj)[0]) {
        printf("     | %s\n", lv_label_get_text(obj));
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        dump_in(lv_obj_get_child(obj, i));
    }
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void tap(const char *label)
{
    lv_obj_t *l = shown(label);

    tap_obj(l ? clickable_of(l) : NULL);
}

static void tap_last(const char *label)
{
    lv_obj_t *l = app_body ? find_last_in(app_body, label) : NULL;

    tap_obj(l ? clickable_of(l) : NULL);
}

static bool wait_for(const char *prefix, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (shown(prefix)) {
            return true;
        }
        pump(20);
    }
    return shown(prefix) != NULL;
}

static const char *shot_orientation = "portrait";

static void shot(const char *name)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    const char *dir = getenv("PACKETSHARK_SHOTS");
    char path[300];
    lv_draw_buf_t *snap;
    unsigned char *rgb;
    uint32_t y;

    if (!dir || !*dir) {
        return;
    }
    pump(60);
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (!snap) {
        return;
    }
    rgb = malloc((size_t)snap->header.w * snap->header.h * 3);
    for (y = 0; rgb && y < snap->header.h; y++) {
        const unsigned char *src = (const unsigned char *)snap->data + (size_t)y * snap->header.stride;
        unsigned char *dst = rgb + (size_t)y * snap->header.w * 3;
        uint32_t x;

        for (x = 0; x < snap->header.w; x++) {
            dst[x * 3] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3];
        }
    }
    snprintf(path, sizeof(path), "%s/packetshark-%s-%s.png", dir, shot_orientation, name);
    if (rgb) {
        lodepng_encode24_file(path, rgb, snap->header.w, snap->header.h);
    }
    free(rgb);
    lv_draw_buf_destroy(snap);
#else
    (void)name;
#endif
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static pid_t child_pid(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = -1;

    while (d && (e = readdir(d)) != NULL) {
        char path[300];
        char buf[256];
        FILE *f;
        int pid;
        int ppid;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (fgets(buf, sizeof(buf), f)) {
            char *rp = strrchr(buf, ')');

            if (rp && sscanf(rp + 2, "%*c %d", &ppid) == 1 && sscanf(buf, "%d", &pid) == 1 &&
                ppid == getpid()) {
                found = pid;
            }
        }
        fclose(f);
    }
    if (d) {
        closedir(d);
    }
    return found;
}

/* ---- the app, hosted ------------------------------------------------------------------ */

static void use_display(enum pos_rotation rotation)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { PANEL_CORNER, PANEL_CORNER, PANEL_CORNER, PANEL_CORNER },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(20);
}

/* Landscape: the display is wider than it is tall. */
static bool landscape(void)
{
    return pocketui_display_geometry()->width > pocketui_display_geometry()->height;
}

/* The height a capture button must have: 64 px at the foot (DS §59.6), 56 in
 * the wide shape's top row, as mockup 08 draws it. */
static int32_t button_h(void)
{
    return landscape() ? 56 : 64;
}

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    /* As the shell does (shell.c): an app that draws its own top row in
     * landscape gets no header there, and its body starts at the top edge. */
    if (!(app_packetshark.header == POCKETOS_HEADER_NONE_LANDSCAPE && landscape())) {
        header = lv_obj_create(app_root);
        lv_obj_remove_style_all(header);
        lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    }
    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    lv_obj_update_layout(app_root);
    app_priv = app_packetshark.create(app_body);
    pump(60);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_packetshark.destroy(app_priv);
    t0 = mono_ms() - t0;
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
    return t0;
}

/* The interface START would capture on, or "" when none is chosen. */
static const char *chosen_iface(void)
{
    const struct psk_app *a = app_priv;

    return a && a->sel >= 0 && a->sel < a->nifs ? a->ifs[a->sel].name : "";
}

static void body_area(lv_area_t *a)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, a);
}

static bool target_ok(lv_obj_t *obj, int32_t min_h, const char *what)
{
    lv_area_t a;
    lv_area_t b;

    if (!obj) {
        printf("     %s: missing\n", what);
        return false;
    }
    body_area(&b);
    lv_obj_get_coords(obj, &a);
    if (lv_area_get_height(&a) < min_h || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN || a.x1 < b.x1 ||
        a.x2 > b.x2 || a.y2 > b.y2) {
        printf("     %s: %d,%d-%d,%d in body %d,%d-%d,%d\n", what, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
               (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2);
        return false;
    }
    if (!pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("     %s: in a corner at %d,%d-%d,%d\n", what, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
        return false;
    }
    return true;
}

/* ---- the captures folder (M8) ----------------------------------------------------------- */

static char captures_dir[300];

static int saved_files(void)
{
    DIR *d = opendir(captures_dir);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.' || strstr(e->d_name, ".part") != NULL;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static bool saved_modes_ok(void)
{
    DIR *d = opendir(captures_dir);
    struct dirent *e;
    struct stat sb;
    bool ok = stat(captures_dir, &sb) == 0 && (sb.st_mode & 0777) == 0700;

    while (d && (e = readdir(d)) != NULL) {
        char path[600];

        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", captures_dir, e->d_name);
        ok = ok && stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600 && sb.st_size > 24;
    }
    if (d) {
        closedir(d);
    }
    return ok;
}

/* ---- journeys -------------------------------------------------------------------------- */

#define CHECK(what, cond)                                                                          \
    do {                                                                                           \
        char name_[200];                                                                           \
        snprintf(name_, sizeof(name_), "%s: %s", shot_orientation, what);                          \
        check(name_, cond);                                                                        \
    } while (0)

static void setup_screens(void)
{
    setenv("POCKETOS_CAPTURE_SOURCE", "", 1);
    g_link = 1;
    app_start();
    CHECK("setup: READY with a link, nothing captured yet",
          shown("READY") && shown("NOTHING IS CAPTURED UNTIL YOU START") && hint_is("NOT CAPTURING"));
    CHECK("eth0 is listed with its link and address", shown("eth0") && shown("LINK UP") &&
                                                          shown("ETHERNET · 192.0.2.44"));
    CHECK("Wi-Fi off: wlan0 listed, not connected, eth0 chosen",
          shown("wlan0") && shown("WI-FI · NOT CONNECTED") && strcmp(chosen_iface(), "eth0") == 0);
    CHECK("sit0 is not listed", shown("sit0") == NULL);
    CHECK("the safe defaults: HEADERS, 15 MIN, EVERYTHING, OFF",
          shown("HEADERS") && shown("15 MIN") && shown("EVERYTHING") && shown("OFF"));
    CHECK("the privacy note before START", shown("Packets stay in memory on this device") != NULL);
    CHECK("START CAPTURE is enabled", enabled("START CAPTURE"));
    {
        lv_obj_t *s = shown("START CAPTURE");
        lv_obj_t *l = shown("LIVE");

        CHECK("START is a full target inside the safe area",
              target_ok(s ? lv_obj_get_parent(s) : NULL, button_h(), "START"));
        CHECK("a tab is a 56 px navigation row", target_ok(l ? clickable_of(l) : NULL, 56, "LIVE tab"));
    }
    shot("setup");

    /* FULL and NO LIMIT ask first. */
    tap("Stored bytes");
    CHECK("FULL asks first", shown("Keep full packets?") != NULL);
    shot("sheet-full");
    tap("FULL");
    CHECK("and is FULL after yes", shown("FULL") && !shown("Keep full packets?"));
    tap("Stored bytes");
    CHECK("back to HEADERS without asking", shown("HEADERS") && !shown("Keep full packets?"));
    tap("Stop after");
    CHECK("15 MIN to 60 MIN", shown("60 MIN") != NULL);
    tap("Stop after");
    CHECK("NO LIMIT asks first", shown("Capture without a time limit?") != NULL);
    tap("CANCEL");
    CHECK("and CANCEL keeps 60 MIN", shown("60 MIN") && !shown("Capture without a time limit?"));
    tap("Capture filter");
    CHECK("the filter presets cycle", shown("IP AND ARP") != NULL);
    {
        /* Not tapped: a tap on a row that takes none falls through to the
         * page under it. Its row simply takes no tap. */
        lv_obj_t *k = shown("Promiscuous mode");

        CHECK("promiscuous mode is not offered: OFF, its row takes no tap",
              k && shown("OFF") && !lv_obj_has_flag(lv_obj_get_parent(k), LV_OBJ_FLAG_CLICKABLE));
    }

    /* A live interface this host build cannot open. */
    tap("START CAPTURE");
    wait_for("! ", 5000);
    CHECK("START on an interface that cannot be opened: the reason on setup",
          shown("! ") && shown("READY") && !shown("CAPTURING"));
    if (!shown("! ")) {
        dump_in(app_body);
    }
    pump(300);
    CHECK("and the helper is gone", no_child());
    app_stop();

    g_link = 0;
    app_start();
    CHECK("no link: NO LINK, START disabled", shown("NO LINK") && shown("CONNECT ETH0 OR JOIN WI-FI") &&
                                                  shown("ETHERNET · UP") == NULL && !enabled("START CAPTURE"));
    shot("no-link");
    g_link = 1;
    pump(2200); /* the app asks sysd every 2 s while setup shows */
    CHECK("a cable arrives: READY without reopening", shown("READY") && enabled("START CAPTURE"));
    app_stop();
}

/* M10: Wi-Fi in managed mode, chosen like Ethernet when it has a carrier. */
static void wifi_screens(void)
{
    int netd_before;

    setenv("POCKETOS_CAPTURE_SOURCE", "", 1);
    g_link = 1;
    g_wifi = 2;
    app_start();
    CHECK("Wi-Fi connected: listed with its network and address",
          shown("WI-FI · Example Net · 192.0.2.45") && shown("CONNECTED"));
    CHECK("Ethernet stays the default when both have a link", strcmp(chosen_iface(), "eth0") == 0);
    CHECK("the concurrent-mode twin is listed, down", shown("wlan1") != NULL && shown("DOWN") != NULL);
    tap("wlan1");
    CHECK("a tap on an interface that is down chooses nothing", strcmp(chosen_iface(), "eth0") == 0);
    tap("wlan0");
    CHECK("a tap chooses Wi-Fi: READY, START enabled",
          strcmp(chosen_iface(), "wlan0") == 0 && shown("READY") && enabled("START CAPTURE"));
    shot("wifi");

    g_wifi = 1;
    pump(2200);
    CHECK("Wi-Fi drops: NOT CONNECTED, the choice falls back to eth0",
          shown("WI-FI · NOT CONNECTED") && !shown("CONNECTED") && strcmp(chosen_iface(), "eth0") == 0 &&
              shown("READY"));
    netd_before = g_netd_calls;
    pump(2200);
    CHECK("netd is not asked while Wi-Fi has no carrier", g_netd_calls == netd_before);
    g_wifi = 2;
    pump(2200);
    CHECK("Wi-Fi rejoins: chosen again", strcmp(chosen_iface(), "wlan0") == 0);
    app_stop();

    g_link = 0;
    g_wifi = 1;
    app_start();
    CHECK("no cable, Wi-Fi not joined: NO LINK, START disabled",
          shown("NO LINK") && shown("CONNECT ETH0 OR JOIN WI-FI") && !enabled("START CAPTURE"));
    g_wifi = 2;
    pump(2200);
    CHECK("Wi-Fi alone: chosen, READY", strcmp(chosen_iface(), "wlan0") == 0 && shown("READY") &&
                                            enabled("START CAPTURE"));
    /* The host helper has no live backend: START reaches it with wlan0 and
     * comes back refused, as for eth0 above. */
    tap("START CAPTURE");
    wait_for("! ", 5000);
    CHECK("START on Wi-Fi asks the helper and shows its answer on setup", shown("! ") && !shown("CAPTURING"));
    pump(300);
    CHECK("and the helper is gone", no_child());
    app_stop();
    g_link = 1;
    g_wifi = 0;
}

/* ---- capture.status, as the shell would ask it (app.h ipc_request; H2) ----------------- */

static cJSON *remote_status(void)
{
    cJSON *res = NULL;
    const char *msg = NULL;

    if (!app_packetshark.ipc_request ||
        app_packetshark.ipc_request(app_priv, "capture.status", NULL, &res, &msg) != 0) {
        cJSON_Delete(res);
        return NULL;
    }
    return res;
}

static bool remote_is(const char *state, const char *reason)
{
    cJSON *o = remote_status();
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "state");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "reason");
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "available")) && cJSON_IsString(s) &&
              cJSON_IsString(w) && strcmp(s->valuestring, state) == 0 && strcmp(w->valuestring, reason) == 0;

    if (!ok) {
        printf("     capture.status says %s / %s\n", cJSON_IsString(s) ? s->valuestring : "(none)",
               cJSON_IsString(w) ? w->valuestring : "(none)");
    }
    cJSON_Delete(o);
    return ok;
}

/* The answer's packets and bytes are the ones on the screen: SESSION's cells
 * on LIVE, or LAST SESSION's on the error panel, printed as the screen
 * prints them. Asked and compared in one pass, so no poll comes between. */
static bool remote_matches_screen(bool error_panel)
{
    struct psk_app *a = app_priv;
    cJSON *o = remote_status();
    const cJSON *seen = cJSON_GetObjectItemCaseSensitive(o, "seen");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(o, "bytes");
    const cJSON *kd = cJSON_GetObjectItemCaseSensitive(o, "kernel_drops");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "iface_drops");
    char b1[48];
    char b2[48];
    char b3[48];
    lv_obj_t *const *cell = error_panel ? a->last : a->cell;
    bool ok;

    if (!cJSON_IsNumber(seen) || !cJSON_IsNumber(bytes) || !cJSON_IsNumber(kd) || !cJSON_IsNumber(id)) {
        cJSON_Delete(o);
        return false;
    }
    psk_fmt_count((uint64_t)seen->valuedouble, b1, sizeof(b1));
    psk_fmt_bytes((uint64_t)bytes->valuedouble, b2, sizeof(b2));
    psk_fmt_count((uint64_t)(kd->valuedouble + id->valuedouble), b3, sizeof(b3));
    ok = seen->valuedouble > 0 && strcmp(lv_label_get_text(cell[0]), b1) == 0 &&
         strcmp(lv_label_get_text(cell[1]), b2) == 0 && strcmp(lv_label_get_text(cell[2]), b3) == 0;
    if (!ok) {
        printf("     the screen: %s, %s, %s; the answer: %s, %s, %s\n", lv_label_get_text(cell[0]),
               lv_label_get_text(cell[1]), lv_label_get_text(cell[2]), b1, b2, b3);
    }
    cJSON_Delete(o);
    return ok;
}

/* A label holding `needle` anywhere in it, within `ms`. */
static bool wait_has(const char *needle, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (containing_in(app_body, needle)) {
            return true;
        }
        pump(20);
    }
    return containing_in(app_body, needle) != NULL;
}

static void capture_journey(void)
{
    lv_obj_t *l;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    CHECK("the simulator's stand-in is ready", shown("synth0") && shown("SIMULATED TRAFFIC · NO NETWORK") &&
                                                   hint_is("SIMULATED"));
    CHECK("capture.status on setup: idle", remote_is("idle", "none") && no_child());
    tap("START CAPTURE");
    CHECK("START: capturing within the hello deadline", wait_for("CAPTURING", 3500));
    CHECK("the header says so", hint_is("CAPTURING · SYNTH0"));
    pump(1500);
    l = containing_in(app_body, " PKT · ");
    CHECK("counters rise: the readout names the newest second's packets", l && !shown("WAITING FOR PACKETS"));
    CHECK("HOLD and STOP are full targets", target_ok(clickable_of(shown("HOLD")), button_h(), "HOLD") &&
                                                target_ok(clickable_of(shown("STOP")), button_h(), "STOP"));
    shot("capturing");
    CHECK("capture.status while capturing: its state, and the screen's own packets, bytes and drops",
          remote_is("capturing", "none") && remote_matches_screen(false));
    pump(120);
    CHECK("and the screen says a companion is reading: in the header, or in the wide shape's own top row",
          strstr(g_hint, " · COMPANION") != NULL &&
              (!((struct psk_app *)app_priv)->wide ||
               strncmp(lv_label_get_text(((struct psk_app *)app_priv)->status_text), "COMPANION · ", 13) == 0));
    tap("HOLD");
    CHECK("HOLD", wait_for("HOLD: ", 2000) && hint_is("HOLD · SYNTH0") && shown("RESUME"));
    pump(600);
    CHECK("capture.status on HOLD: its state, and the screen's figures", remote_is("hold", "none") &&
                                                                              remote_matches_screen(false));
    shot("hold");
    tap("RESUME");
    CHECK("RESUME", wait_for("CAPTURING", 2000) && shown("HOLD") && !shown("RESUME"));
    tap("PACKETS");
    pump(200);
    pump(400);
    CHECK("PACKETS lists the packets the helper holds", containing_in(app_body, " PACKETS") != NULL &&
                                                            containing_in(app_body, "FOLLOWING THE NEWEST") != NULL);
    shot("packets");
    CHECK("Back from PACKETS returns to LIVE", app_packetshark.back(app_priv) == 1 && shown("TRAFFIC"));
    tap("STOP");
    /* (The wide shape's line begins with the mark that a companion is
     * reading while the checks above are fresh, so it is looked for whole.) */
    CHECK("STOP: stopped by you", wait_has("SYNTH0 · BY YOU", 2000) && hint_is("STOPPED · SYNTH0"));
    CHECK("SAVE is offered on a stopped capture", shown("SAVE") && enabled("SAVE") && enabled("DISCARD"));
    pump(300);
    CHECK("capture.status when stopped: by the user, and the screen's figures",
          remote_is("stopped", "user") && remote_matches_screen(false));
    {
        struct psk_app *a = app_priv;
        uint32_t req = a->model->want_req;
        uint32_t view = a->view_id;
        enum psk_tab tab = a->cur;
        int i;

        for (i = 0; i < 50; i++) {
            cJSON_Delete(remote_status());
        }
        CHECK("asking fifty times asks for no rows and moves nothing on the screen (the lint holds the rest)",
              a->model->want_req == req && a->view_id == view && a->cur == tab && shown("STOPPED"));
    }
    shot("stopped");
    tap("DISCARD");
    CHECK("DISCARD asks first, and says nothing was saved",
          shown("Discard this capture?") && containing_in(app_body, "Nothing has been saved.") != NULL);
    tap("STAY");
    CHECK("STAY keeps it", shown("STOPPED") && !shown("Discard this capture?"));

    /* SAVE (M8): asks first, says where the file goes and who can read it,
     * writes one file, and the capture is then a saved one. */
    CHECK("nothing is in the captures folder before SAVE", saved_files() == 0);
    tap("SAVE");
    CHECK("SAVE asks first: the folder, and that /root is readable over SSH (ADR-016 rule 16)",
          shown("Save this capture?") && containing_in(app_body, "Captures on this device") != NULL &&
              containing_in(app_body, "read over SSH") != NULL);
    shot("m8-sheet-save");
    tap("CANCEL");
    pump(300);
    CHECK("CANCEL writes nothing", !shown("Save this capture?") && saved_files() == 0 && enabled("SAVE"));
    tap("SAVE");
    tap_last("SAVE");
    CHECK("SAVE: saved, with the packets, the size and the file's name",
          wait_for("SAVED · ", 5000) && containing_in(app_body, "capture-") != NULL);
    CHECK("one file in the captures folder, 0600 in a 0700 folder", saved_files() == 1 && saved_modes_ok());
    CHECK("the capture is still there, SAVE is done, and nothing else changed",
          shown("SAVED") && !enabled("SAVED") && enabled("DISCARD") && shown("STOPPED") &&
              containing_in(app_body, "This capture is saved.") != NULL);
    shot("m8-saved");
    tap("DISCARD");
    CHECK("DISCARD now says the file stays", containing_in(app_body, "The file you saved stays in Captures.") != NULL);
    tap_last("DISCARD");
    CHECK("DISCARD: setup again", wait_for("START CAPTURE", 1000) && shown("READY"));
    pump(400);
    CHECK("and the helper has gone", no_child());
    CHECK("capture.status after DISCARD: idle again", remote_is("idle", "none"));

    /* The saved captures (ADR-016 rule 10): listed with size and age,
     * deleted only after a confirmation. */
    CHECK("setup counts the saved capture", shown("SAVED CAPTURES") && containing_in(psk_app_part(app_priv, "saves"), "1 · "));
    tap_obj(psk_app_part(app_priv, "saves"));
    CHECK("its list names the file, its size and its age",
          shown("Saved captures") && containing_in(psk_app_part(app_priv, "save0"), "capture-") != NULL &&
              containing_in(psk_app_part(app_priv, "save0"), "B · JUST NOW") != NULL);
    shot("m8-saved-list");
    tap_obj(psk_app_part(app_priv, "save0"));
    CHECK("a tap asks before deleting", shown("Delete this capture?") && containing_in(app_body, "cannot be undone"));
    tap("KEEP");
    CHECK("KEEP deletes nothing", saved_files() == 1 && !shown("Delete this capture?"));
    tap_obj(psk_app_part(app_priv, "save0"));
    tap_last("DELETE");
    pump(200);
    CHECK("DELETE removes the file and the row", saved_files() == 0 && containing_in(app_body, "No capture has been saved"));
    CHECK("Back closes the list", app_packetshark.back(app_priv) == 1 && !shown("Saved captures"));
    CHECK("and setup says NONE", containing_in(psk_app_part(app_priv, "saves"), "NONE") != NULL);

    /* Back while capturing asks; LEAVE goes home. */
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    CHECK("Back during a capture asks first", app_packetshark.back(app_priv) == 1 && shown("Leave PacketShark?"));
    shot("sheet-leave");
    CHECK("Back again closes the sheet", app_packetshark.back(app_priv) == 1 && !shown("Leave PacketShark?"));
    app_packetshark.back(app_priv);
    g_home = 0;
    tap("LEAVE");
    CHECK("LEAVE goes home", g_home == 1);
    {
        int64_t took = app_stop();

        CHECK("closing a capturing app takes at most 500 ms", took <= 500);
    }
    CHECK("and leaves no helper", no_child());
}

/* ---- capture.hold, capture.stop and the events (H3) ------------------------------------ */

/* One request to the app, as the shell would pass it. Returns the error
 * code (0: none) and leaves the result's state and "changing" in the two
 * outputs. */
static int remote_call(const char *method, const char *params_json, char *state, size_t statelen, int *changing)
{
    cJSON *params = params_json ? cJSON_Parse(params_json) : NULL;
    cJSON *res = NULL;
    const char *msg = NULL;
    int rc = app_packetshark.ipc_request(app_priv, method, params, &res, &msg);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(res, "state");
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(res, "changing");

    if (state && statelen) {
        snprintf(state, statelen, "%s", cJSON_IsString(st) ? st->valuestring : rc ? (msg ? msg : "") : "");
    }
    if (changing) {
        *changing = cJSON_IsBool(ch) ? cJSON_IsTrue(ch) : -1;
    }
    cJSON_Delete(params);
    cJSON_Delete(res);
    return rc;
}

/* The capture.state events since `from`, as "idle,capturing,hold". */
static const char *states_since(int from, char *buf, size_t n)
{
    int i;
    size_t at = 0;

    buf[0] = '\0';
    for (i = from; i < g_evn; i++) {
        if (!g_ev[i].stats) {
            at += (size_t)snprintf(buf + at, at < n ? n - at : 0, "%s%s", at ? "," : "", g_ev[i].state);
            if (strcmp(g_ev[i].reason, "none") != 0) {
                at += (size_t)snprintf(buf + at, at < n ? n - at : 0, "/%s", g_ev[i].reason);
            }
        }
    }
    return buf;
}

/* Every capture.stats carries the state of the capture.state before it, and
 * no second holds more events than the emitter's bound. */
static bool events_sound(void)
{
    const char *last = NULL;
    int i;
    int j;

    for (i = 0; i < g_evn; i++) {
        int in_second = 0;

        if (!g_ev[i].stats) {
            last = g_ev[i].state;
            if (g_ev[i].keys != 5 || g_ev[i].seen != -1) {
                printf("     a capture.state event with %d keys\n", g_ev[i].keys);
                return false;
            }
        } else if (!last || strcmp(last, g_ev[i].state) != 0) {
            printf("     capture.stats in state %s after capture.state %s\n", g_ev[i].state, last ? last : "(none)");
            return false;
        }
        /* Of its own kind, in the second that starts with it: four by the
         * emitter's gap, and one more for a clock read a moment apart. */
        for (j = i; j < g_evn && g_ev[j].ms - g_ev[i].ms < 1000; j++) {
            in_second += g_ev[j].stats == g_ev[i].stats;
        }
        if (in_second > 1000 / PSK_REMOTE_STATE_GAP_MS + 1) {
            printf("     %d %s events within a second\n", in_second, g_ev[i].stats ? "stats" : "state");
            return false;
        }
    }
    return true;
}

static int stats_between(int from, int to)
{
    int i;
    int n = 0;

    for (i = from; i < to && i < g_evn; i++) {
        n += g_ev[i].stats;
    }
    return n;
}

static void remote_control(void)
{
    struct psk_app *a;
    char st[64];
    char seq[256];
    int ch;
    int at;
    int n0;
    int64_t t0;
    cJSON *o;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    g_evn = 0;
    g_ev_other = 0;
    g_ev_refused = 0;
    g_ev_front = true;
    app_start();
    a = app_priv;
    pump(200);
    CHECK("opening the app says once that it is on setup",
          g_evn == 1 && !g_ev[0].stats && strcmp(g_ev[0].state, "idle") == 0);
    CHECK("capture.hold on setup: nothing is capturing",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "nothing is capturing") == 0);
    CHECK("capture.stop on setup: nothing is capturing, and no helper is started",
          remote_call("capture.stop", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY && no_child());

    /* The setup rows after START (the capture is what START asked for). */
    psk_option(a, 1);
    CHECK("before START a tap on Stop after moves it (15 to 60 minutes)", psk_limit_min[a->limit_idx] == 60);
    psk_option(a, 0);
    CHECK("before START a tap on Keep asks before FULL", shown("Keep full packets?") && a->keep == PPC_KEEP_HEADERS);
    psk_sheet_answer(a, true);
    CHECK("and FULL is chosen when the sheet is answered", a->keep == PPC_KEEP_FULL && a->sheet_kind == PSK_SHEET_NONE);
    psk_do(a, PSK_ACT_START);
    CHECK("START is on its way", a->starting);
    CHECK("capture.hold while starting: nothing is capturing",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY);
    {
        int limit = a->limit_idx;
        enum ppc_cfilter cf = a->cfilter;
        char shown_keep[32];
        char shown_limit[32];
        char shown_filter[48];

        snprintf(shown_keep, sizeof(shown_keep), "%s", lv_label_get_text(a->opt_val[0]));
        snprintf(shown_limit, sizeof(shown_limit), "%s", lv_label_get_text(a->opt_val[1]));
        snprintf(shown_filter, sizeof(shown_filter), "%s", lv_label_get_text(a->opt_val[2]));
        /* FULL back to HEADERS is one tap with no sheet: the tap that used
         * to show HEADERS over a capture that stores everything. */
        psk_option(a, 0);
        CHECK("after START a tap on Keep changes neither the choice nor the row",
              a->keep == PPC_KEEP_FULL && strcmp(lv_label_get_text(a->opt_val[0]), shown_keep) == 0);
        psk_option(a, 1);
        CHECK("nor one on Stop after", a->limit_idx == limit && strcmp(lv_label_get_text(a->opt_val[1]), shown_limit) == 0 &&
                                           a->sheet_kind == PSK_SHEET_NONE);
        psk_option(a, 2);
        CHECK("nor one on Filter", a->cfilter == cf && strcmp(lv_label_get_text(a->opt_val[2]), shown_filter) == 0);
        a->keep = PPC_KEEP_HEADERS;
        a->sheet_kind = PSK_SHEET_FULL; /* a sheet that was up when START was tapped, answered late */
        psk_sheet_answer(a, true);
        CHECK("nor a FULL sheet answered late", a->keep == PPC_KEEP_HEADERS);
        a->keep = PPC_KEEP_FULL;
    }
    CHECK("capturing", wait_for("CAPTURING", 3500));
    {
        /* Figures are raised while somebody reads (capture.status), and only then. */
        int k;

        for (k = 0; k < 3; k++) {
            cJSON_Delete(remote_status());
            pump(780);
        }
    }
    o = remote_status();
    CHECK("the capture is the one START asked for, and is said so: FULL, 60 minutes",
          cJSON_GetObjectItem(o, "limit_s")->valuedouble == 3600 &&
              strcmp(cJSON_GetObjectItem(o, "keep")->valuestring, "full") == 0);
    cJSON_Delete(o);

    /* A sheet over the capture bar: no finger reaches HOLD or STOP there,
     * and no request does. */
    tap("FILTER");
    pump(200);
    CHECK("the filter sheet is up over the bar", psk_fsheet_shown(a));
    CHECK("capture.hold and capture.stop under a sheet are refused, and the capture runs on",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "a sheet is open on the unit") == 0 &&
              remote_call("capture.stop", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "a sheet is open on the unit") == 0 && remote_is("capturing", "none"));
    CHECK("Back closes the sheet", app_packetshark.back(app_priv) == 1 && !psk_fsheet_shown(a));
    app_packetshark.back(app_priv);
    CHECK("Back over a capture asks before leaving", shown("Leave PacketShark?") != NULL);
    CHECK("capture.stop under the LEAVE sheet is refused too",
          remote_call("capture.stop", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "a sheet is open on the unit") == 0);
    CHECK("Back again closes it, and the capture is as it was",
          app_packetshark.back(app_priv) == 1 && !shown("Leave PacketShark?") && remote_is("capturing", "none"));
    pump(300);
    CHECK("while capturing the figures are raised about once a second", stats_between(0, g_evn) >= 2 &&
                                                                         stats_between(0, g_evn) <= 5);

    /* HOLD, RESUME and STOP by request are the buttons'. */
    CHECK("capture.hold without on, or with one that is not true or false, is refused and holds nothing",
          remote_call("capture.hold", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_INVALID_PARAMS &&
              remote_call("capture.hold", "{\"on\": \"yes\"}", st, sizeof(st), &ch) == POCKETIPC_ERR_INVALID_PARAMS &&
              remote_call("capture.hold", "{\"on\": 1}", st, sizeof(st), &ch) == POCKETIPC_ERR_INVALID_PARAMS &&
              remote_call("capture.hold", "[true]", st, sizeof(st), &ch) == POCKETIPC_ERR_INVALID_PARAMS &&
              shown("HOLD") && !shown("RESUME"));
    CHECK("capture.hold on: taken, and the answer is the state it was asked in",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == 0 && strcmp(st, "capturing") == 0 &&
              ch == 1);
    CHECK("the screen is the one a tap on HOLD gives",
          wait_for("HOLD: ", 2000) && hint_is("HOLD · SYNTH0") && shown("RESUME") && remote_is("hold", "none"));
    CHECK("capture.hold on again: already held, nothing to do",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == 0 && strcmp(st, "hold") == 0 && ch == 0);
    pump(600);
    CHECK("capture.hold off: taken", remote_call("capture.hold", "{\"on\": false}", st, sizeof(st), &ch) == 0 &&
                                         strcmp(st, "hold") == 0 && ch == 1);
    CHECK("the screen is the one a tap on RESUME gives", wait_for("CAPTURING", 2000) && shown("HOLD") && !shown("RESUME"));
    CHECK("capture.hold off again: already capturing, nothing to do",
          remote_call("capture.hold", "{\"on\": false}", st, sizeof(st), &ch) == 0 && strcmp(st, "capturing") == 0 &&
              ch == 0);
    pump(400);
    CHECK("a method that is not served is unknown and does nothing",
          remote_call("capture.discard", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_UNKNOWN_METHOD &&
              remote_call("capture.save", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_UNKNOWN_METHOD &&
              remote_call("capture.start", "{\"iface\": \"synth0\", \"limit_s\": 300}", st, sizeof(st), &ch) ==
                  POCKETIPC_ERR_UNKNOWN_METHOD &&
              remote_is("capturing", "none") && !shown("Discard this capture?"));
    CHECK("capture.stop: taken", remote_call("capture.stop", NULL, st, sizeof(st), &ch) == 0 &&
                                     strcmp(st, "capturing") == 0 && ch == 1);
    CHECK("the screen is the one a tap on STOP gives",
          wait_for("SYNTH0 · BY YOU", 2000) && hint_is("STOPPED · SYNTH0") && shown("SAVE") &&
              enabled("SAVE") && enabled("DISCARD") && remote_is("stopped", "user"));
    CHECK("the stopped capture keeps its packets and its helper", !no_child() && psk_capture_present(a));
    CHECK("capture.stop and capture.hold on a stopped capture: nothing is capturing",
          remote_call("capture.stop", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              remote_call("capture.hold", "{\"on\": false}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              shown("STOPPED"));
    pump(700);
    at = g_evn;
    CHECK("the states were raised in the order they happened, each once",
          strcmp(states_since(0, seq, sizeof(seq)), "idle,capturing,hold,capturing,stopped/user") == 0);
    if (strcmp(seq, "idle,capturing,hold,capturing,stopped/user") != 0) {
        printf("     %s\n", seq);
    }
    CHECK("the last event is the stopped capture's figures, with its totals",
          g_evn > 0 && g_ev[g_evn - 1].stats && strcmp(g_ev[g_evn - 1].state, "stopped") == 0 &&
              g_ev[g_evn - 1].seen > 0);
    CHECK("figures never came before their state, a state event holds no figure, and no second was crowded",
          events_sound());
    pump(2500);
    CHECK("a stopped capture raises nothing more", g_evn == at);
    CHECK("the app raised no other event", g_ev_other == 0);

    /* A unit that is locked: the shell passes nothing on. The change is not
     * lost and not repeated. */
    g_ev_front = false;
    tap("DISCARD");
    tap_last("DISCARD");
    CHECK("DISCARD, tapped while nobody can be told", wait_for("START CAPTURE", 1500));
    pump(1300);
    CHECK("nothing was passed on, and nothing was even made to be dropped", g_evn == at && g_ev_refused == 0);
    g_ev_front = true;
    pump(600);
    CHECK("in front again: setup is said once", g_evn == at + 1 && !g_ev[at].stats && strcmp(g_ev[at].state, "idle") == 0);
    pump(1200);
    CHECK("and not again", g_evn == at + 1);

    /* Leaving with a capture: said once, then nothing of the app remains. */
    n0 = g_evn;
    tap("START CAPTURE");
    CHECK("a second capture", wait_for("CAPTURING", 3500));
    pump(500);
    t0 = app_stop();
    CHECK("leaving ends it within 500 ms and leaves no helper", t0 <= 500 && no_child());
    CHECK("leaving a capture is said: ended, left, and it is the last word",
          g_evn > n0 && !g_ev[g_evn - 1].stats && strcmp(g_ev[g_evn - 1].state, "ended") == 0 &&
              strcmp(g_ev[g_evn - 1].reason, "left") == 0);
    at = g_evn;
    pump(1500);
    CHECK("a closed app raises nothing", g_evn == at);

    /* Leaving from setup says nothing: there was no capture to end. */
    app_start();
    pump(200);
    at = g_evn;
    app_stop();
    CHECK("leaving setup says nothing", g_evn == at);
    CHECK("the event log did not overflow", g_evn < EV_MAX);
}

/* ---- capture.request_start: a sheet that asks, and the reader's mark (H4) ----------------- */

static int ask(const char *params, char *msg, size_t n)
{
    return remote_call("capture.request_start", params, msg, n, NULL);
}

static bool sheet_is_request(void)
{
    const struct psk_app *a = app_priv;

    return a->sheet_kind == PSK_SHEET_REQUEST && !lv_obj_has_flag(a->sheet, LV_OBJ_FLAG_HIDDEN);
}

/* The last event is the request's outcome, raised once, and setup is plain
 * setup again afterwards. */
static bool outcome_was(int before, const char *reason)
{
    cJSON *o;
    bool plain;

    if (g_evn != before + 1 || g_ev[g_evn - 1].stats || strcmp(g_ev[g_evn - 1].state, "idle") != 0 ||
        strcmp(g_ev[g_evn - 1].reason, reason) != 0) {
        printf("     %d events since, the last %s/%s\n", g_evn - before, g_evn ? g_ev[g_evn - 1].state : "",
               g_evn ? g_ev[g_evn - 1].reason : "");
        return false;
    }
    o = remote_status();
    plain = strcmp(cJSON_GetObjectItem(o, "reason")->valuestring, "none") == 0;
    cJSON_Delete(o);
    return plain;
}

static void remote_request(void)
{
    struct psk_app *a;
    char st[96];
    int ch;
    int at;
    cJSON *o;
    const char *good = "{\"iface\": \"synth0\", \"limit_s\": 300, \"client\": \"Flip8\"}";

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    g_evn = 0;
    g_ev_other = 0;
    g_ev_front = true;
    app_start();
    a = app_priv;
    pump(200);
    CHECK("nobody has asked anything: the header carries no mark", strcmp(g_hint, "SIMULATED") == 0 && !a->reading);

    /* What cannot be asked. */
    CHECK("a request without its interface or its limit, or with a client that is not text, is refused as malformed",
          ask(NULL, st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": \"synth0\"}", st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"limit_s\": 300}", st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": \"synth0\", \"limit_s\": \"300\"}", st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": 0, \"limit_s\": 300}", st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"client\": 7}", st, sizeof(st)) == POCKETIPC_ERR_INVALID_PARAMS &&
              !sheet_is_request());
    CHECK("an interface the setup screen does not offer cannot be asked for",
          ask("{\"iface\": \"eth9\", \"limit_s\": 300}", st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "that interface is not offered") == 0 &&
              ask("{\"iface\": \"\", \"limit_s\": 300}", st, sizeof(st)) == POCKETIPC_ERR_POLICY && !sheet_is_request());
    CHECK("no limit, or a limit that is not one of the list, cannot be asked for",
          ask("{\"iface\": \"synth0\", \"limit_s\": 0}", st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "that time limit is not offered") == 0 &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 120}", st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 86400}", st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 300.5}", st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              ask("{\"iface\": \"synth0\", \"limit_s\": -300}", st, sizeof(st)) == POCKETIPC_ERR_POLICY && !sheet_is_request());
    CHECK("capture.request_cancel with no request shown is an error",
          remote_call("capture.request_cancel", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "no request is shown") == 0);
    CHECK("none of that started a helper or raised an event", no_child() && g_evn == 1);

    /* Asked: a sheet, and nothing else. A name from outside is shown only
     * as letters, digits and a few marks. */
    psk_option(a, 1); /* the person's own choice on setup: 60 minutes */
    at = g_evn;
    CHECK("a request raises its sheet",
          ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"client\": \"Flip8 <b>#ff0000 x#\\n%s\", \"keep\": \"full\", "
              "\"promisc\": true}",
              st, sizeof(st)) == 0 &&
              sheet_is_request() && shown("Start a capture?"));
    CHECK("the sheet names who asks, the interface and the minutes, in plain characters only",
          containing_in(app_body, "Flip8 bff0000 xs asks to capture on synth0 for 5 minutes, headers only (filter: ") != NULL &&
              containing_in(app_body, "Nothing is captured unless you tap START.") != NULL &&
              containing_in(app_body, "never packets, addresses or names") != NULL);
    CHECK("its two buttons are full targets, REFUSE first",
          target_ok(a->sheet_safe, button_h(), "REFUSE") && target_ok(a->sheet_go, button_h(), "START") &&
              strcmp(lv_label_get_text(lv_obj_get_child(a->sheet_safe, 0)), "REFUSE") == 0 &&
              strcmp(lv_label_get_text(lv_obj_get_child(a->sheet_go, 0)), "START") == 0);
    shot("h4-request");
    tap_obj(a->sheet_go);
    pump(150);
    CHECK("START tapped in the sheet's first moment is not an answer: the sheet stays, nothing starts",
          sheet_is_request() && no_child() && !a->starting);
    pump(1500);
    CHECK("asking started nothing: no helper, no capture, setup's own choices untouched, no event",
          no_child() && !a->starting && !psk_capture_present(a) && remote_is("idle", "none") &&
              psk_limit_min[a->limit_idx] == 60 && a->keep == PPC_KEEP_HEADERS && g_evn == at);
    {
        cJSON *q = remote_status();

        CHECK("capture.status says a request is shown, to an asker that reads and does not listen",
              strcmp(cJSON_GetObjectItem(q, "request")->valuestring, "shown") == 0);
        cJSON_Delete(q);
    }
    CHECK("a second request while one is shown is refused",
          ask(good, st, sizeof(st)) == POCKETIPC_ERR_POLICY && strcmp(st, "a request is already shown") == 0);
    CHECK("capture.hold and capture.stop have nothing to act on",
          remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              remote_call("capture.stop", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY);
    CHECK("no key answers the sheet", !psk_key(a, LV_KEY_ENTER) && !psk_key(a, 's') && !psk_key(a, ' ') &&
                                          sheet_is_request() && no_child());

    /* REFUSE, Back, cancel, the lock, time. */
    tap_obj(a->sheet_safe);
    pump(400);
    CHECK("REFUSE, tapped: the sheet goes, nothing starts", !sheet_is_request() && no_child() && shown("START CAPTURE"));
    CHECK("and it is said once: refused", outcome_was(at, "refused"));
    pump(1200);
    CHECK("and not again", g_evn == at + 1);
    {
        cJSON *q = remote_status();

        CHECK("capture.status keeps how the request ended, after the event has gone",
              strcmp(cJSON_GetObjectItem(q, "request")->valuestring, "refused") == 0 &&
                  strcmp(cJSON_GetObjectItem(q, "reason")->valuestring, "none") == 0);
        cJSON_Delete(q);
    }
    CHECK("nothing of the request is kept, not even under the hidden sheet",
          a->req_client[0] == '\0' && a->req_iface[0] == '\0' && a->req_limit_s == 0 &&
              lv_label_get_text(a->sheet_text)[0] == '\0');

    at = g_evn;
    CHECK("asked again", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    CHECK("Back is REFUSE", app_packetshark.back(app_priv) == 1 && !sheet_is_request());
    pump(400);
    CHECK("said: refused", outcome_was(at, "refused") && no_child());

    at = g_evn;
    CHECK("asked again", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    CHECK("capture.request_cancel takes the sheet down",
          remote_call("capture.request_cancel", NULL, st, sizeof(st), &ch) == 0 && !sheet_is_request());
    pump(400);
    CHECK("said: refused", outcome_was(at, "refused") && no_child());

    /* A request with a name: only a cancel that names it takes it down. */
    at = g_evn;
    CHECK("a request with a name that is not a short hex name is malformed",
          ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"request\": \"XYZ\"}", st, sizeof(st)) ==
                  POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"request\": \"\"}", st, sizeof(st)) ==
                  POCKETIPC_ERR_INVALID_PARAMS &&
              ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"request\": 7}", st, sizeof(st)) ==
                  POCKETIPC_ERR_INVALID_PARAMS &&
              !sheet_is_request());
    CHECK("asked, as request 0b",
          ask("{\"iface\": \"synth0\", \"limit_s\": 300, \"request\": \"0b\"}", st, sizeof(st)) == 0 && sheet_is_request());
    CHECK("a cancel that names an earlier request leaves this one up",
          remote_call("capture.request_cancel", "{\"request\": \"0a\"}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "that is not the request shown") == 0 && sheet_is_request());
    CHECK("so does a cancel that names none",
          remote_call("capture.request_cancel", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY && sheet_is_request());
    CHECK("the cancel that names it takes it down",
          remote_call("capture.request_cancel", "{\"request\": \"0b\"}", st, sizeof(st), &ch) == 0 && !sheet_is_request());
    CHECK("and the same cancel again finds nothing to take down",
          remote_call("capture.request_cancel", "{\"request\": \"0b\"}", st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "no request is shown") == 0);
    pump(400);
    CHECK("said: refused", outcome_was(at, "refused") && no_child() && a->req_token[0] == '\0');

    at = g_evn;
    CHECK("asked again", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    g_ev_front = false; /* the unit is locked, or an alarm shows */
    pump(200);
    CHECK("the lock takes the sheet down at once, so nobody finds a START they did not see asked for",
          !sheet_is_request() && no_child() && g_evn == at);
    g_ev_front = true;
    pump(500);
    CHECK("said when the unit is open again: refused", outcome_was(at, "refused"));

    at = g_evn;
    CHECK("asked again", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    CHECK("the sheet has 60 seconds", a->req_deadline_ms - psk_now() > 59000 && a->req_deadline_ms - psk_now() <= 60000);
    pump(1200);
    CHECK("and is still there after one of them", sheet_is_request());
    a->req_deadline_ms = psk_now() + 150; /* the other 59, not waited for here */
    pump(500);
    CHECK("time: the sheet goes by itself, nothing starts", !sheet_is_request() && no_child());
    CHECK("said: timeout", outcome_was(at, "timeout"));

    /* Another sheet is up: no request over it. */
    psk_option(a, 0);
    CHECK("a request while another sheet is up is refused",
          shown("Keep full packets?") && ask(good, st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "a sheet is open on the unit") == 0 && shown("Keep full packets?"));
    psk_sheet_answer(a, true); /* the person chooses FULL for their next capture */
    CHECK("the person's own choice on setup is FULL now", a->keep == PPC_KEEP_FULL);

    /* START, tapped on the sheet. */
    at = g_evn;
    CHECK("asked again", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    pump(PSK_REQUEST_SETTLE_MS + 200);
    tap_obj(a->sheet_go);
    CHECK("START, tapped on the sheet: capturing", wait_for("CAPTURING", 3500) && !sheet_is_request());
    CHECK("nothing of the request is kept once it is answered",
          a->req_client[0] == '\0' && a->req_iface[0] == '\0' && a->req_limit_s == 0 &&
              lv_label_get_text(a->sheet_text)[0] == '\0');
    pump(1300);
    o = remote_status();
    CHECK("it is the capture the sheet said: synth0, 5 minutes, HEADERS whatever setup had, and marked as asked for",
          strcmp(cJSON_GetObjectItem(o, "iface")->valuestring, "synth0") == 0 &&
              cJSON_GetObjectItem(o, "limit_s")->valuedouble == 300 &&
              strcmp(cJSON_GetObjectItem(o, "keep")->valuestring, "headers") == 0 &&
              cJSON_IsTrue(cJSON_GetObjectItem(o, "requested_by_companion")) && !a->run_full);
    cJSON_Delete(o);
    CHECK("the listeners hear capturing, with no refusal before it",
          g_evn > at && !g_ev[at].stats && strcmp(g_ev[at].state, "capturing") == 0 && strcmp(g_ev[at].reason, "none") == 0);
    CHECK("a request while a capture runs is refused", ask(good, st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
                                                        strcmp(st, "a capture is running") == 0 && !sheet_is_request());
    CHECK("a cancel cannot reach the capture a tap on START began: it finds no request, and the capture runs on",
          remote_call("capture.request_cancel", NULL, st, sizeof(st), &ch) == POCKETIPC_ERR_POLICY &&
              strcmp(st, "no request is shown") == 0 && remote_is("capturing", "none") && !no_child());

    /* The reader's mark (ADR-016 rule 20). */
    pump(200);
    CHECK("while requests come, the header says a companion is reading, beside the state",
          strcmp(g_hint, "CAPTURING · SYNTH0 · COMPANION") == 0 && a->reading);
    pump(PSK_READER_MS + 400);
    CHECK("five seconds after the last one it no longer says so", strcmp(g_hint, "CAPTURING · SYNTH0") == 0 && !a->reading);
    /* Reading is capture.status and nothing else: a hold and its release
     * are no reader, and the header does not say there is one. */
    CHECK("capture.hold is taken while nobody reads", remote_call("capture.hold", "{\"on\": true}", st, sizeof(st), &ch) == 0 &&
                                                         wait_for("HOLD: ", 2000));
    pump(300);
    CHECK("and lights no reader's mark", !a->reading && strcmp(g_hint, "HOLD · SYNTH0") == 0);
    CHECK("nor does its release", remote_call("capture.hold", "{\"on\": false}", st, sizeof(st), &ch) == 0 &&
                                      wait_for("CAPTURING", 2000) && (pump(300), !a->reading) &&
                                      strcmp(g_hint, "CAPTURING · SYNTH0") == 0);
    at = g_evn;
    pump(2400);
    CHECK("and while it does not, no figures are raised to anybody", stats_between(at, g_evn) == 0);
    cJSON_Delete(remote_status());
    pump(150);
    CHECK("one status request and it says so again", strcmp(g_hint, "CAPTURING · SYNTH0 · COMPANION") == 0);
    pump(1400);
    CHECK("and the figures come again", stats_between(at, g_evn) >= 1);
    tap("PACKETS");
    pump(300);
    CHECK("on PACKETS too", strcmp(g_hint, "CAPTURING · SYNTH0 · COMPANION") == 0);
    tap("LIVE");
    pump(200);

    /* The capture a request began is an ordinary one from there on. */
    CHECK("capture.stop stops it", remote_call("capture.stop", NULL, st, sizeof(st), &ch) == 0 &&
                                       wait_for("SYNTH0 · BY YOU", 2000) && remote_is("stopped", "user"));
    CHECK("a request over a stopped capture is refused", ask(good, st, sizeof(st)) == POCKETIPC_ERR_POLICY &&
                                                          strcmp(st, "a capture is running") == 0);
    tap("DISCARD");
    tap_last("DISCARD");
    CHECK("DISCARD: setup again, HEADERS again, and the person's own 60 minutes back",
          wait_for("START CAPTURE", 1500) && a->keep == PPC_KEEP_HEADERS && psk_limit_min[a->limit_idx] == 60);
    pump(600);
    tap("START CAPTURE");
    CHECK("a capture started on the setup screen", wait_for("CAPTURING", 3500));
    pump(600);
    o = remote_status();
    CHECK("is nobody's request", cJSON_IsFalse(cJSON_GetObjectItem(o, "requested_by_companion")));
    cJSON_Delete(o);
    app_stop();

    /* Leaving with the question unanswered refuses it. */
    app_start();
    a = app_priv;
    pump(200);
    CHECK("asked", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    at = g_evn;
    app_stop();
    CHECK("leaving refuses it: said once as the app goes, and no helper was ever started",
          g_evn == at + 1 && !g_ev[at].stats && strcmp(g_ev[at].state, "idle") == 0 &&
              strcmp(g_ev[at].reason, "refused") == 0 && no_child());

    /* START on the sheet with no helper to start: the request is refused
     * after all, and setup says why. */
    setenv("POCKETOS_CAPTURE_HELPER", "/nonexistent/pos-capture", 1);
    app_start();
    a = app_priv;
    pump(200);
    CHECK("asked", ask(good, st, sizeof(st)) == 0 && sheet_is_request());
    at = g_evn;
    pump(PSK_REQUEST_SETTLE_MS + 200);
    tap_obj(a->sheet_go);
    CHECK("START with no helper: said on setup", wait_for("! THE CAPTURE HELPER IS NOT INSTALLED", 2500));
    pump(500);
    CHECK("and to the listeners: refused, and nothing is marked as asked for", outcome_was(at, "refused") && !a->by_companion);
    app_stop();
    setenv("POCKETOS_CAPTURE_HELPER", getenv("PACKETSHARK_HELPER"), 1);
    CHECK("the app raised no event but its two", g_ev_other == 0 && g_evn < EV_MAX);
}

/* The request's sheet in the shape in force (run for the wide one): it fits,
 * its buttons can be reached, and REFUSE refuses. */
static void request_shape(void)
{
    struct psk_app *a;
    char st[96];

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    a = app_priv;
    pump(200);
    CHECK("a request's sheet in this shape", ask("{\"iface\": \"synth0\", \"limit_s\": 900}", st, sizeof(st)) == 0 &&
                                                 sheet_is_request() && shown("Start a capture?"));
    CHECK("names a companion when no name was given",
          containing_in(app_body, "A companion asks to capture on synth0 for 15 minutes") != NULL);
    CHECK("its buttons are full targets inside the screen",
          target_ok(a->sheet_safe, button_h(), "REFUSE") && target_ok(a->sheet_go, button_h(), "START"));
    shot("h4-request");
    tap_obj(a->sheet_safe);
    pump(300);
    CHECK("REFUSE refuses", !sheet_is_request() && no_child());
    app_stop();
}

/* ---- M4: the graph, the protocols, the layout ----------------------------------------- */

/* A box of mockups/layout.json (01-live-capture.png), within 2 px. */
static bool box_ok(const char *part, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = psk_app_part(app_priv, part);
    lv_area_t a;

    if (!o) {
        printf("     %s: missing\n", part);
        return false;
    }
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    if (abs((int)(a.x1 - x)) > 2 || abs((int)(a.y1 - y)) > 2 || abs((int)(lv_area_get_width(&a) - w)) > 2 ||
        abs((int)(lv_area_get_height(&a) - h)) > 2) {
        printf("     %s: %d,%d %dx%d, the mockup %d,%d %dx%d\n", part, (int)a.x1, (int)a.y1,
               (int)lv_area_get_width(&a), (int)lv_area_get_height(&a), (int)x, (int)y, (int)w, (int)h);
        return false;
    }
    return true;
}

#if LV_USE_SNAPSHOT
/* How many pixels of the graph are exactly this colour, in rows y0..y1 of it
 * (y1 < 0: to the bottom). */
static unsigned pixels_of(lv_color_t c, int y0, int y1)
{
    lv_obj_t *g = psk_app_part(app_priv, "graph");
    lv_draw_buf_t *snap;
    unsigned n = 0;
    uint32_t x;
    uint32_t y;

    if (!g) {
        return 0;
    }
    pump(40);
    snap = lv_snapshot_take(g, LV_COLOR_FORMAT_RGB888);
    if (!snap) {
        return 0;
    }
    if (y1 < 0 || (uint32_t)y1 >= snap->header.h) {
        y1 = (int)snap->header.h - 1;
    }
    for (y = (uint32_t)y0; (int)y <= y1; y++) {
        const unsigned char *row = (const unsigned char *)snap->data + (size_t)y * snap->header.stride;

        for (x = 0; x < snap->header.w; x++) {
            if (row[x * 3 + 2] == c.red && row[x * 3 + 1] == c.green && row[x * 3] == c.blue) {
                n++;
            }
        }
    }
    lv_draw_buf_destroy(snap);
    return n;
}
#endif

static void audit_issue(const struct pocketui_audit_issue *is, void *user)
{
    int *bad = user;

    if (is->kind == POCKETUI_AUDIT_TRUNCATED) {
        return;
    }
    if (*bad < 3) {
        printf("     audit: %s %s '%s' %d,%d-%d,%d / '%s'\n", pocketui_audit_kind_name(is->kind), is->path, is->text,
               (int)is->area.x1, (int)is->area.y1, (int)is->area.x2, (int)is->area.y2, is->other_text);
    }
    (*bad)++;
}

static int audit_live(void)
{
    int bad = 0;

    lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(psk_app_part(app_priv, "traffic"))), 0, LV_ANIM_OFF);
    pump(60);
    lv_obj_update_layout(app_body);
    pocketui_audit(app_body, audit_issue, &bad, NULL);
    return bad;
}

/* A figure on one line of its own font: not "492" over "000" (DS §46.5). */
static int figures_broken(const char *prefix, int count)
{
    int bad = 0;
    int i;

    lv_obj_update_layout(app_body);
    for (i = 0; i < count; i++) {
        char name[16];
        lv_obj_t *lb;
        const lv_font_t *f;

        snprintf(name, sizeof(name), "%s%d", prefix, i);
        lb = psk_app_part(app_priv, name);
        f = lb ? lv_obj_get_style_text_font(lb, LV_PART_MAIN) : NULL;
        if (!lb || !f || lv_obj_get_content_height(lb) > lv_font_get_line_height(f)) {
            if (bad < 3) {
                printf("     %s '%s' is %d px high (one line %d)\n", name, lb ? lv_label_get_text(lb) : "?",
                       lb ? (int)lv_obj_get_content_height(lb) : -1, f ? (int)lv_font_get_line_height(f) : -1);
            }
            bad++;
        }
    }
    return bad;
}

static int readout_second(void)
{
    lv_obj_t *r = psk_app_part(app_priv, "readout");
    int mm;
    int ss;

    if (!r || sscanf(lv_label_get_text(r), "%d:%d", &mm, &ss) != 2) {
        return -1;
    }
    return mm * 60 + ss;
}

static void tap_graph_column(int col)
{
    lv_obj_t *g = psk_app_part(app_priv, "graph");
    lv_area_t ga;

    lv_obj_scroll_to_view_recursive(g, LV_ANIM_OFF);
    pump(40);
    lv_obj_get_coords(g, &ga);
    finger_point.x = ga.x1 + col * 7 + 2;
    finger_point.y = ga.y2 - 20;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

static void m4_live(bool portrait)
{
    int64_t poll_us = 0;
    int64_t frame_us = 0;
    int before;
    int after;
    int i;
    int t;
    int m;
    int bad;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    pump(3200);
    if (portrait) {
        bool ok = box_ok("status", 20, 166, 528, 36) & box_ok("traffic", 20, 228, 528, 374) &
                  box_ok("graph", 40, 320, 418, 209) & box_ok("protocols", 20, 624, 528, 240) &
                  box_ok("proto0", 40, 642, 236, 52) & box_ok("proto1", 292, 642, 236, 52) &
                  box_ok("proto6", 40, 798, 236, 52) & box_ok("proto7", 292, 798, 236, 52) &
                  box_ok("session", 20, 886, 528, 84) & box_ok("tab0", 20, 96, 170, 56) &
                  box_ok("tab2", 377, 96, 171, 56) & box_ok("bar0", 20, 1138, 170, 64) &
                  box_ok("bar2", 377, 1138, 171, 64);

        CHECK("LIVE matches the boxes of mockup 01 within 2 px (signals excepted)", ok);
    }
#if LV_USE_SNAPSHOT
    {
        unsigned c;
        int missing = 0;

        for (c = 0; c < PPC_CLASS_COUNT; c++) {
            int hue = psk_class_hue(c);
            lv_color_t col = hue < 0 ? pos_theme_color(POS_COLOR_TEXT_SECONDARY) : pos_identity_hue((unsigned)hue);

            if (pixels_of(col, 0, -1) == 0) {
                printf("     class %s not drawn\n", psk_class_word(c));
                missing++;
            }
        }
        CHECK("every class of the synthetic traffic is drawn in its own hue", missing == 0);
    }
#endif
    shot("m4-capturing");
    /* A column and its readout (F8 up to the readout). */
    before = readout_second();
    tap_graph_column(57);
    after = readout_second();
    if (!(before >= 2 && after >= 0 && after < before)) {
        printf("     readout %d -> %d '%s'\n", before, after, lv_label_get_text(psk_app_part(app_priv, "readout")));
    }
    CHECK("a tap selects a column: the readout names that earlier second",
          before >= 2 && after >= 0 && after < before && after >= before - 3);
    shot("m4-selected");
    pump(1100);
    CHECK("and the selection stays on its second as time goes on", after >= 0 && readout_second() == after);
    tap("HOLD");
    pump(2600);
#if LV_USE_SNAPSHOT
    CHECK("held seconds are marked above the baseline (T4)",
          pixels_of(pos_theme_color(POS_COLOR_TEXT_MUTED), PSK_GRAPH_BAND - 4, PSK_GRAPH_BAND - 1) > 0);
#endif
    /* A held column, tapped: the newest second, counted and not stored. */
    tap_graph_column(PSK_GRAPH_SECONDS - 1);
    CHECK("the readout of a held second says so", containing_in(app_body, " · HELD · ") != NULL);
    shot("m4-hold");
    tap("RESUME");
    pump(300);
    tap_graph_column(PSK_GRAPH_SECONDS - 9);
    tap("\xe2\x80\xba PACKETS");
    pump(600);
    {
        const struct psk_app *pa = app_priv;
        const struct psk_model *pm = pa->model;
        int sel = readout_second();
        int64_t k = (int64_t)pa->top_rank - (int64_t)pm->rows_first_rank;

        /* The readout names the selected second: the list opens there, held,
         * its first row in that second. */
        bool stored = false;
        bool at;
        unsigned c;

        /* A held second stored nothing: the list then opens at the first
         * packet after it. */
        for (c = 0; sel >= 0 && c < PPC_CLASS_COUNT; c++) {
            stored = stored || pm->bins[sel % PSK_BINS][c] > 0;
        }
        at = pa->cur == PSK_TAB_PACKETS && !pa->follow && pm->row_count > 0 && sel >= 0 && k >= 0 &&
             k < (int64_t)pm->row_count && pm->rows[k].rel_us >= (uint64_t)sel * 1000000u &&
             (!stored || pm->rows[k].rel_us < (uint64_t)(sel + 1) * 1000000u);

        if (!at) {
            printf("     tab %d follow %d sel %d top %u first %u rows %u anchor %u top us %llu\n", (int)pa->cur,
                   (int)pa->follow, sel, (unsigned)pa->top_rank, (unsigned)pm->rows_first_rank,
                   (unsigned)pm->row_count, (unsigned)pm->rows_anchor,
                   k >= 0 && k < (int64_t)pm->row_count ? (unsigned long long)pm->rows[k].rel_us : 0ull);
        }
        CHECK("the readout's PACKETS opens the list at the selected second", at);
    }
    app_packetshark.back(app_priv);
    pump(100);
    tap_obj(psk_app_part(app_priv, "proto0")); /* TCP's cell (the diagnosis has a TCP line too, M11.1) */
    pump(600);
    CHECK("a protocol opens the list filtered on its class",
          ((const struct psk_app *)app_priv)->cur == PSK_TAB_PACKETS && shown("TCP") &&
              containing_in(app_body, " OF ") != NULL);
    psk_set_view(app_priv, PPC_CLASSES_ALL, "", 0, false);
    app_packetshark.back(app_priv);
    pump(100);

    /* Every theme and display mode, then every text size: nothing clipped or
     * overlapping on LIVE (I1 for LIVE). */
    bad = 0;
    for (t = 0; t < pos_theme_count(); t++) {
        for (m = 0; m < POS_MODE_COUNT; m++) {
            char why[96];

            pos_theme_select(pos_theme_at(t)->id, pos_mode_name((enum pos_mode)m), why, sizeof(why));
            pump(60);
            i = audit_live();
            if (i) {
                printf("     theme %s %s: %d issue(s)\n", pos_theme_at(t)->id, pos_mode_name((enum pos_mode)m), i);
            }
            bad += i;
        }
    }
    {
        char why[96];

        pos_theme_select("doors", "normal", why, sizeof(why));
    }
    CHECK("every theme and display mode: LIVE has nothing clipped or overlapping", bad == 0);
    bad = 0;
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        int n;

        pos_theme_select_text_size((enum pos_text_size)i);
        pump(80);
        n = audit_live();
        if (n) {
            printf("     text size %s: %d issue(s)\n", pos_text_size_name((enum pos_text_size)i), n);
        }
        bad += n;
        if (i == POS_TEXT_SIZE_LARGE) {
            shot("m4-large");
        }
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    pump(80);
    CHECK("every text size: LIVE has nothing clipped or overlapping", bad == 0);
    app_stop();

    /* Cost at 8,000 synthetic packets a second (a host regression guard,
     * not a device figure). */
    setenv("POCKETOS_CAPTURE_SOURCE", "synth:8000:1", 1);
    app_start();
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    pump(1500);
    psk_app_cost(app_priv, NULL, NULL, 1);
    pump(3000);
    psk_app_cost(app_priv, &poll_us, &frame_us, 0);
    printf("     8,000 pps: worst poll %lld us, worst frame %lld us\n", (long long)poll_us, (long long)frame_us);
    CHECK("at 8,000 pps the poll's work stays under 4 ms", poll_us > 0 && poll_us < 4000);
    app_stop();
    CHECK("and no helper is left", no_child());

    /* Rates in the thousands at every text size: each figure on one line,
     * SESSION four in a line where they fit and two by two where they do
     * not, a rate under its word where it does not fit beside it. */
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        lv_area_t c0;
        lv_area_t c2;
        bool beside;
        int broken;

        pos_theme_select_text_size((enum pos_text_size)i);
        app_start();
        tap("START CAPTURE");
        wait_for("CAPTURING", 3500);
        pump(2600);
        broken = figures_broken("cell", 4) + figures_broken("rate", PPC_CLASS_COUNT) +
                 figures_broken("share", PPC_CLASS_COUNT);
        lv_obj_get_coords(psk_app_part(app_priv, "cell0"), &c0);
        lv_obj_get_coords(psk_app_part(app_priv, "cell2"), &c2);
        /* Beside its word: in the row that holds the word and the share. */
        beside = lv_obj_get_parent(psk_app_part(app_priv, "rate0")) ==
                 lv_obj_get_parent(psk_app_part(app_priv, "share0"));
        printf("     text size %s: rate0 '%s', SESSION %s, rates %s\n", pos_text_size_name((enum pos_text_size)i),
               lv_label_get_text(psk_app_part(app_priv, "rate0")), c2.y1 > c0.y2 ? "two by two" : "in a line",
               beside ? "beside the word" : "under the word");
        if (i == POS_TEXT_SIZE_SMALL) {
            CHECK("Small, 8,000 pps: four SESSION figures in a line, rates beside their words",
                  c2.y1 == c0.y1 && beside);
        } else if (i == POS_TEXT_SIZE_LARGE) {
            /* Portrait's 528 px panels have no room at Large for four
             * figures in a line or a rate beside its word; the wider
             * landscape panels do, and keep the Small shape. */
            if (portrait) {
                CHECK("Large, portrait: SESSION two by two, each rate under its word", c2.y1 > c0.y2 && !beside);
            }
            shot("m4-large-8000");
        }
        bad = audit_live();
        CHECK(i == POS_TEXT_SIZE_SMALL    ? "Small, 8,000 pps: no figure broken across lines, nothing clipped or overlapping"
              : i == POS_TEXT_SIZE_MEDIUM ? "Medium, 8,000 pps: no figure broken across lines, nothing clipped or overlapping"
                                          : "Large, 8,000 pps: no figure broken across lines, nothing clipped or overlapping",
              broken == 0 && bad == 0);
        if (i == POS_TEXT_SIZE_LARGE) {
            pid_t pid = child_pid();

            /* LAST SESSION on the error panel takes the same shape. */
            if (pid > 0) {
                kill(pid, SIGKILL);
            }
            wait_for("KILLED (SIG 9)", 1500);
            pump(200);
            CHECK("Large: LAST SESSION's figures each on one line", figures_broken("last", 4) == 0);
        }
        app_stop();
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    pump(80);
}

/* ---- M5, M6: the packet list and the inspector ---------------------------------------- */

static struct psk_app *app(void)
{
    return app_priv;
}

/* Rows the list shows, and their seqs top to bottom. */
static int rows_shown(uint32_t *seqs, int max)
{
    int n = 0;
    int i;

    for (i = 0; i < PSK_LIST_ROWS; i++) {
        lv_obj_t *r = app()->lrow[i].row;

        if (!lv_obj_has_flag(r, LV_OBJ_FLAG_HIDDEN) && app()->lrow[i].seq) {
            if (seqs && n < max) {
                seqs[n] = app()->lrow[i].seq;
            }
            n++;
        }
    }
    return n;
}

static const char *row_text(int i, int which)
{
    const struct psk_listrow *r = &app()->lrow[i];
    lv_obj_t *lb = which == 0 ? r->word : which == 1 ? r->len : which == 2 ? r->info : r->addr;

    return lv_label_get_text(lb);
}

/* A drag on an object by dy pixels, in steps (a finger, not a jump). */
static void drag(lv_obj_t *obj, int32_t dy)
{
    lv_area_t a;
    int k;

    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    for (k = 1; k <= 8; k++) {
        finger_point.y = a.y1 + lv_area_get_height(&a) / 2 + dy * k / 8;
        pump(30);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

static void tap_at(lv_obj_t *obj, int32_t fy)
{
    lv_area_t a;

    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + fy;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

static bool udp_word(const char *w)
{
    return strcmp(w, "UDP") == 0 || strcmp(w, "NTP") == 0 || strcmp(w, "SSDP") == 0 || strcmp(w, "NBNS") == 0;
}

#if LV_USE_SNAPSHOT
/* Pixels of exactly this colour inside an object. */
static unsigned obj_pixels(lv_obj_t *o, lv_color_t c)
{
    lv_draw_buf_t *snap;
    unsigned n = 0;
    uint32_t x;
    uint32_t y;

    pump(40);
    snap = o ? lv_snapshot_take(o, LV_COLOR_FORMAT_RGB888) : NULL;
    if (!snap) {
        return 0;
    }
    for (y = 0; y < snap->header.h; y++) {
        const unsigned char *row = (const unsigned char *)snap->data + (size_t)y * snap->header.stride;

        for (x = 0; x < snap->header.w; x++) {
            if (row[x * 3 + 2] == c.red && row[x * 3 + 1] == c.green && row[x * 3] == c.blue) {
                n++;
            }
        }
    }
    lv_draw_buf_destroy(snap);
    return n;
}
#endif

/* Visible dots-mode labels taller than one line of their font: such a label
 * wrapped instead of shortening (DS §46.5), as "02:00:5e:" over "10:00:02". */
static int dots_wrapped(lv_obj_t *obj)
{
    int n = 0;
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT) {
        const lv_font_t *f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);

        if (f && lv_obj_get_content_height(obj) > lv_font_get_line_height(f)) {
            if (n < 3) {
                printf("     '%s' is %d px high (one line %d)\n", lv_label_get_text(obj),
                       (int)lv_obj_get_content_height(obj), (int)lv_font_get_line_height(f));
            }
            n++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += dots_wrapped(lv_obj_get_child(obj, (int32_t)i));
    }
    return n;
}

static int audit_page(void)
{
    int bad = 0;

    pump(60);
    lv_obj_update_layout(app_body);
    pocketui_audit(app_body, audit_issue, &bad, NULL);
    return bad;
}

static bool wait_rows(int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (rows_shown(NULL, 0) > 0) {
            return true;
        }
        pump(20);
    }
    return rows_shown(NULL, 0) > 0;
}

static bool wait_packet(uint32_t not_seq, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        const struct psk_model *m = app()->model;

        if (m->have_pkt && m->pkt.head.seq != not_seq && app()->insp_shown == m->pkt.head.seq) {
            return true;
        }
        pump(20);
    }
    return false;
}

static void m5_m6(bool portrait)
{
    uint32_t s[PSK_LIST_ROWS];
    uint32_t s2[PSK_LIST_ROWS];
    int n;
    int i;
    int ok;
    uint32_t seq;
    int64_t poll_us = 0;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    pump(2500);
    tap("PACKETS");
    CHECK("PACKETS: rows from the helper's window", wait_rows(1500));
    pump(300);
    n = rows_shown(s, PSK_LIST_ROWS);
    printf("     %d rows shown, vis %d\n", n, app()->vis);
    CHECK(portrait ? "portrait: fifteen 56 px rows (DS §59.4, mockup 02)" : "landscape: the rows that fit the column",
          portrait ? n == 15 : n >= 3);
    for (i = 1, ok = 1; i < n; i++) {
        ok = ok && s[i] > s[i - 1];
    }
    CHECK("TIME, oldest first: seqs grow down the list", ok && n > 1);
    CHECK("each row: a protocol word, a length, its sentence",
          row_text(0, 0)[0] && atoi(row_text(0, 1)) > 0 && row_text(0, 2)[0]);
    CHECK("following: the line under the list says so", shown("FOLLOWING THE NEWEST") != NULL);
    if (portrait) {
        bool b = box_ok("filter", 20, 164, 118, 56) & box_ok("list", 20, 256, 508, 840) &
                 box_ok("row0", 20, 256, 508, 56) & box_ok("rail", 528, 256, 20, 840) &
                 box_ok("bar0", 20, 1138, 170, 64);

        CHECK("PACKETS matches the boxes of mockup 02 within 2 px", b);
    }
    CHECK("the filter bar's buttons are 56 px targets",
          target_ok(psk_app_part(app_priv, "filter"), 56, "FILTER") &&
              target_ok(psk_app_part(app_priv, "sort"), 56, "SORT") &&
              target_ok(psk_app_part(app_priv, "follow"), 56, "FOLLOW"));
    CHECK("a row is a 56 px target (DS §59.6), 40 px on one line in the wide shape (mockup 08)",
          target_ok(psk_app_part(app_priv, "row0"), portrait ? 56 : PSK_ROW_H_WIDE, "row") &&
              app()->row_h == (portrait ? 56 : PSK_ROW_H_WIDE));
    shot("m5-packets");
    pump(900);
    rows_shown(s2, PSK_LIST_ROWS);
    CHECK("following: newer packets come in at the bottom", n > 0 && s2[n - 1] > s[n - 1]);

    /* A drag holds the view; LIVE brings it back to the newest. */
    drag(psk_app_part(app_priv, "list"), PSK_ROW_H * 3);
    pump(400);
    rows_shown(s, PSK_LIST_ROWS);
    CHECK("a drag down shows earlier packets and holds the view",
          !app()->follow && s[0] < s2[0] && containing_in(app_body, "VIEW HELD AT #") != NULL);
    pump(1200);
    rows_shown(s2, PSK_LIST_ROWS);
    CHECK("a held view stays on its packets as more arrive", s2[0] == s[0]);
    CHECK("LIVE with the count of newer packets is the bar's primary", shown("LIVE \xe2\x86\x93 ") &&
                                                                          !shown("LIVE \xe2\x86\x93 0"));
    shot("m5-held");
    tap("LIVE \xe2\x86\x93 ");
    pump(600);
    CHECK("LIVE: following the newest again", app()->follow && shown("FOLLOWING THE NEWEST"));

    /* The rail: its top is the oldest packet held. */
    tap_at(psk_app_part(app_priv, "rail"), 2);
    pump(600);
    rows_shown(s, PSK_LIST_ROWS);
    if (!(!app()->follow && app()->top_rank == 0 && s[0] == app()->model->rows_first_seq)) {
        printf("     follow %d top %u s0 %u first seq %u anchor %u match %u\n", (int)app()->follow,
               (unsigned)app()->top_rank, (unsigned)s[0], (unsigned)app()->model->rows_first_seq,
               (unsigned)app()->model->rows_anchor, (unsigned)app()->model->rows_match);
    }
    CHECK("a tap at the rail's top shows the first packets in memory",
          !app()->follow && app()->top_rank == 0 && s[0] == app()->model->rows_first_seq);
    tap_at(psk_app_part(app_priv, "rail"), lv_obj_get_height(psk_app_part(app_priv, "rail")) - 2);
    pump(600);
    CHECK("and at its foot the list follows the newest again", app()->follow);

    /* Sort: newest first, largest first, back to oldest first. */
    tap_obj(psk_app_part(app_priv, "sort"));
    wait_rows(800);
    pump(500);
    n = rows_shown(s, PSK_LIST_ROWS);
    for (i = 1, ok = 1; i < n; i++) {
        ok = ok && s[i] < s[i - 1];
    }
    CHECK("TIME \xe2\x86\x91: the newest first", ok && n > 1 && shown("TIME \xe2\x86\x91"));
    tap_obj(psk_app_part(app_priv, "sort"));
    pump(700);
    n = rows_shown(NULL, 0);
    for (i = 1, ok = 1; i < n; i++) {
        ok = ok && atoi(row_text(i, 1)) <= atoi(row_text(i - 1, 1));
    }
    CHECK("LEN \xe2\x86\x91: the largest first, FOLLOW not offered",
          ok && n > 1 && shown("LEN \xe2\x86\x91") && !enabled("FOLLOW") && !app()->follow);
    shot("m5-len");
    tap_obj(psk_app_part(app_priv, "sort"));
    pump(600);
    CHECK("TIME \xe2\x86\x93 again, following", shown("TIME \xe2\x86\x93") && app()->follow);

    /* The filter sheet. */
    tap_obj(psk_app_part(app_priv, "filter"));
    CHECK("FILTER opens the filter sheet", shown("Filter packets") != NULL);
    shot("m5-filter-sheet");
    tap_last("UDP");
    tap_last("SHOW");
    pump(700);
    n = rows_shown(NULL, 0);
    for (i = 0, ok = 1; i < n; i++) {
        ok = ok && udp_word(row_text(i, 0));
    }
    CHECK("UDP: every row is UDP, the bar says what is shown",
          ok && n > 0 && shown("UDP") && containing_in(app_body, " OF ") != NULL);
    shot("m5-filtered");
    tap_obj(psk_app_part(app_priv, "filter"));
    tap_last("CLEAR ALL");
    tap_last("SHOW");
    pump(600);
    CHECK("CLEAR ALL: every protocol again", shown("ALL PROTOCOLS") && containing_in(app_body, " OF ") == NULL);

    /* The inspector. */
    drag(psk_app_part(app_priv, "list"), app()->row_h * 2);
    pump(400);
    rows_shown(s, PSK_LIST_ROWS);
    tap_obj(psk_app_part(app_priv, "row1"));
    CHECK("a tap on a row opens INSPECT on that packet", wait_packet(0, 1500) && psk_inspect_visible(app()) &&
                                                             app()->model->pkt.head.seq == s[1]);
    {
        char num[24];

        snprintf(num, sizeof(num), "#%u", (unsigned)s[1]);
        CHECK("its number, the Frame and Ethernet layers, its bytes in hex",
              shown(num) && shown("Frame") && shown("Ethernet") && shown("Bytes") &&
                  lv_label_get_text(psk_app_part(app_priv, "hex"))[0] == '0');
    }
    CHECK("PIN, previous and next are 56 px targets", target_ok(psk_app_part(app_priv, "pin"), 56, "PIN") &&
                                                         target_ok(psk_app_part(app_priv, "prev"), 56, "prev") &&
                                                         target_ok(psk_app_part(app_priv, "next"), 56, "next"));
    shot("m6-inspect");
    seq = app()->model->pkt.head.seq;
    tap_obj(psk_app_part(app_priv, "next"));
    CHECK("next: the packet after it in the list", wait_packet(seq, 1500) && app()->model->pkt.head.seq == s[2]);
    tap_obj(psk_app_part(app_priv, "prev"));
    CHECK("previous: back to it", wait_packet(s[2], 1500) && app()->model->pkt.head.seq == seq);
    tap("Frame");
    CHECK("a layer opens on a tap", shown("ON THE WIRE") != NULL);
    tap("Frame");
    CHECK("and closes on another", shown("ON THE WIRE") == NULL);
    /* A field that filters: a port, else an address (its layer opened
     * first). Every filter offered matches its own packet (the dissector
     * offers none on a MAC the record does not keep, such as a broadcast
     * ARP request's Ethernet destination), so the list is never empty.
     * Tapped by its part: words like SOURCE are on the ends' slabs too. */
    {
        const struct psk_packet *pk = &app()->model->pkt;
        int want = -1;
        char part[16];

        for (i = 0; i < (int)pk->nfields && want < 0; i++) {
            if (strncmp(pk->field[i].filter, "port:", 5) == 0) {
                want = i;
            }
        }
        for (i = 0; i < (int)pk->nfields && want < 0; i++) {
            if (strncmp(pk->field[i].filter, "host:", 5) == 0) {
                want = i;
            }
        }
        i = want;
        if (want >= 0 && !app()->layer_open[pk->field[want].layer]) {
            snprintf(part, sizeof(part), "layer%u", (unsigned)pk->field[want].layer);
            tap_obj(psk_app_part(app_priv, part));
        }
        snprintf(part, sizeof(part), "field%d", want);
        tap_obj(want >= 0 ? psk_app_part(app_priv, part) : NULL);
    }
    CHECK("a field row selects its field", app()->insp_field == i && enabled("FILTER BY"));
#if LV_USE_SNAPSHOT
    CHECK("its bytes are marked in the hex", obj_pixels(psk_app_part(app_priv, "hex"),
                                                        pos_theme_color(POS_COLOR_ACCENT_PRIMARY)) > 0);
#endif
    shot("m6-field");
    tap("FILTER BY");
    pump(700);
    CHECK("FILTER BY: the list on that field's port or host, the packet among them",
          app()->cur == PSK_TAB_PACKETS && (app()->host[0] || app()->port) &&
              (containing_in(app_body, "HOST ") != NULL || containing_in(app_body, "PORT ") != NULL) &&
              rows_shown(NULL, 0) > 0);
    if (!app()->wide) {
        tap("INSPECT"); /* wide: it is beside the list already */
    }
    pump(200);
    tap_obj(psk_app_part(app_priv, "end1"));
    pump(700);
    CHECK("an end's slab filters on its host", app()->cur == PSK_TAB_PACKETS && containing_in(app_body, "HOST ") != NULL);
    psk_set_view(app(), PPC_CLASSES_ALL, "", 0, false);
    pump(500);

    /* Pins. */
    if (!app()->wide) {
        tap("INSPECT"); /* wide: it is beside the list already */
    }
    pump(200);
    seq = app()->model->pkt.head.seq;
    tap_obj(psk_app_part(app_priv, "pin"));
    pump(300);
    CHECK("PIN: the packet is pinned and says so", containing_in(app_body, " · PINNED") != NULL &&
                                                      app()->model->pins == 1);
    shot("m6-pinned");
    tap("PACKETS");
    pump(300);
    tap_obj(psk_app_part(app_priv, "filter"));
    tap_last("Pinned only");
    tap_last("SHOW");
    pump(700);
    n = rows_shown(s, PSK_LIST_ROWS);
    CHECK("PINNED ONLY: the pinned packet alone, marked PIN",
          n == 1 && s[0] == seq && !lv_obj_has_flag(app()->lrow[0].pin, LV_OBJ_FLAG_HIDDEN));
    tap_obj(psk_app_part(app_priv, "row0"));
    wait_packet(0, 1000);
    tap_obj(psk_app_part(app_priv, "pin"));
    pump(300);
    CHECK("PIN again unpins it", app()->model->pins == 0 && containing_in(app_body, " · PINNED") == NULL);
    psk_set_view(app(), PPC_CLASSES_ALL, "", 0, false);
    pump(300);

    /* Every text size: nothing clipped or overlapping on PACKETS and INSPECT. */
    ok = 0;
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        int bad;

        pos_theme_select_text_size((enum pos_text_size)i);
        tap("PACKETS");
        pump(500);
        bad = audit_page() + dots_wrapped(app_body);
        /* The filter in words is never shortened: beside the buttons or
         * under them. */
        if (pocketui_text_width(app()->pk_sum1, lv_label_get_text(app()->pk_sum1)) >
                lv_obj_get_content_width(app()->pk_sum1) ||
            pocketui_text_width(app()->pk_sum2, lv_label_get_text(app()->pk_sum2)) >
                lv_obj_get_content_width(app()->pk_sum2)) {
            printf("     the filter in words is shortened: '%s' / '%s'\n", lv_label_get_text(app()->pk_sum1),
                   lv_label_get_text(app()->pk_sum2));
            bad++;
        }
        if (bad) {
            printf("     PACKETS at %s: %d issue(s)\n", pos_text_size_name((enum pos_text_size)i), bad);
        }
        ok += bad;
        if (i == POS_TEXT_SIZE_LARGE) {
            shot("m5-large");
        }
        if (!app()->wide) {
            tap("INSPECT"); /* wide: it is beside the list already */
        }
        pump(300);
        bad = audit_page() + dots_wrapped(app_body);
        if (bad) {
            printf("     INSPECT at %s: %d issue(s)\n", pos_text_size_name((enum pos_text_size)i), bad);
        }
        ok += bad;
        if (i == POS_TEXT_SIZE_LARGE) {
            shot("m6-large");
        }
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    pump(100);
    CHECK("every text size: PACKETS and INSPECT have nothing clipped, overlapping or wrapped that should shorten",
          ok == 0);

    /* Back walks INSPECT -> PACKETS -> LIVE. */
    if (!app()->wide) {
        tap("INSPECT");
        pump(100);
        CHECK("Back from INSPECT returns to PACKETS",
              app_packetshark.back(app_priv) == 1 && app()->cur == PSK_TAB_PACKETS);
    } else {
        tap("PACKETS");
        pump(100);
    }
    CHECK("and from PACKETS to LIVE", app_packetshark.back(app_priv) == 1 && app()->cur == PSK_TAB_LIVE);

    /* Stopped: the packets stay browsable. */
    tap("STOP");
    wait_for("STOPPED", 2000);
    tap("PACKETS");
    pump(500);
    CHECK("stopped: the list stays, with NEW CAPTURE and DISCARD",
          rows_shown(NULL, 0) > 0 && shown("NEW CAPTURE") && shown("DISCARD") && shown("CAPTURE STOPPED"));
    tap_obj(psk_app_part(app_priv, "row0"));
    CHECK("and a packet still opens", wait_packet(0, 1500) && psk_inspect_visible(app()));
    shot("m6-stopped");
    app_stop();
    CHECK("closing leaves no helper", no_child());

    /* PACKETS following at 8,000 pps: the poll stays cheap. */
    setenv("POCKETOS_CAPTURE_SOURCE", "synth:8000:1", 1);
    app_start();
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    tap("PACKETS");
    pump(1500);
    psk_app_cost(app_priv, NULL, NULL, 1);
    pump(3000);
    psk_app_cost(app_priv, &poll_us, NULL, 0);
    printf("     PACKETS at 8,000 pps: worst poll %lld us\n", (long long)poll_us);
    CHECK("PACKETS following 8,000 pps: the poll's work stays under 4 ms", poll_us > 0 && poll_us < 4000);
    app_stop();
}

static void faults(void)
{
    pid_t pid;
    int64_t t0;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1:crash=300", 1);
    app_start();
    tap("START CAPTURE");
    CHECK("a crashing helper: the error panel", wait_for("Capture ended unexpectedly", 5000) &&
                                                    shown("CRASHED (SIG 11)") && shown("RELEASED"));
    CHECK("the header says the capture ended", hint_is("CAPTURE ENDED"));
    CHECK("capture.status on the error panel: ended, the helper crashed, LAST SESSION's figures",
          remote_is("ended", "helper_crashed") && remote_matches_screen(true));
    CHECK("NEW CAPTURE is the one way on", shown("NEW CAPTURE") && enabled("NEW CAPTURE"));
    shot("helper-stopped");
    tap("NEW CAPTURE");
    CHECK("NEW CAPTURE: setup again", wait_for("START CAPTURE", 1000) && shown("READY"));
    CHECK("capture.status after NEW CAPTURE: idle", remote_is("idle", "none"));
    app_stop();

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    pid = child_pid();
    t0 = mono_ms();
    if (pid > 0) {
        kill(pid, SIGKILL);
    }
    CHECK("kill -9 of the helper: the error panel within 1 s (S3)",
          pid > 0 && wait_for("KILLED (SIG 9)", 1000) && mono_ms() - t0 <= 1000);
    tap("PACKETS");
    CHECK("and the shell stays usable", shown("NO PACKETS YET") != NULL);
    app_packetshark.back(app_priv);
    app_stop();

    setenv("POCKETOS_CAPTURE_HELPER", "/nonexistent/pos-capture", 1);
    app_start();
    tap("START CAPTURE");
    CHECK("a missing helper is said on setup", wait_for("! THE CAPTURE HELPER IS NOT INSTALLED", 2000));
    CHECK("capture.status with no helper: idle", remote_is("idle", "none"));
    app_stop();
}

static int timers(void)
{
    int n = 0;
    lv_timer_t *t = lv_timer_get_next(NULL);

    while (t) {
        n++;
        t = lv_timer_get_next(t);
    }
    return n;
}

static void cycles(void)
{
    int t_before;
    uint32_t c_before;
    size_t heap_before;
    int i;
    int ok = 1;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:500:1", 1);
    app_start();
    app_stop();
    t_before = timers();
    c_before = lv_obj_get_child_count(g_content);
    heap_before = mallinfo2().uordblks;
    for (i = 0; i < 20; i++) {
        app_start();
        tap("START CAPTURE");
        ok = ok && wait_for("CAPTURING", 3500);
        pump(100);
        app_stop();
    }
    CHECK("twenty opens, captures and closes", ok);
    CHECK("leave no helper, no timer and no object (I4)",
          no_child() && timers() == t_before && lv_obj_get_child_count(g_content) == c_before);
    printf("     heap %+ld bytes over 20 cycles\n", (long)mallinfo2().uordblks - (long)heap_before);
    CHECK("and no heap growth beyond one screen", (long)mallinfo2().uordblks - (long)heap_before < 1024 * 1024);
}

/* The model has a packet other than not_seq (the inspector need not be shown). */
static bool wait_model_packet(uint32_t not_seq, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        const struct psk_model *m = app()->model;

        if (m->have_pkt && m->pkt.head.seq != not_seq) {
            return true;
        }
        pump(20);
    }
    return false;
}

/* Findings (M7), and the keys and the two shapes (M9). */
static void m7_m9(bool portrait)
{
    struct psk_app *a;
    lv_area_t la;
    lv_area_t lb;
    uint32_t seq;
    unsigned i;
    int n;
    int ok;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:2000:1", 1);
    app_start();
    a = app();
    CHECK("no key starts a capture (ADR-016 rule 1)",
          !psk_key(a, LV_KEY_ENTER) && !psk_key(a, ' ') && !psk_key(a, 's') && (pump(300), no_child()));
    tap("START CAPTURE");
    wait_for("CAPTURING", 3500);
    pump(5600); /* a SYN is unanswered after 3 s, a DNS query after 5 s */

    /* The shape. */
    if (portrait) {
        CHECK("tall: the shell's header above, no back slab, three tabs, the bar at the foot",
              !a->wide && lv_obj_has_flag(psk_app_part(app_priv, "back"), LV_OBJ_FLAG_HIDDEN) && shown("INSPECT") &&
                  lv_obj_get_parent(a->bar) == a->frame);
    } else {
        lv_obj_get_coords(psk_app_part(app_priv, "traffic"), &la);
        lv_obj_get_coords(psk_app_part(app_priv, "protocols"), &lb);
        CHECK("wide: a top row of its own with a back slab, LIVE and PACKETS, the state and the buttons",
              a->wide && target_ok(psk_app_part(app_priv, "back"), 56, "back") && !shown("INSPECT") &&
                  lv_obj_get_parent(a->bar) == a->top && lv_obj_get_parent(a->chip) != NULL &&
                  lv_obj_get_parent(lv_obj_get_parent(a->chip)) == a->top && shown("CAPTURING"));
        CHECK("wide: LIVE in two columns, the graph beside the protocols", la.x2 < lb.x1 && la.y1 == lb.y1);
        lv_obj_get_coords(a->top, &la);
        CHECK("wide: the top row is at the display's top edge, clear of its corners",
              la.y1 < 16 && pos_display_rect_is_safe(pocketui_display_geometry(), la.x1 + a->top_inset_l, la.y1,
                                                     la.x2 - a->top_inset_r, la.y2));
        printf("     top row %d,%d-%d,%d insets %d %d\n", (int)la.x1, (int)la.y1, (int)la.x2, (int)la.y2,
               (int)a->top_inset_l, (int)a->top_inset_r);
    }

    /* SIGNALS on LIVE. */
    ok = shown("SIGNALS · TAP TO LIST") != NULL;
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        char want[24];
        char part[8];
        lv_obj_t *cell;

        snprintf(part, sizeof(part), "sig%u", i);
        cell = psk_app_part(app_priv, part);

        psk_fmt_count(a->model->c.sig[i], want, sizeof(want));
        ok = ok && a->model->c.sig[i] > 0 && cell && containing_in(cell, psk_signal_word(i)) &&
             target_ok(cell, 56, psk_signal_word(i)) && strcmp(lv_label_get_text(a->sig_val[i]), want) == 0;
        if (!(a->model->c.sig[i] > 0)) {
            printf("     %s: none counted\n", psk_signal_word(i));
        }
    }
    CHECK("SIGNALS: six findings, each counted and a full target", ok);
    CHECK("nothing in SIGNALS is clipped, overlapping or broken across lines", audit_page() + dots_wrapped(app_body) == 0);
    shot("m7-signals");

    /* A finding is the way to its packets. */
    tap_obj(psk_app_part(app_priv, "sig1"));
    pump(700);
    n = rows_shown(NULL, 0);
    ok = n > 0;
    for (i = 0; (int)i < n; i++) {
        ok = ok && strncmp(lv_label_get_text(a->lrow[i].info), "! ", 2) == 0 &&
             strstr(lv_label_get_text(a->lrow[i].info), "RST") != NULL;
    }
    CHECK("a tap on TCP RST lists the resets, each marked with its `!`",
          ok && a->cur == PSK_TAB_PACKETS && a->sigmask == PPC_SIG_RST && shown("RESETS") &&
              containing_in(app_body, " OF ") != NULL);
    shot("m7-resets");
    tap_obj(psk_app_part(app_priv, "filter"));
    pump(100);
    CHECK("the filter sheet names the finding", shown("Filter packets") && shown("RESETS") && shown("Findings"));
    tap("Findings");
    CHECK("and a tap changes it", shown("DNS FAILURES") != NULL);
    tap("CLEAR ALL");
    tap("SHOW");
    pump(1800);
    CHECK("every packet again, and the rail is marked where the findings are",
          a->sigmask == 0 && shown("ALL PROTOCOLS") && a->model->marks_view == a->view_id &&
              a->model->marks_buckets > 0 && a->model->marks_mask != 0);
    shot("m7-rail");

    /* The inspector says what the finding is, under its layer. */
    psk_show_signal(a, 0);
    pump(700);
    tap_obj(psk_app_part(app_priv, "row0"));
    CHECK("a retransmission opens with its finding beside its class",
          wait_packet(0, 1500) && (a->model->pkt.head.signal & PPC_SIG_RETRANS) &&
              containing_in(psk_app_part(app_priv, "inspect"), "! RETRANSMISSION") != NULL);
    ok = 0;
    for (i = 0; i < PSK_LAYERS; i++) {
        lv_obj_t *f = a->layer[i].finding;

        if (f && !lv_obj_has_flag(f, LV_OBJ_FLAG_HIDDEN) && strstr(lv_label_get_text(f), "! RETRANSMISSION") &&
            strcmp(a->model->pkt.layer[i].name, "TCP") == 0 && a->layer_open[i]) {
            ok = 1;
        }
    }
    CHECK("and the sentence under its TCP layer, which is open", ok);
    shot("m7-inspect");
    if (!portrait) {
        lv_obj_get_coords(psk_app_part(app_priv, "list"), &la);
        lv_obj_get_coords(psk_app_part(app_priv, "inspect"), &lb);
        CHECK("wide: the list and the inspector side by side, on PACKETS",
              a->cur == PSK_TAB_PACKETS && la.x2 < lb.x1 && !lv_obj_has_flag(a->page[PSK_TAB_INSPECT], LV_OBJ_FLAG_HIDDEN) &&
                  !lv_obj_has_flag(a->page[PSK_TAB_PACKETS], LV_OBJ_FLAG_HIDDEN));
        lv_obj_get_coords(a->lrow[0].info, &la);
        lv_obj_get_coords(a->lrow[0].len, &lb);
        CHECK("wide: a row is one line, the sentence after the length", la.x1 > lb.x2 && la.y1 == lb.y1);
    }
    psk_set_view(a, PPC_CLASSES_ALL, "", 0, false);
    pump(600);

    /* The DNS and TCP diagnosis (M11.1). */
    psk_select_tab(a, PSK_TAB_LIVE);
    pump(1200);
    ok = a->model->have_diag && a->model->d.dns_answered > 0 && a->model->d.tcp_connected > 0;
    for (i = 0; i < PSK_DIAG_KINDS; i++) {
        lv_obj_t *r = psk_app_part(app_priv, i == 0 ? "diag0" : "diag1");

        if (r) {
            lv_obj_scroll_to_view_recursive(r, LV_ANIM_OFF); /* tall: LIVE scrolls to it */
            pump(60);
        }
        char sum[96];
        unsigned sev = psk_diag_summary(a->model, i, sum, sizeof(sum));
        const char *word = sev == PSK_SEV_WARN ? "! CHECK" : sev == PSK_SEV_NOTE ? "NOTE" : "OK";
        int good;

        /* A dots-mode label cuts its own text: the line is read from the
         * model, its start from the label. */
        good = r && target_ok(r, 56, i == 0 ? "DNS diagnosis" : "TCP diagnosis") &&
               strstr(sum, i == 0 ? " ANSWERED" : " CONNECTED") && strstr(sum, " MS") &&
               strncmp(lv_label_get_text(a->diag_text[i]), sum, 8) == 0 &&
               containing_in(r, i == 0 ? "DNS" : "TCP") && strcmp(lv_label_get_text(a->diag_sev[i]), word) == 0;
        if (!good) {
            printf("     %s: '%s' / '%s' / '%s'\n", i == 0 ? "DNS" : "TCP", sum, lv_label_get_text(a->diag_text[i]),
                   lv_label_get_text(a->diag_sev[i]));
        }
        ok = ok && good;
    }
    CHECK("SIGNALS: a DNS and a TCP line, each answered or connected, timed, with its severity", ok);
    CHECK("the DNS line says CHECK: queries went unanswered", containing_in(psk_app_part(app_priv, "diag0"), "! CHECK") &&
                                                                  a->model->c.sig[5] > 0);
    CHECK("nothing in SIGNALS is clipped with the diagnosis either", audit_page() + dots_wrapped(app_body) == 0);
    shot("m11-signals");

    tap_obj(psk_app_part(app_priv, "diag0"));
    pump(100);
    ok = psk_dsheet_shown(a) && shown("DNS diagnosis") && containing_in(a->dsheet, "QUERIES ANSWERED") &&
         containing_in(a->dsheet, "NAME NOT FOUND (NXDOMAIN)") && containing_in(a->dsheet, "! ") &&
         containing_in(a->dsheet, "NOT ANSWERED WITHIN 5 S") && containing_in(a->dsheet, "Not proof of a DNS failure") &&
         containing_in(a->dsheet, "heuristics, not proof") && containing_in(a->dsheet, "OPEN") &&
         containing_in(a->dsheet, "LIST");
    CHECK("a tap on DNS opens its diagnosis: answered and timed, NXDOMAIN, unanswered not called a failure", ok);
    CHECK("nothing in the DNS diagnosis is clipped or broken", dots_wrapped(a->dsheet) == 0);
    shot("m11-dns-sheet");
    tap_obj(containing_in(a->dsheet, "NOT ANSWERED WITHIN 5 S"));
    pump(300);
    wait_rows(1500);
    ok = !psk_dsheet_shown(a) && a->cur == PSK_TAB_PACKETS && a->sigmask == PPC_SIG_DNS_NOREPLY &&
         containing_in(app_body, "UNANSWERED DNS") && rows_shown(NULL, 0) > 0;
    if (!ok) {
        printf("     sheet %d tab %d mask %x words %d rows %d\n", psk_dsheet_shown(a), (int)a->cur, a->sigmask,
               containing_in(app_body, "UNANSWERED DNS QUERIES") != NULL, rows_shown(NULL, 0));
    }
    CHECK("its unanswered line lists the unanswered queries", ok);
    shot("m11-noreply-list");
    seq = a->insp_shown; /* the retransmission inspected above */
    tap_obj(psk_app_part(app_priv, "row0"));
    CHECK("an unanswered query says so, and that it is not proof of a failure",
          wait_packet(seq, 1500) && (a->model->pkt.head.signal & PPC_SIG_DNS_NOREPLY) &&
              containing_in(psk_app_part(app_priv, "inspect"), "! DNS QUERY NOT ANSWERED WITHIN 5 S") &&
              containing_in(psk_app_part(app_priv, "inspect"), "NOT PROOF OF A DNS FAILURE"));
    shot("m11-inspect-noreply");

    psk_set_view_diag(a, PPC_CLASSES_ALL, "", 0, false, 0, PPC_DIAG_DNS_ANSWER);
    psk_select_tab(a, PSK_TAB_PACKETS);
    pump(300);
    wait_rows(1500);
    seq = a->insp_shown;
    tap_obj(psk_app_part(app_priv, "row0"));
    CHECK("a DNS answer names its query and how long it took",
          a->diagf == PPC_DIAG_DNS_ANSWER && wait_packet(seq, 1500) &&
              containing_in(psk_app_part(app_priv, "inspect"), "ANSWER TO #") &&
              containing_in(psk_app_part(app_priv, "inspect"), " MS AFTER IT"));
    shot("m11-inspect-answer");

    psk_select_tab(a, PSK_TAB_LIVE);
    pump(100);
    tap_obj(psk_app_part(app_priv, "diag1"));
    pump(100);
    ok = psk_dsheet_shown(a) && shown("TCP diagnosis") && containing_in(a->dsheet, "CONNECTIONS OPENED") &&
         containing_in(a->dsheet, "REFUSED") && containing_in(a->dsheet, "Not a network fault") &&
         containing_in(a->dsheet, "RETRANSMISSIONS") && containing_in(a->dsheet, "SYN NOT ANSWERED");
    CHECK("a tap on TCP opens its diagnosis: connections and their time, refusals not called faults", ok);
    CHECK("nothing in the TCP diagnosis is clipped or broken", dots_wrapped(a->dsheet) == 0);
    shot("m11-tcp-sheet");
    CHECK("Back closes the diagnosis", app_packetshark.back(app_priv) == 1 && !psk_dsheet_shown(a));
    tap_obj(psk_app_part(app_priv, "diag1"));
    pump(100);
    tap_obj(containing_in(a->dsheet, " REFUSED"));
    pump(300);
    wait_rows(1500);
    n = rows_shown(NULL, 0);
    ok = n > 0;
    for (i = 0; (int)i < n; i++) {
        ok = ok && strstr(lv_label_get_text(a->lrow[i].info), "RST") != NULL;
    }
    CHECK("its refusals line lists the refused connections, each a reset",
          ok && a->cur == PSK_TAB_PACKETS && a->diagf == PPC_DIAG_TCP_REFUSED && a->sigmask == 0 &&
              shown("REFUSED CONNECTIONS"));
    shot("m11-refused");
    seq = a->insp_shown;
    tap_obj(psk_app_part(app_priv, "row0"));
    CHECK("a refusal says what it answered and that it is not a network fault",
          wait_packet(seq, 1500) && containing_in(psk_app_part(app_priv, "inspect"), "CONNECTION REFUSED") &&
              containing_in(psk_app_part(app_priv, "inspect"), "NOT A NETWORK FAULT"));
    shot("m11-inspect-refused");
    psk_select_tab(a, PSK_TAB_PACKETS);
    pump(100);
    tap_obj(psk_app_part(app_priv, "filter"));
    pump(100);
    CHECK("the filter sheet names the diagnosis, and the Findings row lets it go",
          shown("REFUSED CONNECTIONS") != NULL && (tap("Findings"), a->fs_diagv == 0 && shown("ANY PACKET") != NULL));
    tap("CANCEL");
    psk_select_tab(a, PSK_TAB_LIVE);
    pump(100);
    tap_obj(psk_app_part(app_priv, "diag1"));
    pump(100);
    seq = a->insp_shown;
    tap_obj(containing_in(a->dsheet, "CONNECTIONS OPENED"));
    CHECK("the summary opens the slowest handshake, which says how long it took",
          wait_packet(seq, 1500) && a->model->pkt.head.seq == a->model->d.tcp_hs_max_seq &&
              containing_in(psk_app_part(app_priv, "inspect"), "HANDSHAKE COMPLETE") &&
              containing_in(psk_app_part(app_priv, "inspect"), " AFTER SYN #"));
    shot("m11-inspect-handshake");
    psk_set_view(a, PPC_CLASSES_ALL, "", 0, false);
    pump(600);

    /* Keys (M9). */
    psk_key(a, '1');
    CHECK("key 1: LIVE", a->cur == PSK_TAB_LIVE);
    psk_key(a, '2');
    pump(600);
    CHECK("key 2: PACKETS", a->cur == PSK_TAB_PACKETS);
    a->insp_seq = 0;
    a->model->have_pkt = false;
    psk_key(a, LV_KEY_DOWN);
    CHECK("Down: the first packet shown is selected, the tab stays",
          wait_model_packet(0, 1500) && a->cur == PSK_TAB_PACKETS && a->sel_seq == a->model->pkt.head.seq);
    seq = a->model->pkt.head.seq;
    psk_key(a, LV_KEY_DOWN);
    CHECK("Down again: the next packet of the list", wait_model_packet(seq, 1500) && a->model->pkt.head.seq > seq);
    psk_key(a, LV_KEY_UP);
    CHECK("Up: back to the one before",
          wait_model_packet(a->model->pkt.head.seq, 1500) && a->model->pkt.head.seq == seq);
    psk_key(a, LV_KEY_ENTER);
    CHECK("Enter: the packet in INSPECT", psk_inspect_visible(a) && (a->wide || a->cur == PSK_TAB_INSPECT));
    psk_key(a, LV_KEY_RIGHT);
    CHECK("Right: the next packet", wait_packet(seq, 1500) && a->model->pkt.head.seq > seq);
    if (!a->wide) {
        psk_key(a, LV_KEY_DOWN);
        CHECK("Down in INSPECT: a field is selected", a->insp_field >= 0);
    }
    psk_key(a, 'p');
    pump(300);
    CHECK("P: pinned", (a->model->pkt.head.flags & PPC_ROWF_PINNED) != 0);
    psk_key(a, 'f');
    CHECK("F: the filter sheet, and no key answers a sheet but Esc",
          shown("Filter packets") && !psk_key(a, ' ') && !psk_key(a, LV_KEY_ENTER) && shown("Filter packets"));
    psk_key(a, LV_KEY_ESC);
    pump(100);
    CHECK("Esc closes it", !shown("Filter packets"));
    psk_key(a, ' ');
    CHECK("Space: HOLD", wait_for("RESUME", 2000) && a->model->state == PPC_STATE_HOLD);
    psk_key(a, ' ');
    pump(400);
    CHECK("Space again: capturing", a->model->state == PPC_STATE_CAPTURING);
    psk_key(a, 'S');
    pump(500);
    CHECK("S: stopped", a->model->state == PPC_STATE_STOPPED);
    psk_key(a, '1');
    g_home = 0;
    if (a->wide) {
        tap_obj(psk_app_part(app_priv, "back"));
        pump(100);
        CHECK("wide: the back slab is Back: it asks before leaving a capture",
              shown("Leave PacketShark?") && g_home == 0);
        psk_key(a, LV_KEY_ESC);
        pump(100);
    }
    psk_key(a, LV_KEY_ESC);
    pump(100);
    CHECK("Esc at the top level asks before leaving a capture", shown("Leave PacketShark?") && g_home == 0);
    tap("STAY");
    app_stop();
    CHECK("closing leaves no helper", no_child());
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("PACKETSHARK_HELPER");
    char cmd[700];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set PACKETSHARK_HELPER to tools/capture/pos-capture\n");
        return 2;
    }
    snprintf(root, sizeof(root), "/tmp/packetshark-app-%ld", (long)getpid());
    mkdir(root, 0700);
    setenv("POCKETOS_RUNTIME_DIR", root, 1);
    setenv("POCKETOS_STATE_DIR", root, 1);
    setenv("POCKETOS_LOG_DIR", root, 1);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    /* SAVE writes nothing under /tmp (ADR-016 rule 9): the test's captures
     * folder is beside where it runs, and removed at the end. */
    {
        char cwd[200];

        snprintf(captures_dir, sizeof(captures_dir), "%s/packetshark-app-captures-%ld",
                 getcwd(cwd, sizeof(cwd)) ? cwd : ".", (long)getpid());
        setenv("POCKETOS_CAPTURES_DIR", captures_dir, 1);
    }

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

    use_display(POS_ROTATION_0);
    setenv("POCKETOS_CAPTURE_HELPER", helper, 1);
    setup_screens();
    wifi_screens();
    capture_journey();
    remote_control();
    remote_request();
    m4_live(true);
    m5_m6(true);
    m7_m9(true);
    faults();
    setenv("POCKETOS_CAPTURE_HELPER", helper, 1);
    shot_orientation = "landscape";
    use_display(POS_ROTATION_90);
    setup_screens();
    wifi_screens();
    capture_journey();
    request_shape();
    m4_live(false);
    m5_m6(false);
    m7_m9(false);
    shot_orientation = "portrait";
    use_display(POS_ROTATION_0);
    cycles();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", root, captures_dir);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("packetshark_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
