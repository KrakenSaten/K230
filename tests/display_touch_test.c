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
 * And a restarted shell: with the input core's rule that an unchanged value
 * is not sent modelled, a first touch repeating the last raw X, Y or both
 * lands where it is drawn once the pointer is seeded (ui/shell/touch_seed.c),
 * and is lost without the seed, so that check can fail too.
 *
 * Needs LVGL with the evdev driver, so it is built by ui/shell/CMakeLists.txt
 * beside the shell on Linux hosts and run by tests/display_geometry_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "pos_display.h"
#include "touch_seed.h"

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

/* ---- a restarted shell (ui/shell/touch_seed.h) ------------------------------
 *
 * The input core holds each slot's last position and sends no ABS_MT event
 * for a value that has not changed. A new shell's LVGL pointer starts at 0,0,
 * so a first touch repeating the last raw X or Y used to land at 0 on that
 * axis (Zabbix unit A gate, 2026-09-25). Here the kernel is modelled - slot 0
 * holds where the last shell's finger was, and only changed values go down
 * the pipe - and the same touches are made with LVGL's pointer created plain
 * and created seeded from the slot. */

static int32_t slot_x;
static int32_t slot_y;

static void kernel_mt(uint16_t code, int32_t v, int32_t *held)
{
    if (v != *held) {
        *held = v;
        event(EV_ABS, code, v);
    }
}

static void raw_of_native(int px, int py, int32_t *rx, int32_t *ry)
{
    *rx = (px * RAW_MAX_X + (NATIVE_W - 1) / 2) / (NATIVE_W - 1);
    *ry = (py * RAW_MAX_Y + (NATIVE_H - 1) / 2) / (NATIVE_H - 1);
}

