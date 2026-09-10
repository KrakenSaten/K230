/*
 * DRM + evdev backend for the K230 (RM69A10 panel on /dev/dri/card0, GT9895
 * touch as an evdev node). Device paths and bench-time overrides come from
 * the environment (exported by S90pocketos-shell from
 * /etc/default/pocketos-shell) so the shell carries no board-specific
 * constants:
 *   POCKETOS_DRM_DEVICE    default /dev/dri/card0
 *   POCKETOS_TOUCH_DEVICE  default: evdev discovery of the first touch device
 *   POCKETOS_DRM_ROTATION  0 | 90 | 180 | 270, default 0. Uses the vendor
 *                          LVGL DRM patch (lv_linux_drm_set_rotation, DRM
 *                          plane rotation); absent from stock LVGL, in which
 *                          case a set value is reported and ignored. 90 and
 *                          270 swap the framebuffer to 1232x568, which the
 *                          shell layout does not follow: diagnostic only.
 *   POCKETOS_TOUCH_CALIB   minx,miny,maxx,maxy raw touch range mapped onto
 *                          the panel, default: the device's own ABS ranges.
 *   POCKETOS_TOUCH_SWAP    1 swaps the touch X and Y axes, default 0.
 * An invalid value logs a warning and leaves the default in place. There is
 * no automatic hardware detection beyond what LVGL's evdev driver does.
 *
 * Status: UNTESTED until hardware arrives. Whether the vendor-patched LVGL
 * DRM driver drives this panel correctly with this call sequence is ASSUMED.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "platform.h"
#include "pocketlog/pocketlog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct touch_override {
    int have_calib;
    int calib[4]; /* minx, miny, maxx, maxy */
    int swap;
};

static struct touch_override touch;

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

/* POCKETOS_DRM_ROTATION -> DRM rotation index 0..3, or 0 when unset/invalid. */
static int rotation_from_env(void)
{
    const char *v = getenv("POCKETOS_DRM_ROTATION");
    int deg;

    if (!v || !*v) {
        return 0;
    }
    if (parse_int(v, &deg) < 0 || (deg != 0 && deg != 90 && deg != 180 && deg != 270)) {
        LOG_WARN("POCKETOS_DRM_ROTATION=%s is not 0, 90, 180 or 270; using 0", v);
        return 0;
    }
    return deg / 90;
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
 * read whose state or point changed. It wraps the driver's read callback,
 * so it is installed only for a device named by POCKETOS_TOUCH_DEVICE:
 * discovery recognises its own devices by that callback and would attach
 * a second reader to a wrapped one. */
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

static void touch_apply(lv_indev_t *indev, const char *what)
{
    if (touch.have_calib) {
        lv_evdev_set_calibration(indev, touch.calib[0], touch.calib[1], touch.calib[2], touch.calib[3]);
        LOG_INFO("%s: touch calibration %d,%d,%d,%d", what, touch.calib[0], touch.calib[1],
                 touch.calib[2], touch.calib[3]);
    }
    if (touch.swap) {
        lv_evdev_set_swap_axes(indev, true);
        LOG_INFO("%s: touch axes swapped", what);
    }
}

static void on_evdev_found(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
    (void)user_data;
    if (type == LV_EVDEV_TYPE_ABS || type == LV_EVDEV_TYPE_REL) {
        LOG_INFO("input device attached (%s)",
                 type == LV_EVDEV_TYPE_ABS ? "touch" : "pointer");
        if (type == LV_EVDEV_TYPE_ABS) {
            touch_apply(indev, "discovered touch device");
        }
    }
}

lv_display_t *pocketos_platform_init(void)
{
    const char *drm_dev = getenv("POCKETOS_DRM_DEVICE");
    const char *touch_dev = getenv("POCKETOS_TOUCH_DEVICE");
    int rotation = rotation_from_env();
    lv_display_t *disp;

    lv_tick_set_cb(tick_ms);
    touch_overrides_from_env();
    disp = lv_linux_drm_create();
    if (!disp) {
        return NULL;
    }
    if (rotation != 0) {
#ifdef POCKETOS_DRM_ROTATION_API
        lv_linux_drm_set_rotation(disp, rotation);
        LOG_INFO("DRM plane rotation %d degrees (POCKETOS_DRM_ROTATION)", rotation * 90);
#else
        LOG_WARN("POCKETOS_DRM_ROTATION set but this LVGL build has no lv_linux_drm_set_rotation; ignored");
#endif
    }
    if (lv_linux_drm_set_file(disp, drm_dev ? drm_dev : "/dev/dri/card0", -1) != LV_RESULT_OK) {
        LOG_ERROR("cannot open DRM device %s", drm_dev ? drm_dev : "/dev/dri/card0");
        return NULL;
    }
    if (touch_dev) {
        lv_indev_t *indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_dev);

        if (indev) {
            touch_apply(indev, touch_dev);
            if (getenv("POCKETOS_INPUT_TRACE")) {
                touch_read_orig = lv_indev_get_read_cb(indev);
                lv_indev_set_read_cb(indev, touch_read_traced);
                LOG_INFO("%s: touch trace on (POCKETOS_INPUT_TRACE)", touch_dev);
            }
        } else {
            LOG_WARN("cannot open touch device %s", touch_dev);
        }
    } else {
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
