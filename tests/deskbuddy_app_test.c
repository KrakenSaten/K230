/*
 * DeskBuddy in the running app: vision events handed to the real screen,
 * taps on the real buttons through a real LVGL pointer, keys through the
 * real stream, and the preferences and the guard log against a real store
 * in a temporary directory.
 *
 * The state machine's own suite (tests/db_brain_test.c) proves the rules.
 * This one proves what it cannot: that the screen follows the brain, that
 * the buttons do what they say, that what should be saved is saved and what
 * should not is not, that a closed app leaves no timer, no object and no
 * focus behind however often it is opened, that a sleeping face does not
 * repaint, and that the screen fits in portrait and landscape.
 *
 * The shell is not linked: this file hosts the app the way ui/shell/shell.c
 * does (a header and a padded body) and defines the app.h entry points.
 * Built by ui/shell/CMakeLists.txt beside the shell (host builds only) and
 * run by tests/deskbuddy_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "chrome.h"
#include "db_brain.h"
#include "db_guard.h"
#include "db_store.h"
#include "deskbuddy_app.h"
#include "pocketui.h"
#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H                                                                                                  \
    chrome_height(chrome_resolve(app_deskbuddy.chrome,                                                            \
                                 pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_deskbuddy;

static int failed;
static int checks;
static char state_dir[] = "/tmp/deskbuddy_app_testXXXXXX";

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the shell's side of app.h ------------------------------------------ */

static int reduced_motion;
static int keyboard_requests;

void pocketos_shell_set_status_hint(const char *text) { (void)text; }
void pocketos_shell_go_home(void) {}
int pocketos_shell_reduced_motion(void) { return reduced_motion; }
int64_t pocketos_shell_system_day(void) { return -1; } /* the board's clock is not set */
const char *pocketos_shell_radio_state(void) { return NULL; }
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    keyboard_requests++;
}
void pocketos_shell_keyboard_hide(void) { keyboard_requests++; }
int pocketos_shell_keyboard_visible(void) { return 0; }

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 4];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_display_t *g_disp;

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

static void capture_frame(void);
static bool capturing;
static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
        if (capturing && lv_tick_get() % 100 == 0) capture_frame();
    }
}

