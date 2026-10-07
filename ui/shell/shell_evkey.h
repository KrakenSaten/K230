/*
 * A board key in the running shell: one evdev key code, found, read and
 * timed here (power_key.h), acted on by the shell (shell.c) through hooks.
 * Two instances: the power key (KEY_POWER, the PMU's INT0) and the BOOT key
 * (KEY_BACK, gpio-keys on IO0 - docs/hardware/K230_BUTTONS.md).
 *
 * Which device: the instance's environment variable when set ("none" turns
 * the key off), else the /dev/input/eventN that reports the key with the
 * fewest keys - the board's own one-key device ("K230 PMU Power Key", "K230
 * BOOT Key") rather than a USB keyboard that also lists that key. The event
 * number is not assumed.
 *
 * How: opened read-only, non-blocking and close-on-exec, never grabbed, so
 * every other reader sees the same events and nothing the touch, the
 * keyboard base or an app reads is touched. A timer drains it every
 * SHELL_EVKEY_POLL_MS and times the hold on the monotonic clock
 * (power_key.h); nothing waits for a release. The event timestamps are
 * asked for on CLOCK_MONOTONIC so a busy loop does not stretch a press.
 *
 * Lost input - the device gone, a read error, dropped events - ends a held
 * press with no action, and the key's real state is read back before it
 * counts again (power_key_resync). A missing device is looked for again
 * every SHELL_EVKEY_RESCAN_MS, quietly.
 *
 * The kernel's own 5 s hold of the power key still powers the device off;
 * the shell neither sees nor stops that (power_key.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_SHELL_EVKEY_H
#define DOORS_SHELL_EVKEY_H

#include "lvgl.h"
#include "power_key.h"

#include <stdbool.h>

#define SHELL_POWER_KEY_ENV "POCKETOS_POWER_KEY_DEVICE"
#define SHELL_BACK_KEY_ENV "POCKETOS_BACK_KEY_DEVICE"
#define SHELL_EVKEY_POLL_MS 25
#define SHELL_EVKEY_RESCAN_MS 3000

struct shell_evkey_config {
    const char *name;      /* for the log: "power key" */
    const char *env;       /* SHELL_POWER_KEY_ENV */
    unsigned code;         /* KEY_POWER */
    const char *code_name; /* for the log: "KEY_POWER" */
    const char *long_does; /* for the log: "for the menu" */
};

struct shell_evkey_hooks {
    /* The key went down. false: the press only woke the screen and is
     * swallowed (power_key_swallow). NULL: every press goes on. */
    bool (*press)(void);
    /* A short press, at its release. */
    void (*short_press)(void);
    /* A long press, once, while the key is still held. */
    void (*long_press)(void);
};

/* One key. Owned by the caller (static, = SHELL_EVKEY_INIT so a shutdown
 * before any init closes nothing); every field is the reader's. */
#define SHELL_EVKEY_INIT { .fd = -1 }
struct shell_evkey {
    struct shell_evkey_config cfg;
    struct shell_evkey_hooks hooks;
    struct power_key key;
    lv_timer_t *timer;
    int fd;
    bool enabled;
    bool explicit_path;  /* the environment named it */
    bool mono_stamps;    /* the device stamps events on CLOCK_MONOTONIC */
    bool dropping;       /* SYN_DROPPED seen: skip to the next SYN_REPORT */
    bool missing_logged; /* "not found" said once until it is found */
    uint32_t next_scan_ms;
    char device[64];
    unsigned opens;
    unsigned losses;
};

/* Find the key and start reading it. Once, after the display. */
void shell_evkey_init(struct shell_evkey *k, const struct shell_evkey_config *cfg,
                      const struct shell_evkey_hooks *hooks);
/* The exit path (also before a rotation's exec): timer and descriptor go. */
void shell_evkey_shutdown(struct shell_evkey *k);

struct shell_evkey_status {
    bool enabled;       /* false with the environment variable "none" */
    bool connected;
    char device[64];    /* the node in use, or the last one tried */
    struct power_key key;
    unsigned opens;     /* times a device was opened */
    unsigned losses;    /* times an open device was lost */
};
void shell_evkey_status(const struct shell_evkey *k, struct shell_evkey_status *out);

#endif
