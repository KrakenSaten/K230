/*
 * Reading the clock: what counts as a real time, and what a boot with no RTC
 * looks like.
 *
 * Run under TZ=UTC (the Makefile sets it), so the local-time arithmetic has
 * one known answer.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock_time.h"

#include <stdio.h>
#include <string.h>

/* 2026-09-11T00:00:00Z, a Friday. Every epoch below is an offset from it. */
#define FRIDAY 1789084800LL

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want);
    }
}

static void check_i64(const char *what, int64_t got, int64_t want)
{
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: got %lld, want %lld\n", what, (long long)got,
               (long long)want);
    }
}

static void test_validity(void)
{
    struct clock_wall w;

    /* This is what the board actually shows after a power cycle: the epoch,
     * because there is no battery-backed clock to remember anything. */
    clock_wall_from_epoch(&w, 0);
    check("the epoch is not a time", !w.valid);
    clock_wall_from_epoch(&w, 16);
    check("nor is sixteen seconds after boot", !w.valid);
    clock_wall_from_epoch(&w, CLOCK_WALL_VALID_FROM - 1);
    check("nor the last second before the threshold", !w.valid);
    clock_wall_from_epoch(&w, CLOCK_WALL_VALID_FROM);
    check("the threshold itself is", w.valid);
    check_i64("and it is 2024-01-01", w.day, 20240101);
    check_i64("a Monday", w.wday, 1);
    check_i64("at midnight", w.hour, 0);

    /* A negative epoch is possible on a system whose clock was set wrong. */
    clock_wall_from_epoch(&w, -100000);
    check("a time before 1970 is not one either", !w.valid);
}

static void test_breakdown(void)
{
    struct clock_wall w;
    char buf[32];

    clock_wall_from_epoch(&w, FRIDAY + 14 * 3600 + 5 * 60 + 9);
    check("a real time is valid", w.valid);
    check_i64("hour", w.hour, 14);
    check_i64("minute", w.minute, 5);
    check_i64("second", w.second, 9);
    check_i64("weekday is Friday", w.wday, 5);
    check_i64("and the day is the local date", w.day, 20260911);
    check_i64("the epoch is kept as it came in", w.epoch,
              FRIDAY + 14 * 3600 + 5 * 60 + 9);

    clock_format_wall(&w, buf, sizeof(buf));
    check_str("the clock face", buf, "14:05");
    clock_format_date(&w, buf, sizeof(buf));
    check_str("and the date under it", buf, "Friday 11 September");
    /* A single-digit day must not arrive with a padding space in it. */
    clock_wall_from_epoch(&w, 1790812800LL); /* 2026-10-01T00:00:00Z */
    clock_format_date(&w, buf, sizeof(buf));
    check_str("the first of a month has no gap in it", buf, "Thursday 1 October");

    /* One second before midnight and one second after: the day number has to
     * move, because it is what stops an alarm ringing twice. */
    clock_wall_from_epoch(&w, FRIDAY + 86399);
    check_i64("the last second of the day", w.day, 20260911);
    check_i64("is still Friday", w.wday, 5);
    clock_wall_from_epoch(&w, FRIDAY + 86400);
    check_i64("the first of the next is a new day", w.day, 20260912);
    check_i64("and a new weekday", w.wday, 6);

    /* Month and year ends, where a naive day counter goes wrong. */
    clock_wall_from_epoch(&w, 1790812800LL); /* 2026-10-01T00:00:00Z */
    check_i64("the first of October", w.day, 20261001);
    clock_wall_from_epoch(&w, 1798761600LL); /* 2027-01-01T00:00:00Z */
    check_i64("and a new year", w.day, 20270101);
}

static void test_unset_never_shows_a_time(void)
{
    struct clock_wall w;
    char buf[32];

    clock_wall_from_epoch(&w, 12);
    clock_format_wall(&w, buf, sizeof(buf));
    check_str("an unset clock shows no time at all", buf, "--:--");
    check("and not a single digit of one", strpbrk(buf, "0123456789") == NULL);
    clock_format_date(&w, buf, sizeof(buf));
    check_str("and no date", buf, "");

    clock_format_wall(NULL, buf, sizeof(buf));
    check_str("no clock at all is the same as an unset one", buf, "--:--");
    clock_format_date(NULL, buf, sizeof(buf));
    check_str("with no date", buf, "");
}

static void test_hm(void)
{
    char buf[16];

    clock_format_hm(0, 0, buf, sizeof(buf));
    check_str("midnight", buf, "00:00");
    clock_format_hm(7, 5, buf, sizeof(buf));
    check_str("single digits are padded", buf, "07:05");
    clock_format_hm(23, 59, buf, sizeof(buf));
    check_str("and the last minute of the day", buf, "23:59");
}

static void test_now(void)
{
    struct clock_now a;
    struct clock_now b;

    /* No sleeping: two reads in a row, and only the properties that hold at
     * any instant are checked. */
    clock_now_read(&a);
    clock_now_read(&b);
    check("the monotonic clock is readable", a.mono_ms > 0);
    check("and never goes backwards", b.mono_ms >= a.mono_ms);
    check("validity follows the threshold and nothing else",
          a.wall.valid == (a.wall.epoch >= CLOCK_WALL_VALID_FROM));
    if (a.wall.valid) {
        check("a valid reading has a real hour", a.wall.hour >= 0 && a.wall.hour <= 23);
        check("a real minute", a.wall.minute >= 0 && a.wall.minute <= 59);
        check("and a day number that looks like a date", a.wall.day > 19700101);
    } else {
        /* A build host whose clock is not set is not a test failure, but the
         * fields still have to be safe to read. */
        check("an invalid reading carries no hour", a.wall.hour == 0);
        check("no minute", a.wall.minute == 0);
        check("and no day", a.wall.day == 0);
    }
    clock_now_read(NULL);
    check("reading into nothing does not crash", 1);
}

int main(void)
{
    test_validity();
    test_breakdown();
    test_unset_never_shows_a_time();
    test_hm();
    test_now();
    printf("clock_time_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
