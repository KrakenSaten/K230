/*
 * DRM + evdev backend for the K230 (RM69A10 panel on /dev/dri/card0, GT9895
 * touch as an evdev node). Device paths and bench-time overrides come from
 * the environment (exported by S90pocketos-shell from
 * /etc/default/pocketos-shell) so the shell carries no board-specific
 * constants:
 *   POCKETOS_DRM_DEVICE    default /dev/dri/card0
 *   POCKETOS_TOUCH_DEVICE  default: the first evdev node with ABS_X and ABS_Y
 *   POCKETOS_TOUCH_CALIB   minx,miny,maxx,maxy: the raw values, after any
 *                          swap, at the panel's native top-left and
 *                          bottom-right, default: the device's own ABS ranges.
 *   POCKETOS_TOUCH_SWAP    1 when the controller's X and Y axes are swapped
 *                          relative to the panel, default 0.
 * CALIB and SWAP describe how the controller sits on the panel, so they hold
 * in every orientation. Rotation is not among them: it is decided by the
 * shell (shell_display.c; POCKETOS_DRM_ROTATION is a bench override there)
 * and arrives here as one geometry, from which the DRM plane rotation and the
 * touch transform are both set. An invalid value logs a warning and leaves
 * the default in place.
 *
 * Rotation uses the vendor LVGL DRM patch (lv_linux_drm_set_rotation, DRM
 * plane rotation, before the device is opened). Without it the backend runs
 * at rotation 0 and tells the shell so.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "platform.h"
#include "pocketlog/pocketlog.h"
#include "touch_seed.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct touch_override {
    int have_calib;
    int calib[4]; /* minx, miny, maxx, maxy */
    int swap;
};

static struct touch_override touch;
static struct pos_display_geometry active; /* what the display was set up with */

static uint32_t tick_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Whole-string decimal integer, or -1 with *out untouched. */
static int parse_int(const char *s, int *out)
{
    char *end;
    long v;

    if (!s || !*s) {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < -1000000 || v > 1000000) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static void touch_overrides_from_env(void)
{
    const char *calib = getenv("POCKETOS_TOUCH_CALIB");
    const char *swap = getenv("POCKETOS_TOUCH_SWAP");

    memset(&touch, 0, sizeof(touch));
    if (calib && *calib) {
        char buf[64];
        char *save = NULL;
        char *tok;
        int n = 0;
        int ok = strlen(calib) < sizeof(buf);

        if (ok) {
            snprintf(buf, sizeof(buf), "%s", calib);
            for (tok = strtok_r(buf, ",", &save); tok && n < 4; tok = strtok_r(NULL, ",", &save)) {
                if (parse_int(tok, &touch.calib[n]) < 0) {
                    ok = 0;
                    break;
                }
                n++;
            }
            ok = ok && n == 4 && tok == NULL && touch.calib[0] != touch.calib[2] &&
                 touch.calib[1] != touch.calib[3];
        }
        if (ok) {
            touch.have_calib = 1;
        } else {
            LOG_WARN("POCKETOS_TOUCH_CALIB=%s is not minx,miny,maxx,maxy with minx!=maxx and "
                     "miny!=maxy; using the device ranges", calib);
        }
    }
    if (swap && *swap) {
        if (strcmp(swap, "1") == 0) {
            touch.swap = 1;
        } else if (strcmp(swap, "0") != 0) {
            LOG_WARN("POCKETOS_TOUCH_SWAP=%s is not 0 or 1; using 0", swap);
        }
    }
}

/* Diagnostic trace of what LVGL reads from the touch device, on when
 * POCKETOS_INPUT_TRACE is set: the state and the calibrated point of every
 * read whose state or point changed. */
static lv_indev_read_cb_t touch_read_orig;

static void touch_read_traced(lv_indev_t *indev, lv_indev_data_t *data)
{
    static unsigned reads;
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    static lv_point_t last_point = { -1, -1 };

    touch_read_orig(indev, data);
    reads++;
    if (data->state != last_state || (data->state == LV_INDEV_STATE_PRESSED &&
                                      (data->point.x != last_point.x || data->point.y != last_point.y))) {
        LOG_INFO("touch trace: read %u %s x=%d y=%d t=%u", reads,
                 data->state == LV_INDEV_STATE_PRESSED ? "down" : "up", (int)data->point.x,
                 (int)data->point.y, (unsigned)lv_tick_get());
    }
    last_state = data->state;
    last_point = data->point;
}

#define BIT_SET(bits, n) (((bits)[(n) / (8 * sizeof(unsigned long))] >> ((n) % (8 * sizeof(unsigned long)))) & 1UL)

/* Whether an evdev node reports absolute X and Y, and their ranges. */
static int abs_ranges(int fd, struct input_absinfo *x, struct input_absinfo *y)
{
    unsigned long ev[(EV_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    unsigned long abs[(ABS_MAX + 1) / (8 * sizeof(unsigned long)) + 1];

    memset(ev, 0, sizeof(ev));
    memset(abs, 0, sizeof(abs));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !BIT_SET(ev, EV_ABS) ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0 || !BIT_SET(abs, ABS_X) ||
        !BIT_SET(abs, ABS_Y)) {
        return -1;
    }
    if (ioctl(fd, EVIOCGABS(ABS_X), x) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), y) < 0) {
        return -1;
    }
    return 0;
}

