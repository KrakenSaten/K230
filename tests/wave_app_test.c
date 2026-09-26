/*
 * Wave in the running app, under a real LVGL pointer device and the real
 * key stream, driving tests/fake_pos_wave.sh in place of its audio helper.
 *
 * The view model and the helper client have their own tests. This one proves
 * the screen: that the mode buttons swap the panels, that typing reaches the
 * message field and Transmit follows it, that Enter transmits, that a listen
 * turns the microphone banner and the status-bar hint on for exactly as long
 * as the helper runs, that a received message lands in the list, that Stop
 * works, that errors are shown in words, that leaving the app ends a running
 * helper - including one that ignores SIGTERM - within its bound, that every
 * control is a 64 px target, and that nothing is stored.
 *
 * No audio device is opened at any point: the helper is a shell script.
 * Needs LVGL; built by ui/shell/CMakeLists.txt (host only) and run by
 * tests/wave_shell_test.sh with WAVE_FAKE_HELPER set.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "pocketui.h"
#include "wave_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
/* Where the shell's content area starts for Wave (ui/shell/chrome.h): the
 * top edge, as for every app since the full-width bar went (DS §36). */
#define STATUS_H chrome_height(chrome_resolve(app_wave.chrome, false, false))

extern const struct pocketos_app app_wave;

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the shell's side of app.h ----------------------------------------- */

static char hint[32];
static int hint_writes;
static int kb_shown;
static int kb_show_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(hint, sizeof(hint), "%s", text ? text : "");
    hint_writes++;
}
void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
int64_t pocketos_shell_system_day(void) { return -1; }
const char *pocketos_shell_radio_state(void) { return NULL; }
/* The system volume the app reads before a send (ui/shell/volume.h). */
static int g_volume_effective = 100;
int pocketos_shell_volume_effective(void) { return g_volume_effective; }
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    kb_shown = 1;
    kb_show_calls++;
}
void pocketos_shell_keyboard_hide(void) { kb_shown = 0; }
int pocketos_shell_keyboard_visible(void) { return kb_shown; }

/* ---- display, finger, keys, time --------------------------------------- */

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

static int64_t real_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static lv_obj_t *g_content;
static lv_obj_t *app_body;
static void *app_priv;

static lv_obj_t *find_label(lv_obj_t *obj, const char *text, int partial)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && (partial ? strstr(t, text) != NULL : strcmp(t, text) == 0)) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_label(lv_obj_get_child(obj, i), text, partial);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int shows(const char *text) { return find_label(app_body, text, 0) != NULL; }
static int shows_part(const char *text) { return find_label(app_body, text, 1) != NULL; }

