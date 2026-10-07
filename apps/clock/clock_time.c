/*
 * Reading the clocks. See clock_time.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void clock_wall_from_epoch(struct clock_wall *w, int64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm;

    if (!w) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->epoch = epoch;
    if (epoch < CLOCK_WALL_VALID_FROM || !localtime_r(&t, &tm)) {
        w->valid = false;
        return;
    }
    w->valid = true;
    w->hour = tm.tm_hour;
    w->minute = tm.tm_min;
    w->second = tm.tm_sec;
    w->wday = tm.tm_wday;
    /* The local date as one number, YYYYMMDD. The engine only ever asks
     * whether two readings fall on the same local day, and this answers that
     * with no timezone arithmetic and no drift across a DST boundary - where
     * counting 86400-second blocks from the epoch would get it wrong. */
    w->day = (int64_t)(tm.tm_year + 1900) * 10000 +
             (int64_t)(tm.tm_mon + 1) * 100 + (int64_t)tm.tm_mday;
}

void clock_now_read(struct clock_now *now)
{
    struct timespec ts;

    if (!now) {
        return;
    }
    memset(now, 0, sizeof(*now));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        now->mono_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        clock_wall_from_epoch(&now->wall, (int64_t)ts.tv_sec);
    } else {
        /* No readable wall clock is the same situation as an unset one, and
         * is reported the same way rather than guessed at. */
        clock_wall_from_epoch(&now->wall, 0);
    }
}

void clock_format_date(const struct clock_wall *w, char *out, size_t out_len)
{
    time_t t;
    struct tm tm;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!w || !w->valid) {
        return;
    }
    t = (time_t)w->epoch;
    if (!localtime_r(&t, &tm)) {
        return;
    }
    /* Assembled rather than left to one strftime: "%e" pads a single-digit
     * day with a space, so the first of the month would come out with a gap
     * in it, and the "%-d" that would fix that is a GNU extension this
     * target's C library may not have. */
    {
        char weekday[24];
        char month[24];

        if (strftime(weekday, sizeof(weekday), "%A", &tm) == 0 ||
            strftime(month, sizeof(month), "%B", &tm) == 0) {
            return;
        }
        snprintf(out, out_len, "%s %d %s", weekday, tm.tm_mday, month);
    }
}

void clock_format_wall(const struct clock_wall *w, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!w || !w->valid) {
        /* Never a plausible-looking time the board does not actually know. */
        snprintf(out, out_len, "--:--");
        return;
    }
    clock_format_hm(w->hour, w->minute, out, out_len);
}

void clock_format_hm(int hour, int minute, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%02d:%02d", hour, minute);
}
