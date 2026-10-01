/*
 * PocketCalendar date arithmetic: leap years, month lengths, Monday-first
 * offsets and the year boundaries.
 *
 * The arithmetic is pure, so every case here is a fixed input and a fixed
 * answer. One test is not: the cross-check below runs the calendar's weekday
 * against PocketClock's own, for every day over fifty years, so the two
 * cannot disagree about what day a date is. It needs TZ=UTC, which the
 * Makefile sets.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cal_date.h"

#include "clock_engine.h"
#include "clock_time.h"

#include <stdio.h>
#include <string.h>

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

static void eq_int(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: got %d, want %d\n", what, got, want);
    }
}

static void eq_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static int weekday_of(int y, int m, int d)
{
    struct cal_date date = { y, m, d };

    return cal_weekday(&date);
}

static void date_text_is(const char *what, int y, int m, int d, const char *want)
{
    struct cal_date date = { y, m, d };
    char got[CAL_DATE_TEXT_MAX];

    cal_format_date(&date, got, sizeof(got));
    eq_str(what, got, want);
}

/* Every cell a month's grid uses, checked together: the leading blanks are
 * the first's weekday, the days run 1..length with no gap, and nothing
 * follows them. */
static void grid_is(const char *what, int y, int m, int want_lead, int want_len,
                    int want_rows)
{
    uint8_t g[CAL_CELLS];
    int rows = cal_month_grid(y, m, g);
    int lead = 0;
    int i;
    int ok = 1;

    while (lead < CAL_CELLS && g[lead] == 0) {
        lead++;
    }
    for (i = 0; i < want_len; i++) {
        if (g[want_lead + i] != (uint8_t)(i + 1)) {
            ok = 0;
        }
    }
    for (i = want_lead + want_len; i < CAL_CELLS; i++) {
        if (g[i] != 0) {
            ok = 0;
        }
    }
    checks++;
    if (lead != want_lead || rows != want_rows || !ok) {
        failed++;
        printf("FAIL %s: lead %d (want %d), rows %d (want %d), days %s\n", what,
               lead, want_lead, rows, want_rows, ok ? "ok" : "wrong");
    }
}

