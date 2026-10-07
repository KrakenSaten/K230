/*
 * The keyboard backlight. See kbd_light.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_light.h"
#include "hw_actions.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int put(const char *dir, const char *attr, const char *value)
{
    char path[400];
    FILE *f;
    int rc;

    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    rc = fputs(value, f) < 0 ? -1 : 0;
    /* sysfs reports a refused value at the write, which fclose flushes. */
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

static int putn(const char *dir, const char *attr, long value)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%ld", value);
    return put(dir, attr, buf);
}

static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int kbd_light_probe(struct kbd_light *l, const char *sysfs_root)
{
    char dir[160];
    char path[480];
    char name[64];
    struct dirent *e;
    DIR *d;
    FILE *f;

    if (!l) {
        return -1;
    }
    memset(l, 0, sizeof(*l));
    l->percent = -1;
    snprintf(dir, sizeof(dir), "%s/class/pwm", sysfs_root ? sysfs_root : "/sys");
    d = opendir(dir);
    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "pwmchip", 7) != 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s/device/of_node/name", dir, e->d_name) >= (int)sizeof(path)) {
            continue;
        }
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        name[0] = '\0';
        if (!fgets(name, sizeof(name), f)) {
            name[0] = '\0';
        }
        fclose(f);
        /* of_node/name carries no newline, but a fake tree may. */
        name[strcspn(name, "\r\n")] = '\0';
        if (strcmp(name, KBD_LIGHT_OF_NODE) == 0) {
            if (snprintf(l->chip, sizeof(l->chip), "%s/%s", dir, e->d_name) < (int)sizeof(l->chip) &&
                snprintf(l->dir, sizeof(l->dir), "%s/pwm%d", l->chip, KBD_LIGHT_CHANNEL) < (int)sizeof(l->dir)) {
                l->supported = 1;
            }
            break;
        }
    }
    closedir(d);
    return l->supported ? 0 : -1;
}

int kbd_light_set(struct kbd_light *l, int percent)
{
    long duty;

    if (!l || !l->supported) {
        errno = ENODEV;
        return -1;
    }
    if (percent < HW_KBD_LIGHT_MIN_PCT) {
        percent = HW_KBD_LIGHT_MIN_PCT;
    }
    if (percent > HW_KBD_LIGHT_MAX_PCT) {
        percent = HW_KBD_LIGHT_MAX_PCT;
    }
    percent = (percent + HW_KBD_LIGHT_STEP_PCT / 2) / HW_KBD_LIGHT_STEP_PCT * HW_KBD_LIGHT_STEP_PCT;
    if (!is_dir(l->dir) && putn(l->chip, "export", KBD_LIGHT_CHANNEL) < 0 && errno != EBUSY) {
        return -1;
    }
    duty = (long)KBD_LIGHT_PERIOD_NS * (100 - percent) / 100;
    /* The vendor's order: the duty may never exceed the period, and the
     * polarity can only change while the channel is disabled. */
    if (put(l->dir, "enable", "0") < 0 || putn(l->dir, "duty_cycle", 0) < 0 ||
        putn(l->dir, "period", KBD_LIGHT_PERIOD_NS) < 0 || put(l->dir, "polarity", "inversed") < 0 ||
        putn(l->dir, "duty_cycle", duty) < 0 || put(l->dir, "enable", "1") < 0) {
        return -1;
    }
    l->percent = percent;
    return percent;
}

int kbd_light_parse(const char *s, int *out)
{
    char *end;
    long v;

    if (!s || !*s) {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < HW_KBD_LIGHT_MIN_PCT || v > HW_KBD_LIGHT_MAX_PCT ||
        v % HW_KBD_LIGHT_STEP_PCT != 0) {
        return -1;
    }
    if (out) {
        *out = (int)v;
    }
    return 0;
}