/* A finger on native pixel (px, py) as the input core passes it on. */
static lv_point_t device_tap(lv_indev_t *indev, int px, int py)
{
    int32_t rx;
    int32_t ry;
    lv_point_t p = { -1, -1 };

    raw_of_native(px, py, &rx, &ry);
    event(EV_ABS, ABS_MT_TRACKING_ID, ++track_id);
    kernel_mt(ABS_MT_POSITION_X, rx, &slot_x);
    kernel_mt(ABS_MT_POSITION_Y, ry, &slot_y);
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

enum restart_case { SAME_POINT, SAME_X, SAME_Y, ELSEWHERE, MOVED_AFTER_SEED };
static const char *const case_name[] = { "the same point", "the same raw X", "the same raw Y",
                                         "a different point", "a point the device moved to after the seed was read" };

/* The last shell's finger lifted at native A; a new shell starts; one tap.
 * Returns whether it landed where the display draws it. */
static int restart_tap(enum pos_rotation r, int seeded, enum restart_case c, int verbose)
{
    static const int A[2] = { 218, 133 }; /* the gate's tab, portrait native */
    static const int B[2] = { 400, 900 };
    const struct pos_panel panel = { NATIVE_W, NATIVE_H, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    const struct pos_touch_raw raw = { 0, 0, RAW_MAX_X, RAW_MAX_Y, false };
    struct pos_display_geometry g;
    struct pos_evdev_config cfg;
    struct touch_seed seed;
    lv_display_t *disp;
    lv_indev_t *indev;
    lv_point_t p;
    int dev[2];
    int tx;
    int ty;
    int32_t ex;
    int32_t ey;
    bool applied = false;
    char what[240];
    int ok;

    raw_of_native(A[0], A[1], &slot_x, &slot_y);
    pos_display_geometry_init(&g, &panel, r);
    disp = lv_display_create(g.width, g.height);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);
    /* A blocking pipe, as a device node is opened: the seeded path must make
     * the device non-blocking itself or the first read hangs (the alarm in
     * main turns that into a failure). */
    if (pipe(dev) != 0) {
        check("a pipe for the device", 0);
        return 0;
    }
    ev_fd = dev[1];
    if (seeded) {
        seed.valid = true;
        seed.from_slot = true;
        seed.x = slot_x;
        seed.y = slot_y;
        if (c == MOVED_AFTER_SEED) {
            /* The finger moved (no touch down) between the seed being read
             * and the driver taking the device: the device queued it. */
            raw_of_native(B[0], B[1], &ex, &ey);
            kernel_mt(ABS_MT_POSITION_X, ex, &slot_x);
            kernel_mt(ABS_MT_POSITION_Y, ey, &slot_y);
            event(EV_SYN, SYN_REPORT, 0);
        }
        indev = touch_seed_evdev_create(dev[0], &seed, &applied);
    } else {
        indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, dev[0]);
    }
    if (!indev) {
        check("LVGL's evdev driver accepts the device", 0);
        return 0;
    }
    if (seeded) {
        check("the seed was applied", applied);
    }
    lv_indev_set_display(indev, disp);
    pos_display_touch_config(r, &raw, &cfg);
    lv_evdev_set_swap_axes(indev, cfg.swap_axes);
    lv_evdev_set_calibration(indev, cfg.cal_x1, cfg.cal_y1, cfg.cal_x2, cfg.cal_y2);
    pump(20);

    tx = c == SAME_Y || c == ELSEWHERE || c == MOVED_AFTER_SEED ? B[0] : A[0];
    ty = c == SAME_X || c == ELSEWHERE || c == MOVED_AFTER_SEED ? B[1] : A[1];
    p = device_tap(indev, tx, ty);
    pos_rotation_native_to_logical(r, NATIVE_W, NATIVE_H, tx, ty, &ex, &ey);
    ok = abs(p.x - ex) <= 1 && abs(p.y - ey) <= 1;
    if (verbose) {
        snprintf(what, sizeof(what), "rotation %d, %s pointer: after a restart, a tap on %s lands at (%d,%d), drawn at (%d,%d)",
                 pos_rotation_degrees(r), seeded ? "seeded" : "plain", case_name[c], (int)p.x, (int)p.y, (int)ex,
                 (int)ey);
        check(what, ok);
    }
    lv_indev_delete(indev);
    close(dev[1]);
    lv_display_delete(disp);
    return ok;
}

static void test_restart(void)
{
    static const enum pos_rotation rs[] = { POS_ROTATION_0, POS_ROTATION_270 };
    struct touch_seed seed;
    char what[160];
    size_t i;
    int c;
    int plain_lost = 0;
    int pipefd[2];

    for (i = 0; i < 2; i++) {
        for (c = SAME_POINT; c <= MOVED_AFTER_SEED; c++) {
            restart_tap(rs[i], 1, (enum restart_case)c, 1);
        }
        /* The defect, so these checks can fail: plain, every repeat is lost. */
        for (c = SAME_POINT; c <= SAME_Y; c++) {
            plain_lost += !restart_tap(rs[i], 0, (enum restart_case)c, 0);
        }
        snprintf(what, sizeof(what), "rotation %d, plain pointer: a tap on a different point lands",
                 pos_rotation_degrees(rs[i]));
        check(what, restart_tap(rs[i], 0, ELSEWHERE, 0));
    }
    snprintf(what, sizeof(what), "the plain pointer loses all %d repeated taps (the defect is modelled: %d)", 6, plain_lost);
    check(what, plain_lost == 6);

    /* Not an input device: no seed, and the pointer is created plain. */
    if (pipe(pipefd) == 0) {
        check("a pipe has no current position to read", touch_seed_read(pipefd[0], &seed) == -1 && !seed.valid);
        close(pipefd[0]);
        close(pipefd[1]);
    }
}

int main(void)
{
    static const enum pos_rotation all[] = { POS_ROTATION_0, POS_ROTATION_90, POS_ROTATION_180, POS_ROTATION_270 };
    char what[160];
    size_t i;

    alarm(60);
    lv_init();
    test_restart();
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
