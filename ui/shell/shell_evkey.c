/*
 * A board key in the running shell. See shell_evkey.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "shell_evkey.h"

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

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* When the event happened: its own stamp when that is on the monotonic
 * clock, else when it was read. Never later than now. */
static uint32_t event_ms(const struct shell_evkey *k, const struct input_event *ev, uint32_t now)
{
    uint32_t t;

    if (!k->mono_stamps) {
        return now;
    }
    t = (uint32_t)((uint64_t)ev->input_event_sec * 1000u + (uint64_t)ev->input_event_usec / 1000u);
    return (int32_t)(now - t) < 0 ? now : t;
}

/* How many keys the node reports, and whether the code is one of them; -1
 * when it does not report the code or is no evdev node at all. */
static int key_rank(int fd, unsigned code)
{
    unsigned long ev[NLONGS(EV_MAX + 1)];
    unsigned long keys[NLONGS(KEY_MAX + 1)];
    int count = 0;
    unsigned i;

    memset(ev, 0, sizeof(ev));
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !BIT_SET(ev, EV_KEY) ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !BIT_SET(keys, code)) {
        return -1;
    }
    for (i = 0; i <= KEY_MAX; i++) {
        count += (int)BIT_SET(keys, i);
    }
    return count;
}

/* The key as the kernel holds it now; -1 when that cannot be read. */
static int key_down_now(int fd, unsigned code)
{
    unsigned long keys[NLONGS(KEY_MAX + 1)];

    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
        return -1;
    }
    return (int)BIT_SET(keys, code);
}

static void resync(struct shell_evkey *k)
{
    int down = key_down_now(k->fd, k->cfg.code);

    if (down < 0) {
        /* Unknown: end any press with no action. A press still held then
         * ends in a release with no press, which is ignored. */
        power_key_lost(&k->key);
    } else {
        power_key_resync(&k->key, down == 1);
    }
}

static void close_device(struct shell_evkey *k)
{
    if (k->fd >= 0) {
        close(k->fd);
        k->fd = -1;
    }
    k->dropping = false;
    k->mono_stamps = false;
}

static void lose(struct shell_evkey *k, const char *why)
{
    LOG_WARN("%s: %s lost (%s); a press in progress is dropped", k->cfg.name, k->device, why);
    close_device(k);
    power_key_lost(&k->key);
    k->losses++;
    k->next_scan_ms = now_ms() + SHELL_EVKEY_RESCAN_MS;
}