int main(void)
{
    /* ---- leap years: the whole Gregorian rule, not just the fours ---- */
    check("2024 is a leap year", cal_is_leap(2024));
    check("2023 is not", !cal_is_leap(2023));
    check("2026 is not", !cal_is_leap(2026));
    check("1900 is not a leap year, being a century", !cal_is_leap(1900));
    check("2100 is not either", !cal_is_leap(2100));
    check("2200 is not", !cal_is_leap(2200));
    check("2000 is, being a fourth century", cal_is_leap(2000));
    check("2400 is too", cal_is_leap(2400));
    check("1996 is", cal_is_leap(1996));

    /* ---- every month length, both Februaries ---- */
    eq_int("January has 31", cal_days_in_month(2026, 1), 31);
    eq_int("February has 28 in a common year", cal_days_in_month(2026, 2), 28);
    eq_int("February has 29 in a leap year", cal_days_in_month(2024, 2), 29);
    eq_int("February 2000 has 29", cal_days_in_month(2000, 2), 29);
    eq_int("February 1900 has 28", cal_days_in_month(1900, 2), 28);
    eq_int("February 2100 has 28", cal_days_in_month(2100, 2), 28);
    eq_int("March has 31", cal_days_in_month(2026, 3), 31);
    eq_int("April has 30", cal_days_in_month(2026, 4), 30);
    eq_int("May has 31", cal_days_in_month(2026, 5), 31);
    eq_int("June has 30", cal_days_in_month(2026, 6), 30);
    eq_int("July has 31", cal_days_in_month(2026, 7), 31);
    eq_int("August has 31", cal_days_in_month(2026, 8), 31);
    eq_int("September has 30", cal_days_in_month(2026, 9), 30);
    eq_int("October has 31", cal_days_in_month(2026, 10), 31);
    eq_int("November has 30", cal_days_in_month(2026, 11), 30);
    eq_int("December has 31", cal_days_in_month(2026, 12), 31);
    eq_int("month 0 has no length", cal_days_in_month(2026, 0), 0);
    eq_int("month 13 has no length", cal_days_in_month(2026, 13), 0);
    eq_int("a negative month has none", cal_days_in_month(2026, -1), 0);

    /* ---- a date that exists ---- */
    {
        struct cal_date d = { 2024, 2, 29 };

        check("29 February 2024 exists", cal_date_valid(&d));
        d.year = 2026;
        check("29 February 2026 does not", !cal_date_valid(&d));
        d.month = 4;
        d.day = 31;
        check("31 April does not", !cal_date_valid(&d));
        d.day = 30;
        check("30 April does", cal_date_valid(&d));
        d.day = 0;
        check("day 0 does not", !cal_date_valid(&d));
        d.day = 1;
        d.month = 13;
        check("month 13 does not", !cal_date_valid(&d));
        check("NULL does not", !cal_date_valid(NULL));
    }

    /* ---- weekday vectors, Monday-first (0 = Monday) ---- */
    eq_int("1970-01-01 was a Thursday", weekday_of(1970, 1, 1), 3);
    eq_int("1970-01-05 was a Monday", weekday_of(1970, 1, 5), 0);
    eq_int("2024-01-01 was a Monday", weekday_of(2024, 1, 1), 0);
    eq_int("2024-02-29 was a Thursday", weekday_of(2024, 2, 29), 3);
    eq_int("2000-02-29 was a Tuesday", weekday_of(2000, 2, 29), 1);
    eq_int("2026-09-12 is a Saturday", weekday_of(2026, 9, 12), 5);
    eq_int("2026-12-31 is a Thursday", weekday_of(2026, 12, 31), 3);
    eq_int("2027-01-01 is a Friday", weekday_of(2027, 1, 1), 4);
    eq_int("2027-02-28 is a Sunday", weekday_of(2027, 2, 28), 6);
    eq_int("2100-03-01 is a Monday", weekday_of(2100, 3, 1), 0);
    eq_int("1969-12-31 was a Wednesday", weekday_of(1969, 12, 31), 2);
    eq_int("1900-01-01 was a Monday", weekday_of(1900, 1, 1), 0);
    /* Every weekday is reachable, so the mapping cannot be off by a constant
     * and still pass the vectors above. */
    {
        int seen[7];
        int i;
        int all = 1;

        memset(seen, 0, sizeof(seen));
        for (i = 1; i <= 7; i++) {
            seen[weekday_of(2026, 6, i)] = 1;
        }
        for (i = 0; i < 7; i++) {
            if (!seen[i]) {
                all = 0;
            }
        }
        check("a week covers all seven columns", all);
    }

    /* ---- the same weekday PocketClock would give, for fifty years ----
     *
     * This is the one that matters. The calendar works the weekday out with
     * integer arithmetic and PocketClock gets it from localtime_r; if they
     * ever disagreed, the grid would put today in the wrong column. struct tm
     * counts from Sunday, this counts from Monday, which is the +6 below. */
    {
        int64_t epoch;
        int mismatches = 0;
        int days = 0;

        for (epoch = CLOCK_WALL_VALID_FROM; days < 20000; days++, epoch += 86400) {
            struct clock_wall w;
            struct cal_date d;

            clock_wall_from_epoch(&w, epoch);
            if (!w.valid || !cal_date_from_day(w.day, &d)) {
                mismatches++;
                continue;
            }
            if (cal_weekday(&d) != (w.wday + 6) % 7) {
                if (mismatches == 0) {
                    printf("     first mismatch at %lld: day %lld, cal %d, tm %d\n",
                           (long long)epoch, (long long)w.day, cal_weekday(&d), w.wday);
                }
                mismatches++;
            }
        }
        eq_int("the weekday agrees with PocketClock for 20000 days", mismatches, 0);
    }

    /* ---- the fallback month is the floor of what PocketOS believes ----
     *
     * CAL_FALLBACK_* is declared without including the clock's header, so
     * that the app does not depend on it. This is what keeps the two in
     * step. */
    {
        struct clock_wall w;
        struct cal_date d;

        clock_wall_from_epoch(&w, CLOCK_WALL_VALID_FROM);
        check("the validity floor is a valid reading", w.valid);
        check("and a date", cal_date_from_day(w.day, &d));
        eq_int("the fallback year is the floor's year", CAL_FALLBACK_YEAR, d.year);
        eq_int("the fallback month is the floor's month", CAL_FALLBACK_MONTH, d.month);
        clock_wall_from_epoch(&w, CLOCK_WALL_VALID_FROM - 1);
        check("a second earlier is not a time at all", !w.valid);
    }

    /* ---- stepping months, which is where years turn over ---- */
    {
        int y, m;

        y = 2026; m = 12;
        cal_month_add(&y, &m, 1);
        check("December steps into January of the next year", y == 2027 && m == 1);

        y = 2027; m = 1;
        cal_month_add(&y, &m, -1);
        check("January steps back into December of the previous one", y == 2026 && m == 12);

        y = 2026; m = 9;
        cal_month_add(&y, &m, 0);
        check("no step changes nothing", y == 2026 && m == 9);

        y = 2026; m = 9;
        cal_month_add(&y, &m, 12);
        check("+12 is the same month a year on", y == 2027 && m == 9);

        y = 2026; m = 9;
        cal_month_add(&y, &m, -12);
        check("-12 is the same month a year back", y == 2025 && m == 9);

        y = 2026; m = 9;
        cal_month_add(&y, &m, 25);
        check("+25 is two years and a month on", y == 2028 && m == 10);

        y = 2026; m = 9;
        cal_month_add(&y, &m, -25);
        check("-25 is two years and a month back", y == 2024 && m == 8);

        y = 2026; m = 1;
        cal_month_add(&y, &m, -1);
        check("January back one lands in December 2025", y == 2025 && m == 12);

        y = 2026; m = 1;
        cal_month_add(&y, &m, -13);
        check("January back thirteen lands in December 2024", y == 2024 && m == 12);

        y = 2026; m = 12;
        cal_month_add(&y, &m, 13);
        check("December forward thirteen lands in January 2028", y == 2028 && m == 1);

        /* Twelve steps out and twelve back is where an off-by-one in the
         * division shows up. */
        y = 2026; m = 3;
        {
            int i;

            for (i = 0; i < 12; i++) {
                cal_month_add(&y, &m, 1);
            }
            for (i = 0; i < 12; i++) {
                cal_month_add(&y, &m, -1);
            }
        }
        check("twelve forward and twelve back returns to the same month",
              y == 2026 && m == 3);

        y = 2026; m = 13;
        cal_month_add(&y, &m, 1);
        check("a month outside the range is refused", y == 2026 && m == 13);
    }

    /* ---- the local date as one number ---- */
    {
        struct cal_date d;

        check("a valid day splits", cal_date_from_day(20260912, &d));
        check("into its three parts", d.year == 2026 && d.month == 9 && d.day == 12);
        check("1 January 2024 splits", cal_date_from_day(20240101, &d));
        check("into its parts", d.year == 2024 && d.month == 1 && d.day == 1);
        check("the epoch's own date is a real date", cal_date_from_day(19700101, &d));
        check("an unset clock is refused", !cal_date_from_day(-1, &d));
        check("so is any negative", !cal_date_from_day(-20260912, &d));
        check("zero is not a date", !cal_date_from_day(0, &d));
        check("nor is 29 February in a common year", !cal_date_from_day(20260229, &d));
        check("29 February in a leap year is", cal_date_from_day(20240229, &d));
        check("month 13 is refused", !cal_date_from_day(20261301, &d));
        check("day 0 is refused", !cal_date_from_day(20260900, &d));
        check("day 31 of September is refused", !cal_date_from_day(20260931, &d));
        check("a NULL out is refused", !cal_date_from_day(20260912, NULL));
    }

    /* ---- the same day ---- */
    {
        struct cal_date a = { 2026, 9, 12 };
        struct cal_date b = { 2026, 9, 12 };

        check("two readings of one day are equal", cal_date_equal(&a, &b));
        b.day = 13;
        check("a different day is not", !cal_date_equal(&a, &b));
        b.day = 12; b.month = 10;
        check("a different month is not", !cal_date_equal(&a, &b));
        b.month = 9; b.year = 2027;
        check("a different year is not", !cal_date_equal(&a, &b));
        check("NULL is equal to nothing", !cal_date_equal(&a, NULL));
    }

    /* ---- Monday-first grids ---- */
    /* September 2026 starts on a Tuesday: one blank, 30 days, 5 rows. */
    grid_is("September 2026", 2026, 9, 1, 30, 5);
    /* February 2024: a leap February starting on a Thursday. */
    grid_is("February 2024", 2024, 2, 3, 29, 5);
    /* February 2027 starts on a Monday and has 28 days: four clean rows, the
     * shortest a month can be. */
    grid_is("February 2027", 2027, 2, 0, 28, 4);
    /* February 2026 starts on a Sunday: six blanks and 28 days. */
    grid_is("February 2026", 2026, 2, 6, 28, 5);
    /* November 2026: 30 days from a Sunday, six rows. */
    grid_is("November 2026", 2026, 11, 6, 30, 6);
    /* August 2026: 31 days from a Saturday. */
    grid_is("August 2026", 2026, 8, 5, 31, 6);
    /* A 31-day month that starts on a Sunday is the widest a month gets: six
     * leading blanks and 31 days is 37 cells, which is why the grid has six
     * rows and not five. */
    grid_is("March 2026 (31 days from a Sunday)", 2026, 3, 6, 31, 6);
    grid_is("January 2024 (the fallback month)", 2024, 1, 0, 31, 5);
    grid_is("December 2026", 2026, 12, 1, 31, 5);
    grid_is("January 2027", 2027, 1, 4, 31, 5);
    {
        uint8_t g[CAL_CELLS];
        int i;
        int zeroed = 1;

        eq_int("a month outside the range fills no rows", cal_month_grid(2026, 0, g), 0);
        for (i = 0; i < CAL_CELLS; i++) {
            if (g[i] != 0) {
                zeroed = 0;
            }
        }
        check("and leaves the grid empty", zeroed);
        eq_int("month 13 likewise", cal_month_grid(2026, 13, g), 0);
        eq_int("a NULL grid is refused", cal_month_grid(2026, 9, NULL), 0);
    }
    /* The leading blanks are the first's weekday, in every month of a year.
     * This is the rule the grid rests on, checked rather than assumed. */
    {
        int m;
        int wrong = 0;

        for (m = 1; m <= 12; m++) {
            struct cal_date first = { 2026, m, 1 };
            uint8_t g[CAL_CELLS];
            int lead = 0;

            cal_month_grid(2026, m, g);
            while (lead < CAL_CELLS && g[lead] == 0) {
                lead++;
            }
            if (lead != cal_weekday(&first)) {
                wrong++;
            }
        }
        eq_int("every month of 2026 starts in its first's column", wrong, 0);
    }
    /* And the grid never loses or invents a day, over a long stretch of
     * months that includes both kinds of February. */
    {
        int y, m;
        int wrong = 0;

        for (y = 2023; y <= 2030; y++) {
            for (m = 1; m <= 12; m++) {
                uint8_t g[CAL_CELLS];
                int i, count = 0;

                cal_month_grid(y, m, g);
                for (i = 0; i < CAL_CELLS; i++) {
                    if (g[i]) {
                        count++;
                    }
                }
                if (count != cal_days_in_month(y, m)) {
                    wrong++;
                }
            }
        }
        eq_int("ninety-six months all hold exactly their days", wrong, 0);
    }

    /* ---- names ---- */
    eq_str("month 1 is January", cal_month_name(1), "January");
    eq_str("month 9 is September", cal_month_name(9), "September");
    eq_str("month 12 is December", cal_month_name(12), "December");
    check("month 0 has no name", cal_month_name(0) == NULL);
    check("month 13 has no name", cal_month_name(13) == NULL);
    eq_str("column 0 is Mon", cal_weekday_short(0), "Mon");
    eq_str("column 5 is Sat", cal_weekday_short(5), "Sat");
    eq_str("column 6 is Sun", cal_weekday_short(6), "Sun");
    check("column 7 has no name", cal_weekday_short(7) == NULL);
    check("column -1 has no name", cal_weekday_short(-1) == NULL);
    eq_str("column 0 is Monday", cal_weekday_name(0), "Monday");
    eq_str("column 6 is Sunday", cal_weekday_name(6), "Sunday");
    check("column 7 has no long name", cal_weekday_name(7) == NULL);
    /* Three letters is what a 72 px column was laid out for. */
    {
        int i;
        int wrong = 0;

        for (i = 0; i < 7; i++) {
            if (strlen(cal_weekday_short(i)) != 3) {
                wrong++;
            }
        }
        eq_int("every column heading is three letters", wrong, 0);
    }

    /* ---- the date in words ---- */
    date_text_is("a two-digit day", 2026, 9, 12, "Saturday 12 September 2026");
    /* A single-digit day is written without a pad: strftime's "%e" would put
     * a space there and leave a gap in the line. */
    date_text_is("a single-digit day has no gap", 2024, 1, 1, "Monday 1 January 2024");
    date_text_is("a leap day", 2024, 2, 29, "Thursday 29 February 2024");
    date_text_is("the last day of a year", 2026, 12, 31, "Thursday 31 December 2026");
    date_text_is("the first of a year", 2027, 1, 1, "Friday 1 January 2027");
    date_text_is("a date that does not exist is not written", 2026, 2, 29, "");
    date_text_is("nor is a month outside the range", 2026, 13, 1, "");
    {
        char buf[CAL_DATE_TEXT_MAX];

        cal_format_date(NULL, buf, sizeof(buf));
        eq_str("NULL writes nothing", buf, "");
        /* The longest line the app can show has to fit the buffer it is
         * given, terminator and all. */
        {
            struct cal_date d = { 2026, 9, 30 };

            cal_format_date(&d, buf, sizeof(buf));
            eq_str("the longest date fits", buf, "Wednesday 30 September 2026");
        }
    }

    /* ---- the month in words ---- */
    {
        char buf[CAL_MONTH_TEXT_MAX];

        cal_format_month(2026, 9, buf, sizeof(buf));
        eq_str("the month and the year", buf, "September 2026");
        cal_format_month(2024, 1, buf, sizeof(buf));
        eq_str("the fallback month reads plainly", buf, "January 2024");
        cal_format_month(2026, 13, buf, sizeof(buf));
        eq_str("a month outside the range writes nothing", buf, "");
    }

    printf("cal_date_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
