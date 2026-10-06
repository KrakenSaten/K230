/*
 * The board's power key in the running shell. See shell_power_key.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "shell_power_key.h"

#include "lvgl.h"
#include "pocketlog/pocketlog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef input_event_sec
#define input_event_sec time.tv_sec
#define input_event_usec time.tv_usec
#endif

#define BITS_PER_LONG_ (8 * sizeof(unsigned long))
#define NLONGS(n) (((n) + BITS_PER_LONG_ - 1) / BITS_PER_LONG_)
#define BIT_SET(a, b) (((a)[(b) / BITS_PER_LONG_] >> ((b) % BITS_PER_LONG_)) & 1UL)
#define SCAN_NODES 32

static struct {
    struct shell_power_key_hooks hooks;
    struct power_key key;
    lv_timer_t *timer;
    int fd;
    bool enabled;
    bool explicit_path;  /* POCKETOS_POWER_KEY_DEVICE named it */
    bool mono_stamps;    /* the device stamps events on CLOCK_MONOTONIC */
    bool dropping;       /* SYN_DROPPED seen: skip to the next SYN_REPORT */
    bool missing_logged; /* "not found" said once until it is found */
    uint32_t next_scan_ms;
    char device[64];
    unsigned opens;
    unsigned losses;
} pk = { .fd = -1 };

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* When the event happened: its own stamp when that is on the monotonic
 * clock, else when it was read. Never later than now. */
static uint32_t event_ms(const struct input_event *ev, uint32_t now)
{
    uint32_t t;

    if (!pk.mono_stamps) {
        return now;
    }
    t = (uint32_t)((uint64_t)ev->input_event_sec * 1000u + (uint64_t)ev->input_event_usec / 1000u);
    return (int32_t)(now - t) < 0 ? now : t;
}

/* How many keys the node reports, and whether KEY_POWER is one of them; -1
 * when it reports no KEY_POWER or is no evdev node at all. */
static int power_key_rank(int fd)
{
    unsigned long ev[NLONGS(EV_MAX + 1)];
    unsigned long keys[NLONGS(KEY_MAX + 1)];
    int count = 0;
    unsigned i;

    memset(ev, 0, sizeof(ev));
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !BIT_SET(ev, EV_KEY) ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !BIT_SET(keys, KEY_POWER)) {
        return -1;
    }
    for (i = 0; i <= KEY_MAX; i++) {
        count += (int)BIT_SET(keys, i);
    }
    return count;
}

/* The key as the kernel holds it now; -1 when that cannot be read. */
static int key_down_now(int fd)
{
    unsigned long keys[NLONGS(KEY_MAX + 1)];

    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
        return -1;
    }
    return (int)BIT_SET(keys, KEY_POWER);
}

static void resync(void)
{
    int down = key_down_now(pk.fd);

    if (down < 0) {
        /* Unknown: end any press with no action. A press still held then
         * ends in a release with no press, which is ignored. */
        power_key_lost(&pk.key);
    } else {
        power_key_resync(&pk.key, down == 1);
    }
}

static void close_device(void)
{
    if (pk.fd >= 0) {
        close(pk.fd);
        pk.fd = -1;
    }
    pk.dropping = false;
    pk.mono_stamps = false;
}

static void lose(const char *why)
{
    LOG_WARN("power key: %s lost (%s); a press in progress is dropped", pk.device, why);
    close_device();
    power_key_lost(&pk.key);
    pk.losses++;
    pk.next_scan_ms = now_ms() + SHELL_POWER_KEY_RESCAN_MS;
}

