/*
 * Recorder in the running app, driven by a real LVGL pointer device, with
 * the real helper (tests/pos-record-testhooks) over the file-backed sound
 * card behind it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners, portrait and landscape,
 * the body under the 72 px header the NONE chrome leaves. Time is real: the
 * helper runs on the wall clock, so every pump sleeps as long as it advances
 * LVGL's tick.
 *
 * What is checked: every control is a usable, safe, non-overlapping touch
 * target in both orientations; RECORD, PAUSE, RESUME and STOP give a saved
 * file that the list shows and selects; the meter moves with the real
 * samples and is empty when paused; PLAY plays it; DELETE asks and then
 * deletes; the header says MIC ON exactly while the microphone may be on;
 * rotating (a new app on the new body) while idle and while recording keeps
 * the files right; closing mid-recording saves; twenty opens and closes
 * leave no helper, no .part and no LVGL objects behind.
 *
 * REC_SHOTS=<dir> also saves the body as PNGs: portrait and landscape idle,
 * and recording.
 *
 * Needs: RECORD_HELPER, the path of tests/pos-record-testhooks. Built by
 * ui/shell/CMakeLists.txt, run by tests/recorder_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketui.h"
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
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
    chrome_height(chrome_resolve(app_recorder.chrome,                                             \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_recorder;

static int failed;
static int checks;
static char root[128];
static char recs[256];
static const char *shots;

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

/* ---- the shell's side of app.h ------------------------------------------------ */

static char g_hint[64];
static int g_volume = 70;
static int g_keyboard_asked;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

int64_t pocketos_shell_system_day(void)
{
    return -1; /* no clock: names are sequence numbers */
}

int pocketos_shell_volume_effective(void)
{
    return g_volume;
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    g_keyboard_asked++;
}

/* ---- display and finger -------------------------------------------------------- */

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

/* ---- finding things -------------------------------------------------------------- */

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

static lv_obj_t *label(const char *text)
{
    return app_body ? find_label_in(app_body, text, false) : NULL;
}

static bool shows(const char *prefix)
{
    return app_body && find_label_in(app_body, prefix, true) != NULL;
}

static lv_obj_t *button(const char *text)
{
    lv_obj_t *l = label(text);

    return l ? lv_obj_get_parent(l) : NULL;
}

static bool enabled(const char *text)
{
    lv_obj_t *b = button(text);

    return b && !lv_obj_has_state(b, LV_STATE_DISABLED);
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
    pump(40);
}

static void tap(const char *text)
{
    tap_obj(button(text));
}

static const char *want;

/* A label that says exactly this, or starts with it when it ends in a
 * space or a dash ("Saved REC-"); "RECORDING" is not "RECORDINGS 1". */
static bool text_shown(void)
{
    size_t n = strlen(want);

    return n && (want[n - 1] == ' ' || want[n - 1] == '-' || strncmp(want, "Deleted", 7) == 0 ||
                 strncmp(want, "Sound is", 8) == 0)
               ? shows(want)
               : label(want) != NULL;
}

static bool wait_text(const char *text, int ms)
{
    int64_t end = mono_ms() + ms;

    want = text;
    while (mono_ms() < end) {
        if (text_shown()) {
            return true;
        }
        pump(20);
    }
    return text_shown();
}

static void dump(lv_obj_t *obj)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        printf("     label '%s'\n", lv_label_get_text(obj));
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        dump(lv_obj_get_child(obj, i));
    }
}

