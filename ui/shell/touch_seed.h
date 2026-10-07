/*
 * The touch pointer starts where the kernel says the finger last was.
 *
 * The Linux input core drops an ABS event whose value equals what the device
 * already holds (input_handle_abs_event: per device for ABS_X, per slot for
 * ABS_MT_*). LVGL's evdev driver starts its point at 0,0 and learns positions
 * only from events, so after the shell restarts - S90doors-shell restart, or
 * the exec a rotation change is - a touch that repeats the last raw X or Y
 * brings no event for that axis, and LVGL puts it at raw 0 on that axis.
 * Found in the Zabbix unit A gate on 2026-09-25 (docs/hardware/
 * TOUCH_RESTART_SEED_GATE.md).
 *
 * The fix gives LVGL's own parser the device's current position before the
 * first real event: the seed is written down a pipe LVGL's driver is created
 * on, the driver reads it, and the device is then put under the same file
 * descriptor number with dup3(). The driver's state is private, so this is how
 * it is set without reaching into it; vendor/lvgl is untouched.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_TOUCH_SEED_H
#define POCKETOS_TOUCH_SEED_H

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

struct touch_seed {
    bool valid;
    bool from_slot; /* multitouch slot 0 rather than ABS_X/ABS_Y */
    int32_t x;
    int32_t y;
};

/* Read the device's current raw position: multitouch slot 0 when the device
 * has slots (the GT9895 reports fingers only there, starting at slot 0), else
 * ABS_X and ABS_Y. Returns 0, or -1 with seed->valid false when fd is not an
 * input device that can say. */
int touch_seed_read(int fd, struct touch_seed *seed);

/* LVGL's evdev pointer on fd, starting at seed when it is valid. Takes fd
 * over (LVGL closes it when the indev is deleted) and makes it non-blocking.
 * A seed that cannot be applied leaves a plain lv_evdev_create_fd() pointer;
 * *seeded says which. Needs a default display, as LVGL's read does. */
lv_indev_t *touch_seed_evdev_create(int fd, const struct touch_seed *seed, bool *seeded);

#endif
