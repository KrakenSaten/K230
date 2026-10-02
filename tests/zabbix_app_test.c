/*
 * Zabbix in the running app, driven by a real LVGL pointer device, with the
 * real helper (tools/zabbix/pos-zabbix) on the fake server behind it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners, portrait and landscape,
 * the body under the 72 px header the NONE chrome leaves. Time is real: the
 * helper runs on the wall clock, so every pump sleeps as long as it advances
 * LVGL's tick, and the session's deadlines mean what they say.
 *
 * What is checked: OVERVIEW shows the worst severity and the counts; the
 * four tabs are full touch targets inside the safe area; PROBLEMS and HOSTS
 * list what the server has, most important first; a row opens its host and
 * BACK returns; STATUS names the fake and switches its scenario; a server
 * that goes away leaves the data on screen under a banner that says so; a
 * refused token, no configuration (and its demo), a missing helper and a
 * crashing one each say what happened; a large estate stays bounded; twenty
 * opens and closes leave no helper behind.
 *
 * Needs: ZABBIX_HELPER, the path of tools/zabbix/pos-zabbix (built by make).
 * Built by ui/shell/CMakeLists.txt, run by tests/zabbix_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pocketui_audit.h"
#include "pos_theme.h"

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#include <dirent.h>
#include <errno.h>
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
    chrome_height(chrome_resolve(app_zabbix.chrome,                                               \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_zabbix;

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

/* ---- the shell's side of app.h ---------------------------------------------------- */

static char g_hint[64];
static char g_logged[256];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

/* The shell's one keyboard: only whether it is asked for (the body keeps its
 * height here). */
static bool g_kb_visible;
static int g_kb_shows;

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    g_kb_visible = true;
    g_kb_shows++;
}

void pocketos_shell_keyboard_hide(void)
{
    g_kb_visible = false;
}

int pocketos_shell_keyboard_visible(void)
{
    return g_kb_visible;
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = pocketui_display_geometry()->width > pocketui_display_geometry()->height;
    out->next_landscape = out->landscape;
}

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_logged, sizeof(g_logged), fmt, ap);
    va_end(ap);
}

/* ---- display and finger -------------------------------------------------------------- */

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

/* ---- finding things on screen ----------------------------------------------------------- */

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

static lv_obj_t *shown(const char *prefix)
{
    return app_body ? find_label_in(app_body, prefix, true) : NULL;
}

/* Something clickable that holds this label: a button, a tab or a row. */
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

static void count_in(lv_obj_t *obj, const char *const *words, int *n)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        int k;

        for (k = 0; words[k]; k++) {
            if (strcmp(lv_label_get_text(obj), words[k]) == 0) {
                (*n)++;
                return;
            }
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        count_in(lv_obj_get_child(obj, i), words, n);
    }
}

static int count_labels(const char *const *words)
{
    int n = 0;

    if (app_body) {
        count_in(app_body, words, &n);
    }
    return n;
}

static const char *const severity_words[] = { "DISASTER", "HIGH", "AVERAGE", "WARNING", "INFO", "N/C",
                                              NULL };
static const char *const avail_words[] = { "UP", "DOWN", "UNKNOWN", NULL };

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

static void body_area(lv_area_t *a);

/* A finger drag, the way a person scrolls: pressed on this object where it
 * is now (nothing is scrolled into view first), moved up by dy, released.
 * Whatever LVGL finds under the finger decides what scrolls. */