static int files_named(const char *suffix)
{
    DIR *d = opendir(recs);
    struct dirent *e;
    int n = 0;
    size_t s = strlen(suffix);

    while (d && (e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);

        n += e->d_name[0] != '.' && l > s && strcmp(e->d_name + l - s, suffix) == 0;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/* The meter's fill: the only child of the track, as a percentage of it. */
static int meter_width(void)
{
    lv_obj_t *l = label("-");
    (void)l;
    {
        /* The track is the second child of the deck. */
        lv_obj_t *frame = lv_obj_get_child(app_body, 0);
        lv_obj_t *body = frame ? lv_obj_get_child(frame, 0) : NULL;
        lv_obj_t *deck = body ? lv_obj_get_child(body, 0) : NULL;
        lv_obj_t *track = deck ? lv_obj_get_child(deck, 1) : NULL;
        lv_obj_t *fill = track ? lv_obj_get_child(track, 0) : NULL;

        if (!fill || lv_obj_has_flag(fill, LV_OBJ_FLAG_HIDDEN)) {
            return 0;
        }
        lv_obj_update_layout(fill);
        return (int)(lv_obj_get_width(fill) * 100 / lv_obj_get_content_width(track));
    }
}

/* ---- hosting --------------------------------------------------------------------- */

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
    app_priv = app_recorder.create(app_body);
    pump(20);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_recorder.destroy(app_priv);
    t0 = mono_ms() - t0;
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
    return t0;
}

static void shot(const char *name)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    char path[512];
    lv_draw_buf_t *snap;
    unsigned char *rgb;
    uint32_t y;

    if (!shots) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", shots, name);
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
    if (rgb) {
        lodepng_encode24_file(path, rgb, snap->header.w, snap->header.h);
    }
    free(rgb);
    lv_draw_buf_destroy(snap);
    printf("note saved %s\n", path);
#else
    (void)name;
#endif
}

/* ---- layout -------------------------------------------------------------------- */

static int n_targets;
static lv_area_t targets[64];

static void target(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    lv_area_t a;
    int k;

    lv_obj_get_coords(obj, &a);
    if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN ||
        a.x1 < body->x1 || a.x2 > body->x2 ||
        !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("FAIL %s: a target at %d,%d-%d,%d is not usable\n", what, (int)a.x1, (int)a.y1, (int)a.x2,
               (int)a.y2);
        (*bad)++;
    }
    for (k = 0; k < n_targets; k++) {
        if (a.x1 <= targets[k].x2 && targets[k].x1 <= a.x2 && a.y1 <= targets[k].y2 && targets[k].y1 <= a.y2) {
            printf("FAIL %s: two targets overlap at %d,%d\n", what, (int)a.x1, (int)a.y1);
            (*bad)++;
        }
    }
    if (n_targets < 64) {
        targets[n_targets++] = a;
    }
}

static void walk(lv_obj_t *obj, const lv_area_t *clip, const lv_area_t *body, const char *what, int *bad)
{
    uint32_t i;
    lv_area_t a;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    /* Rows scrolled out of the list's view are not targets now. */
    if (a.y2 < clip->y1 || a.y1 > clip->y2) {
        return;
    }
    if (obj != app_body && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        !lv_obj_check_type(obj, &lv_label_class)) {
        target(obj, body, what, bad);
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_area_t sub = *clip;

        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && obj != app_body) {
            lv_area_t mine;

            lv_obj_get_coords(obj, &mine);
            _lv_area_intersect(&sub, clip, &mine);
        }
        walk(c, &sub, body, what, bad);
    }
}

static void check_layout(const char *what)
{
    lv_area_t body;
    int bad = 0;
    char line[160];
    lv_obj_t *frame = lv_obj_get_child(app_body, 0);
    lv_area_t fa;

    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    n_targets = 0;
    walk(app_body, &body, &body, what, &bad);
    snprintf(line, sizeof(line), "%s: %d targets, each at least 64 px, inside the safe area, none overlapping",
             what, n_targets);
    check(line, bad == 0 && n_targets >= 5);
    lv_obj_get_coords(frame, &fa);
    snprintf(line, sizeof(line), "%s: the screen fits its body: nothing scrolls but the list", what);
    check(line, lv_obj_get_scroll_bottom(app_body) <= 0 && fa.y2 <= body.y2);
}

/* ---- journeys ------------------------------------------------------------------- */

