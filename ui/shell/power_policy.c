/*
 * Power & Sleep decisions. See power_policy.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "power_policy.h"

#include <stdio.h>
#include <string.h>

/* Never is last: a stepper's + goes from 10 min to never, its - back. */
static const int screen_options[] = { 30, 60, 120, 300, 600, 0 };
static const int lock_options[] = { 60, 120, 300, 600, 1800, 0 };

static const int *options(enum power_timer t, int *n)
{
    if (t == POWER_TIMER_LOCK) {
        *n = (int)(sizeof(lock_options) / sizeof(lock_options[0]));
        return lock_options;
    }
    *n = (int)(sizeof(screen_options) / sizeof(screen_options[0]));
    return screen_options;
}

int power_option_count(enum power_timer t)
{
    int n;

    (void)options(t, &n);
    return n;
}

int power_option_at(enum power_timer t, int i)
{
    int n;
    const int *o = options(t, &n);

    return i >= 0 && i < n ? o[i] : -1;
}

int power_option_index(enum power_timer t, int seconds)
{
    int n;
    const int *o = options(t, &n);
    int i;

    for (i = 0; i < n; i++) {
        if (o[i] == seconds) {
            return i;
        }
    }
    return -1;
}

int power_option_step(enum power_timer t, int seconds, int dir)
{
    int n;
    const int *o = options(t, &n);
    int i = power_option_index(t, seconds);

    if (i < 0) {
        return o[0];
    }
    if (dir < 0 && i > 0) {
        i--;
    } else if (dir > 0 && i < n - 1) {
        i++;
    }
    return o[i];
}

void power_option_label(int seconds, char *out, size_t n)
{
    if (seconds <= 0) {
        snprintf(out, n, "Never");
    } else if (seconds < 60) {
        snprintf(out, n, "%d s", seconds);
    } else {
        snprintf(out, n, "%d min", seconds / 60);
    }
}

int power_parse_setting(enum power_timer t, const char *s, int *seconds)
{
    long v = 0;
    size_t i;

    if (!s || !s[0] || strlen(s) > 6) {
        return -1;
    }
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    if (power_option_index(t, (int)v) < 0) {
        return -1;
    }
    *seconds = (int)v;
    return 0;
}

unsigned power_policy_due(const struct power_policy *p, uint32_t idle_ms, bool hold, bool screen_off,
                          bool locked)
{
    unsigned due = 0;

    if (!p || hold) {
        return 0;
    }
    if (p->screen_off_s > 0 && !screen_off && idle_ms >= (uint32_t)p->screen_off_s * 1000u) {
        due |= POWER_DO_SCREEN_OFF;
    }
    if (p->lock_s > 0 && !locked && idle_ms >= (uint32_t)p->lock_s * 1000u) {
        due |= POWER_DO_LOCK;
    }
    return due;
}
