/*
 * Video in the running app, driven by a real LVGL pointer device, with the
 * real helper (tests/pos-video-testhooks, fake backend, no sound card) behind
 * it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners; portrait under the 72 px
 * header, landscape with no header at all (the app declares NONE_LANDSCAPE).
 * Time is real: the helper runs on the wall clock, so every pump sleeps as
 * long as it advances LVGL's tick.
 *
 * What is checked, in both orientations: the list shows the folder's MP4
 * files and only those; choosing one plays it (pictures on screen, the
 * position moving); PAUSE, PLAY, STOP; a tap on the progress bar seeks; every
 * player control is a usable, safe, non-overlapping touch target; FULLSCREEN
 * gives the picture the whole body and a tap on it comes back; BACK returns to
 * the list with no helper left; a damaged file says so and BACK still works;
 * the landscape list's own back slab goes home; closing the app while playing
 * is quick and leaves no helper; twenty opens and closes leave no helper and
 * no LVGL objects behind.
 *
 * VIDEO_SHOTS=<dir> also saves the screen as PNGs.
 *
 * Needs: VIDEO_HELPER, the path of tests/pos-video-testhooks. Built by
 * ui/shell/CMakeLists.txt (SDL builds), run by tests/video_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#include <errno.h>
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
#define SLIDER_EXT 15

extern const struct pocketos_app app_video;

static int failed;
static int checks;
static char root[128];
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
    fflush(stdout);
}

/* ---- the shell's side of app.h ------------------------------------------------ */

static char g_hint[64];
static int g_home;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

int pocketos_shell_volume_effective(void)
{
    return 60;
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = pocketui_display_geometry()->width > pocketui_display_geometry()->height;
    out->next_landscape = out->landscape;
}

void pocketos_shell_go_home(void)
{
    g_home++;
}

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
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
static bool landscape;

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

/* A label containing text anywhere (the status label has two lines). */
static lv_obj_t *find_containing(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), text)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = find_containing(lv_obj_get_child(obj, i), text);

        if (f) {
            return f;
        }
    }
    return NULL;
}

static bool says(const char *text)
{
    return app_body && find_containing(app_body, text) != NULL;
}

static lv_obj_t *button(const char *text)
{
    lv_obj_t *l = label(text);

    return l ? lv_obj_get_parent(l) : NULL;
}

static lv_obj_t *find_class(lv_obj_t *obj, const lv_obj_class_t *cls)
{
    uint32_t i;

    if (lv_obj_check_type(obj, cls)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = find_class(lv_obj_get_child(obj, i), cls);

        if (f) {
            return f;
        }
    }
    return NULL;
}

static lv_obj_t *picture(void)
{
    return app_body ? find_class(app_body, &lv_image_class) : NULL;
}

static lv_obj_t *slider(void)
{
    return app_body ? find_class(app_body, &lv_slider_class) : NULL;
}

static bool picture_on_screen(void)
{
    lv_obj_t *img = picture();

    return img && !lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN) && lv_image_get_src(img) != NULL &&
           !lv_obj_has_flag(lv_obj_get_parent(img), LV_OBJ_FLAG_HIDDEN);
}

static void press_at(int32_t x, int32_t y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(40);
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
    press_at(a.x1 + lv_area_get_width(&a) / 2, a.y1 + lv_area_get_height(&a) / 2);
}

static void tap(const char *text)
{
    tap_obj(button(text));
}

static bool wait_for(bool (*cond)(void), int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (cond()) {
            return true;
        }
        pump(20);
    }
    return cond();
}

static const char *want;

static bool want_says(void)
{
    return says(want);
}

static bool wait_says(const char *text, int ms)
{
    want = text;
    return wait_for(want_says, ms);
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
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
    landscape = g.width > g.height;
    lv_display_set_resolution(disp, g.width, g.height);
    /* Chrome NONE: no status bar, the content is the whole screen. */
    lv_obj_set_size(g_content, g.width, g.height);
    lv_obj_set_pos(g_content, 0, 0);
    pump(20);
}

static void app_start(void)
{
    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    /* The shell's header in portrait; none in landscape (NONE_LANDSCAPE). */
    if (!landscape) {
        lv_obj_t *header = lv_obj_create(app_root);

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
    app_priv = app_video.create(app_body);
    pump(40);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_video.destroy(app_priv);
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

/* ---- the touch targets -------------------------------------------------------------- */

static int n_targets;
static lv_area_t targets[64];

static void target(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    lv_area_t a;
    int k;

    lv_obj_get_coords(obj, &a);
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        /* The track is thin; its touch target is its 40 px layout rect
         * (video_app.c extends the click area by (40 - 10) / 2). */
        a.y1 -= SLIDER_EXT;
        a.y2 += SLIDER_EXT;
    } else if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN) {
        printf("FAIL %s: a target at %d,%d-%d,%d is below the touch minimum\n", what, (int)a.x1, (int)a.y1,
               (int)a.x2, (int)a.y2);
        (*bad)++;
    }
    if (a.x1 < body->x1 || a.x2 > body->x2 ||
        !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("FAIL %s: a target at %d,%d-%d,%d is outside the safe body\n", what, (int)a.x1, (int)a.y1,
               (int)a.x2, (int)a.y2);
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

/* Every visible clickable control of the player except the picture itself
 * (a tap target by design, but the whole frame). */
static void walk(lv_obj_t *obj, const lv_area_t *body, lv_obj_t *skip, const char *what, int *bad)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || obj == skip) {
        return;
    }
    if (obj != app_body && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        !lv_obj_check_type(obj, &lv_label_class)) {
        target(obj, body, what, bad);
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        walk(lv_obj_get_child(obj, i), body, skip, what, bad);
    }
}