static void drag_up_from(lv_obj_t *obj, int dy)
{
    lv_area_t a;
    int i;

    if (!obj) {
        printf("FAIL drag from a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    {
        /* The finger stays on the body: LVGL warns about a point off the
         * screen, and a clean log is part of zabbix_shell_test. */
        lv_area_t b;

        body_area(&b);
        if (finger_point.y - dy < b.y1 + 1) {
            dy = finger_point.y - b.y1 - 1;
        }
    }
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    for (i = 1; i <= 20; i++) {
        finger_point.y = a.y1 + lv_area_get_height(&a) / 2 - dy * i / 20;
        pump(15);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(600); /* the throw settles */
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

/* ZABBIX_SHOTS=<dir>: keep a picture of the screen at each named step, for
 * review and for the docs. Nothing is checked on it. */
static const char *shot_orientation = "portrait";

static void shot(const char *name)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    const char *dir = getenv("ZABBIX_SHOTS");
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
    snprintf(path, sizeof(path), "%s/zabbix-%s-%s.png", dir, shot_orientation, name);
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

/* The helper: our one child process. */
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

/* ---- the app, hosted --------------------------------------------------------------------- */

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
    app_priv = app_zabbix.create(app_body);
    pump(20);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_zabbix.destroy(app_priv);
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

/* A target: at least 64 x 64, inside the body across, and - where it is on
 * screen at all - clear of the panel's corners. */
static bool target_ok(lv_obj_t *obj, const char *what)
{
    lv_area_t a;
    lv_area_t b;

    body_area(&b);
    lv_obj_get_coords(obj, &a);
    if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN ||
        a.x1 < b.x1 || a.x2 > b.x2) {
        printf("     %s: %d,%d-%d,%d in body %d,%d-%d,%d\n", what, (int)a.x1, (int)a.y1, (int)a.x2,
               (int)a.y2, (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2);
        return false;
    }
    if (a.y1 >= b.y1 && a.y2 <= b.y2 &&
        !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("     %s: in a corner at %d,%d-%d,%d\n", what, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
        return false;
    }
    return true;
}

/* Wholly inside the body, where it is now: on screen without scrolling. */
static bool in_body(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t b;

    if (!obj) {
        return false;
    }
    body_area(&b);
    lv_obj_get_coords(obj, &a);
    return a.y1 >= b.y1 && a.y2 <= b.y2;
}

/* ---- the journeys ----------------------------------------------------------------------- */

static void journey(const char *o)
{
    char name[160];
    int64_t took;
    int i;
    bool tabs_ok = true;
    static const char *const tabs[] = { "OVERVIEW", "PROBLEMS", "HOSTS", "STATUS" };

    setenv("POCKETOS_ZABBIX_BACKEND", "fake", 1);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    app_start();
#define CHECK(what, cond)                                                                          \
    do {                                                                                           \
        snprintf(name, sizeof(name), "%s: %s", o, what);                                           \
        check(name, cond);                                                                         \
    } while (0)
    CHECK("OVERVIEW shows the worst open severity", wait_for("9 open", 5000) &&
                                                        shown("1 at this severity, of 9 open"));
    CHECK("the header says SIMULATED", strcmp(g_hint, "SIMULATED") == 0);
    CHECK("the counts", shown("9 open · 6 unacknowledged") && shown("36 monitored · 2 down") &&
                            shown("1 unknown · 1 in maintenance"));
    CHECK("the server line names the fake", shown("Demo server · Zabbix 7.0.31 · SIMULATED") != NULL);
    for (i = 0; i < 4; i++) {
        lv_obj_t *t = shown(tabs[i]);

        if (!t || !target_ok(clickable_of(t), tabs[i])) {
            tabs_ok = false;
        }
    }
    CHECK("four tabs, each a full target clear of the corners", tabs_ok);
    shot_orientation = o;
    shot("overview");

    tap("PROBLEMS");
    CHECK("PROBLEMS: the title counts them", wait_for("9 open · most severe first", 2000));
    shot("problems");
    CHECK("PROBLEMS: nine rows", count_labels(severity_words) == 9);
    CHECK("PROBLEMS: the disaster's host is listed", shown("edge-osl-01") != NULL);
    {
        lv_obj_t *row = clickable_of(shown("edge-osl-01"));

        CHECK("PROBLEMS: a row is a full target", row && target_ok(row, "problem row"));
        tap_obj(row);
    }
    CHECK("a problem opens its host", wait_for("LATEST VALUES", 3000) && shown("edge-osl-01") &&
                                          shown("DOWN · 1 problem · DISASTER"));
    CHECK("the host's values are listed", shown("Zabbix agent ping") && shown("Down"));
    shot("detail-down");
    {
        lv_obj_t *back = clickable_of(shown("\xe2\x80\xb9 BACK"));

        CHECK("BACK is a full target", back && target_ok(back, "back"));
        tap_obj(back);
    }
    CHECK("BACK returns to the list", wait_for("9 open · most severe first", 2000) &&
                                          !shown("LATEST VALUES"));

    tap("HOSTS");
    CHECK("HOSTS: every host, attention first", wait_for("36 monitored · attention first", 2000) &&
                                                  count_labels(avail_words) == 36);
    shot("hosts");
    tap_obj(clickable_of(shown("db-osl-02")));
    CHECK("HOSTS: a host opens", wait_for("UP · 1 problem · HIGH", 3000) &&
                                     shown("MySQL: Service is down"));
    shot("detail-up");
    tap("STATUS");
    CHECK("STATUS: names the fake and never shows the token",
          wait_for("SIMULATED · scenario demo", 2000) && shown("API token (never shown)") &&
              !shown("doors-demo-token"));
    shot("status");
    CHECK("STATUS: REFRESH NOW is a target", target_ok(clickable_of(shown("REFRESH NOW")), "refresh"));
    {
        /* Reached by finger. In landscape the buttons are below the fold,
         * and a drag starting on a status line - nothing clickable there -
         * must scroll the page (unit A: it did not, when the pages were not
         * clickable themselves). */
        lv_obj_t *refresh = clickable_of(shown("REFRESH NOW"));
        bool below = !in_body(refresh);

        if (strcmp(o, "landscape") == 0) {
            CHECK("STATUS: in landscape REFRESH NOW starts below the fold", below);
        }
        if (below) {
            drag_up_from(shown("CONNECTION"), 300);
        }
        CHECK("STATUS: a finger drag from a status line brings REFRESH NOW into view", in_body(refresh));
    }
    tap("SCENARIO: demo");
    CHECK("STATUS: the scenario button switches the fake", wait_for("SCENARIO: healthy", 4000));
    tap("OVERVIEW");
    CHECK("the healthy server is ALL CLEAR", wait_for("ALL CLEAR", 4000) &&
                                                 shown("0 open · 0 unacknowledged"));
    shot("healthy");
    took = app_stop();
    CHECK("closing takes the helper along, quickly", no_child() && took < 600);
#undef CHECK
}

static void faults(void)
{
    pid_t pid;
    int i;
    int bounded;

    /* The server goes away: the data stays, under a banner. */
    setenv("POCKETOS_ZABBIX_FAKE", "drop", 1);
    app_start();
    check("drop: data first", wait_for("9 open", 5000));
    tap("STATUS");
    tap("REFRESH NOW");
    tap("OVERVIEW");
    check("drop: the banner says the server is gone and how old the data is",
          wait_for("Server not reachable · retry in", 5000) && shown("Server not reachable") &&
              strstr(lv_label_get_text(shown("Server not reachable")), "showing data from"));
    check("drop: and the data is still on screen", shown("9 open") && shown("1 at this severity"));
    shot_orientation = "portrait";
    shot("offline");
    tap("STATUS");
    check("drop: STATUS has the last error", wait_for("Offline, retrying", 2000) &&
                                                 shown("Server not reachable · "));
    app_stop();

    /* A refused token. */
    setenv("POCKETOS_ZABBIX_FAKE", "auth", 1);
    app_start();
    check("auth: says to check the token", wait_for("Access refused: check the API token", 5000));
    check("auth: no data invented", shown("--") && !shown("9 open"));
    app_stop();

    /* Nothing set up, and the demo. */
    setenv("POCKETOS_ZABBIX_BACKEND", "live", 1);
    unsetenv("POCKETOS_ZABBIX_FAKE");
    app_start();
    check("unconfigured: OVERVIEW explains how to set it up", wait_for("Not set up", 5000) &&
                                                                 shown("This unit has no Zabbix server"));
    check("unconfigured: TRY THE DEMO is a target", target_ok(clickable_of(shown("TRY THE DEMO")), "demo"));
    check("unconfigured: not SIMULATED", strcmp(g_hint, "") == 0);
    shot("setup");
    tap("TRY THE DEMO");
    check("the demo runs", wait_for("9 open", 5000) && strcmp(g_hint, "SIMULATED") == 0);
    tap("STATUS");
    check("the demo can be left", wait_for("LEAVE THE DEMO", 2000));
    tap("LEAVE THE DEMO");
    check("leaving it returns to set-up", wait_for("Not set up", 5000));
    app_stop();
    check("and no helper is left", no_child());

    /* No helper at all. */
    setenv("POCKETOS_ZABBIX_BACKEND", "fake", 1);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    {
        char *saved = strdup(getenv("POCKETOS_ZABBIX_HELPER"));

        setenv("POCKETOS_ZABBIX_HELPER", "/nonexistent/pos-zabbix", 1);
        app_start();
        check("missing helper: says it could not start", wait_for("Zabbix helper stopped (could not start)", 3000));
        app_stop();
        setenv("POCKETOS_ZABBIX_HELPER", saved, 1);
        free(saved);
    }

    /* A helper that crashes comes back. */
    app_start();
    check("crash: running first", wait_for("9 open", 5000));
    pid = child_pid();
    check("crash: the helper is our child", pid > 0);
    if (pid > 0) {
        kill(pid, SIGSEGV);
    }
    check("crash: said", wait_for("Zabbix helper stopped (crashed)", 3000));
    check("crash: the data stays meanwhile", shown("9 open") != NULL);
    check("crash: it restarts and the banner goes", wait_for("Updated", 6000) &&
                                                        (pump(3000), !shown("Zabbix helper stopped")));
    app_stop();

    /* A large estate stays bounded. */
    setenv("POCKETOS_ZABBIX_FAKE", "large", 1);
    app_start();
    check("large: totals exact", wait_for("1 200 open · 800 unacknowledged", 8000));
    tap("PROBLEMS");
    check("large: the list says it is the most severe 100 of 1 200",
          wait_for("100 of 1 200 open · most severe first", 3000));
    bounded = count_labels(severity_words);
    check("large: at most ZBX_PROBLEM_MAX rows", bounded == 100);
    shot("large-problems");
    tap("HOSTS");
    check("large: 200 of 1 500 hosts", wait_for("200 of 1 500 · attention first", 5000) &&
                                           count_labels(avail_words) == 200);
    if (!shown("200 of 1 500")) {
        lv_obj_t *t = shown("HOSTS");

        printf("     hosts title: %s; rows %d; banner %s; log %s\n", t ? lv_label_get_text(t) : "(none)",
               count_labels(avail_words),
               shown("Zabbix helper") ? lv_label_get_text(shown("Zabbix helper")) : "-", g_logged);
    }
    {
        lv_obj_t *l = shown("very-long-hostname");
        lv_area_t a;

        check("large: a long host name is cut with dots on one line",
              l && (lv_obj_update_layout(l), lv_obj_get_coords(l, &a), lv_area_get_height(&a) < 40) &&
                  strstr(lv_label_get_text(l), "...") != NULL);
    }
    tap("PROBLEMS");
    pump(200);
    {
        lv_obj_t *l = shown("Very long trigger name");
        lv_obj_t *row = l ? clickable_of(l) : NULL;
        lv_area_t a;
        lv_area_t r;

        check("large: a long problem name stays on one line, cut with dots, its row unchanged",
              l && row && (lv_obj_update_layout(row), lv_obj_get_coords(l, &a), lv_obj_get_coords(row, &r),
                           lv_area_get_height(&a) < 40 && lv_area_get_height(&r) < 2 * POCKETUI_TOUCH_MIN) &&
                  strstr(lv_label_get_text(l), "...") != NULL);
    }
    app_stop();

    /* Twenty opens and closes. */
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    bounded = 0;
    for (i = 0; i < 20; i++) {
        app_start();
        pump(i % 2 ? 150 : 20); /* sometimes before the helper answered, sometimes after */
        app_stop();
        bounded += no_child();
    }
    check("twenty opens and closes leave no helper behind", bounded == 20);
}

/* ---- CONNECTION ------------------------------------------------------------------------ */

/* The text field whose placeholder starts with this, visible or not. */
static lv_obj_t *field_in(lv_obj_t *obj, const char *placeholder)
{
    uint32_t i;

    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        const char *p = lv_textarea_get_placeholder_text(obj);

        return p && strncmp(p, placeholder, strlen(placeholder)) == 0 ? obj : NULL;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = field_in(lv_obj_get_child(obj, i), placeholder);

        if (f) {
            return f;
        }
    }
    return NULL;
}

static lv_obj_t *field(const char *placeholder)
{
    return app_body ? field_in(app_body, placeholder) : NULL;
}

/* Every label in the body, shown or not, that contains this text. */
static int labels_containing(lv_obj_t *obj, const char *needle)
{
    uint32_t i;
    int n = 0;

    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), needle)) {
        n++;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += labels_containing(lv_obj_get_child(obj, i), needle);
    }
    return n;
}

