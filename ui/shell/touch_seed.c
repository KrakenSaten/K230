/*
 * The touch pointer starts where the kernel says the finger last was. See
 * touch_seed.h for why.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE /* pipe2, dup3 */
#include "touch_seed.h"

#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* One multitouch value of slot 0 (EVIOCGMTSLOTS: the code in, one value per
 * slot out, as many slots as the buffer has room for). */
static int slot0_value(int fd, uint32_t code, int32_t *out)
{
    int32_t req[2] = { (int32_t)code, 0 };

    if (ioctl(fd, EVIOCGMTSLOTS(sizeof(req)), req) < 0) {
        return -1;
    }
    *out = req[1];
    return 0;
}

int touch_seed_read(int fd, struct touch_seed *seed)
{
    struct input_absinfo ax;
    struct input_absinfo ay;

    memset(seed, 0, sizeof(*seed));
    /* Slot 0, not the slot the kernel last reported: a single finger lands in
     * slot 0 again even when a second finger was the last to lift. */
    if (slot0_value(fd, ABS_MT_POSITION_X, &seed->x) == 0 && slot0_value(fd, ABS_MT_POSITION_Y, &seed->y) == 0) {
        seed->valid = true;
        seed->from_slot = true;
        return 0;
    }
    if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0) {
        seed->x = ax.value;
        seed->y = ay.value;
        seed->valid = true;
        return 0;
    }
    memset(seed, 0, sizeof(*seed));
    return -1;
}

static bool put(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    return write(fd, &e, sizeof(e)) == (ssize_t)sizeof(e);
}

lv_indev_t *touch_seed_evdev_create(int fd, const struct touch_seed *seed, bool *seeded)
{
    int p[2];
    int flags;
    bool ok;
    lv_indev_t *indev;
    lv_indev_data_t data;

    *seeded = false;
    /* LVGL sets O_NONBLOCK on the descriptor it is given, which on the seeded
     * path is the pipe's; the device's is set here for both paths. */
    flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (!seed || !seed->valid || !lv_display_get_default() || pipe2(p, O_CLOEXEC) != 0) {
        return lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fd);
    }
    /* The position as the device reports a finger, and nothing else: no
     * tracking id and no BTN_TOUCH, so the pointer stays released. The MT
     * codes move LVGL's point with or without its gesture recognition. */
    ok = put(p[1], EV_ABS, ABS_MT_POSITION_X, seed->x) && put(p[1], EV_ABS, ABS_MT_POSITION_Y, seed->y) &&
         put(p[1], EV_SYN, SYN_REPORT, 0);
    close(p[1]);
    if (!ok) {
        close(p[0]);
        return lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fd);
    }
    /* On the pipe the driver's EVIOCGABS range query fails (logged at info);
     * the caller sets the calibration afterwards in any case. Its device
     * identity is the pipe's too, which only its discovery compares. */
    indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, p[0]);
    if (!indev) {
        return lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fd);
    }
    /* The driver reads the seed now, through its own parser. */
    memset(&data, 0, sizeof(data));
    lv_indev_get_read_cb(indev)(indev, &data);
    /* Then the device takes the pipe's place under the same number. Anything
     * the device queued since fd was opened is still queued, and lands on
     * top of the seed. */
    if (dup3(fd, p[0], O_CLOEXEC) < 0) {
        lv_indev_delete(indev);
        return lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fd);
    }
    close(fd);
    *seeded = true;
    return indev;
}