static void controls_usable(const char *o)
{
    lv_area_t body;
    char what[96];
    int bad = 0;
    lv_obj_t *img = picture();

    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    n_targets = 0;
    walk(app_body, &body, img ? lv_obj_get_parent(img) : NULL, o, &bad);
    snprintf(what, sizeof(what), "%s: every player control is a usable, safe, separate target (%d)", o,
             n_targets);
    check(what, bad == 0 && n_targets >= 5);
}

/* ---- the journey ------------------------------------------------------------------- */

static void write_file(const char *name, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/videos/%s", root, name);
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int slider_value(void)
{
    lv_obj_t *s = slider();

    return s ? (int)lv_slider_get_value(s) : -1;
}

static bool moving(void)
{
    static int last = -1;
    int v = slider_value();
    bool r = last >= 0 && v > last;

    last = v;
    return r;
}

static void journey(const char *o)
{
    char name[128];
    lv_area_t a;
    lv_area_t body;
    int v0;

    app_start();
    snprintf(name, sizeof(name), "%s: the list shows the three MP4 files and nothing else", o);
    check(name, label("clip.mp4") && label("damaged.mp4") && label("long.mp4") && !label("notes.txt") &&
                    says("3 videos"));
    snprintf(name, sizeof(name), "%s: the list's own back slab only where there is no header", o);
    check(name, (label(LV_SYMBOL_LEFT) != NULL) == landscape);
    snprintf(name, sizeof(name), "%s: SIMULATED in the header (the fake backend)", o);
    check(name, strcmp(g_hint, "SIMULATED") == 0);
    snprintf(name, sizeof(name), "list-%s.png", o);
    shot(name);

    /* Choose: it plays. */
    tap("long.mp4");
    snprintf(name, sizeof(name), "%s: choosing a file plays it: a picture on screen", o);
    check(name, wait_for(picture_on_screen, 3000));
    snprintf(name, sizeof(name), "%s: and it says Playing, with PAUSE offered", o);
    check(name, wait_says("Playing", 2000) && label("PAUSE") != NULL);
    pump(700);
    snprintf(name, sizeof(name), "%s: the progress bar moves", o);
    moving();
    pump(500);
    check(name, moving());
    controls_usable(o);
    snprintf(name, sizeof(name), "player-%s.png", o);
    shot(name);

    /* Pause, play, stop. */
    tap("PAUSE");
    snprintf(name, sizeof(name), "%s: PAUSE pauses", o);
    check(name, wait_says("Paused", 2000) && label("PLAY") != NULL);
    v0 = slider_value();
    pump(500);
    snprintf(name, sizeof(name), "%s: nothing moves while paused", o);
    check(name, slider_value() == v0);
    tap("PLAY");
    snprintf(name, sizeof(name), "%s: PLAY plays again", o);
    check(name, wait_says("Playing", 2000));
    tap("STOP");
    snprintf(name, sizeof(name), "%s: STOP goes back to the start", o);
    check(name, wait_says("Stopped", 2000) && label("0:00") != NULL && slider_value() == 0);

    /* A tap three quarters along the bar seeks there. */
    lv_obj_update_layout(slider());
    lv_obj_get_coords(slider(), &a);
    press_at(a.x1 + lv_area_get_width(&a) * 3 / 4, (a.y1 + a.y2) / 2);
    pump(600);
    snprintf(name, sizeof(name), "%s: a tap on the bar seeks there (%d of 1000)", o, slider_value());
    check(name, slider_value() >= 650 && slider_value() <= 800 && label("0:07") != NULL);

    /* Fullscreen and back. */
    tap("PLAY");
    tap(landscape ? "FULLSCREEN" : "FULL");
    pump(300);
    lv_obj_get_coords(app_body, &body);
    lv_obj_get_coords(lv_obj_get_parent(picture()), &a);
    snprintf(name, sizeof(name), "%s: FULLSCREEN gives the picture the whole body, and no controls", o);
    check(name, a.x1 == body.x1 && a.y1 == body.y1 && a.x2 == body.x2 && a.y2 == body.y2 &&
                    label("PAUSE") == NULL && label("STOP") == NULL && slider_value() >= 0 &&
                    lv_obj_has_flag(slider(), LV_OBJ_FLAG_HIDDEN));
    snprintf(name, sizeof(name), "%s: the picture is larger in fullscreen", o);
    check(name, wait_for(picture_on_screen, 1000) && lv_obj_get_width(picture()) >= 300);
    snprintf(name, sizeof(name), "fullscreen-%s.png", o);
    pump(300);
    shot(name);
    tap_obj(lv_obj_get_parent(picture()));
    pump(300);
    snprintf(name, sizeof(name), "%s: a tap on the picture leaves fullscreen, still playing", o);
    check(name, label("PAUSE") != NULL && label("STOP") != NULL && says("Playing"));

    /* The end. */
    tap("PAUSE");
    lv_obj_get_coords(slider(), &a);
    press_at(a.x2 - 2, (a.y1 + a.y2) / 2);
    tap("PLAY");
    snprintf(name, sizeof(name), "%s: played to the end, it says Ended and offers PLAY", o);
    check(name, wait_says("Ended", 3000) && label("PLAY") != NULL);

    /* BACK. */
    tap("BACK");
    pump(100);
    snprintf(name, sizeof(name), "%s: BACK returns to the list with no helper left", o);
    check(name, label("clip.mp4") != NULL && label("BACK") == NULL && wait_for(no_child, 1000));

    /* A damaged file. */
    tap("damaged.mp4");
    snprintf(name, sizeof(name), "%s: a damaged file says so", o);
    check(name, wait_says("This file is damaged", 3000) && !picture_on_screen());
    snprintf(name, sizeof(name), "error-%s.png", o);
    shot(name);
    tap("BACK");
    snprintf(name, sizeof(name), "%s: and BACK still returns to the list", o);
    check(name, label("clip.mp4") != NULL && wait_for(no_child, 1000));

    /* Leaving from the list: home (the shell's back in portrait). */
    if (landscape) {
        int homes = g_home;

        tap_obj(lv_obj_get_parent(label(LV_SYMBOL_LEFT)));
        snprintf(name, sizeof(name), "%s: the list's back slab goes home", o);
        check(name, g_home == homes + 1);
    }

    /* Closing the app while it plays. */
    tap("clip.mp4");
    wait_says("Playing", 3000);
    {
        int64_t took = app_stop();

        snprintf(name, sizeof(name), "%s: closing while playing takes %lld ms and leaves no helper", o,
                 (long long)took);
        check(name, took < 600 && wait_for(no_child, 1000));
    }
}

static void churn(void)
{
    int i;
    int bad = 0;
    uint32_t before;

    app_start();
    app_stop();
    before = lv_obj_get_child_count(g_content);
    for (i = 0; i < 20; i++) {
        app_start();
        tap(i % 2 ? "clip.mp4" : "long.mp4");
        if (!wait_for(picture_on_screen, 3000)) {
            bad++;
        }
        if (i % 3 == 0) {
            tap("BACK");
        }
        app_stop();
    }
    check("twenty opens and closes: pictures every time", bad == 0);
    check("... no helper left", wait_for(no_child, 1000));
    check("... and no objects left behind", lv_obj_get_child_count(g_content) == before);
}

int main(void)
{
    char path[256];
    char cmd[320];
    lv_indev_t *finger;
    const char *helper = getenv("VIDEO_HELPER");

    if (!helper || !*helper) {
        fprintf(stderr, "video_app_test: set VIDEO_HELPER to tests/pos-video-testhooks\n");
        return 2;
    }
    shots = getenv("VIDEO_SHOTS");
    snprintf(root, sizeof(root), "/tmp/video_app_test.XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/videos", root);
    mkdir(path, 0755);
    setenv("POCKETOS_VIDEOS_DIR", path, 1);
    write_file("clip.mp4", "DOORS-FAKE-VIDEO w=640 h=360 fps=25 ms=3000\n");
    write_file("long.mp4", "DOORS-FAKE-VIDEO w=1280 h=720 fps=25 ms=10000\n");
    write_file("damaged.mp4", "not a video at all\n");
    write_file("notes.txt", "DOORS-FAKE-VIDEO\n");
    snprintf(path, sizeof(path), "%s/run", root);
    mkdir(path, 0755);
    setenv("POCKETOS_RUNTIME_DIR", path, 1);
    setenv("POCKETOS_VIDEO_HELPER", helper, 1);
    setenv("POCKETOS_VIDEO_BACKEND", "fake", 1);
    setenv("POS_VIDEO_NO_AUDIO", "1", 1);

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

    check("the app is Video, fullscreen, own header in landscape",
          strcmp(app_video.id, "video") == 0 && strcmp(app_video.name, "Video") == 0 &&
              app_video.chrome == POCKETOS_CHROME_NONE &&
              app_video.header == POCKETOS_HEADER_NONE_LANDSCAPE && app_video.icon_mask != NULL);
    use_display(POS_ROTATION_0);
    journey("portrait");
    use_display(POS_ROTATION_90);
    journey("landscape");
    churn();
    if (failed && app_body) {
        dump(app_body);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("video_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
