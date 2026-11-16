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
 *     LINK, START disabled; Wi-Fi listed and not selectable;
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

/* sysd's interface list, as the test says it is. */
static int g_link = 1;
static int g_sysd_calls;

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
          shown("READY") && shown("NOTHING IS CAPTURED UNTIL YOU START") && strcmp(g_hint, "NOT CAPTURING") == 0);
    CHECK("eth0 is listed with its link and address", shown("eth0") && shown("LINK UP") &&
                                                          shown("ETHERNET · 192.0.2.44"));
    CHECK("wlan0 is listed and not offered", shown("wlan0") && shown("WI-FI · NOT AVAILABLE IN THIS VERSION"));
    CHECK("sit0 is not listed", shown("sit0") == NULL);
    CHECK("the safe defaults: HEADERS, 15 MIN, EVERYTHING, OFF",
          shown("HEADERS") && shown("15 MIN") && shown("EVERYTHING") && shown("OFF"));
    CHECK("the privacy note before START", shown("Packets stay in memory on this device") != NULL);
    CHECK("START CAPTURE is enabled", enabled("START CAPTURE"));
    {
        lv_obj_t *s = shown("START CAPTURE");
        lv_obj_t *l = shown("LIVE");

        CHECK("START is a full target inside the safe area", target_ok(s ? lv_obj_get_parent(s) : NULL, 64, "START"));
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
    CHECK("no link: NO LINK, START disabled", shown("NO LINK") && shown("CONNECT A CABLE TO ETH0") &&
                                                  shown("ETHERNET · UP") == NULL && !enabled("START CAPTURE"));
    shot("no-link");
    g_link = 1;
    pump(2200); /* the app asks sysd every 2 s while setup shows */
    CHECK("a cable arrives: READY without reopening", shown("READY") && enabled("START CAPTURE"));
    app_stop();
}

static void capture_journey(void)
{
    lv_obj_t *l;

    setenv("POCKETOS_CAPTURE_SOURCE", "synth:300:1", 1);
    app_start();
    CHECK("the simulator's stand-in is ready", shown("synth0") && shown("SIMULATED TRAFFIC · NO NETWORK") &&
                                                   strcmp(g_hint, "SIMULATED") == 0);
    tap("START CAPTURE");
    CHECK("START: capturing within the hello deadline", wait_for("CAPTURING", 3500));
    CHECK("the header says so", strcmp(g_hint, "CAPTURING · SYNTH0") == 0);
    pump(1500);
    l = containing_in(app_body, " PKT · ");
    CHECK("counters rise: the readout names the newest second's packets", l && !shown("WAITING FOR PACKETS"));
    CHECK("HOLD and STOP are full targets", target_ok(clickable_of(shown("HOLD")), 64, "HOLD") &&
                                                target_ok(clickable_of(shown("STOP")), 64, "STOP"));
    shot("capturing");
    tap("HOLD");
    CHECK("HOLD", wait_for("HOLD: ", 2000) && strcmp(g_hint, "HOLD · SYNTH0") == 0 && shown("RESUME"));
    pump(600);
    shot("hold");
    tap("RESUME");
    CHECK("RESUME", wait_for("CAPTURING", 2000) && shown("HOLD") && !shown("RESUME"));
    tap("PACKETS");
    pump(200);
    CHECK("PACKETS says how many packets the helper holds", containing_in(app_body, " PACKETS IN MEMORY") != NULL);
    shot("packets");
    CHECK("Back from PACKETS returns to LIVE", app_packetshark.back(app_priv) == 1 && shown("TRAFFIC"));
    tap("STOP");
    CHECK("STOP: stopped by you", wait_for("SYNTH0 · BY YOU", 2000) && strcmp(g_hint, "STOPPED · SYNTH0") == 0);
    CHECK("SAVE is shown and not available yet", shown("SAVE") && !enabled("SAVE") && enabled("DISCARD"));
    shot("stopped");
    tap("DISCARD");
    CHECK("DISCARD asks first", shown("Discard this capture?") != NULL);
    tap("STAY");
    CHECK("STAY keeps it", shown("STOPPED") && !shown("Discard this capture?"));
    tap("DISCARD");
    tap_last("DISCARD");
    CHECK("DISCARD: setup again", wait_for("START CAPTURE", 1000) && shown("READY"));
    pump(400);
    CHECK("and the helper has gone", no_child());

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
    tap("\xe2\x80\xba PACKETS");
    pump(100);
    CHECK("the readout's PACKETS opens the PACKETS tab", containing_in(app_body, "PACKETS IN MEMORY") != NULL);
    app_packetshark.back(app_priv);
    pump(100);
    tap("TCP");
    pump(100);
    CHECK("a protocol opens the PACKETS tab", containing_in(app_body, "PACKETS IN MEMORY") != NULL);
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
    CHECK("the header says the capture ended", strcmp(g_hint, "CAPTURE ENDED") == 0);
    CHECK("NEW CAPTURE is the one way on", shown("NEW CAPTURE") && enabled("NEW CAPTURE"));
    shot("helper-stopped");
    tap("NEW CAPTURE");
    CHECK("NEW CAPTURE: setup again", wait_for("START CAPTURE", 1000) && shown("READY"));
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

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("PACKETSHARK_HELPER");
    char cmd[300];

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
    capture_journey();
    m4_live(true);
    faults();
    setenv("POCKETOS_CAPTURE_HELPER", helper, 1);
    shot_orientation = "landscape";
    use_display(POS_ROTATION_90);
    setup_screens();
    capture_journey();
    m4_live(false);
    shot_orientation = "portrait";
    use_display(POS_ROTATION_0);
    cycles();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("packetshark_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