static void push_key(pos_key_t key)
{
    int t;

    pos_input_push_key(key);
    for (t = 0; t < 1000 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static lv_point_t centre_of(lv_obj_t *obj)
{
    lv_area_t a;
    lv_point_t p;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    p.x = a.x1 + lv_area_get_width(&a) / 2;
    p.y = a.y1 + lv_area_get_height(&a) / 2;
    return p;
}

static void tap_obj(lv_obj_t *obj)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        check("tap on a missing or hidden object", 0);
        return;
    }
    finger_point = centre_of(obj);
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static int timer_count(void)
{
    lv_timer_t *t;
    int n = 0;

    for (t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app;

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
    app = app_deskbuddy.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    app_deskbuddy.destroy(app);
    app = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static const struct db_brain *brain(void)
{
    return deskbuddy_app_brain(app);
}

static const char *caption(void)
{
    lv_obj_t *c = deskbuddy_app_caption(app);

    return c ? lv_label_get_text(c) : "(missing)";
}

static int shows(lv_obj_t *obj, const char *text)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    return !text || strcmp(lv_label_get_text(lv_obj_get_child(obj, 0)), text) == 0;
}

/* The first eye's height as drawn: laid out first, since a size set by the
 * app is only a request until LVGL's next layout pass. */
static int32_t eye_h(void)
{
    lv_obj_t *eye = deskbuddy_app_eye(app, 0);

    lv_obj_update_layout(eye);
    return lv_obj_get_height(eye);
}

static int hidden(lv_obj_t *obj)
{
    return obj && lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static int stored(const char *file)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/deskbuddy/%s", state_dir, file);
    return access(path, F_OK) == 0;
}

static int file_has(const char *file, const char *needle)
{
    char path[512];
    char text[8192];
    size_t n = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/deskbuddy/%s", state_dir, file);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

static void forget(void)
{
    char cmd[600];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s/deskbuddy'", state_dir);
    if (system(cmd) != 0) {
        printf("note: could not clear the store\n");
    }
}

#include "games_frame.h"

static int32_t status_h(void)
{
    return STATUS_H;
}

/* ---- opening ---------------------------------------------------------------- */

static void test_open(void)
{
    int timers = timer_count();

    forget();
    app_start();
    check("the app returns its state", app != NULL);
    check("it opens in Companion, idle", brain() && brain()->state == DB_ST_IDLE);
    check("with no vision it says so, once, and runs blind", brain()->seen == DB_SEEN_UNAVAILABLE);
    check("it made exactly one timer", timer_count() == timers + 1 && deskbuddy_app_timer(app) != NULL);
    check("the three modes and SET are offered",
          shows(deskbuddy_app_mode_button(app, DB_MODE_COMPANION), "BUDDY") &&
              shows(deskbuddy_app_mode_button(app, DB_MODE_GUARD), "GUARD") &&
              shows(deskbuddy_app_mode_button(app, DB_MODE_NIGHT), "NIGHT") &&
              shows(deskbuddy_app_settings_button(app), "SET"));
    check("Companion has no guard button", hidden(deskbuddy_app_action_button(app)));
    check("both eyes are drawn", !hidden(deskbuddy_app_eye(app, 0)) && !hidden(deskbuddy_app_eye(app, 1)) &&
                                     eye_h() > 40);
    check("the night clock is not shown by day", hidden(deskbuddy_app_clock(app)));
    check("the settings are closed", hidden(deskbuddy_app_settings_panel(app)));
    check("the screen fits the body, every button whole", games_screen_fits(app_body, app_body, "portrait") == 0);
    app_deskbuddy.tick(app);
    app_stop();
    check("opening and closing wrote nothing", !stored(DB_STORE_PREFS) && !stored(DB_STORE_GUARD));
    check("closing left no timer behind", timer_count() == timers);
}

/* ---- companion ------------------------------------------------------------------ */

static void test_companion(void)
{
    int32_t open_h;

    forget();
    app_start();
    open_h = eye_h();
    deskbuddy_app_inject(app, DB_VISION_PERSON_DETECTED);
    check("somebody arrives: it wakes", brain()->state == DB_ST_WAKE);
    check("the eyes open wide", eye_h() > open_h);
    pump(DB_WAKE_MS + 100);
    check("then it tries to recognise them", brain()->state == DB_ST_RECOGNIZING);
    deskbuddy_app_inject(app, DB_VISION_OWNER_RECOGNIZED);
    check("the owner: HELLO", brain()->state == DB_ST_OWNER_GREETING && strcmp(caption(), "HELLO") == 0);
    check("with happy eyes, shorter than open ones", eye_h() < open_h);
    pump(DB_GREETING_MS + 100);
    check("the greeting ends by itself", brain()->state == DB_ST_IDLE && strcmp(caption(), "") == 0);
    deskbuddy_app_inject(app, DB_VISION_UNKNOWN_PERSON);
    check("a stranger: a wary look and a question", brain()->state == DB_ST_UNKNOWN_REACTION &&
                                                        strcmp(caption(), "HM. WHO'S THIS?") == 0);
    pump(DB_UNKNOWN_REACTION_MS + 100);
    deskbuddy_app_inject(app, DB_VISION_NO_PERSON);
    pump(DB_IDLE_SLEEP_MS + 1000);
    check("alone for DB_IDLE_SLEEP_MS: asleep, eyes shut", brain()->state == DB_ST_SLEEP &&
                                                            eye_h() < open_h / 4);
    tap_obj(deskbuddy_app_face(app));
    check("a tap on the face wakes it", brain()->state == DB_ST_WAKE);
    app_deskbuddy.tick(app);
    app_stop();
    check("Companion keeps no log", !stored(DB_STORE_GUARD));
}

/* ---- guard, through the buttons --------------------------------------------------- */

static void test_guard(void)
{
    lv_obj_t *action;

    forget();
    app_start();
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_GUARD));
    action = deskbuddy_app_action_button(app);
    check("GUARD: disarmed, with an ARM button", brain()->state == DB_ST_GUARD_DISARMED && shows(action, "ARM") &&
                                                     strcmp(caption(), "GUARD OFF") == 0);
    {
        lv_area_t face;
        lv_area_t btn;

        lv_obj_update_layout(app_body);
        lv_obj_get_coords(deskbuddy_app_face(app), &face);
        lv_obj_get_coords(action, &btn);
        /* The face grew over a button that appeared after it was sized,
         * once: a column packed from its foot measured nothing. */
        check("a button that appears pushes the face up, it is not covered by it", face.y2 < btn.y1);
    }
    tap_obj(action);
    check("ARM arms it (blind: nobody to wait for)", brain()->state == DB_ST_GUARD_ARMED && shows(action, "DISARM") &&
                                                         strcmp(caption(), "WATCHING THE DESK") == 0);
    app_deskbuddy.tick(app);
    check("the armed state is saved at once", file_has(DB_STORE_PREFS, "guard_armed=1") &&
                                                  file_has(DB_STORE_PREFS, "mode=guard"));
    deskbuddy_app_inject(app, DB_VISION_UNKNOWN_PERSON);
    check("a stranger is noted", brain()->state == DB_ST_GUARD_UNKNOWN && strcmp(caption(), "VISITOR NOTED") == 0);
    deskbuddy_app_inject(app, DB_VISION_NO_PERSON);
    app_deskbuddy.tick(app);
    check("the visit is saved", file_has(DB_STORE_GUARD, " unknown ") && !file_has(DB_STORE_GUARD, " owner "));
    deskbuddy_app_inject(app, DB_VISION_OWNER_RECOGNIZED);
    check("the owner back: the news, and a button to take it",
          brain()->state == DB_ST_GUARD_ALERT_PENDING && strcmp(caption(), "1 VISIT WHILE YOU WERE AWAY") == 0 &&
              shows(action, "SEEN IT"));
    tap_obj(action);
    check("SEEN IT: disarmed, nothing left to report", brain()->state == DB_ST_GUARD_DISARMED &&
                                                           db_guard_unacknowledged(brain()->log) == 0 &&
                                                           shows(action, "ARM"));
    app_deskbuddy.tick(app);
    check("the log is saved acknowledged, with the owner's return",
          file_has(DB_STORE_GUARD, " owner ") && file_has(DB_STORE_GUARD, " unknown -1 1 0") &&
              file_has(DB_STORE_PREFS, "guard_armed=0"));
    tap_obj(action);
    check("armed again", db_state_armed(brain()->state));
    tap_obj(action);
    check("DISARM by hand", brain()->state == DB_ST_GUARD_DISARMED);
    app_stop();
}

/* ---- close and reopen -------------------------------------------------------------- */

static void test_reopen(void)
{
    int timers;
    int ok = 1;
    int i;

    forget();
    app_start();
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_GUARD));
    tap_obj(deskbuddy_app_action_button(app));
    deskbuddy_app_inject(app, DB_VISION_UNKNOWN_PERSON);
    app_stop(); /* no tick: destroy() must save on its own */
    timers = timer_count();
    app_start();
    check("reopened: still in GUARD, still armed", brain()->state == DB_ST_GUARD_ARMED && brain()->prefs.guard_armed);
    check("with the visitor still in the log", db_guard_count(brain()->log) == 1 &&
                                                   db_guard_unacknowledged(brain()->log) == 1);
    app_stop();
    for (i = 0; i < 25; i++) {
        app_start();
        deskbuddy_app_inject(app, (enum db_vision_kind)(i % DB_VISION_KIND_COUNT));
        pump(i * 37 % 400);
        app_stop();
        ok &= timer_count() == timers && lv_obj_get_child_count(g_content) == 0u && pos_input_focused() == NULL;
    }
    check("25 open/close rounds mid-animation: no timer, object or focus left behind", ok);
    pump(5000);
    check("and nothing calls back into a closed app", timer_count() == timers);
}

