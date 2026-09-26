/*
 * Wave in the running app, under a real LVGL pointer device and the real
 * key stream, driving tests/fake_pos_wave.sh in place of its audio helper.
 *
 * The model, the controller, the layout policy and the helper client have
 * their own tests. This one proves the screen, in portrait and in landscape:
 * one screen with no mode switch; typing reaches the field and SEND follows
 * it; SEND and Enter send and clear the field; LISTEN turns the microphone
 * banner, the chip and the header hint on for exactly as long as a helper
 * runs; a send while listening pauses and resumes the listen; CAPTURE records
 * and decodes; received and sent messages land in the history, which survives
 * a restart and clears with two taps; a tap on an entry copies it into the
 * field; the preset cycles and is remembered; errors are shown in words;
 * leaving the app ends a running helper - including one that ignores SIGTERM
 * - within its bound; every control is a 64 px target; nothing makes the body
 * scroll. And the keyboard: in portrait a tap on the field brings it up; in
 * landscape it never comes up by itself, KEYS brings it, and with it up the
 * screen is the composer row alone.
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

/* A square display that holds both orientations' bodies. */
#define PANEL_W 1232
#define PANEL_H 1232
/* Where the shell's content area starts for Wave (ui/shell/chrome.h): the
 * top edge, as for every app since the full-width bar went (DS §36). */
#define STATUS_H chrome_height(chrome_resolve(app_wave.chrome, false, false))
#define PORTRAIT_W 568
#define PORTRAIT_H 1232
/* The landscape body is the 1232 x 568 display under the app header, and
 * the touch keyboard (296) takes its height from the body while it is up;
 * the test body pads 20 around and 24 on top. */
#define LANDSCAPE_W 1232
#define LANDSCAPE_H (568 - STATUS_H - POCKETUI_HEADER_H)
#define LANDSCAPE_KB_H (LANDSCAPE_H - 296)

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
static int landscape;
static lv_obj_t *g_content;
static void body_follows_keyboard(void);
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
    body_follows_keyboard();
}
void pocketos_shell_keyboard_hide(void)
{
    kb_shown = 0;
    body_follows_keyboard();
}
int pocketos_shell_keyboard_visible(void) { return kb_shown; }
void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = landscape;
    out->next_landscape = landscape;
    out->keyboard = landscape ? POCKETOS_KEYBOARD_PRESENT : POCKETOS_KEYBOARD_ABSENT;
}
/* As the shell does: the touch keyboard takes its height from the body. */
static void body_follows_keyboard(void)
{
    if (!g_content) {
        return;
    }
    if (landscape) {
        lv_obj_set_size(g_content, LANDSCAPE_W, kb_shown ? LANDSCAPE_KB_H : LANDSCAPE_H);
    } else {
        lv_obj_set_size(g_content, PORTRAIT_W,
                        PORTRAIT_H - STATUS_H - POCKETUI_HEADER_H - (kb_shown ? 296 : 0));
    }
}

/* ---- display, finger, keys, time --------------------------------------- */

static uint8_t draw_buf[PANEL_W * 40 * 4];
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
    body_follows_keyboard();
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

/* The body never scrolls: whatever is laid out fits the content box. */
static int body_fits(void)
{
    lv_obj_update_layout(app_body);
    return lv_obj_get_scroll_y(app_body) == 0 && !lv_obj_has_flag(app_body, LV_OBJ_FLAG_HIDDEN) &&
           ({
               lv_area_t body;
               lv_area_t frame;
               lv_obj_t *f = lv_obj_get_child(app_body, 0);

               lv_obj_get_coords(app_body, &body);
               lv_obj_get_coords(f, &frame);
               frame.y2 <= body.y2 && frame.x2 <= body.x2;
           });
}