static int open_node(const char *path)
{
    return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* The named node, or the best node for the code under /dev/input. */
static int find_device(struct shell_evkey *k)
{
    const char *env = getenv(k->cfg.env);
    int best_fd = -1;
    int best_rank = 0;
    int n;

    if (k->explicit_path) {
        int fd = open_node(env);

        snprintf(k->device, sizeof(k->device), "%s", env);
        if (fd >= 0 && key_rank(fd, k->cfg.code) < 0 && !k->missing_logged) {
            /* Named on purpose; read it anyway, that code's events only. */
            LOG_WARN("%s: %s does not report %s as an input device", k->cfg.name, env, k->cfg.code_name);
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
        rank = key_rank(fd, k->cfg.code);
        if (rank > 0 && (best_fd < 0 || rank < best_rank)) {
            if (best_fd >= 0) {
                close(best_fd);
            }
            best_fd = fd;
            best_rank = rank;
            snprintf(k->device, sizeof(k->device), "%s", path);
        } else {
            close(fd);
        }
    }
    return best_fd;
}

static void try_open(struct shell_evkey *k, uint32_t now)
{
    int clk = CLOCK_MONOTONIC;
    char name[64];

    if ((int32_t)(now - k->next_scan_ms) < 0) {
        return;
    }
    k->next_scan_ms = now + SHELL_EVKEY_RESCAN_MS;
    k->fd = find_device(k);
    if (k->fd < 0) {
        if (!k->missing_logged) {
            LOG_INFO("%s: no device found%s%s; looking again every %d s", k->cfg.name,
                     k->explicit_path ? " at " : "", k->explicit_path ? k->device : "",
                     SHELL_EVKEY_RESCAN_MS / 1000);
            k->missing_logged = true;
        }
        return;
    }
    k->missing_logged = false;
    k->opens++;
    k->mono_stamps = ioctl(k->fd, EVIOCSCLOCKID, &clk) == 0;
    name[0] = '\0';
    if (ioctl(k->fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        snprintf(name, sizeof(name), "?");
    }
    name[sizeof(name) - 1] = '\0';
    LOG_INFO("%s: reading %s (%s), short press under %u ms, hold %u ms %s%s", k->cfg.name, k->device, name,
             POWER_KEY_LONG_MS, POWER_KEY_LONG_MS, k->cfg.long_does, k->mono_stamps ? "" : ", read-time stamps");
    /* Whatever the key is doing now, it did not start in our sight. */
    resync(k);
}

static void dispatch(struct shell_evkey *k, enum power_key_press p)
{
    if (p == POWER_KEY_SHORT) {
        LOG_INFO("%s: short press", k->cfg.name);
        if (k->hooks.short_press) {
            k->hooks.short_press();
        }
    } else if (p == POWER_KEY_LONG) {
        LOG_INFO("%s: held %u ms", k->cfg.name, POWER_KEY_LONG_MS);
        if (k->hooks.long_press) {
            k->hooks.long_press();
        }
    }
}

/* One EV_KEY event for the code. A press that the press hook turns down
 * (it only woke the screen) is swallowed whole. */
static void key_event(struct shell_evkey *k, int value, uint32_t t_ms)
{
    bool was_up = k->key.state == POWER_KEY_UP;
    enum power_key_press p = power_key_input(&k->key, value, t_ms);

    if (value == 1 && was_up && k->key.state == POWER_KEY_DOWN && k->hooks.press && !k->hooks.press()) {
        power_key_swallow(&k->key);
        LOG_INFO("%s: the press woke the screen; nothing else", k->cfg.name);
    }
    dispatch(k, p);
}

static void drain(struct shell_evkey *k, uint32_t now)
{
    struct input_event ev[16];

    for (;;) {
        ssize_t n = read(k->fd, ev, sizeof(ev));
        size_t i;

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lose(k, strerror(errno));
            }
            return;
        }
        if (n == 0 || (size_t)n % sizeof(ev[0]) != 0) {
            lose(k, n == 0 ? "end of input" : "short read");
            return;
        }
        for (i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            const struct input_event *e = &ev[i];

            if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                k->dropping = true;
                continue;
            }
            if (k->dropping) {
                if (e->type == EV_SYN && e->code == SYN_REPORT) {
                    k->dropping = false;
                    LOG_WARN("%s: events dropped by the kernel; key state read back", k->cfg.name);
                    resync(k);
                }
                continue;
            }
            if (e->type == EV_KEY && e->code == k->cfg.code) {
                key_event(k, e->value, event_ms(k, e, now));
                if (k->fd < 0) {
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
    struct shell_evkey *k = lv_timer_get_user_data(t);
    uint32_t now = now_ms();

    if (k->fd < 0) {
        try_open(k, now);
        if (k->fd < 0) {
            return;
        }
    }
    drain(k, now);
    if (k->timer) {
        dispatch(k, power_key_poll(&k->key, now_ms()));
    }
}

void shell_evkey_init(struct shell_evkey *k, const struct shell_evkey_config *cfg,
                      const struct shell_evkey_hooks *hooks)
{
    const char *env = getenv(cfg->env);

    shell_evkey_shutdown(k);
    memset(k, 0, sizeof(*k));
    k->fd = -1;
    k->cfg = *cfg;
    power_key_init(&k->key);
    if (hooks) {
        k->hooks = *hooks;
    }
    if (env && strcmp(env, "none") == 0) {
        LOG_INFO("%s: off (%s=none)", k->cfg.name, k->cfg.env);
        return;
    }
    k->enabled = true;
    k->explicit_path = env && env[0];
    k->next_scan_ms = now_ms();
    try_open(k, k->next_scan_ms);
    k->timer = lv_timer_create(on_poll, SHELL_EVKEY_POLL_MS, k);
}

void shell_evkey_shutdown(struct shell_evkey *k)
{
    if (k->timer) {
        lv_timer_delete(k->timer);
        k->timer = NULL;
    }
    close_device(k);
    power_key_lost(&k->key);
}

void shell_evkey_status(const struct shell_evkey *k, struct shell_evkey_status *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = k->enabled;
    out->connected = k->fd >= 0;
    snprintf(out->device, sizeof(out->device), "%s", k->device);
    out->key = k->key;
    out->opens = k->opens;
    out->losses = k->losses;
}