/* ---- settings ------------------------------------------------------------------------- */

static void test_settings(void)
{
    forget();
    app_start();
    tap_obj(deskbuddy_app_settings_button(app));
    check("SET opens the settings", !hidden(deskbuddy_app_settings_panel(app)));
    check("every switch starts ON", shows(deskbuddy_app_toggle(app, DB_PREF_NIGHT), "ON") &&
                                        shows(deskbuddy_app_toggle(app, DB_PREF_IDLE_ANIMATION), "ON"));
    tap_obj(deskbuddy_app_toggle(app, DB_PREF_NIGHT));
    check("a tap turns Night mode off", shows(deskbuddy_app_toggle(app, DB_PREF_NIGHT), "OFF") &&
                                            !brain()->prefs.on[DB_PREF_NIGHT]);
    tap_obj(deskbuddy_app_settings_done(app));
    check("DONE closes them", hidden(deskbuddy_app_settings_panel(app)));
    check("and NIGHT is no longer offered", hidden(deskbuddy_app_mode_button(app, DB_MODE_NIGHT)));
    tap_obj(deskbuddy_app_settings_button(app));
    push_key(LV_KEY_ESC);
    check("Esc closes them too", hidden(deskbuddy_app_settings_panel(app)));
    app_deskbuddy.tick(app);
    check("the choice is saved", file_has(DB_STORE_PREFS, "night=0"));
    app_stop();
    app_start();
    check("and still there after reopening", !brain()->prefs.on[DB_PREF_NIGHT] &&
                                                 hidden(deskbuddy_app_mode_button(app, DB_MODE_NIGHT)));
    app_stop();
}

