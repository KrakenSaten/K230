/*
 * Display and touch turn together, through LVGL's real evdev driver.
 *
 * For each rotation, a display of the logical size the geometry gives is
 * created, and the K230 touch controller's own event grammar (GT9895,
 * protocol B: tracking id, MT position, BTN_TOUCH, SYN_REPORT) goes down a
 * pipe into lv_evdev_create_fd, configured exactly as the DRM backend does it:
 * pos_display_touch_config() for the geometry's rotation and the controller's
 * raw ranges. A touch on a native panel pixel must come out of LVGL on the
 * logical pixel the rotated display draws there - the four corners exactly,
 * the centre on the centre, off-axis points within a pixel - and a
 * configuration for the wrong rotation must not, so the check can fail.
 *
 * Needs LVGL with the evdev driver, so it is built by ui/shell/CMakeLists.txt
 * beside the shell on Linux hosts and run by tests/display_geometry_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "lvgl.h"
#include "pos_display.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NATIVE_W 568
#define NATIVE_H 1232
#define RAW_MAX_X 1059 /* the device's ABS ranges (timber_input_test.c, BRINGUP_SESSION_2026-09-07.md) */
#define RAW_MAX_Y 2399

static int checks;
static int failed;
static int ev_fd = -1;
static int track_id;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static uint8_t draw_buf[NATIVE_H * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void event(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    if (write(ev_fd, &e, sizeof(e)) != (ssize_t)sizeof(e)) {
        check("an input event is written", 0);
    }
}

/* A press at a native panel pixel, as the controller reports it. With swap,
 * the controller's X axis runs along the panel's Y. Returns where LVGL put
 * the pointer while pressed. */
static lv_point_t touch_native(lv_indev_t *indev, int swap, int px, int py)
{
    int nx = (px * RAW_MAX_X + (NATIVE_W - 1) / 2) / (NATIVE_W - 1);
    int ny = (py * RAW_MAX_Y + (NATIVE_H - 1) / 2) / (NATIVE_H - 1);
    lv_point_t p = { -1, -1 };

    event(EV_ABS, ABS_MT_TRACKING_ID, ++track_id);
    event(EV_ABS, ABS_MT_POSITION_X, swap ? ny : nx);
    event(EV_ABS, ABS_MT_POSITION_Y, swap ? nx : ny);
    event(EV_KEY, BTN_TOUCH, 1);
    event(EV_SYN, SYN_REPORT, 0);
    pump(60);
    lv_indev_get_point(indev, &p);
    event(EV_ABS, ABS_MT_TRACKING_ID, -1);
    event(EV_KEY, BTN_TOUCH, 0);
    event(EV_SYN, SYN_REPORT, 0);
    pump(60);
    return p;
}

static const int samples[][2] = { { 0, 0 }, { NATIVE_W - 1, 0 }, { NATIVE_W - 1, NATIVE_H - 1 },
                                  { 0, NATIVE_H - 1 }, { 283, 615 }, { 100, 1000 }, { 500, 40 } };

/* Run the samples on a display of rotation display_r with touch configured
 * for touch_r. Returns the number of samples that land wrong. */
static int run(enum pos_rotation display_r, enum pos_rotation touch_r, int swap, int verbose)
{
    const struct pos_panel panel = { NATIVE_W, NATIVE_H, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    const struct pos_touch_raw raw = { 0, 0, RAW_MAX_X, RAW_MAX_Y, swap != 0 };
    struct pos_display_geometry g;
    struct pos_evdev_config cfg;
    lv_display_t *disp;
    lv_indev_t *indev;
    int pipefd[2];
    int wrong = 0;
    char what[160];
    size_t i;

    pos_display_geometry_init(&g, &panel, display_r);
    disp = lv_display_create(g.width, g.height);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);
    if (verbose) {
        snprintf(what, sizeof(what), "rotation %d: the display is %dx%d logical", pos_rotation_degrees(display_r),
                 (int)g.width, (int)g.height);
        check(what, lv_display_get_horizontal_resolution(disp) == g.width &&
                        lv_display_get_vertical_resolution(disp) == g.height);
    }
    if (pipe(pipefd) != 0) {
        check("a pipe for the controller's events", 0);
        return 99;
    }
    ev_fd = pipefd[1];
    indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, pipefd[0]);
    if (!indev) {
        check("LVGL's evdev driver accepts the pipe", 0);
        return 99;
    }
    lv_indev_set_display(indev, disp);
    pos_display_touch_config(touch_r, &raw, &cfg);
    lv_evdev_set_swap_axes(indev, cfg.swap_axes);
    lv_evdev_set_calibration(indev, cfg.cal_x1, cfg.cal_y1, cfg.cal_x2, cfg.cal_y2);
    pump(20);

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int32_t ex;
        int32_t ey;
        lv_point_t p = touch_native(indev, swap, samples[i][0], samples[i][1]);
        int corner = i < 4;
        int ok;

        pos_rotation_native_to_logical(display_r, NATIVE_W, NATIVE_H, samples[i][0], samples[i][1], &ex, &ey);
        ok = corner ? (p.x == ex && p.y == ey) : (abs(p.x - ex) <= 1 && abs(p.y - ey) <= 1);
        if (!ok) {
            wrong++;
        }
        if (verbose) {
            snprintf(what, sizeof(what),
                     "rotation %d%s: native (%d,%d) touches logical (%d,%d), the display draws it at (%d,%d)",
                     pos_rotation_degrees(display_r), swap ? ", swapped controller" : "", samples[i][0],
                     samples[i][1], (int)p.x, (int)p.y, (int)ex, (int)ey);
            check(what, ok);
        }
    }
    if (verbose) {
        int32_t cx;
        int32_t cy;

        pos_rotation_native_to_logical(display_r, NATIVE_W, NATIVE_H, 283, 615, &cx, &cy);
        snprintf(what, sizeof(what), "rotation %d: the panel centre is the logical centre", pos_rotation_degrees(display_r));
        check(what, abs(cx - (g.width - 1) / 2) <= 1 && abs(cy - (g.height - 1) / 2) <= 1);
    }
    lv_indev_delete(indev);
    close(pipefd[1]);
    lv_display_delete(disp);
    return wrong;
}

int main(void)
{
    static const enum pos_rotation all[] = { POS_ROTATION_0, POS_ROTATION_90, POS_ROTATION_180, POS_ROTATION_270 };
    char what[160];
    size_t i;

    lv_init();
    for (i = 0; i < 4; i++) {
        run(all[i], all[i], 0, 1);
    }
    run(POS_ROTATION_270, POS_ROTATION_270, 1, 1);
    run(POS_ROTATION_0, POS_ROTATION_0, 1, 1);

    /* The same checks see a mismatch: a landscape display with portrait
     * touch, and each wrong landscape. */
    snprintf(what, sizeof(what), "a 270 display with touch for rotation 0 is caught");
    check(what, run(POS_ROTATION_270, POS_ROTATION_0, 0, 0) >= 3);
    snprintf(what, sizeof(what), "a 270 display with touch for rotation 90 is caught");
    check(what, run(POS_ROTATION_270, POS_ROTATION_90, 0, 0) >= 3);
    snprintf(what, sizeof(what), "a 90 display with touch for rotation 270 is caught");
    check(what, run(POS_ROTATION_90, POS_ROTATION_270, 0, 0) >= 3);
    snprintf(what, sizeof(what), "a 0 display with touch for rotation 180 is caught");
    check(what, run(POS_ROTATION_0, POS_ROTATION_180, 0, 0) >= 3);

    printf("display_touch_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