/* The touch node: POCKETOS_TOUCH_DEVICE, or the first /dev/input/eventN that
 * reports absolute X and Y. Its ranges are read here because the touch
 * transform needs them for every rotation, and LVGL's driver keeps its own
 * copy private. */
static int find_touch(const char *named, char *path, size_t path_len, struct input_absinfo *x,
                      struct input_absinfo *y)
{
    int n;

    for (n = 0; n < 64; n++) {
        int fd;
        int ok;

        if (named) {
            snprintf(path, path_len, "%s", named);
        } else {
            snprintf(path, path_len, "/dev/input/event%d", n);
        }
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (named) {
                return -1;
            }
            continue;
        }
        ok = abs_ranges(fd, x, y) == 0;
        close(fd);
        if (ok || named) {
            return ok ? 0 : -1;
        }
    }
    return -1;
}

/* The one place touch is configured: from the geometry the display was set
 * up with, never from a rotation of its own. */
static void touch_configure(lv_indev_t *indev, const char *what, const struct input_absinfo *x,
                            const struct input_absinfo *y)
{
    struct pos_touch_raw raw;
    struct pos_evdev_config cfg;

    if (touch.have_calib) {
        raw.x_at_left = touch.calib[0];
        raw.y_at_top = touch.calib[1];
        raw.x_at_right = touch.calib[2];
        raw.y_at_bottom = touch.calib[3];
    } else {
        raw.x_at_left = x->minimum;
        raw.y_at_top = y->minimum;
        raw.x_at_right = x->maximum;
        raw.y_at_bottom = y->maximum;
    }
    raw.swap = touch.swap != 0;
    pos_display_touch_config(active.rotation, &raw, &cfg);
    lv_evdev_set_swap_axes(indev, cfg.swap_axes);
    lv_evdev_set_calibration(indev, cfg.cal_x1, cfg.cal_y1, cfg.cal_x2, cfg.cal_y2);
    LOG_INFO("%s: touch raw x %d..%d y %d..%d%s%s; rotation %d: swap %d, calibration %d,%d,%d,%d onto %dx%d",
             what, (int)raw.x_at_left, (int)raw.x_at_right, (int)raw.y_at_top, (int)raw.y_at_bottom,
             raw.swap ? ", swapped" : "", touch.have_calib ? " (POCKETOS_TOUCH_CALIB)" : "",
             pos_rotation_degrees(active.rotation), cfg.swap_axes ? 1 : 0, (int)cfg.cal_x1, (int)cfg.cal_y1,
             (int)cfg.cal_x2, (int)cfg.cal_y2, (int)active.width, (int)active.height);
}

/* LVGL's pointer on the touch node, starting where the device last had a
 * finger. Without that, a restarted shell (S90 restart, or a rotation's exec)
 * loses the first touch that repeats the last raw X or Y: the kernel sends no
 * event for an unchanged value and LVGL starts at 0,0 (touch_seed.h). */
static lv_indev_t *touch_create(const char *path)
{
    struct touch_seed seed;
    bool seeded;
    lv_indev_t *indev;
    int fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);

    if (fd < 0) {
        return NULL;
    }
    touch_seed_read(fd, &seed);
    indev = touch_seed_evdev_create(fd, &seed, &seeded);
    if (indev && seeded) {
        LOG_INFO("%s: touch starts at raw %d,%d (the device's %s)", path, (int)seed.x, (int)seed.y,
                 seed.from_slot ? "slot 0 position" : "ABS_X and ABS_Y");
    } else if (indev) {
        LOG_WARN("%s: touch starts at raw 0,0; the device's position could not be applied, so a first "
                 "touch repeating the last raw X or Y lands at 0 on that axis", path);
    }
    return indev;
}