/* ---- night -------------------------------------------------------------------------------- */

static void test_night(void)
{
    lv_obj_t *eye;

    forget();
    app_start();
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_NIGHT));
    eye = deskbuddy_app_eye(app, 0);
    check("NIGHT: the clock is shown", brain()->state == DB_ST_NIGHT_IDLE && !hidden(deskbuddy_app_clock(app)));
    check("an unset clock is dashes, never 1970", strcmp(lv_label_get_text(deskbuddy_app_clock(app)), "--:--") == 0);
    check("the eyes are dimmed", lv_obj_get_style_bg_opa(eye, 0) < LV_OPA_COVER);
    check("no guard button at night", hidden(deskbuddy_app_action_button(app)));
    deskbuddy_app_inject(app, DB_VISION_PERSON_DETECTED);
    check("somebody: the eyes open, still dim", brain()->state == DB_ST_NIGHT_PRESENCE &&
                                                   lv_obj_get_style_bg_opa(eye, 0) < LV_OPA_COVER);
    pump(DB_NIGHT_PRESENCE_MS + 100);
    check("and settle again", brain()->state == DB_ST_NIGHT_IDLE);
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_COMPANION));
    check("back to Companion: full brightness, no clock", lv_obj_get_style_bg_opa(eye, 0) == LV_OPA_COVER &&
                                                            hidden(deskbuddy_app_clock(app)));
    app_stop();
}

/* ---- quiet: no repaint, few wake-ups ------------------------------------------------------ */

static void test_quiet(void)
{
    unsigned paints;
    unsigned steps;

    forget();
    app_start();
    reduced_motion = 1;
    app_deskbuddy.tick(app);
    deskbuddy_app_pump(app);
    steps = deskbuddy_app_steps(app);
    paints = deskbuddy_app_face_paints(app);
    pump(30000);
    check("reduced motion, nobody there: 30 s without one repaint", deskbuddy_app_face_paints(app) == paints);
    check("and its timer wakes about once a second, not every frame", deskbuddy_app_steps(app) - steps <= 40);
    reduced_motion = 0;
    app_deskbuddy.tick(app);
    paints = deskbuddy_app_face_paints(app);
    pump(20000);
    check("with motion it blinks now and then", deskbuddy_app_face_paints(app) > paints);
    check("but not every frame", deskbuddy_app_face_paints(app) - paints < 40);
    app_stop();
}

/* ---- rapid events, and the simulation path ------------------------------------------------ */