static void set_env_file(const char *name, const char *dir, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static int log_has(const char *path, const char *text)
{
    char buf[8192];
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;
    char state_dir[] = "/tmp/wave-app-state-XXXXXX";
    char run_dir[] = "/tmp/wave-app-run-XXXXXX";
    char tmp[] = "/tmp/wave-app-XXXXXX";
    char helper[PATH_MAX];
    char logpath[PATH_MAX];
    const char *fake = getenv("WAVE_FAKE_HELPER");
    char script[PATH_MAX];
    FILE *f;
    lv_obj_t *field;

    if (!fake || !realpath(fake, script) || !mkdtemp(state_dir) || !mkdtemp(run_dir) || !mkdtemp(tmp)) {
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
    set_env_file("helper.log", tmp, logpath, sizeof(logpath));
    setenv("POCKETOS_WAVE_HELPER", helper, 1);
    setenv("WAVE_FAKE_PIDFILE", pidfile, 1);
    setenv("WAVE_FAKE_LOG", logpath, 1);
    setenv("WAVE_FAKE", "auto", 1);
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
    setenv("POCKETOS_RUNTIME_DIR", run_dir, 1);

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
    lv_obj_set_pos(g_content, 0, STATUS_H + POCKETUI_HEADER_H);

    /* ==== portrait ========================================================== */
    app_start();
    check("one screen: no SEND/RECEIVE mode buttons, everything at once",
          !shows("RECEIVE") && shows("SEND") && shows("LISTEN") && shows("CAPTURE") && shows("HISTORY"));
    check("the chip says READY and the status that the microphone is off",
          shows("READY") && shows("Ready. Microphone off") && !shows("MICROPHONE ON"));
    check("the preset is named with its speed", shows("STANDARD - FAST") && shows("Fast speed, sent once"));
    check("SEND is disabled with nothing typed", disabled("SEND"));
    check("the counter starts at 0 of 64", shows("0 / 64 bytes"));
    check("an empty history says how to start", shows_part("Nothing yet."));
    check("CLEAR is disabled with nothing to clear", disabled("CLEAR"));
    check("opening writes no status hint", hint_writes == 0);
    check("opening does not throw up the keyboard", kb_show_calls == 0);
    check("opening starts no helper", !log_has(logpath, "listen") && !log_has(logpath, "send"));
    check("portrait has no KEYS button", !shows("KEYS"));
    check("every control is at least 64 px (DS 7)", small_targets(app_body) == 0);
    check("the body does not scroll", body_fits());
    field = find_field(app_body);
    check("the message field is the focused object", field && pos_input_focused() == field);

    tap_obj(field, "message field");
    check("portrait: tapping the field asks the shell for the keyboard", kb_shown && kb_show_calls == 1);
    check("with the keyboard up everything still fits, history included",
          body_fits() && shows("HISTORY") && small_targets(app_body) == 0);
    type("DOORS");
    check("typed characters reach the field", field && strcmp(lv_textarea_get_text(field), "DOORS") == 0);
    check("the counter follows", shows("5 / 64 bytes"));
    check("SEND is enabled", !disabled("SEND"));
    check("the field kept the focus through the taps", pos_input_focused() == field);

    /* ---- sending --------------------------------------------------------- */
    setenv("WAVE_FAKE_SEND_MS", "400", 1);
    tap("SEND");
    check("sending puts the keyboard away", !kb_shown);
    check("and empties the field for the next message", strcmp(lv_textarea_get_text(field), "") == 0);
    check("the chip says SENDING and the header hint too", shows("SENDING") && strcmp(hint, "SENDING") == 0);
    check("SEND becomes STOP", shows("STOP"));
    check("and then says Sent", wait_for("Sent", 0, 3000));
    check("the hint is cleared when it is done", strcmp(hint, "") == 0);
    check("the message is in the history as sent", shows_part("TX ") && shows("DOORS"));
    check("CLEAR is enabled now", !disabled("CLEAR"));

    type("ENTER");
    pos_input_focus(field);
    pos_input_push_key(LV_KEY_ENTER);
    pump(60);
    check("Enter sends too (the one key stream)", wait_for("ENTER", 0, 3000) && wait_for("Sent", 0, 3000));
    unsetenv("WAVE_FAKE_SEND_MS");

    /* ---- listening ------------------------------------------------------- */
    setenv("WAVE_FAKE_RX", "48454c4c4f", 1); /* HELLO */
    unlink(pidfile);
    tap("LISTEN");
    check("the MICROPHONE ON banner is up at once", shows("MICROPHONE ON"));
    check("the header hint says MIC ON", strcmp(hint, "MIC ON") == 0);
    check("the chip says LISTENING", shows("LISTENING"));
    check("the toggle reads STOP LISTEN", shows("STOP LISTEN"));
    check("a received message lands in the history", wait_for("HELLO", 0, 3000) && shows_part("RX "));
    check("every control while listening is at least 64 px", small_targets(app_body) == 0);
    check("and the body still does not scroll", body_fits());

    /* ---- talking while listening ------------------------------------------ */
    type("REPLY");
    tap("SEND");
    {
        int64_t end = real_ms() + 3000;

        while (real_ms() < end && !log_has(logpath, "text 5245504c59")) {
            pump(10);
        }
        check("a send while listening pauses the microphone and sends", log_has(logpath, "text 5245504c59"));
    }
    check("then listening resumes by itself", wait_for("LISTENING", 0, 3000) && shows("MICROPHONE ON"));
    check("the reply is in the history", shows("REPLY"));
    {
        pid_t pid = wait_helper_pid(3000);

        tap("STOP LISTEN");
        check("STOP LISTEN turns the microphone off", wait_for("Microphone off", 0, 3000));
        check("the banner goes when the helper has gone", !shows("MICROPHONE ON") && strcmp(hint, "") == 0);
        check("and the helper really has gone", gone(pid));
    }
    unsetenv("WAVE_FAKE_RX");

    /* ---- capture ---------------------------------------------------------- */
    setenv("WAVE_FAKE_CAPTURE", "4341505455524544", 1); /* CAPTURED */
    tap("CAPTURE");
    check("CAPTURE turns the microphone on", shows("MICROPHONE ON") && shows("CAPTURING"));
    check("and offers to decode early", shows("DECODE NOW"));
    check("the recording is decoded into the history", wait_for("CAPTURED", 0, 5000));
    check("marked as from a capture", shows_part("CAPTURE") && !shows("MICROPHONE ON"));

    /* STOP during a capture throws the recording away. */
    setenv("WAVE_FAKE_CAPTURE", "44495343415244", 1); /* DISCARD */
    setenv("WAVE_FAKE_RECORD_MS", "3000", 1);
    unlink(logpath);
    tap("CAPTURE");
    check("while capturing the SEND button reads STOP", shows("STOP") && !disabled("STOP"));
    tap("STOP");
    check("STOP ends the capture", wait_mic_off(3000) && shows("CAPTURE"));
    check("and throws the recording away: nothing is decoded",
          !wait_for("DISCARD", 0, 1000) && !log_has(logpath, "decode"));
    unsetenv("WAVE_FAKE_RECORD_MS");
    unsetenv("WAVE_FAKE_CAPTURE");

    /* ---- the history -------------------------------------------------------- */
    tap("HELLO");
    check("a tap on an entry copies its text into the field", strcmp(lv_textarea_get_text(field), "HELLO") == 0);
    check("without taking the keyboard up", !kb_shown);
    lv_textarea_set_text(field, "");
    pump(20);

    /* ---- presets ------------------------------------------------------------- */
    tap("STANDARD - FAST");
    check("the preset button cycles to ROBUST", shows("ROBUST - NORMAL") && shows("Slowest speed, sent twice"));
    type("TWICE");
    unlink(logpath);
    setenv("WAVE_FAKE_SEND_MS", "400", 1);
    tap("SEND");
    check("ROBUST plays two copies", wait_for("SENDING 1/2", 0, 3000) || wait_for("SENDING 2/2", 0, 3000));
    check("and says so when done", wait_for("Sent, 2 copies", 0, 5000));
    check("on the slowest speed", log_has(logpath, "audible_normal"));
    unsetenv("WAVE_FAKE_SEND_MS");

    /* ---- errors -------------------------------------------------------------- */
    setenv("POCKETOS_WAVE_HELPER", "/nonexistent/pos-wave", 1);
    tap("LISTEN");
    check("a missing helper says so", wait_for("Wave helper is not installed", 0, 3000));
    check("the chip says ERROR and the microphone is off", shows("ERROR") && wait_mic_off(3000));
    check("and nothing is retried", shows("LISTEN") && !shows("STOP LISTEN"));
    setenv("POCKETOS_WAVE_HELPER", helper, 1);
    g_volume_effective = 0;
    type("muted");
    tap("SEND");
    check("muted: nothing is sent, and it says why", wait_for("Sound is muted", 1, 1000));
    g_volume_effective = 100;
    lv_textarea_set_text(field, "");

    /* ---- restart: what is remembered ------------------------------------------ */
    app_stop();
    app_start();
    check("after a restart the preset is still ROBUST", shows("ROBUST - NORMAL"));
    check("and the history is all there", shows("DOORS") && shows("HELLO") && shows("CAPTURED") &&
                                              shows("TWICE"));
    check("but the microphone is off (the toggle is not remembered)", !shows("MICROPHONE ON") &&
                                                                        shows("LISTEN"));
    tap("CLEAR");
    check("CLEAR once asks to confirm", shows("CONFIRM") && shows("DOORS"));
    tap("CONFIRM");
    check("CONFIRM clears the history", !shows("DOORS") && shows_part("Nothing yet."));
    tap("ROBUST - NORMAL");
    tap("QUICK - FASTEST");
    check("the preset cycles back round to STANDARD", shows("STANDARD - FAST"));

    /* ---- leaving while the microphone is on -------------------------------- */
    unlink(pidfile);
    tap("LISTEN");
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
    setenv("WAVE_FAKE", "ignore_term", 1);
    unlink(pidfile);
    tap("LISTEN");
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
    setenv("WAVE_FAKE", "auto", 1);

    /* ==== landscape ========================================================= */
    landscape = 1;
    kb_show_calls = 0;
    app_start();
    field = find_field(app_body);
    check("landscape: opening does not bring the keyboard up", kb_show_calls == 0 && !kb_shown);
    check("landscape: KEYS is offered", shows("KEYS"));
    check("landscape: history, controls and composer are all there",
          shows("HISTORY") && shows("LISTEN") && shows("CAPTURE") && shows("SEND"));
    check("landscape: every control is at least 64 px", small_targets(app_body) == 0);
    check("landscape: the body does not scroll", body_fits());
    tap_obj(field, "message field");
    check("landscape: a tap on the field does NOT bring the touch keyboard up",
          kb_show_calls == 0 && !kb_shown && pos_input_focused() == field);
    type("typed on a keyboard");
    check("landscape: a physical keyboard types into the field",
          strcmp(lv_textarea_get_text(field), "typed on a keyboard") == 0);
    tap("KEYS");
    pump(40);
    check("landscape: KEYS brings the touch keyboard up", kb_shown && kb_show_calls == 1);
    check("landscape with the keyboard up: only the composer row is left",
          !shows("HISTORY") && !shows("LISTEN") && shows("SEND") && shows("HIDE"));
    check("and it fits the 100 px above the keyboard", body_fits() && small_targets(app_body) == 0);
    tap("HIDE");
    pump(40);
    check("landscape: HIDE puts it away and gives the screen back", !kb_shown && shows("HISTORY") &&
                                                                       shows("KEYS"));
    tap("KEYS");
    pump(40);
    tap("SEND");
    check("landscape: sending with the touch keyboard up puts it away", wait_for("Sent", 0, 3000) &&
                                                                           !kb_shown && shows("HISTORY"));
    check("landscape: the message is in the history", shows("typed on a keyboard"));
    app_stop();

    check("nothing was left in the runtime directory (no stray recording)", ({
              char p[PATH_MAX];
              snprintf(p, sizeof(p), "%s/wave", run_dir);
              dir_empty(p);
          }));
    {
        char p[PATH_MAX];

        snprintf(p, sizeof(p), "%s/wave/history", state_dir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/wave/wave.conf", state_dir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/wave", state_dir);
        rmdir(p);
        snprintf(p, sizeof(p), "%s/wave", run_dir);
        rmdir(p);
    }
    unlink(helper);
    unlink(pidfile);
    unlink(logpath);
    rmdir(tmp);
    rmdir(state_dir);
    rmdir(run_dir);
    printf("wave_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
