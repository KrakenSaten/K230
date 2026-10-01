/*
 * PocketCalendar date arithmetic. See cal_date.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cal_date.h"

#include <stdio.h>
#include <string.h>

static const char *const month_names[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"
};

/* Monday-first, which is the column order. English rather than strftime's
 * locale: the names are part of the layout - "Wed" has to fit a 72 px
 * column - and a module that called strftime would be reading a clock. */
static const char *const weekday_names[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};
static const char *const weekday_shorts[7] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

static const int month_lengths[12] = { 31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31 };

static bool month_in_range(int month)
{
    return month >= 1 && month <= 12;
}

bool cal_is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int cal_days_in_month(int year, int month)
{
    if (!month_in_range(month)) {
        return 0;
    }
    if (month == 2 && cal_is_leap(year)) {
        return 29;
    }
    return month_lengths[month - 1];
}

bool cal_date_valid(const struct cal_date *d)
{
    int len;

    if (!d || !month_in_range(d->month)) {
        return false;
    }
    len = cal_days_in_month(d->year, d->month);
    return d->day >= 1 && d->day <= len;
}

/* Days from 1970-01-01 to the given date, proleptic Gregorian (Howard
 * Hinnant's days_from_civil). Integer only, correct for any year, and with
 * no 2038 edge and no table to fall out of date. */
static int64_t days_from_civil(int year, int month, int day)
{
    int64_t y = year;
    int64_t era;
    int64_t yoe, doy, doe;

    y -= (month <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                    /* 0..399 */
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1; /* 0..365 */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* 0..146096 */
    return era * 146097 + doe - 719468;
}

int cal_weekday(const struct cal_date *d)
{
    int64_t days;

    if (!cal_date_valid(d)) {
        return 0;
    }
    days = days_from_civil(d->year, d->month, d->day);
    /* 1970-01-01 was a Thursday, which is column 3 when Monday is column 0,
     * so the epoch's remainder is shifted by three. The double modulo keeps
     * dates before 1970 positive. */
    return (int)((((days % 7) + 7) % 7 + 3) % 7);
}

void cal_month_add(int *year, int *month, int delta)
{
    int64_t total;

    if (!year || !month || !month_in_range(*month)) {
        return;
    }
    /* Count in months from year 0 and divide back out, so the year carries
     * itself in both directions and by any distance. Flooring the division
     * is what makes a step back from January land in the previous year
     * rather than truncating towards zero. */
    total = (int64_t)*year * 12 + (*month - 1) + delta;
    *year = (int)((total >= 0 ? total : total - 11) / 12);
    *month = (int)(total - (int64_t)*year * 12) + 1;
}

bool cal_date_from_day(int64_t yyyymmdd, struct cal_date *out)
{
    struct cal_date d;

    if (!out || yyyymmdd < 0) {
        return false;
    }
    d.year = (int)(yyyymmdd / 10000);
    d.month = (int)((yyyymmdd / 100) % 100);
    d.day = (int)(yyyymmdd % 100);
    if (!cal_date_valid(&d)) {
        return false;
    }
    *out = d;
    return true;
}

bool cal_date_equal(const struct cal_date *a, const struct cal_date *b)
{
    if (!a || !b) {
        return false;
    }
    return a->year == b->year && a->month == b->month && a->day == b->day;
}

int cal_month_grid(int year, int month, uint8_t out[CAL_CELLS])
{
    struct cal_date first;
    int lead, len, i, used;

    if (!out) {
        return 0;
    }
    memset(out, 0, CAL_CELLS);
    if (!month_in_range(month)) {
        return 0;
    }
    first.year = year;
    first.month = month;
    first.day = 1;
    lead = cal_weekday(&first);
    len = cal_days_in_month(year, month);
    for (i = 0; i < len; i++) {
        out[lead + i] = (uint8_t)(i + 1);
    }
    used = lead + len;
    return (used + CAL_COLS - 1) / CAL_COLS;
}

const char *cal_month_name(int month)
{
    return month_in_range(month) ? month_names[month - 1] : NULL;
}

const char *cal_weekday_short(int weekday)
{
    return (weekday >= 0 && weekday < 7) ? weekday_shorts[weekday] : NULL;
}

const char *cal_weekday_name(int weekday)
{
    return (weekday >= 0 && weekday < 7) ? weekday_names[weekday] : NULL;
}

void cal_format_date(const struct cal_date *d, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!cal_date_valid(d)) {
        return;
    }
    snprintf(out, out_len, "%s %d %s %d", cal_weekday_name(cal_weekday(d)),
             d->day, cal_month_name(d->month), d->year);
}

void cal_format_month(int year, int month, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!month_in_range(month)) {
        return;
    }
    snprintf(out, out_len, "%s %d", cal_month_name(month), year);
}