static void test_rapid(void)
{
    int i;

    forget();
    app_start();
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_GUARD));
    tap_obj(deskbuddy_app_action_button(app));
    for (i = 0; i < 3000; i++) {
        deskbuddy_app_inject(app, i & 1 ? DB_VISION_NO_PERSON : DB_VISION_UNKNOWN_PERSON);
    }
    check("three thousand events in a burst: one visit logged", db_guard_count(brain()->log) == 1);
    for (i = 0; i < DB_GUARD_CAP + 8; i++) {
        deskbuddy_app_inject(app, DB_VISION_UNKNOWN_PERSON);
        pump(5);
        deskbuddy_app_inject(app, DB_VISION_NO_PERSON);
        pump(DB_VISIT_MERGE_MS + 10);
    }
    check("forty separate visits: the log stays bounded", db_guard_count(brain()->log) == DB_GUARD_CAP);
    app_stop();
}

static void test_sim(void)
{
    int timers = timer_count();

    forget();
    setenv("DESKBUDDY_SIM", "100:person 400:owner@900 5000:none", 1);
    setenv("DESKBUDDY_MODE", "companion", 1);
    app_start();
    pump(600);
    check("a script plays through the provider", brain()->state == DB_ST_OWNER_GREETING);
    push_key('3');
    check("the developer keys work in a simulation", brain()->seen == DB_SEEN_UNKNOWN);
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_GUARD));
    tap_obj(deskbuddy_app_action_button(app));
    push_key('3');
    app_deskbuddy.tick(app);
    app_stop();
    check("a simulation writes nothing, whatever happened in it", !stored(DB_STORE_PREFS) && !stored(DB_STORE_GUARD));
    setenv("DESKBUDDY_SIM", "100:ghost", 1);
    app_start();
    pump(300);
    check("a malformed script: blind, not broken", brain()->seen == DB_SEEN_UNAVAILABLE &&
                                                        brain()->state == DB_ST_IDLE);
    app_stop();
    unsetenv("DESKBUDDY_SIM");
    unsetenv("DESKBUDDY_MODE");
    app_start();
    push_key('3');
    check("without the variable the developer keys do nothing", brain()->seen == DB_SEEN_UNAVAILABLE);
    app_stop();
    check("no timer left by any of it", timer_count() == timers);
}

/* ---- landscape (DS §21) --------------------------------------------------------------- */

static void test_landscape(void)
{
    lv_area_t body;
    lv_area_t face;
    lv_area_t action;

    forget();
    finger_point = (lv_point_t){0};
    games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
    pump(60);
    app_start();
    tap_obj(deskbuddy_app_mode_button(app, DB_MODE_GUARD));
    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    check("landscape: the body is wider than tall", lv_area_get_width(&body) > lv_area_get_height(&body));
    check("landscape: the screen fits the body, every button whole",
          games_screen_fits(app_body, app_body, "landscape") == 0);
    lv_obj_get_coords(deskbuddy_app_face(app), &face);
    lv_obj_get_coords(deskbuddy_app_action_button(app), &action);
    check("landscape: the face is beside the controls, not over them", face.x2 < action.x1);
    tap_obj(deskbuddy_app_settings_button(app));
    check("landscape: DONE is inside the body",
          games_inside((lv_obj_get_coords(deskbuddy_app_settings_done(app), &action), &action), &body));
    tap_obj(deskbuddy_app_settings_done(app));
    finger_point = (lv_point_t){0};
    games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
    pump(100);
    check("turned back to portrait with the app open, the screen fits again",
          games_screen_fits(app_body, app_body, "portrait again") == 0);
    lv_obj_get_coords(deskbuddy_app_face(app), &face);
    lv_obj_get_coords(deskbuddy_app_action_button(app), &action);
    check("portrait: the face is above the controls", face.y2 < action.y1);
    app_stop();
}

/* Optional real LVGL application snapshots, using the shell's RGB888/PNG
 * capture path. This test hosts the actual app; it draws no mock character. */
