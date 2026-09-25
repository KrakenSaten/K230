/*
 * Camera in the running app, driven by a real LVGL pointer device, with the
 * real helper (pos-camera-testhooks) on the fake backend behind it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners, portrait and landscape, the
 * body under the 72 px header the NONE chrome leaves. Time is real: the
 * helper runs on the wall clock, so every pump here sleeps as long as it
 * advances LVGL's tick, and the session's deadlines mean what they say.
 *
 * What is checked: the preview comes up and dominates the body, every
 * control is a usable, safe, non-overlapping target, a photo is taken, kept,
 * reviewed again, deleted after a confirmation; the photo is on disk exactly
 * when the screen says it is; a full disk is said and changes nothing; no
 * camera, a missing helper and a crashing one each end on their own screen
 * with a way back; closing mid-capture and twenty opens and closes leave no
 * helper behind.
 *
 * Needs: CAMERA_HELPER, the path of tests/pos-camera-testhooks (built by
 * make). Built by ui/shell/CMakeLists.txt, run by tests/camera_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define STATUS_H                                                                                  \
    chrome_height(chrome_resolve(app_camera.chrome,                                               \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_camera;

static int failed;
static int checks;
static char root[128];
static char photos[256];

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

/* LVGL's tick follows the wall clock, as on the device. */
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

static lv_obj_t *shown_label(const char *text)
{
    return app_body ? find_label_in(app_body, text, false) : NULL;
}

static bool shows_text(const char *prefix)
{
    return app_body && find_label_in(app_body, prefix, true) != NULL;
}

/* A visible button by its label. */
static lv_obj_t *button(const char *label)
{
    lv_obj_t *l = shown_label(label);

    return l ? lv_obj_get_parent(l) : NULL;
}

static bool enabled(const char *label)
{
    lv_obj_t *b = button(label);

    return b && lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

/* The last-photo slab: the only clickable non-button in the frame. */
static lv_obj_t *last_button(void)
{
    lv_obj_t *frame = lv_obj_get_child(app_body, 0);
    uint32_t i;

    for (i = 0; frame && i < lv_obj_get_child_count(frame); i++) {
        lv_obj_t *o = lv_obj_get_child(frame, i);

        if (!lv_obj_check_type(o, &lv_button_class) && !lv_obj_check_type(o, &lv_label_class) &&
            lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
            return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? NULL : o;
        }
    }
    return NULL;
}

/* The picture: the image in the first child of the frame. */
static lv_obj_t *picture(void)
{
    lv_obj_t *frame = lv_obj_get_child(app_body, 0);
    lv_obj_t *box = frame ? lv_obj_get_child(frame, 0) : NULL;
    lv_obj_t *img = box ? lv_obj_get_child(box, 0) : NULL;

    return img && lv_obj_check_type(img, &lv_image_class) &&
                   !lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN)
               ? img
               : NULL;
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
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(40);
}

static bool wait_until(bool (*cond)(void), int ms)
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

static const char *want_text;

static bool text_shown(void)
{
    return shows_text(want_text);
}

static bool wait_until_text(const char *text, int ms)
{
    want_text = text;
    return wait_until(text_shown, ms);
}

static bool live(void)
{
    return picture() != NULL && enabled("TAKE PHOTO");
}

static bool reviewing(void)
{
    return button("KEEP") != NULL;
}

static bool confirming(void)
{
    return button("CANCEL") != NULL;
}

static bool no_camera(void)
{
    return shown_label("No camera") != NULL && button("CHECK AGAIN") != NULL;
}

static bool failed_screen(void)
{
    return button("TRY AGAIN") != NULL;
}

static int photos_on_disk(void)
{
    DIR *d = opendir(photos);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
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
    app_priv = app_camera.create(app_body);
    pump(20);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_camera.destroy(app_priv);
    t0 = mono_ms() - t0;
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
    return t0;
}

/* ---- layout ------------------------------------------------------------------------------ */

static int n_targets;
static lv_area_t targets[16];

