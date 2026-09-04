/*
 * DRM + evdev backend for the K230 (RM69A10 panel on /dev/dri/card0, GT9895
 * touch as an evdev node). Device paths come from the environment so the
 * shell carries no board-specific constants:
 *   POCKETOS_DRM_DEVICE   default /dev/dri/card0
 *   POCKETOS_TOUCH_DEVICE default: evdev discovery of the first touch device
 *
 * Status: UNTESTED until hardware arrives. The vendor launcher additionally
 * needs a GDMA rotation patch in its LVGL DRM driver; whether the stock LVGL
 * DRM driver at the pinned commit drives this panel correctly is ASSUMED.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint32_t tick_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void on_evdev_found(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
    (void)user_data;
    if (type == LV_EVDEV_TYPE_ABS || type == LV_EVDEV_TYPE_REL) {
        printf("shell: input device attached (%s)\n",
               type == LV_EVDEV_TYPE_ABS ? "touch" : "pointer");
    }
    (void)indev;
}

lv_display_t *pocketos_platform_init(void)
{
    const char *drm_dev = getenv("POCKETOS_DRM_DEVICE");
    const char *touch_dev = getenv("POCKETOS_TOUCH_DEVICE");
    lv_display_t *disp;

    lv_tick_set_cb(tick_ms);
    disp = lv_linux_drm_create();
    if (!disp) {
        return NULL;
    }
    if (lv_linux_drm_set_file(disp, drm_dev ? drm_dev : "/dev/dri/card0", -1) != LV_RESULT_OK) {
        fprintf(stderr, "shell: cannot open DRM device\n");
        return NULL;
    }
    if (touch_dev) {
        lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_dev);
    } else {
        lv_evdev_discovery_start(on_evdev_found, NULL);
    }
    return disp;
}

void pocketos_platform_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