static unsigned capture_number;
static void capture_named(const char *name)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    const char *dir = getenv("DESKBUDDY_CAPTURE_DIR");
    if (!dir) return;
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    check("application snapshot exists", snap != NULL);
    if (!snap) return;
    unsigned char *rgb = malloc((size_t)snap->header.w * snap->header.h * 3);
    if (!rgb) { lv_draw_buf_destroy(snap); check("snapshot allocation", false); return; }
    for (unsigned y = 0; y < snap->header.h; y++) {
        const unsigned char *src = snap->data + (size_t)y * snap->header.stride;
        unsigned char *dst = rgb + (size_t)y * snap->header.w * 3;
        for (unsigned x = 0; x < snap->header.w; x++) {
            dst[x * 3] = src[x * 3 + 2]; dst[x * 3 + 1] = src[x * 3 + 1]; dst[x * 3 + 2] = src[x * 3];
        }
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    unsigned rc = lodepng_encode24_file(path, rgb, snap->header.w, snap->header.h);
    check("PNG capture succeeds", rc == 0);
    free(rgb); lv_draw_buf_destroy(snap);
#else
    (void)name;
#endif
}
static void capture_frame(void)
{
    char name[40];
    snprintf(name, sizeof(name), "frame-%04u", capture_number++);
    capture_named(name);
}
static void stroke_obj(lv_obj_t *obj, int distance, int duration)
{
    lv_point_t start = centre_of(obj);
    start.x -= distance / 2;
    finger_point = start;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (int n = 1; n <= 10; n++) { finger_point.x = start.x + distance * n / 10; pump(duration / 10); }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}