/* Discovery (the node appeared after the shell started) creates LVGL's
 * pointer itself, unseeded. A node that has only just appeared normally still
 * holds 0,0, which is where LVGL starts. */
static void on_evdev_found(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
    char path[64];
    struct input_absinfo ax;
    struct input_absinfo ay;

    (void)user_data;
    if (type != LV_EVDEV_TYPE_ABS) {
        return;
    }
    if (find_touch(NULL, path, sizeof(path), &ax, &ay) == 0) {
        touch_configure(indev, "discovered touch device", &ax, &ay);
    } else {
        LOG_WARN("discovered touch device: its ranges cannot be read; left uncalibrated");
    }
}

lv_display_t *pocketos_platform_init(const struct pos_panel *panel, struct pos_display_geometry *geometry)
{
    const char *drm_dev = getenv("POCKETOS_DRM_DEVICE");
    const char *touch_dev = getenv("POCKETOS_TOUCH_DEVICE");
    char touch_path[64];
    struct input_absinfo ax;
    struct input_absinfo ay;
    lv_display_t *disp;
    int32_t w;
    int32_t h;

    lv_tick_set_cb(tick_ms);
    touch_overrides_from_env();
    disp = lv_linux_drm_create();
    if (!disp) {
        return NULL;
    }
    if (geometry->rotation != POS_ROTATION_0) {
#ifdef POCKETOS_DRM_ROTATION_API
        lv_linux_drm_set_rotation(disp, (int)geometry->rotation);
        LOG_INFO("DRM plane rotation %d degrees", pos_rotation_degrees(geometry->rotation));
#else
        LOG_ERROR("rotation %d requested but this LVGL build has no lv_linux_drm_set_rotation; "
                  "display and touch stay at rotation 0", pos_rotation_degrees(geometry->rotation));
        pos_display_geometry_init(geometry, panel, POS_ROTATION_0);
#endif
    }
    if (lv_linux_drm_set_file(disp, drm_dev ? drm_dev : "/dev/dri/card0", -1) != LV_RESULT_OK) {
        LOG_ERROR("cannot open DRM device %s", drm_dev ? drm_dev : "/dev/dri/card0");
        return NULL;
    }
    w = lv_display_get_horizontal_resolution(disp);
    h = lv_display_get_vertical_resolution(disp);
    if (w != geometry->width || h != geometry->height) {
        /* The panel mode is not what platform.h says. Lay out, and map touch,
         * in what the display actually is rather than in a guess. */
        struct pos_panel real = *panel;
        bool quarter = geometry->rotation == POS_ROTATION_90 || geometry->rotation == POS_ROTATION_270;

        real.width = quarter ? h : w;
        real.height = quarter ? w : h;
        LOG_WARN("display came up %dx%d, expected %dx%d; using the display's size",
                 (int)w, (int)h, (int)geometry->width, (int)geometry->height);
        pos_display_geometry_init(geometry, &real, geometry->rotation);
    }
    active = *geometry;

    if (find_touch(touch_dev, touch_path, sizeof(touch_path), &ax, &ay) == 0) {
        lv_indev_t *indev = touch_create(touch_path);

        if (indev) {
            touch_configure(indev, touch_path, &ax, &ay);
            if (getenv("POCKETOS_INPUT_TRACE")) {
                touch_read_orig = lv_indev_get_read_cb(indev);
                lv_indev_set_read_cb(indev, touch_read_traced);
                LOG_INFO("%s: touch trace on (POCKETOS_INPUT_TRACE)", touch_path);
            }
        } else {
            LOG_WARN("cannot open touch device %s", touch_path);
        }
    } else if (touch_dev) {
        LOG_WARN("touch device %s cannot be opened or has no absolute X and Y: touch unavailable", touch_dev);
    } else {
        /* Not there yet at start: let LVGL watch /dev/input for it, and
         * configure it from the same geometry when it appears. */
        LOG_WARN("no touch device under /dev/input yet; watching for one");
        lv_evdev_discovery_start(on_evdev_found, NULL);
    }
    return disp;
}

/* The panel has no host keyboard; text comes from the touch keyboard, and
 * later from a physical one, both through the same stream (DS §17.4). */
lv_indev_t *pocketos_platform_keyboard(void)
{
    return NULL;
}

void pocketos_platform_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
