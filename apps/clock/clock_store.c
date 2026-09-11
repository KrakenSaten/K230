/*
 * PocketClock persistence. See clock_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STORE_PATH_MAX 256
/* Longest legal line: the keyword, four small numbers and a full label. */
#define STORE_LINE_MAX 128
/* A settings file far larger than this is not one of ours. */
#define STORE_MAX_LINES (CLOCK_MAX_ALARMS + 8)

static char dir_buf[STORE_PATH_MAX];

const char *clock_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : CLOCK_STORE_DEFAULT_DIR, CLOCK_STORE_SUBDIR);
    return dir_buf;
}

int clock_store_path(char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s/%s", clock_store_dir(), CLOCK_STORE_FILE);

    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

/* Create every missing component of the store directory. Best effort: a
 * failure surfaces as a failed write, which the caller already handles. */
static void make_dirs(const char *dir)
{
    char work[STORE_PATH_MAX];
    size_t i;

    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        mkdir(work, 0755);
        work[i] = '/';
    }
    mkdir(work, 0755);
}

int clock_label_is_storable(const char *label)
{
    size_t i;

    if (!label) {
        return 1; /* no label is perfectly storable */
    }
    if (strlen(label) >= CLOCK_LABEL_MAX) {
        return 0;
    }
    for (i = 0; label[i]; i++) {
        unsigned char c = (unsigned char)label[i];

        /* Anything below space would either end the line early or come back
         * as something else. Bytes above 0x7F are UTF-8 continuation and
         * lead bytes, which survive a line intact and are kept. */
        if (c < 0x20 || c == 0x7F) {
            return 0;
        }
    }
    return 1;
}

/* ---- loading ----------------------------------------------------------- */

/* One unsigned field, with no strtol surprises: a field that is not entirely
 * digits is a broken file, not a zero. */
static int parse_uint(const char *s, const char **end, long *out)
{
    long v = 0;
    int digits = 0;

    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        if (v > 100000000L) {
            return -1;
        }
        v = v * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    *end = s;
    return 0;
}

static int parse_alarm(const char *rest, struct clock_engine *e)
{
    long enabled, hour, minute, repeat;
    const char *p = rest;
    int index;

    if (parse_uint(p, &p, &enabled) != 0 || parse_uint(p, &p, &hour) != 0 ||
        parse_uint(p, &p, &minute) != 0 || parse_uint(p, &p, &repeat) != 0) {
        return -1;
    }
    if (enabled > 1 || hour > 23 || minute > 59 || repeat >= CLOCK_REPEAT_COUNT) {
        return -1;
    }
    if (*p == ' ') {
        p++;
    }
    if (!clock_label_is_storable(p)) {
        return -1;
    }
    /* NULL for "now": a stored alarm carries no opinion about today, and the
     * engine settles that on its first valid reading instead. */
    index = clock_alarm_add(e, (int)hour, (int)minute,
                            (enum clock_repeat)repeat, *p ? p : NULL, NULL);
    if (index < 0) {
        return -1;
    }
    if (!enabled) {
        clock_alarm_set_enabled(e, index, false);
    }
    return 0;
}

int clock_store_load(struct clock_engine *e)
{
    char path[STORE_PATH_MAX];
    char line[STORE_LINE_MAX];
    struct clock_engine work;
    FILE *f;
    int lines = 0;
    int rc = -1;

    if (!e || clock_store_path(path, sizeof(path)) != 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }

    /* Built up beside the real engine, so a file that turns out to be
     * damaged half way through cannot leave three of five alarms behind. */
    clock_engine_init(&work);

    if (!fgets(line, sizeof(line), f)) {
        goto out;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, CLOCK_STORE_MAGIC) != 0) {
        goto out;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        if (++lines > STORE_MAX_LINES) {
            goto out;
        }
        /* A line that filled the buffer without a newline was truncated, and
         * a truncated label is not the label that was saved. */
        if (len + 1 >= sizeof(line) && line[len - 1] != '\n') {
            goto out;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strncmp(line, "alarm ", 6) == 0) {
            if (parse_alarm(line + 6, &work) != 0) {
                goto out;
            }
        } else if (strncmp(line, "timer ", 6) == 0) {
            const char *p = line + 6;
            long seconds;

            if (parse_uint(p, &p, &seconds) != 0 || *p != '\0') {
                goto out;
            }
            /* Zero is a legal stored duration: it means none was ever set.
             * Anything the engine would refuse is a file we do not trust. */
            if (seconds > 0 && !clock_timer_set(&work, 0, 0, (int)seconds)) {
                goto out;
            }
        } else {
            goto out; /* an unknown keyword is a file we do not understand */
        }
    }
    if (ferror(f)) {
        goto out;
    }

    /* Only the persisted parts are taken across; the stopwatch, the ringing
     * state and every fired_day belong to this run. */
    memcpy(e->alarms, work.alarms, sizeof(e->alarms));
    e->alarm_count = work.alarm_count;
    e->timer = work.timer;
    rc = 0;
out:
    fclose(f);
    return rc;
}

/* ---- saving ------------------------------------------------------------ */

int clock_store_save(const struct clock_engine *e)
{
    char path[STORE_PATH_MAX];
    char tmp[STORE_PATH_MAX + 8];
    FILE *f;
    int i;

    if (!e || clock_store_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        return -1;
    }
    make_dirs(clock_store_dir());
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    fprintf(f, "%s\n", CLOCK_STORE_MAGIC);
    /* The duration that was set, never the remaining time of a countdown:
     * one is a setting, the other is elapsed time and does not survive. */
    fprintf(f, "timer %lld\n", (long long)(e->timer.duration_ms / 1000));
    for (i = 0; i < e->alarm_count; i++) {
        const struct clock_alarm *a = &e->alarms[i];
        const char *label = clock_label_is_storable(a->label) ? a->label : "";

        fprintf(f, "alarm %d %u %u %u %s\n", a->enabled ? 1 : 0,
                (unsigned)a->hour, (unsigned)a->minute, (unsigned)a->repeat,
                label);
    }
    if (ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    /* rename() is atomic on the same filesystem: a reader sees the previous
     * alarms or these, never a half-written set. An interrupted save leaves
     * only the temporary, which the next save overwrites. */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
