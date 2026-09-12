/*
 * PocketCalendar date arithmetic: what day of the week a date falls on, how
 * long a month is, and where its days sit in a Monday-first grid.
 *
 * Pure. No LVGL, no I/O and - the point of the file - no clock of its own:
 * every date is handed in, so leap years, month lengths and the Monday-first
 * offsets can be tested for any year without the test suite knowing what day
 * it is (tests/calendar_lint.sh checks that nothing under apps/calendar reads
 * a clock).
 *
 * The weekday comes from integer arithmetic rather than from localtime_r.
 * The grid needs the weekday of the first of an arbitrary displayed month,
 * which may be years from today, and mktime on such a date is a timezone
 * question this app has no business asking: PocketClock already resolved the
 * local date, and this module only arranges it. tests/cal_date_test.c pins
 * the arithmetic to PocketClock's own weekday so the two cannot disagree.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETCALENDAR_DATE_H
#define POCKETCALENDAR_DATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A month grid is always seven columns by six rows. Six is the most any
 * month needs - a 31-day month that starts on a Sunday spans 37 cells - and
 * a fixed six means the grid does not change height as the user browses. */
#define CAL_COLS 7
#define CAL_ROWS 6
#define CAL_CELLS (CAL_COLS * CAL_ROWS)

/* Longest output of cal_format_date: "Wednesday 30 September 2026" is 27
 * bytes with its terminator, and a five-digit year would still fit. */
#define CAL_DATE_TEXT_MAX 40
/* Longest output of cal_format_month: "September 2026". */
#define CAL_MONTH_TEXT_MAX 24

/* The month browsed when the system date is not set. It is the month of
 * CLOCK_WALL_VALID_FROM, the earliest reading PocketOS counts as a real
 * time, so the calendar falls back to the floor of what is believable
 * rather than to 1970 or to a guess. tests/cal_date_test.c asserts the two
 * agree; this header does not include the clock's, because this app does not
 * depend on it. */
#define CAL_FALLBACK_YEAR 2024
#define CAL_FALLBACK_MONTH 1

struct cal_date {
    int year;
    int month; /* 1-12 */
    int day;   /* 1-31 */
};

/* The Gregorian rule in full: every fourth year, except centuries, except
 * every fourth century. 1900 and 2100 are not leap years; 2000 and 2024
 * are. */
bool cal_is_leap(int year);

/* 28, 29, 30 or 31. Returns 0 for a month outside 1-12, so a caller that
 * passes one through gets a length it cannot mistake for a real one. */
int cal_days_in_month(int year, int month);

/* True when year, month and day together name a day that exists - which is
 * what rejects 29 February in a common year and 31 April in any. */
bool cal_date_valid(const struct cal_date *d);

/* The weekday, Monday-first: 0 = Monday .. 6 = Sunday. This is the column a
 * date occupies, so it is deliberately not the 0 = Sunday that struct tm
 * uses. The date must be valid. */
int cal_weekday(const struct cal_date *d);

/* Move a year and month by delta months, in either direction and by any
 * amount. This is where December becomes January of the next year and
 * January becomes December of the previous one. */
void cal_month_add(int *year, int *month, int delta);

/* Split the local date PocketClock reports as one number, YYYYMMDD, into its
 * three parts. Returns false for a negative day - the shell's "the wall
 * clock is not set" - and for a number that does not name a real date, and
 * in both cases out is left alone. Nothing here invents a date. */
bool cal_date_from_day(int64_t yyyymmdd, struct cal_date *out);

/* True when the two name the same day. */
bool cal_date_equal(const struct cal_date *a, const struct cal_date *b);

/* Fill a Monday-first month grid: out[i] is the day of the month in cell i,
 * or 0 where the cell falls outside this month. Cell 0 is the Monday of the
 * week the first of the month falls in, so the leading zeros are exactly
 * cal_weekday(the first). Returns the number of rows the month actually
 * occupies (4 to 6), which is information, not layout: the grid is drawn
 * with CAL_ROWS rows whatever this says. A month outside 1-12 fills the grid
 * with zeros and returns 0. */
int cal_month_grid(int year, int month, uint8_t out[CAL_CELLS]);

/* "September". NULL for a month outside 1-12. */
const char *cal_month_name(int month);
/* "Mon" .. "Sun" for 0-6, the column order. NULL outside it. */
const char *cal_weekday_short(int weekday);
/* "Monday" .. "Sunday" for 0-6. NULL outside it. */
const char *cal_weekday_name(int weekday);

/* "Saturday 12 September 2026". Assembled rather than left to strftime: the
 * day is written without padding, which "%e" cannot do and "%-d" is a GNU
 * extension for, and this module has no business calling a time function
 * anyway. Writes "" for a date that is not valid. */
void cal_format_date(const struct cal_date *d, char *out, size_t out_len);
/* "September 2026". Writes "" for a month outside 1-12. */
void cal_format_month(int year, int month, char *out, size_t out_len);

#endif