static lv_obj_t *target_of(const char *text)
{
    lv_obj_t *o = find_label(app_body, text, 0);

    while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

static void tap_obj(lv_obj_t *obj, const char *what)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on \"%s\": not on screen\n", what);
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
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void tap(const char *text)
{
    tap_obj(target_of(text), text);
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

static lv_obj_t *find_field(lv_obj_t *obj)
{
    uint32_t i;

    if (!obj) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_field(lv_obj_get_child(obj, i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* Let the helper run in real time while LVGL keeps polling it, until the
 * screen shows text (or, with partial, contains it). */
static int wait_for(const char *text, int partial, int bound_ms)
{
    int64_t end = real_ms() + bound_ms;

    while (real_ms() < end) {
        struct timespec d = { 0, 5 * 1000000L };

        pump(10);
        if (partial ? shows_part(text) : shows(text)) {
            return 1;
        }
        nanosleep(&d, NULL);
    }
    return 0;
}

/* The opposite: until the screen no longer shows text and the hint is
 * empty. The microphone indicator goes when the helper has gone, and a
 * helper may print its error some time before it exits - under load, long
 * enough that a check made the moment the error appears still sees MIC ON. */
static int wait_mic_off(int bound_ms)
{
    int64_t end = real_ms() + bound_ms;

    while (real_ms() < end) {
        struct timespec d = { 0, 5 * 1000000L };

        pump(10);
        if (!shows("MICROPHONE ON") && strcmp(hint, "") == 0) {
            return 1;
        }
        nanosleep(&d, NULL);
    }
    return 0;
}

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

static void app_start(void)
{
    app_body = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_size(app_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_wave.create(app_body);
    pump(80);
}

static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_wave.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_body);
    app_body = NULL;
    pocketos_shell_set_status_hint(""); /* as shell.c app_close() does */
    pump(20);
}

static char pidfile[PATH_MAX];

static pid_t helper_pid(void)
{
    FILE *f = fopen(pidfile, "r");
    long pid = -1;

    if (f) {
        if (fscanf(f, "%ld", &pid) != 1) {
            pid = -1;
        }
        fclose(f);
    }
    return (pid_t)pid;
}

/* The helper's pid, once it has started and written it. */
static pid_t wait_helper_pid(int bound_ms)
{
    int64_t end = real_ms() + bound_ms;
    pid_t pid;

    while ((pid = helper_pid()) <= 0 && real_ms() < end) {
        struct timespec d = { 0, 5 * 1000000L };

        pump(10);
        nanosleep(&d, NULL);
    }
    return pid;
}

static int gone(pid_t pid)
{
    return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH;
}

static int dir_empty(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return n == 0;
}

/* ---- the tests --------------------------------------------------------- */

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;
    char state_dir[] = "/tmp/wave-app-state-XXXXXX";
    char tmp[] = "/tmp/wave-app-XXXXXX";
    char helper[PATH_MAX];
    const char *fake = getenv("WAVE_FAKE_HELPER");
    char script[PATH_MAX];
    FILE *f;
    lv_obj_t *field;

    if (!fake || !realpath(fake, script) || !mkdtemp(state_dir) || !mkdtemp(tmp)) {
        fprintf(stderr, "set WAVE_FAKE_HELPER to tests/fake_pos_wave.sh\n");
        return 2;
    }
    snprintf(helper, sizeof(helper), "%s/pos-wave", tmp);
    f = fopen(helper, "w");
    if (!f) {
        return 2;
    }
    fprintf(f, "#!/bin/sh\nexec bash '%s' \"$@\"\n", script);
    fclose(f);
    chmod(helper, 0755);
    snprintf(pidfile, sizeof(pidfile), "%s/helper.pid", tmp);
    setenv("POCKETOS_WAVE_HELPER", helper, 1);
    setenv("WAVE_FAKE_PIDFILE", pidfile, 1);
    setenv("POCKETOS_STATE_DIR", state_dir, 1);

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

    /* ---- opening --------------------------------------------------------- */
    app_start();
    check("opens in SEND with its message panel", shows("SEND") && shows("MESSAGE") && !shows("RECEIVED"));
    check("TRANSMIT is there and disabled with nothing typed", shows("TRANSMIT") && disabled("TRANSMIT"));
    check("the status says ready", shows("Ready to send"));
    check("the counter starts at 0 of 64", shows("0 / 64 bytes"));
    check("the three speeds are offered", shows("NORMAL") && shows("FAST") && shows("FASTEST"));
    check("opening writes no status hint", hint_writes == 0);
    check("opening does not throw up the keyboard", kb_show_calls == 0);
    check("every control is at least 64 px (DS 7)", small_targets(app_body) == 0);
    field = find_field(app_body);
    check("the message field is the focused object", field && pos_input_focused() == field);

    /* ---- typing ---------------------------------------------------------- */
    tap_obj(field, "message field");
    check("tapping the field asks the shell for the keyboard", kb_shown && kb_show_calls == 1);
    type("DOORS");
    check("typed characters reach the field", field && strcmp(lv_textarea_get_text(field), "DOORS") == 0);
    check("the counter follows", shows("5 / 64 bytes"));
    check("TRANSMIT is enabled", !disabled("TRANSMIT"));
    check("the field kept the focus through the taps", pos_input_focused() == field);

    /* ---- sending --------------------------------------------------------- */
    setenv("WAVE_FAKE", "send_ok", 1);
    tap("TRANSMIT");
    check("transmitting puts the keyboard away", !kb_shown);
    check("the button becomes STOP at once", shows("STOP"));
    check("the status bar says SENDING", strcmp(hint, "SENDING") == 0);
    check("the mode cannot be switched while sending", disabled("RECEIVE"));
    check("and then says Sent", wait_for("Sent", 0, 3000));
    check("the hint is cleared when it is done", strcmp(hint, "") == 0);
    check("TRANSMIT is back", shows("TRANSMIT") && !disabled("TRANSMIT"));

    tap("FASTEST");
    setenv("WAVE_FAKE", "args", 1);
    pos_input_focus(field);
    pos_input_push_key(LV_KEY_ENTER);
    pump(60);
    /* The "args" helper exits without saying "sent", so a status that says
     * the send did not finish can only come from a send Enter started. */
    check("Enter transmits too (the one key stream)", wait_for("Sending did not finish", 0, 3000));
    wait_for("TRANSMIT", 0, 3000);

    /* ---- receiving ------------------------------------------------------- */
    tap("RECEIVE");
    check("RECEIVE swaps the panels", shows("RECEIVED") && !shows("MESSAGE"));
    check("the microphone is said to be off", shows("Microphone off") && !shows("MICROPHONE ON"));
    check("with nothing received yet", shows("Nothing received yet"));
    check("START LISTENING is offered", shows("START LISTENING") && !disabled("START LISTENING"));
    check("every control in RECEIVE is at least 64 px", small_targets(app_body) == 0);

    setenv("WAVE_FAKE", "listen_ok", 1);
    unlink(pidfile);
    tap("START LISTENING");
    check("the MICROPHONE ON banner is up at once", shows("MICROPHONE ON"));
    check("the status bar says MIC ON", strcmp(hint, "MIC ON") == 0);
    check("the button stops", shows("STOP LISTENING"));
    check("a received message lands in the list", wait_for("DOORS", 0, 3000));
    check("and the empty note is gone", !shows("Nothing received yet"));
    check("the mode stays locked while listening", disabled("SEND"));
    tap("SEND");
    check("tapping SEND while listening changes nothing", shows("STOP LISTENING") && shows("MICROPHONE ON"));
    {
        pid_t pid = wait_helper_pid(3000);

        tap("STOP LISTENING");
        check("stopping", wait_for("Stopped", 0, 3000));
        check("the banner goes when the helper has gone", !shows("MICROPHONE ON") && strcmp(hint, "") == 0);
        check("and the helper really has gone", gone(pid));
    }
    check("the message stays on screen after stopping", shows("DOORS"));

    /* ---- errors ------------------------------------------------------------ */
    setenv("WAVE_FAKE", "garbage", 1);
    tap("START LISTENING");
    check("a helper's error is shown in words", wait_for("The speaker is not enabled on this device yet", 0, 3000));
    /* The error is shown while the helper still runs; the indicator stays
     * honest until it has exited, and must then go. */
    check("and the microphone indicator is off once the helper has exited", wait_mic_off(3000));

    setenv("POCKETOS_WAVE_HELPER", "/nonexistent/pos-wave", 1);
    tap("START LISTENING");
    check("a missing helper says so", wait_for("Wave helper is not installed", 0, 3000));
    check("and the failed start is over before the next one", wait_mic_off(3000));
    setenv("POCKETOS_WAVE_HELPER", helper, 1);

    /* ---- leaving while the microphone is on -------------------------------- */
    setenv("WAVE_FAKE", "listen_ok", 1);
    unlink(pidfile);
    tap("START LISTENING");
    {
        pid_t pid = wait_helper_pid(3000);
        int64_t t0 = real_ms();

        check("the listen's helper is running before the app is left", pid > 0 && !gone(pid));

        app_stop();
        check("leaving the app ends the listen", gone(pid));
        check("within the destroy grace", real_ms() - t0 < 1000);
        check("and the hint is cleared", strcmp(hint, "") == 0);
    }

    /* A helper that ignores SIGTERM is killed, still within the bound. */
    app_start();
    tap("RECEIVE");
    setenv("WAVE_FAKE", "ignore_term", 1);
    unlink(pidfile);
    tap("START LISTENING");
    wait_for("MICROPHONE ON", 0, 1000);
    {
        struct timespec d = { 0, 200 * 1000000L };
        pid_t pid;
        int64_t t0;

        nanosleep(&d, NULL);
        pid = wait_helper_pid(3000);
        t0 = real_ms();
        app_stop();
        check("a helper that ignores SIGTERM is killed when the app is left", gone(pid));
        check("destroy stayed within grace + reap", real_ms() - t0 < 300 + 200 + 300);
    }

    check("Wave stored nothing", dir_empty(state_dir));
    unlink(helper);
    unlink(pidfile);
    rmdir(tmp);
    rmdir(state_dir);
    printf("wave_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