/* Typing as a keyboard would: the characters arrive in the field. */
static void type_into(lv_obj_t *ta, const char *text)
{
    if (!ta) {
        printf("FAIL typing into a missing field\n");
        failed++;
        checks++;
        return;
    }
    lv_textarea_set_text(ta, "");
    lv_textarea_add_text(ta, text);
    pump(40);
}

static bool file_has(const char *path, const char *needle)
{
    char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* The secret field: the one in password mode. */
static lv_obj_t *secret_field(void)
{
    lv_obj_t *f = field("Stored");

    if (!f) {
        f = field("API token");
    }
    if (!f) {
        f = field("Password");
    }
    return f;
}

static void connection(const char *o)
{
    char name[160];
    char conf[200];
    char sdir[200];
    char secret[220];
    FILE *f;
    lv_obj_t *ta;

    snprintf(conf, sizeof(conf), "%s/zabbix.conf", root);
    snprintf(sdir, sizeof(sdir), "%s/zabbix", root);
    snprintf(secret, sizeof(secret), "%s/secret", sdir);
    f = fopen(conf, "w");
    fprintf(f, "url=https://old.example.com/\nlabel=Desk\n");
    fclose(f);
    mkdir(sdir, 0700);
    f = fopen(secret, "w");
    fprintf(f, "token=stored-token-1f2e3d4c\n");
    fclose(f);
    chmod(secret, 0600);
    setenv("POCKETOS_ZABBIX_BACKEND", "fake", 1);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    shot_orientation = o;
#define CHECK(what, cond)                                                                          \
    do {                                                                                           \
        snprintf(name, sizeof(name), "%s: connection: %s", o, what);                               \
        check(name, cond);                                                                         \
    } while (0)

    app_start();
    CHECK("the app runs first", wait_for("9 open", 5000));
    tap("STATUS");
    CHECK("STATUS offers the connection settings", wait_for("CONNECTION SETTINGS", 2000) &&
                                                       target_ok(clickable_of(shown("CONNECTION SETTINGS")),
                                                                 "settings"));
    tap("CONNECTION SETTINGS");
    CHECK("opens over the tabs", wait_for("Connection", 2000) && !shown("OVERVIEW") && !shown("STATUS"));
    ta = field("https://");
    CHECK("the settings load: the stored address",
          ta && strcmp(lv_textarea_get_text(ta), "https://old.example.com/") == 0);
    CHECK("the sign-in is the token, marked in words", shown("\xe2\x80\xa2 API TOKEN") && shown("PASSWORD") &&
                                                           !shown("USER"));
    CHECK("a stored token is said to be there, never shown",
          shown("A token is stored and never shown") && secret_field() &&
              strcmp(lv_textarea_get_text(secret_field()), "") == 0 &&
              labels_containing(app_body, "stored-token") == 0);
    CHECK("BACK, the fields, TEST and SAVE are targets",
          target_ok(clickable_of(shown("\xe2\x80\xb9 BACK")), "back") && target_ok(ta, "url") &&
              target_ok(clickable_of(shown("TEST CONNECTION")), "test") &&
              target_ok(clickable_of(shown("SAVE")), "save"));
    shot("connection");

    /* A finger on a field brings the keyboard. */
    g_kb_visible = false;
    tap_obj(ta);
    CHECK("a tap on a field asks for the keyboard", g_kb_visible);

    /* Masked: what is typed is in no label, not even for a moment. */
    tap("PASSWORD");
    CHECK("PASSWORD shows the user field", wait_for("USER", 1000) && shown("\xe2\x80\xa2 PASSWORD") &&
                                               shown("No password is stored yet."));
    type_into(field("User name"), "nobody");
    type_into(secret_field(), "zq-hidden-77");
    CHECK("the password is masked: in no label anywhere",
          labels_containing(app_body, "zq-hidden") == 0 && labels_containing(app_body, "hidden-77") == 0 &&
              lv_textarea_get_password_mode(secret_field()));
    CHECK("only its length is said", shown("12 characters typed") != NULL);
    shot("connection-typed");

    tap("TEST CONNECTION");
    CHECK("TEST CONNECTION: a refused password is AUTH FAILED", wait_for("AUTH FAILED", 5000));
    CHECK("and the keyboard went away for the result", !g_kb_visible);
    shot("connection-authfail");
    type_into(field("User name"), "demo");
    tap("TEST CONNECTION");
    CHECK("TEST CONNECTION: the right user is CONNECTED", wait_for("CONNECTED", 5000) &&
                                                              wait_for("Zabbix 7.0.31 answered", 1000));
    CHECK("a test stores nothing", file_has(conf, "url=https://old.example.com/") &&
                                       file_has(secret, "token=stored-token"));
    ta = field("https://");
    type_into(ta, "ftp://old.example.com/");
    CHECK("an edit takes the last result off the screen", !shown("CONNECTED") &&
                                                              !shown("Zabbix 7.0.31 answered"));
    tap("TEST CONNECTION");
    CHECK("an address that is not https:// is INVALID CONFIG", wait_for("INVALID CONFIG", 5000));
    type_into(ta, "https://new.example.com/");
    type_into(secret_field(), "s3cret-word");
    tap("SAVE");
    CHECK("SAVE: connected and stored", wait_for("Saved and in use", 5000) && shown("CONNECTED"));
    CHECK("SAVE: the typed password is wiped from the field", strcmp(lv_textarea_get_text(secret_field()), "") == 0);
    CHECK("SAVE: the files hold the new settings, the label kept",
          file_has(conf, "url=https://new.example.com/") && file_has(conf, "auth=password") &&
              file_has(conf, "user=demo") && file_has(conf, "label=Desk") &&
              file_has(secret, "password=s3cret-word"));
    CHECK("SAVE: applied without a restart of anything but the helper",
          wait_for("A password is stored and never shown", 5000) && app_priv != NULL);
    shot("connection-saved");
    CHECK("Back closes CONNECTION, as its button does", app_zabbix.back(app_priv) == 1 &&
                                                         wait_for("OVERVIEW", 1000) && !shown("Connection"));
    CHECK("and Back on the tabs leaves the app to the shell", app_zabbix.back(app_priv) == 0);
    tap("OVERVIEW");
    CHECK("the data comes back", wait_for("9 open", 5000));
    app_stop();
    CHECK("closing leaves no helper", no_child());

    /* Opened again: what was saved is what loads. */
    app_start();
    tap("STATUS");
    tap("CONNECTION SETTINGS");
    CHECK("reopened: the saved settings load",
          wait_for("A password is stored", 5000) && field("https://") &&
              strcmp(lv_textarea_get_text(field("https://")), "https://new.example.com/") == 0 &&
              field("User name") && strcmp(lv_textarea_get_text(field("User name")), "demo") == 0 &&
              shown("\xe2\x80\xa2 PASSWORD"));
    /* A save that does not connect keeps the previous settings. */
    type_into(field("User name"), "nobody");
    type_into(secret_field(), "wrong");
    tap("SAVE");
    CHECK("SAVE refused: AUTH FAILED, not saved",
          wait_for("AUTH FAILED", 5000) &&
              labels_containing(app_body, "Not saved: the previous settings stay in use") == 1);
    CHECK("SAVE refused: the files are the previous ones",
          file_has(conf, "user=demo") && file_has(secret, "password=s3cret-word"));
    {
        lv_obj_t *back = clickable_of(shown("\xe2\x80\xb9 BACK"));

        tap_obj(back);
    }
    CHECK("its BACK button closes it too", wait_for("STATUS", 1000) && !shown("Connection"));
    app_stop();

    /* A server that is not there. */
    setenv("POCKETOS_ZABBIX_FAKE", "refused", 1);
    app_start();
    tap("STATUS");
    tap("CONNECTION SETTINGS");
    wait_for("Connection", 2000);
    tap("TEST CONNECTION");
    CHECK("a server that is not there is UNREACHABLE", wait_for("UNREACHABLE", 5000));
    type_into(secret_field(), "typed-not-saved");
    app_stop();
    CHECK("closing with a password typed and not saved: no helper left, the files unchanged",
          no_child() && file_has(secret, "password=s3cret-word"));
#undef CHECK
    unlink(conf);
    unlink(secret);
    rmdir(sdir);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
}

/* CONNECTION at every text size (DS §46): the layout audit finds nothing
 * clipped, drawn over something else or without a size. Truncation with
 * "..." is the app's own rule for a one-line label and is only listed. */
static void audit_issue(const struct pocketui_audit_issue *is, void *user)
{
    int *bad = user;

    if (is->kind == POCKETUI_AUDIT_TRUNCATED) {
        return;
    }
    (*bad)++;
    printf("     %s %s \"%s\" (%d,%d-%d,%d) other %s \"%s\"\n", pocketui_audit_kind_name(is->kind), is->path,
           is->text, (int)is->area.x1, (int)is->area.y1, (int)is->area.x2, (int)is->area.y2, is->other_path,
           is->other_text);
}

static void connection_sizes(const char *o)
{
    static const char *const names[] = { "small", "medium", "large" };
    char name[160];
    char conf[200];
    char sdir[200];
    char secret[220];
    FILE *f;
    int z;

    snprintf(conf, sizeof(conf), "%s/zabbix.conf", root);
    snprintf(sdir, sizeof(sdir), "%s/zabbix", root);
    snprintf(secret, sizeof(secret), "%s/secret", sdir);
    f = fopen(conf, "w");
    fprintf(f, "url=https://zabbix.example.com/\nauth=password\nuser=demo\n");
    fclose(f);
    mkdir(sdir, 0700);
    f = fopen(secret, "w");
    fprintf(f, "password=anything\n");
    fclose(f);
    chmod(secret, 0600);
    setenv("POCKETOS_ZABBIX_BACKEND", "fake", 1);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    for (z = POS_TEXT_SIZE_SMALL; z <= POS_TEXT_SIZE_LARGE; z++) {
        int bad = 0;
        int pass;

        pos_theme_select_text_size((enum pos_text_size)z);
        app_start();
        tap("STATUS");
        tap("CONNECTION SETTINGS");
        wait_for("Connection", 2000);
        tap("PASSWORD");
        tap("TEST CONNECTION");
        wait_for("CONNECTED", 5000);
        /* The page at its top, and scrolled to its foot: a control only
         * reachable by scrolling is audited where it can be seen too. */
        for (pass = 0; pass < 2; pass++) {
            lv_obj_update_layout(app_body);
            pocketui_audit(app_body, audit_issue, &bad, NULL);
            lv_obj_scroll_to_view_recursive(clickable_of(shown("SAVE")), LV_ANIM_OFF);
            pump(40);
        }
        lv_obj_scroll_to_y(clickable_of(shown("Connection")), 0, LV_ANIM_OFF);
        pump(40);
        snprintf(name, sizeof(name), "%s: connection at %s text: nothing clipped, overlapping or without a size",
                 o, names[z]);
        check(name, bad == 0 && shown("CONNECTED") != NULL);
        if (z == POS_TEXT_SIZE_LARGE) {
            shot_orientation = o;
            shot("connection-large");
        }
        app_stop();
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    unlink(conf);
    unlink(secret);
    rmdir(sdir);
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("ZABBIX_HELPER");
    char cmd[300];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set ZABBIX_HELPER to tools/zabbix/pos-zabbix\n");
        return 2;
    }
    snprintf(root, sizeof(root), "/tmp/zabbix-app-%ld", (long)getpid());
    mkdir(root, 0700);
    setenv("POCKETOS_CONFIG_DIR", root, 1); /* no zabbix.conf: the unconfigured case */
    setenv("POCKETOS_STATE_DIR", root, 1);
    setenv("POCKETOS_LOG_DIR", root, 1);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    setenv("POCKETOS_ZABBIX_HELPER", helper, 1);

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
    journey("portrait");
    use_display(POS_ROTATION_90);
    journey("landscape");
    use_display(POS_ROTATION_0);
    faults();
    connection("portrait");
    connection_sizes("portrait");
    use_display(POS_ROTATION_90);
    connection("landscape");
    connection_sizes("landscape");
    use_display(POS_ROTATION_0);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("zabbix_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