static void journey(const char *o)
{
    char line[160];
    int saved_before = files_named(".wav");

    app_start();
    check("the app opens checking, then READY", wait_text("READY", 4000) && enabled("RECORD"));
    check_layout(o);
    snprintf(line, sizeof(line), "%s-idle.png", o);
    shot(line);
    check("no keyboard is ever asked for", g_keyboard_asked == 0);
    check("PAUSE, PLAY and DELETE are off with nothing going on and nothing selected",
          !enabled("PAUSE") && !enabled("PLAY") && !enabled("DELETE"));

    tap("RECORD");
    check("RECORD: the button says STOP at once, and the header says MIC ON",
          button("STOP") != NULL && strcmp(g_hint, "MIC ON") == 0);
    check("the chip says RECORDING when the helper runs", wait_text("RECORDING", 3000));
    pump(1500);
    check("the meter moves with the real samples", meter_width() > 50);
    check("and says their level", shows("-") && (shows("-1") || shows("-2") || shows("-3") || shows("-4") ||
                                                  shows("-5") || shows("-6") || shows("-7") || shows("-8") ||
                                                  shows("-9") || shows("0 dB")));
    if (strcmp(o, "portrait") == 0) {
        snprintf(line, sizeof(line), "%s-recording.png", o);
        shot(line);
    }
    check("the preset cannot be changed while recording", !enabled("VOICE 16 kHz"));
    tap("PAUSE");
    check("PAUSE pauses", wait_text("PAUSED", 2000) && button("RESUME") != NULL);
    pump(300);
    check("paused: the meter is empty, the header no longer says MIC ON",
          meter_width() == 0 && strcmp(g_hint, "") == 0);
    tap("RESUME");
    check("RESUME records again", wait_text("RECORDING", 3000) && strcmp(g_hint, "MIC ON") == 0);
    if (!label("RECORDING")) {
        dump(app_body);
    }
    pump(800);
    tap("STOP");
    check("STOP saves it", wait_text("Saved REC-", 5000) && enabled("RECORD"));
    check("one more recording on disk, no .part", files_named(".wav") == saved_before + 1 && files_named(".part") == 0);
    check("MIC ON is gone", strcmp(g_hint, "") == 0);
    check("the new recording is listed and selected, so PLAY and DELETE work",
          enabled("PLAY") && enabled("DELETE"));

    tap("PLAY");
    check("PLAY plays it", wait_text("PLAYING", 3000) && button("STOP") && strcmp(g_hint, "PLAYING") == 0);
    check("RECORD stays available while playing", enabled("RECORD"));
    check("it plays to the end", wait_text("READY", 8000) && strcmp(g_hint, "") == 0);

    g_volume = 0;
    tap("PLAY");
    check("muted, it says so and plays nothing", wait_text("Sound is muted", 1000) && !shows("PLAYING"));
    g_volume = 70;

    snprintf(line, sizeof(line), "%s: closing leaves no helper", o);
    app_stop();
    check(line, no_child());
}

static void delete_and_rotate(void)
{
    int before;

    use_display(POS_ROTATION_0);
    app_start();
    wait_text("READY", 4000);
    before = files_named(".wav");
    tap_obj(lv_obj_get_parent(find_label_in(app_body, "REC-", true)));
    check("tapping a row selects it", enabled("PLAY") && enabled("DELETE"));
    tap("DELETE");
    check("the first DELETE asks", button("CONFIRM") != NULL && files_named(".wav") == before);
    tap("CONFIRM");
    check("CONFIRM deletes", wait_text("Deleted", 1000) && files_named(".wav") == before - 1);

    /* The shell rotates by recreating the app on the new body; while
     * recording that is a close (the recording is saved) and a reopen. */
    tap("RECORD");
    wait_text("RECORDING", 3000);
    pump(900);
    before = files_named(".wav");
    app_stop();
    check("rotating mid-recording saves the recording", files_named(".wav") == before + 1 &&
                                                        files_named(".part") == 0 && no_child());
    use_display(POS_ROTATION_90);
    app_start();
    check("and the app comes back ready in the new orientation", wait_text("READY", 4000));
    check_layout("landscape after rotating");
    app_stop();
    use_display(POS_ROTATION_0);
}