static void target(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    lv_area_t a;
    int k;

    lv_obj_get_coords(obj, &a);
    if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN ||
        a.x1 < body->x1 || a.x2 > body->x2 || a.y1 < body->y1 || a.y2 > body->y2 ||
        !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("FAIL %s: a target at %d,%d-%d,%d is not usable in %d,%d-%d,%d\n", what, (int)a.x1,
               (int)a.y1, (int)a.x2, (int)a.y2, (int)body->x1, (int)body->y1, (int)body->x2,
               (int)body->y2);
        (*bad)++;
    }
    for (k = 0; k < n_targets; k++) {
        if (a.x1 <= targets[k].x2 && targets[k].x1 <= a.x2 && a.y1 <= targets[k].y2 &&
            targets[k].y1 <= a.y2) {
            printf("FAIL %s: two targets overlap at %d,%d\n", what, (int)a.x1, (int)a.y1);
            (*bad)++;
        }
    }
    if (n_targets < 16) {
        targets[n_targets++] = a;
    }
}

static void walk(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (obj != app_body && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        !lv_obj_check_type(obj, &lv_label_class) && !lv_obj_check_type(obj, &lv_image_class)) {
        target(obj, body, what, bad);
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        walk(lv_obj_get_child(obj, i), body, what, bad);
    }
}

static void check_targets(const char *what)
{
    lv_area_t body;
    lv_area_t pic;
    int bad = 0;
    char msg[160];

    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, &body);
    n_targets = 0;
    walk(app_body, &body, what, &bad);
    snprintf(msg, sizeof(msg), "%s: %d targets, all usable, inside, safe and apart", what, n_targets);
    check(msg, bad == 0 && n_targets > 0);
    if (picture()) {
        lv_obj_get_coords(lv_obj_get_parent(picture()), &pic);
        snprintf(msg, sizeof(msg), "%s: the picture is inside the body and outside the corners", what);
        check(msg, pic.x1 >= body.x1 && pic.x2 <= body.x2 && pic.y1 >= body.y1 &&
                       pic.y2 <= body.y2 &&
                       pos_display_rect_is_safe(pocketui_display_geometry(), pic.x1, pic.y1, pic.x2,
                                                pic.y2));
        snprintf(msg, sizeof(msg), "%s: and dominates it (%d%% of the body)", what,
                 (int)((int64_t)lv_area_get_size(&pic) * 100 / lv_area_get_size(&body)));
        check(msg, lv_area_get_size(&pic) * 100 / lv_area_get_size(&body) >= 55);
    }
}

/* ---- journeys ------------------------------------------------------------------------------ */

static void journey(const char *shape)
{
    char msg[160];
    int before = photos_on_disk();

#define CHECK(text, ok) do { snprintf(msg, sizeof(msg), "%s: %s", shape, text); check(msg, ok); } while (0)
    app_start();
    CHECK("the preview comes up", wait_until(live, 4000));
    CHECK("under the fake the header says SIMULATED", strcmp(g_hint, "SIMULATED") == 0);
    check_targets(shape);
    CHECK("no last photo yet", last_button() == NULL);

    tap_obj(button("TAKE PHOTO"));
    CHECK("the shutter leads to the review", wait_until(reviewing, 5000));
    CHECK("the photo is on disk", photos_on_disk() == before + 1);
    CHECK("the review shows the picture and DELETE", picture() != NULL && button("DELETE") != NULL);
    CHECK("and names the photo", shows_text("IMG_"));
    check_targets(shape);
    tap_obj(button("KEEP"));
    CHECK("keep: the preview again", wait_until(live, 3000));
    CHECK("the photo is kept", photos_on_disk() == before + 1);
    CHECK("and offered as the last photo", last_button() != NULL);
    check_targets(shape);

    tap_obj(last_button());
    CHECK("the last photo opens in review", wait_until(reviewing, 2000));
    tap_obj(button("DELETE"));
    CHECK("delete asks first", confirming() && shows_text("Delete this photo?"));
    CHECK("nothing is deleted yet", photos_on_disk() == before + 1);
    check_targets(shape);
    tap_obj(button("CANCEL"));
    CHECK("cancel goes back", reviewing() && !confirming());
    tap_obj(button("DELETE"));
    tap_obj(button("DELETE")); /* the confirming one, in Keep's place */
    CHECK("confirmed: the preview again", wait_until(live, 3000));
    CHECK("the photo is gone from disk", photos_on_disk() == before);
    CHECK("and the screen says so", shows_text("Photo deleted"));
    CHECK("no last photo any more", last_button() == NULL);
    CHECK("closing is quick", app_stop() < 600);
    CHECK("and leaves no helper", no_child());
#undef CHECK
}

