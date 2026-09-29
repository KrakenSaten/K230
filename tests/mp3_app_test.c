/*
 * MP3 in the running app, driven by a real LVGL pointer device, with the
 * real helper (tests/pos-mp3-testhooks, which decodes WAV) over the
 * file-backed sound card behind it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners, portrait and landscape,
 * the body under the 72 px header the NONE chrome leaves. Time is real: the
 * helper runs on the wall clock, so every pump sleeps as long as it advances
 * LVGL's tick.
 *
 * What is checked: every control is a usable, safe, non-overlapping touch
 * target in both orientations, idle and playing, and the screen fits its
 * body; the places, a folder and UP; a track tapped plays, with its name,
 * its time and the header hint; PAUSE, PLAY, NEXT, PREV and STOP; a tap on
 * the progress bar seeks; VOL + changes the system volume; a folder plays
 * to its end; rotating (a new app on the new body) while playing; closing
 * mid-track leaves no helper; twenty opens and closes leave no LVGL objects
 * and no heap behind.
 *
 * MP3_SHOTS=<dir> also saves the body as PNGs.
 *
 * Needs: MP3_HELPER, the path of tests/pos-mp3-testhooks. Built by
 * ui/shell/CMakeLists.txt, run by tests/mp3_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketui.h"
#include "pocketwav/pocketwav.h"
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

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
    chrome_height(chrome_resolve(app_mp3.chrome,                                                  \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_mp3;

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
}

/* ---- the shell's side of app.h ------------------------------------------------ */

static char g_hint[64];
static int g_volume = 70;
static int g_muted;
static int g_keyboard_asked;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

int pocketos_shell_volume_get(void)
{
    return g_volume;
}

int pocketos_shell_volume_muted(void)
{
    return g_muted;
}

int pocketos_shell_volume_available(void)
{
    return 1;
}

int pocketos_shell_volume_set(int percent)
{
    g_volume = percent;
    return 0;
}

int pocketos_shell_volume_set_muted(int muted)
{
    g_muted = muted;
    return 0;
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

static bool wait_label(const char *text, bool prefix, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (app_body && find_label_in(app_body, text, prefix)) {
            return true;
        }
        pump(20);
    }
    return app_body && find_label_in(app_body, text, prefix);
}

