/*
 * PocketCalendar's state: the unset clock, the moment it is set, today
 * against the selection, and browsing across year boundaries.
 *
 * The system date is handed in, so the two states this app exists to get
 * right - a board that does not know the date, and the second it learns one -
 * are reached here without waiting for either.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cal_view.h"

#include <stdio.h>
#include <string.h>

/* 12 September 2026 is a Saturday; the grid for that month has one leading
 * blank, so the 12th sits in cell 12. */
#define DAY_2026_09_12 20260912LL
#define DAY_2026_09_13 20260913LL
#define DAY_2026_12_31 20261231LL

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

static void selected_is(const char *what, const struct cal_view *v, const char *want)
{
    char got[CAL_SELECTED_TEXT_MAX];

    cal_view_selected_text(v, got, sizeof(got));
    eq_str(what, got, want);
}

static void month_is(const char *what, const struct cal_view *v, const char *want)
{
    char got[CAL_MONTH_TEXT_MAX];

    cal_view_month_text(v, got, sizeof(got));
    eq_str(what, got, want);
}

static void view_at(const char *what, const struct cal_view *v, int year, int month)
{
    checks++;
    if (v->view_year != year || v->view_month != month) {
        failed++;
        printf("FAIL %s: showing %04d-%02d, want %04d-%02d\n", what, v->view_year,
               v->view_month, year, month);
    }
}

/* How many cells carry each mark, and which day does. */
static int marked_today(const struct cal_view *v, int *day)
{
    struct cal_cell g[CAL_CELLS];
    int i, n = 0;

    cal_view_grid(v, g);
    for (i = 0; i < CAL_CELLS; i++) {
        if (g[i].is_today) {
            n++;
            if (day) {
                *day = g[i].day;
            }
        }
    }
    return n;
}

static int marked_selected(const struct cal_view *v, int *day, int *cell)
{
    struct cal_cell g[CAL_CELLS];
    int i, n = 0;

    cal_view_grid(v, g);
    for (i = 0; i < CAL_CELLS; i++) {
        if (g[i].is_selected) {
            n++;
            if (day) {
                *day = g[i].day;
            }
            if (cell) {
                *cell = i;
            }
        }
    }
    return n;
}