static void drag_snack_to(lv_point_t target)
{
    lv_point_t start = centre_of(deskbuddy_app_snack(app));
    finger_point = start;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (int n = 1; n <= 12; n++) {
        finger_point.x = start.x + (target.x - start.x) * n / 12;
        finger_point.y = start.y + (target.y - start.y) * n / 12;
        pump(50);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}
static void test_personality(void)
{
    int timers = timer_count();
    forget();
    unsetenv("DESKBUDDY_VISION");
    app_start();
    check("Buddy defaults to the none provider without a camera override", strcmp(deskbuddy_app_provider(app), "none") == 0);
    setenv("DESKBUDDY_VISION", "none", 1);
    capturing = getenv("DESKBUDDY_CAPTURE_DIR") != NULL;
    capture_named("portrait-idle");
    pump(400);
    tap_obj(deskbuddy_app_face(app));
    check("real pointer tap causes a personality poke", deskbuddy_app_personality(app)->reaction == DB_REACT_POKE);
    capture_named("portrait-poke");
    pump(220);
    check("poke connects to curious eyes", deskbuddy_app_personality(app)->reaction == DB_REACT_CURIOUS);
    tap_obj(deskbuddy_app_face(app));
    tap_obj(deskbuddy_app_face(app));
    check("rapid taps briefly annoy", deskbuddy_app_personality(app)->reaction == DB_REACT_ANNOYED);
    pump(1000);
    check("annoyance naturally settles", deskbuddy_app_personality(app)->reaction == DB_REACT_CALM);
    stroke_obj(deskbuddy_app_face(app), 100, 700);
    check("one gentle stroke gives one happy reaction", deskbuddy_app_personality(app)->reaction == DB_REACT_HAPPY && deskbuddy_app_personality(app)->variation == 4);
    pump(220);
    capture_named("portrait-petting");
    pump(1700);
    stroke_obj(deskbuddy_app_face(app), 100, 100);
    check("a fast swipe does not pet or poke", deskbuddy_app_personality(app)->reaction == DB_REACT_CALM);
    tap_obj(deskbuddy_app_feed(app));
    check("Feed reveals a snack without poking", deskbuddy_app_personality(app)->snack && deskbuddy_app_personality(app)->pokes == 0);
    pump(250);
    capture_named("portrait-snack");
    drag_snack_to(centre_of(deskbuddy_app_face(app)));
    check("a real drag feeds the character", deskbuddy_app_personality(app)->reaction == DB_REACT_EATING && hidden(deskbuddy_app_snack(app)));
    pump(240);
    capture_named("portrait-eating");
    pump(2400);
    push_key('f');
    check("keyboard F reveals the same snack", deskbuddy_app_personality(app)->snack);
    push_key(LV_KEY_ENTER);
    check("keyboard Enter feeds it", deskbuddy_app_personality(app)->reaction == DB_REACT_EATING);
    pump(2400);
    tap_obj(deskbuddy_app_feed(app));
    tap_obj(deskbuddy_app_snack(app));
    check("tapping the snack is the accessible feeding alternative", deskbuddy_app_personality(app)->reaction == DB_REACT_EATING);
    pump(2400);
    tap_obj(deskbuddy_app_feed(app));
    lv_point_t outside = centre_of(deskbuddy_app_face(app));
    outside.x = 40; outside.y = 200;
    drag_snack_to(outside);
    check("dropping away cancels cleanly", !deskbuddy_app_personality(app)->snack && deskbuddy_app_personality(app)->reaction == DB_REACT_CALM);
    tap_obj(deskbuddy_app_feed(app));
    check("Back consumes only the open snack", app_deskbuddy.back(app) == 1 && !deskbuddy_app_personality(app)->snack);
    pump(60);
    tap_obj(deskbuddy_app_rest(app));
    check("Rest starts with heavy eyelids", deskbuddy_app_personality(app)->reaction == DB_REACT_DROWSY);
    pump(1100);
    check("then the character rests", deskbuddy_app_personality(app)->reaction == DB_REACT_ASLEEP);
    capture_named("portrait-sleeping");
    tap_obj(deskbuddy_app_face(app));
    check("touch wakes the resting character", deskbuddy_app_personality(app)->reaction == DB_REACT_WAKE);
    pump(1000);
    capturing = false;
    pump(DB_DROWSY_MS);
    check("inactivity first makes Buddy drowsy", deskbuddy_app_personality(app)->reaction == DB_REACT_DROWSY);
    pump(31000);
    check("continued inactivity lets Buddy sleep", deskbuddy_app_personality(app)->reaction == DB_REACT_ASLEEP);
    tap_obj(deskbuddy_app_rest(app));
    check("the explicit Wake button works", deskbuddy_app_personality(app)->reaction == DB_REACT_WAKE);
    pump(1000);
    tap_obj(deskbuddy_app_feed(app));
    finger_point = (lv_point_t){0};
    games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
    pump(100);
    check("rotation cancels a pending snack", !deskbuddy_app_personality(app)->snack);
    check("Buddy controls fit in landscape", games_screen_fits(app_body, app_body, "buddy landscape") == 0);
    capture_named("landscape-idle");
    stroke_obj(deskbuddy_app_face(app), 100, 700);
    pump(220);
    capture_named("landscape-petting");
    tap_obj(deskbuddy_app_feed(app));
    pump(250);
    capture_named("landscape-snack");
    drag_snack_to(centre_of(deskbuddy_app_face(app)));
    check("landscape feeding works", deskbuddy_app_personality(app)->reaction == DB_REACT_EATING);
    tap_obj(deskbuddy_app_feed(app));
    finger_point = centre_of(deskbuddy_app_snack(app));
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    app_stop();
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(100);
    check("close during a drag leaves no timer or focus", timer_count() == timers && pos_input_focused() == NULL);
    app_start();
    check("reopen starts calm without a stale snack", deskbuddy_app_personality(app)->reaction == DB_REACT_CALM && hidden(deskbuddy_app_snack(app)));
    app_stop();
    check("touch reactions persist no animation state", !stored(DB_STORE_PREFS) && !stored(DB_STORE_GUARD));
    finger_point = (lv_point_t){0};
    games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
    pump(100);
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;

    if (!mkdtemp(state_dir)) {
        printf("FAIL cannot make a temporary directory\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
    unsetenv("DESKBUDDY_SIM");
    unsetenv("DESKBUDDY_MODE");

    lv_init();
    g_disp = disp = lv_display_create(PANEL_W, PANEL_H);
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
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);

    setenv("DESKBUDDY_VISION", "none", 1);
    test_personality();
    test_open();
    test_companion();
    test_guard();
    test_reopen();
    test_settings();
    test_night();
    test_quiet();
    test_rapid();
    test_sim();
    test_landscape();

    check("no keyboard was asked for", keyboard_requests == 0);
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state_dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", state_dir);
        }
    }
    printf("deskbuddy_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