static int open_node(const char *path)
{
    return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* The named node, or the best KEY_POWER node under /dev/input. */
static int find_device(void)
{
    const char *env = getenv(SHELL_POWER_KEY_ENV);
    int best_fd = -1;
    int best_rank = 0;
    int n;

    if (pk.explicit_path) {
        int fd = open_node(env);

        snprintf(pk.device, sizeof(pk.device), "%s", env);
        if (fd >= 0 && power_key_rank(fd) < 0 && !pk.missing_logged) {
            /* Named on purpose; read it anyway, KEY_POWER events only. */
            LOG_WARN("power key: %s does not report KEY_POWER as an input device", env);
        }
        return fd;
    }
    for (n = 0; n < SCAN_NODES; n++) {
        char path[32];
        int fd;
        int rank;

        snprintf(path, sizeof(path), "/dev/input/event%d", n);
        fd = open_node(path);
        if (fd < 0) {
            continue;
        }
        rank = power_key_rank(fd);
        if (rank > 0 && (best_fd < 0 || rank < best_rank)) {
            if (best_fd >= 0) {
                close(best_fd);
            }
            best_fd = fd;
            best_rank = rank;
            snprintf(pk.device, sizeof(pk.device), "%s", path);
        } else {
            close(fd);
        }
    }
    return best_fd;
}

static void try_open(uint32_t now)
{
    int clk = CLOCK_MONOTONIC;
    char name[64];

    if ((int32_t)(now - pk.next_scan_ms) < 0) {
        return;
    }
    pk.next_scan_ms = now + SHELL_POWER_KEY_RESCAN_MS;
    pk.fd = find_device();
    if (pk.fd < 0) {
        if (!pk.missing_logged) {
            LOG_INFO("power key: no device found%s%s; looking again every %d s",
                     pk.explicit_path ? " at " : "", pk.explicit_path ? pk.device : "",
                     SHELL_POWER_KEY_RESCAN_MS / 1000);
            pk.missing_logged = true;
        }
        return;
    }
    pk.missing_logged = false;
    pk.opens++;
    pk.mono_stamps = ioctl(pk.fd, EVIOCSCLOCKID, &clk) == 0;
    name[0] = '\0';
    if (ioctl(pk.fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        snprintf(name, sizeof(name), "?");
    }
    name[sizeof(name) - 1] = '\0';
    LOG_INFO("power key: reading %s (%s), short press under %u ms, hold %u ms for the menu%s", pk.device, name,
             POWER_KEY_LONG_MS, POWER_KEY_LONG_MS, pk.mono_stamps ? "" : ", read-time stamps");
    /* Whatever the key is doing now, it did not start in our sight. */
    resync();
}

static void dispatch(enum power_key_press p)
{
    if (p == POWER_KEY_SHORT) {
        LOG_INFO("power key: short press");
        if (pk.hooks.short_press) {
            pk.hooks.short_press();
        }
    } else if (p == POWER_KEY_LONG) {
        LOG_INFO("power key: held %u ms", POWER_KEY_LONG_MS);
        if (pk.hooks.long_press) {
            pk.hooks.long_press();
        }
    }
}

static void drain(uint32_t now)
{
    struct input_event ev[16];

    for (;;) {
        ssize_t n = read(pk.fd, ev, sizeof(ev));
        size_t i;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lose(strerror(errno));
            }
            return;
        }
        if (n == 0 || (size_t)n % sizeof(ev[0]) != 0) {
            lose(n == 0 ? "end of input" : "short read");
            return;
        }
        for (i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            const struct input_event *e = &ev[i];

            if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                pk.dropping = true;
                continue;
            }
            if (pk.dropping) {
                if (e->type == EV_SYN && e->code == SYN_REPORT) {
                    pk.dropping = false;
                    LOG_WARN("power key: events dropped by the kernel; key state read back");
                    resync();
                }
                continue;
            }
            if (e->type == EV_KEY && e->code == KEY_POWER) {
                dispatch(power_key_input(&pk.key, e->value, event_ms(e, now)));
                if (pk.fd < 0) {
                    return; /* a hook shut us down */
                }
            }
        }
        if ((size_t)n < sizeof(ev)) {
            return;
        }
    }
}

static void on_poll(lv_timer_t *t)
{
    uint32_t now = now_ms();

    (void)t;
    if (pk.fd < 0) {
        try_open(now);
        if (pk.fd < 0) {
            return;
        }
    }
    drain(now);
    dispatch(power_key_poll(&pk.key, now_ms()));
}

void shell_power_key_init(const struct shell_power_key_hooks *hooks)
{
    const char *env = getenv(SHELL_POWER_KEY_ENV);

    shell_power_key_shutdown();
    memset(&pk, 0, sizeof(pk));
    pk.fd = -1;
    power_key_init(&pk.key);
    if (hooks) {
        pk.hooks = *hooks;
    }
    if (env && strcmp(env, "none") == 0) {
        LOG_INFO("power key: off (%s=none)", SHELL_POWER_KEY_ENV);
        return;
    }
    pk.enabled = true;
    pk.explicit_path = env && env[0];
    pk.next_scan_ms = now_ms();
    try_open(pk.next_scan_ms);
    pk.timer = lv_timer_create(on_poll, SHELL_POWER_KEY_POLL_MS, NULL);
}

void shell_power_key_shutdown(void)
{
    if (pk.timer) {
        lv_timer_delete(pk.timer);
        pk.timer = NULL;
    }
    close_device();
    power_key_lost(&pk.key);
}

void shell_power_key_status(struct shell_power_key_status *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = pk.enabled;
    out->connected = pk.fd >= 0;
    snprintf(out->device, sizeof(out->device), "%s", pk.device);
    out->key = pk.key;
    out->opens = pk.opens;
    out->losses = pk.losses;
}