static void faults(void)
{
    int64_t took;
    int i;
    bool all = true;
    int64_t worst = 0;

    /* No camera: its own screen, and a way to look again. */
    setenv("POCKETOS_CAMERA_FAKE", "open=nodev", 1);
    app_start();
    check("no camera: said, with CHECK AGAIN", wait_until(no_camera, 3000));
    check("no shutter without a camera", button("TAKE PHOTO") == NULL);
    check_targets("no camera");
    tap_obj(button("CHECK AGAIN"));
    check("checking again finds no camera again", wait_until(no_camera, 3000));
    app_stop();

    /* The helper is not installed. */
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,period=20", 1);
    {
        char *keep = strdup(getenv("POCKETOS_CAMERA_HELPER"));

        setenv("POCKETOS_CAMERA_HELPER", "/nonexistent/pos-camera", 1);
        app_start();
        check("a missing helper: an error screen with TRY AGAIN", wait_until(failed_screen, 3000));
        check("that says why", shows_text("The camera helper could not be started"));
        setenv("POCKETOS_CAMERA_HELPER", keep, 1);
        free(keep);
        tap_obj(button("TRY AGAIN"));
        check("once it is there, trying again brings the preview", wait_until(live, 4000));
        app_stop();
    }

    /* A full disk: said, and nothing changes. */
    setenv("POCKETCAM_TEST_FREE_BYTES", "1000", 1);
    app_start();
    wait_until(live, 4000);
    tap_obj(button("TAKE PHOTO"));
    check("a full disk is said", wait_until_text("Storage is full", 3000));
    check("no photo was written", photos_on_disk() == 0);
    check("and the preview goes on", wait_until(live, 3000));
    app_stop();
    unsetenv("POCKETCAM_TEST_FREE_BYTES");

    /* The helper crashes in the middle of the preview. */
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,period=20,crash_at=15", 1);
    app_start();
    check("a crashing helper ends on the error screen", wait_until(failed_screen, 4000));
    check("that says it crashed", shows_text("The camera helper crashed"));
    check("with no helper left", no_child());
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,period=20", 1);
    tap_obj(button("TRY AGAIN"));
    check("try again: the preview", wait_until(live, 4000));
    app_stop();

    /* Closed in the middle of a capture. */
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,period=20,capture_delay=1500", 1);
    app_start();
    wait_until(live, 4000);
    tap_obj(button("TAKE PHOTO"));
    took = app_stop();
    printf("     closed mid-capture in %lld ms\n", (long long)took);
    check("closing mid-capture is bounded", took < 600);
    check("and leaves no helper", no_child());
    check("and no photo, whole or half", photos_on_disk() == 0);

    /* Twenty visits. */
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,period=20", 1);
    for (i = 0; i < 20; i++) {
        app_start();
        if (i % 2) {
            all &= wait_until(live, 4000);
        }
        took = app_stop();
        worst = took > worst ? took : worst;
    }
    printf("     slowest of twenty closes: %lld ms\n", (long long)worst);
    check("twenty opens and closes", all);
    check("each close bounded", worst < 600);
    check("no helper left behind", no_child());
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("CAMERA_HELPER");
    char cmd[300];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set CAMERA_HELPER to tests/pos-camera-testhooks\n");
        return 2;
    }
    snprintf(root, sizeof(root), "/tmp/camera-app-%ld", (long)getpid());
    snprintf(photos, sizeof(photos), "%s/camera", root);
    setenv("POCKETOS_STATE_DIR", root, 1);
    setenv("POCKETOS_CAMERA_HELPER", helper, 1);
    setenv("POCKETOS_CAMERA_BACKEND", "fake", 1);
    setenv("POCKETOS_CAMERA_FAKE", "size=64x36,still=128x72,period=20", 1);
    unsetenv("POCKETCAM_TEST_FREE_BYTES");

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

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("camera_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