static void churn(void)
{
    int k;
    uint32_t before;
    int ok = 1;
    struct mallinfo2 m0;
    struct mallinfo2 m1;
    struct mallinfo2 m2;

    app_start();
    wait_text("READY", 4000);
    app_stop();
    before = lv_obj_get_child_count(g_content);
    for (k = 0; k < 20; k++) {
        app_start();
        wait_text("READY", 4000);
        if (k % 2) {
            tap("RECORD");
            wait_text("RECORDING", 3000);
            pump(600);
        }
        app_stop();
        ok &= no_child() && files_named(".part") == 0;
    }
    check("twenty opens and closes, half of them mid-recording: no helper, no .part", ok);
    check("and no LVGL objects left behind", lv_obj_get_child_count(g_content) == before);

    /* The heap, with the folder no longer changing: what one open takes, and
     * that twenty more opens and closes give all of it back. LVGL allocates
     * from the C heap here (LV_STDLIB_CLIB), as on the unit. */
    for (k = 0; k < 10; k++) {
        /* Warm LVGL's own caches (glyphs, draw layers): a few KB, once. */
        app_start();
        wait_text("READY", 4000);
        app_stop();
    }
    m0 = mallinfo2();
    app_start();
    wait_text("READY", 4000);
    m1 = mallinfo2();
    printf("note heap with Recorder open (%d recordings listed): %ld bytes\n", files_named(".wav"),
           (long)m1.uordblks - (long)m0.uordblks);
    app_stop();
    for (k = 0; k < 20; k++) {
        app_start();
        wait_text("READY", 4000);
        app_stop();
    }
    m2 = mallinfo2();
    printf("note heap after twenty more opens and closes: %+ld bytes\n", (long)m2.uordblks - (long)m0.uordblks);
    check("and they leak nothing (the heap is back within 1 KB)", (long)m2.uordblks - (long)m0.uordblks <= 1024);
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("RECORD_HELPER");
    char cmd[300];
    char path[300];
    FILE *f;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set RECORD_HELPER to tests/pos-record-testhooks\n");
        return 2;
    }
    shots = getenv("REC_SHOTS");
    snprintf(root, sizeof(root), "/tmp/recorder-app-%ld", (long)getpid());
    snprintf(recs, sizeof(recs), "%s/home/Recordings", root);
    mkdir(root, 0700);
    snprintf(path, sizeof(path), "%s/audio", root);
    mkdir(path, 0700);
    setenv("POS_RECORD_FAKE_AUDIO", path, 1);
    snprintf(path, sizeof(path), "%s/audio/route", root);
    f = fopen(path, "w");
    fputs("1\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/audio/capture.raw", root);
    f = fopen(path, "wb");
    for (i = 0; i < 48000 * 20; i++) {
        /* Speech-like: a 220 Hz tone whose loudness rises and falls. */
        double env = 0.25 + 0.75 * fabs(sin(2.0 * M_PI * 1.5 * i / 48000.0));
        int16_t v = (int16_t)lrint(9000.0 * env * sin(2.0 * M_PI * 220.0 * i / 48000.0));

        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    snprintf(path, sizeof(path), "%s/free", root);
    f = fopen(path, "w");
    fputs("3000000000\n", f);
    fclose(f);
    setenv("POS_RECORD_FREE_FILE", path, 1);
    snprintf(path, sizeof(path), "%s/run", root);
    setenv("POCKETOS_RUNTIME_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/state", root);
    setenv("POCKETOS_STATE_DIR", path, 1);
    setenv("POCKETOS_RECORDINGS_DIR", recs, 1);
    setenv("POCKETOS_RECORD_HELPER", helper, 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "capture,playback", 1);

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
    delete_and_rotate();
    churn();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("rec_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