int main(void)
{
    struct cal_view v;

    /* ---- a board that does not know the date ---- */
    cal_view_init(&v, -1);
    check("the date is not set", !cal_view_date_set(&v));
    view_at("the fallback month is shown", &v, CAL_FALLBACK_YEAR, CAL_FALLBACK_MONTH);
    month_is("and named", &v, "January 2024");
    eq_int("nothing is today", marked_today(&v, NULL), 0);
    eq_int("nothing is selected", marked_selected(&v, NULL, NULL), 0);
    selected_is("and no date is written", &v, "");

    /* The epoch is a real date, but it is not a time this board knows: the
     * shell screens it and hands over -1. Nothing here may turn that into a
     * day in 1970. */
    cal_view_init(&v, -1);
    {
        struct cal_cell g[CAL_CELLS];
        int i;
        int any = 0;

        cal_view_grid(&v, g);
        for (i = 0; i < CAL_CELLS; i++) {
            if (g[i].is_today || g[i].is_selected) {
                any = 1;
            }
        }
        check("an unset clock marks no cell at all", !any);
    }
    check("a date that is not a date leaves it unset",
          !cal_view_set_system_day(&v, 0) && !cal_view_date_set(&v));
    check("nor does 29 February in a common year set it",
          !cal_view_set_system_day(&v, 20260229LL) && !cal_view_date_set(&v));

    /* ---- browsing stays usable while the date is unset ---- */
    cal_view_init(&v, -1);
    cal_view_next_month(&v);
    view_at("next month works with no date", &v, 2024, 2);
    cal_view_prev_month(&v);
    cal_view_prev_month(&v);
    view_at("and so does stepping back over a year boundary", &v, 2023, 12);
    cal_view_select(&v, 25);
    eq_int("a day can be selected with no date set", marked_selected(&v, NULL, NULL), 1);
    selected_is("and it is written out", &v, "Monday 25 December 2023");
    check("which still does not make the date set", !cal_view_date_set(&v));
    eq_int("and still nothing is today", marked_today(&v, NULL), 0);
    cal_view_today(&v);
    view_at("Today does nothing when there is no today", &v, 2023, 12);

    /* No fake today anywhere, over a long browse in both directions. */
    cal_view_init(&v, -1);
    {
        int i;
        int any = 0;

        for (i = 0; i < 18; i++) {
            cal_view_next_month(&v);
            if (marked_today(&v, NULL) != 0) {
                any = 1;
            }
        }
        for (i = 0; i < 36; i++) {
            cal_view_prev_month(&v);
            if (marked_today(&v, NULL) != 0) {
                any = 1;
            }
        }
        check("no month browsed has a today while the date is unset", !any);
        view_at("and the browse ended where the arithmetic says", &v, 2022, 7);
    }

    /* ---- a board that knows the date ---- */
    cal_view_init(&v, DAY_2026_09_12);
    check("the date is set", cal_view_date_set(&v));
    view_at("its own month is shown", &v, 2026, 9);
    month_is("and named", &v, "September 2026");
    {
        int day = 0;
        int cell = -1;

        eq_int("exactly one cell is today", marked_today(&v, &day), 1);
        eq_int("and it is the twelfth", day, 12);
        eq_int("exactly one cell is selected", marked_selected(&v, &day, &cell), 1);
        eq_int("the same day", day, 12);
        /* September 2026 starts on a Tuesday, so cell 0 is the blank Monday
         * and the 12th lands in cell 12. This is the Monday-first offset
         * arriving on screen. */
        eq_int("in the cell the Monday-first offset puts it in", cell, 12);
    }
    selected_is("the date is written with the today suffix", &v,
                "Saturday 12 September 2026 - Today");

    /* ---- the moment the clock is set: the ordinary path on this board ---- */
    cal_view_init(&v, -1);
    cal_view_next_month(&v);
    cal_view_next_month(&v); /* the user was browsing March 2024 */
    view_at("browsing before the clock is set", &v, 2024, 3);
    check("learning the date is a change", cal_view_set_system_day(&v, DAY_2026_09_12));
    check("the date is now set", cal_view_date_set(&v));
    view_at("and the view has moved to it", &v, 2026, 9);
    {
        int day = 0;

        eq_int("today is marked", marked_today(&v, &day), 1);
        eq_int("on the right day", day, 12);
        eq_int("and it is selected", marked_selected(&v, &day, NULL), 1);
        eq_int("likewise", day, 12);
    }
    check("the same date again is not a change",
          !cal_view_set_system_day(&v, DAY_2026_09_12));

    /* ---- and the other way ---- */
    check("losing the date is a change", cal_view_set_system_day(&v, -1));
    check("the date is not set", !cal_view_date_set(&v));
    eq_int("no cell is today", marked_today(&v, NULL), 0);
    view_at("the month is left where the user was", &v, 2026, 9);
    eq_int("the selection is left alone", marked_selected(&v, NULL, NULL), 1);
    selected_is("and written without the suffix, there being no today", &v,
                "Saturday 12 September 2026");
    check("staying unset is not a change", !cal_view_set_system_day(&v, -1));

    /* ---- midnight ---- */
    cal_view_init(&v, DAY_2026_09_12);
    check("a new day is a change", cal_view_set_system_day(&v, DAY_2026_09_13));
    {
        int day = 0;

        eq_int("today has moved", marked_today(&v, &day), 1);
        eq_int("to the thirteenth", day, 13);
        eq_int("the selection has not", marked_selected(&v, &day, NULL), 1);
        eq_int("it is still the twelfth", day, 12);
    }
    view_at("and the month is unchanged", &v, 2026, 9);
    selected_is("the selected day is no longer today, and stops saying so", &v,
                "Saturday 12 September 2026");

    /* Midnight across a year boundary moves today into a month that is not
     * the one on screen, which must leave the grid with no today in it. */
    cal_view_init(&v, DAY_2026_12_31);
    view_at("the last month of the year", &v, 2026, 12);
    check("into the new year", cal_view_set_system_day(&v, 20270101LL));
    eq_int("today is no longer in the month on screen", marked_today(&v, NULL), 0);
    eq_int("but the selection still is", marked_selected(&v, NULL, NULL), 1);
    cal_view_today(&v);
    view_at("Today follows it into January 2027", &v, 2027, 1);
    {
        int day = 0;

        eq_int("where it is marked", marked_today(&v, &day), 1);
        eq_int("on the first", day, 1);
    }
    selected_is("and selected", &v, "Friday 1 January 2027 - Today");

    /* ---- browsing with a date set: the selection does not follow ---- */
    cal_view_init(&v, DAY_2026_09_12);
    cal_view_next_month(&v);
    view_at("October", &v, 2026, 10);
    eq_int("today is not in this month", marked_today(&v, NULL), 0);
    eq_int("and neither is the selection", marked_selected(&v, NULL, NULL), 0);
    selected_is("but the selected date is still the one that was chosen", &v,
                "Saturday 12 September 2026 - Today");
    cal_view_prev_month(&v);
    view_at("back to September", &v, 2026, 9);
    eq_int("today is marked again", marked_today(&v, NULL), 1);
    eq_int("and the selection survived the trip", marked_selected(&v, NULL, NULL), 1);

    /* Out to December, over into January, and back. The selection is a date,
     * not a position, so there is no 31 January to clamp into February. */
    cal_view_init(&v, 20260131LL);
    selected_is("the 31st is selected", &v, "Saturday 31 January 2026 - Today");
    cal_view_next_month(&v);
    view_at("February", &v, 2026, 2);
    eq_int("February shows no selection", marked_selected(&v, NULL, NULL), 0);
    selected_is("and the 31st of January is still the selected date", &v,
                "Saturday 31 January 2026 - Today");
    cal_view_prev_month(&v);
    {
        int day = 0;

        eq_int("back in January it is marked again", marked_selected(&v, &day, NULL), 1);
        eq_int("undamaged", day, 31);
    }

    /* ---- stepping across both year boundaries ---- */
    cal_view_init(&v, DAY_2026_12_31);
    cal_view_next_month(&v);
    view_at("December steps into January of the next year", &v, 2027, 1);
    month_is("and says so", &v, "January 2027");
    cal_view_prev_month(&v);
    view_at("and back into December", &v, 2026, 12);
    cal_view_prev_month(&v);
    view_at("and on back into November", &v, 2026, 11);
    {
        int i;

        for (i = 0; i < 11; i++) {
            cal_view_prev_month(&v);
        }
        view_at("eleven more reaches December 2025", &v, 2025, 12);
        for (i = 0; i < 12; i++) {
            cal_view_next_month(&v);
        }
        view_at("and twelve forward returns to December 2026", &v, 2026, 12);
    }
    eq_int("today is back in view", marked_today(&v, NULL), 1);

    /* ---- selecting ---- */
    cal_view_init(&v, DAY_2026_09_12);
    cal_view_select(&v, 1);
    selected_is("the first of the month", &v, "Tuesday 1 September 2026");
    cal_view_select(&v, 30);
    selected_is("the last", &v, "Wednesday 30 September 2026");
    cal_view_select(&v, 31);
    selected_is("a day September does not have is ignored", &v,
                "Wednesday 30 September 2026");
    cal_view_select(&v, 0);
    selected_is("and so is day zero", &v, "Wednesday 30 September 2026");
    cal_view_select(&v, -3);
    selected_is("and a negative day", &v, "Wednesday 30 September 2026");
    /* An empty cell carries day 0, so tapping one cannot select anything -
     * this is the same rule the app relies on for its blank cells. */
    cal_view_next_month(&v);
    cal_view_select(&v, 31);
    selected_is("October does have a 31st", &v, "Saturday 31 October 2026");
    cal_view_next_month(&v);
    cal_view_select(&v, 31);
    selected_is("November does not", &v, "Saturday 31 October 2026");
    /* February, both kinds. */
    cal_view_init(&v, 20240210LL);
    cal_view_select(&v, 29);
    selected_is("29 February 2024 can be selected", &v, "Thursday 29 February 2024");
    cal_view_init(&v, 20260210LL);
    cal_view_select(&v, 29);
    selected_is("29 February 2026 cannot", &v, "Tuesday 10 February 2026 - Today");

    /* ---- Today ---- */
    cal_view_init(&v, DAY_2026_09_12);
    cal_view_select(&v, 1);
    {
        int i;

        for (i = 0; i < 7; i++) {
            cal_view_next_month(&v);
        }
    }
    view_at("far from today", &v, 2027, 4);
    cal_view_today(&v);
    view_at("Today comes back to its month", &v, 2026, 9);
    {
        int day = 0;

        eq_int("and selects it", marked_selected(&v, &day, NULL), 1);
        eq_int("on the right day", day, 12);
    }
    selected_is("saying so", &v, "Saturday 12 September 2026 - Today");

    /* ---- the grid holds the month, marks aside ---- */
    cal_view_init(&v, DAY_2026_09_12);
    {
        struct cal_cell g[CAL_CELLS];
        int i, days = 0, blanks = 0;

        cal_view_grid(&v, g);
        for (i = 0; i < CAL_CELLS; i++) {
            if (g[i].day) {
                days++;
            } else {
                blanks++;
                /* A blank cell is never marked: the app hangs the "no date
                 * here" rule on exactly this. */
                if (g[i].is_today || g[i].is_selected) {
                    failed++;
                    checks++;
                    printf("FAIL a blank cell carries a mark at %d\n", i);
                }
            }
        }
        eq_int("September has thirty days in the grid", days, 30);
        eq_int("and the rest are blank", blanks, CAL_CELLS - 30);
    }

    /* ---- the callers that pass nothing ---- */
    {
        char buf[CAL_SELECTED_TEXT_MAX];

        cal_view_init(NULL, DAY_2026_09_12);
        cal_view_prev_month(NULL);
        cal_view_next_month(NULL);
        cal_view_today(NULL);
        cal_view_select(NULL, 1);
        cal_view_grid(NULL, NULL);
        check("a NULL view is not set", !cal_view_date_set(NULL));
        check("and reports no change", !cal_view_set_system_day(NULL, DAY_2026_09_12));
        cal_view_month_text(NULL, buf, sizeof(buf));
        eq_str("and writes no month", buf, "");
        cal_view_selected_text(NULL, buf, sizeof(buf));
        eq_str("and no date", buf, "");
    }

    printf("cal_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
