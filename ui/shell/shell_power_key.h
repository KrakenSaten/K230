/*
 * The board's power key in the running shell: found, read and timed here,
 * acted on by the shell (shell.c) through two hooks.
 *
 * Which device: POCKETOS_POWER_KEY_DEVICE when set ("none" turns the key
 * off), else the /dev/input/eventN that reports KEY_POWER with the fewest
 * keys - the board's PMU key ("K230 PMU Power Key", one key) rather than a
 * USB keyboard that also lists a power key. The event number is not assumed.
 *
 * How: opened read-only, non-blocking and close-on-exec, never grabbed, so
 * every other reader sees the same events and nothing the touch, the
 * keyboard base or an app reads is touched. A timer drains it every
 * SHELL_POWER_KEY_POLL_MS and times the hold on the monotonic clock
 * (power_key.h); nothing waits for a release. The event timestamps are
 * asked for on CLOCK_MONOTONIC so a busy loop does not stretch a press.
 *
 * Lost input - the device gone, a read error, dropped events - ends a held
 * press with no action, and the key's real state is read back before it
 * counts again (power_key_resync). A missing device is looked for again
 * every SHELL_POWER_KEY_RESCAN_MS, quietly.
 *
 * The kernel's own 5 s hold still powers the device off; the shell neither
 * sees nor stops that (power_key.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_SHELL_POWER_KEY_H
#define DOORS_SHELL_POWER_KEY_H

#include "power_key.h"

#include <stdbool.h>

#define SHELL_POWER_KEY_ENV "POCKETOS_POWER_KEY_DEVICE"
#define SHELL_POWER_KEY_POLL_MS 25
#define SHELL_POWER_KEY_RESCAN_MS 3000

struct shell_power_key_hooks {
    /* A short press, at its release. */
    void (*short_press)(void);
    /* A long press, once, while the key is still held. */
    void (*long_press)(void);
};

/* Find the key and start reading it. Once, after the display. */
void shell_power_key_init(const struct shell_power_key_hooks *hooks);
/* The exit path (also before a rotation's exec): timer and descriptor go. */
void shell_power_key_shutdown(void);

struct shell_power_key_status {
    bool enabled;       /* false with POCKETOS_POWER_KEY_DEVICE=none */
    bool connected;
    char device[64];    /* the node in use, or the last one tried */
    struct power_key key;
    unsigned opens;     /* times a device was opened */
    unsigned losses;    /* times an open device was lost */
};
void shell_power_key_status(struct shell_power_key_status *out);

#endif