static bool wait_text(const char *text, int ms)
{
    return wait_label(text, false, ms);
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

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/* The deck's progress bar: frame > body > deck > seek (child 3) > bar. */
static lv_obj_t *seek_bar(void)
{
    lv_obj_t *frame = lv_obj_get_child(app_body, 0);
    lv_obj_t *body = frame ? lv_obj_get_child(frame, 0) : NULL;
    lv_obj_t *deck = body ? lv_obj_get_child(body, 0) : NULL;
    lv_obj_t *seek = deck ? lv_obj_get_child(deck, 3) : NULL;

    return seek ? lv_obj_get_child(seek, 0) : NULL;
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
    app_priv = app_mp3.create(app_body);
    pump(20);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_mp3.destroy(app_priv);
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
static lv_area_t targets[96];

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
    if (n_targets < 96) {
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
    char line[200];
    lv_obj_t *frame = lv_obj_get_child(app_body, 0);
    lv_area_t fa;

    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    n_targets = 0;
    walk(app_body, &body, &body, what, &bad);
    snprintf(line, sizeof(line), "%s: %d targets, each at least 64 px, inside the safe area, none overlapping",
             what, n_targets);
    check(line, bad == 0 && n_targets >= 9);
    lv_obj_get_coords(frame, &fa);
    snprintf(line, sizeof(line), "%s: the screen fits its body: nothing scrolls but the list", what);
    check(line, lv_obj_get_scroll_bottom(app_body) <= 0 && fa.y2 <= body.y2);
}

/* ---- journeys ------------------------------------------------------------------- */

static void open_row(const char *name)
{
    lv_obj_t *l = label(name);

    tap_obj(l ? lv_obj_get_parent(l) : NULL);
}

/* The folder is read on a thread: wait until a listing is on screen - the
 * places, or a folder of Music. */
static bool settled(int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (label("Home") || shows("Music/")) {
            return true;
        }
        pump(20);
    }
    return label("Home") || shows("Music/");
}

static void to_album(void)
{
    settled(3000);
    if (!shows("Music/album")) {
        while (enabled("UP") && !label("LIBRARY")) {
            tap("UP");
            pump(150);
        }
        wait_text("Music", 2000);
        open_row("Music");
        wait_text("album", 2000);
        open_row("album");
    }
    wait_text("Music/album", 2000);
}

static void journey(const char *o)
{
    char line[160];

    app_start();
    check("the app opens on the places, or the folder last shown", settled(3000));
    check_layout(o);
    snprintf(line, sizeof(line), "%s-library.png", o);
    shot(line);
    check("no keyboard is ever asked for", g_keyboard_asked == 0);

    to_album();
    check("a place, then a folder: the caption says where", shows("Music/album") && enabled("UP"));
    check("STOP and NEXT are off with nothing chosen; PLAY is on (the folder has tracks)",
          !enabled("STOP") && !enabled("NEXT") && enabled("PLAY"));
    open_row("a1.wav");
    check("a track tapped plays: PLAYING, its name, PAUSE, the header hint",
          wait_text("PLAYING", 3000) && label("a1.wav") && button("PAUSE") && strcmp(g_hint, "PLAYING") == 0);
    check("  its place in the folder and its length", label("1 / 3") && label("0:01"));
    check_layout(strcmp(o, "portrait") == 0 ? "portrait playing" : "landscape playing");
    snprintf(line, sizeof(line), "%s-playing.png", o);
    shot(line);

    tap("PAUSE");
    check("PAUSE pauses: the chip, PLAY offered, the hint gone",
          wait_text("PAUSED", 1500) && button("PLAY") && strcmp(g_hint, "") == 0);
    tap("PLAY");
    check("PLAY resumes", wait_text("PLAYING", 1500) && button("PAUSE"));
    tap("NEXT");
    check("NEXT plays the next track", wait_text("a2.wav", 2000) && wait_text("2 / 3", 2000) &&
                                           wait_text("PLAYING", 3000));
    tap("PREV");
    check("PREV plays the one before", wait_text("1 / 3", 2000) && wait_text("PLAYING", 3000));
    tap("STOP");
    check("STOP stops", wait_text("STOPPED", 1500) && !enabled("STOP") && strcmp(g_hint, "") == 0);
    tap("VOL +");
    check("VOL + raises the system volume a step", g_volume == 80 && label("80 %"));
    tap("VOL -");
    check("VOL - lowers it", g_volume == 70 && label("70 %"));

    open_row("a3.wav");
    check("the last track plays to its end, and the folder's end is said",
          wait_text("PLAYING", 3000) && wait_text("End of album.", 4000) && wait_text("STOPPED", 500));

    snprintf(line, sizeof(line), "%s: closing mid-track leaves no helper", o);
    open_row("a1.wav");
    wait_text("PLAYING", 3000);
    check(line, app_stop() < 1000 && no_child());
}

static void seeking(void)
{
    lv_obj_t *bar;
    lv_area_t a;

    use_display(POS_ROTATION_0);
    app_start();
    to_album();
    tap("UP");
    wait_text("long", 2000);
    open_row("long");
    wait_text("Music/long", 2000);
    open_row("l1.wav");
    wait_text("PLAYING", 3000);
    pump(300);
    bar = seek_bar();
    check("the progress bar is there", bar != NULL);
    if (bar) {
        lv_obj_update_layout(bar);
        lv_obj_get_coords(bar, &a);
        press_at(a.x1 + lv_area_get_width(&a) * 8 / 10, a.y1 + lv_area_get_height(&a) / 2);
    }
    check("a tap at 80 % of the bar seeks there (0:04 of 0:05)", wait_text("0:04", 1500) && label("0:05"));
    if (bar) {
        /* Pressed at 10 %, dragged to 50 %, lifted: one seek, to 50 %. */
        finger_point.x = a.x1 + lv_area_get_width(&a) / 10;
        finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(60);
        finger_point.x = a.x1 + lv_area_get_width(&a) * 5 / 10;
        pump(60);
        check("  dragging previews the place under the finger", label("0:02") != NULL);
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(60);
    }
    check("  and lifting seeks there", wait_text("0:02", 1500));
    shot("portrait-seek.png");
    app_stop();
    check("closed: no helper", no_child());
}

static void rotate_while_playing(void)
{
    use_display(POS_ROTATION_0);
    app_start();
    to_album();
    open_row("a1.wav");
    wait_text("PLAYING", 3000);
    /* The shell rotates by recreating the app on the new body. */
    check("rotating mid-track: the old app closes at once and leaves no helper", app_stop() < 1000 && no_child());
    use_display(POS_ROTATION_90);
    app_start();
    check("and the new one comes back on the same folder", wait_text("Music/album", 3000));
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
    wait_text("Music/album", 3000);
    app_stop();
    before = lv_obj_get_child_count(g_content);
    for (k = 0; k < 20; k++) {
        app_start();
        wait_text("Music/album", 3000);
        if (k % 2) {
            open_row("a2.wav");
            wait_text("PLAYING", 3000);
        }
        app_stop();
        ok &= no_child();
    }
    check("twenty opens and closes, half of them playing: no helper", ok);
    check("and no LVGL objects left behind", lv_obj_get_child_count(g_content) == before);

    /* The heap, with the folder no longer changing: what one open takes, and
     * that sixty more opens and closes give it back. LVGL allocates from the
     * C heap here (LV_STDLIB_CLIB), as on the unit. Every open starts one
     * short-lived scan thread, and glibc's per-thread bookkeeping moves the
     * heap by 1.9 to 3.6 KB over sixty opens (four runs, 2026-09-29), in
     * steps that then stay flat; a leak of anything the app allocates per
     * open - three list rows are over 1 KB, the folder list 55 KB, the queue
     * 51 KB - is far more. So the bound is 256 bytes per open on average.
     * Leaks as such are LeakSanitizer's job: make mp3-san-test, and this
     * test built with -fsanitize=address (docs/apps/MP3.md, Tests). */
    for (k = 0; k < 20; k++) {
        app_start();
        wait_text("Music/album", 3000);
        app_stop();
    }
    pump(200);
    m0 = mallinfo2();
    app_start();
    wait_text("Music/album", 3000);
    m1 = mallinfo2();
    printf("note heap with MP3 open on a folder: %ld bytes\n", (long)m1.uordblks - (long)m0.uordblks);
    app_stop();
    for (k = 0; k < 60; k++) {
        app_start();
        wait_text("Music/album", 3000);
        app_stop();
    }
    pump(200);
    m2 = mallinfo2();
    printf("note heap after sixty more opens and closes: %+ld bytes\n", (long)m2.uordblks - (long)m0.uordblks);
    check("and they leak nothing (under 256 bytes per open on average)",
          (long)m2.uordblks - (long)m0.uordblks < 61 * 256);
}

static void write_wav(const char *path, unsigned ms)
{
    uint8_t h[POCKETWAV_HEADER_BYTES];
    unsigned frames = 16000u * ms / 1000u;
    FILE *f = fopen(path, "wb");
    unsigned i;

    pocketwav_header(h, 16000, 1, frames * 2u);
    fwrite(h, 1, sizeof(h), f);
    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(9000.0 * sin(2.0 * M_PI * 440.0 * i / 16000.0));

        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("MP3_HELPER");
    char cmd[300];
    char path[300];
    FILE *f;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set MP3_HELPER to tests/pos-mp3-testhooks\n");
        return 2;
    }
    shots = getenv("MP3_SHOTS");
    snprintf(root, sizeof(root), "/tmp/mp3-app-%ld", (long)getpid());
    mkdir(root, 0700);
    snprintf(path, sizeof(path), "%s/audio", root);
    mkdir(path, 0700);
    setenv("POS_MP3_FAKE_AUDIO", path, 1);
    snprintf(path, sizeof(path), "%s/audio/route", root);
    f = fopen(path, "w");
    fputs("0\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/home", root);
    mkdir(path, 0700);
    setenv("HOME", path, 1);
    snprintf(path, sizeof(path), "%s/home/Music", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/home/Music/album", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/home/Music/long", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/home/Music/album/a1.wav", root);
    write_wav(path, 1200);
    snprintf(path, sizeof(path), "%s/home/Music/album/a2.wav", root);
    write_wav(path, 1200);
    snprintf(path, sizeof(path), "%s/home/Music/album/a3.wav", root);
    write_wav(path, 1200);
    snprintf(path, sizeof(path), "%s/home/Music/long/l1.wav", root);
    write_wav(path, 5000);
    snprintf(path, sizeof(path), "%s/run", root);
    mkdir(path, 0700);
    setenv("POCKETOS_RUNTIME_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/state", root);
    setenv("POCKETOS_STATE_DIR", path, 1);
    setenv("POCKETOS_MP3_MEDIA_ROOTS", "", 1);
    unsetenv("POCKETOS_MUSIC_DIR");
    unsetenv("POCKETOS_RECORDINGS_DIR");
    setenv("POCKETOS_MP3_HELPER", helper, 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "playback", 1);

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
    seeking();
    rotate_while_playing();
    churn();
    if (failed) {
        app_start();
        pump(500);
        dump(app_body);
        app_stop();
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("mp3_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
