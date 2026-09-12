/*
 * PocketCalendar's state: which month is on screen, which day is selected,
 * and whether the board knows what day it is at all.
 *
 * Pure. No LVGL, no I/O and no clock - the system date is handed in, which
 * is what lets the two states that matter most here be tested without
 * waiting for either: a board that does not know the date, and the moment
 * it learns it.
 *
 * THE RULE THIS FILE EXISTS FOR. This hardware has no clock that survives a
 * power cut (docs/hardware/T-DISPLAY-K230.md), so every boot starts at the
 * epoch and jumps to the real date only when SNTP or an operator sets it.
 * A calendar that trusted the wall clock blindly would mark 1 January 1970
 * as today. So nothing is today until the shell says the date is valid, and
 * when it later becomes valid the view moves to it by itself.
 *
 * WHAT IS NOT HERE. Nothing is stored: there are no events to keep and the
 * selection is not worth a file. Opening the app lands on today, which is
 * the right answer every time (tests/calendar_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETCALENDAR_VIEW_H
#define POCKETCALENDAR_VIEW_H

#include "cal_date.h"

/* One cell of the drawn month. The two marks are separate because they mean
 * different things and are drawn differently: today is the date the board
 * is on, the selection is the date the user tapped, and one cell can be
 * both. */
struct cal_cell {
    uint8_t day;      /* day of the month, or 0 for a cell outside it */
    bool is_today;
    bool is_selected;
};

struct cal_view {
    int view_year;
    int view_month;             /* the month on screen, 1-12 */
    bool have_today;            /* false when the system date is not set */
    struct cal_date today;      /* only meaningful when have_today */
    bool have_selection;
    struct cal_date selected;   /* only meaningful when have_selection */
};

/* Start the view from the system date, as the shell reports it: a local
 * date as YYYYMMDD, or negative when the wall clock is not set. A valid
 * date opens on its own month with itself selected; an invalid one opens on
 * CAL_FALLBACK_YEAR/MONTH with nothing selected and nothing marked today. */
void cal_view_init(struct cal_view *v, int64_t system_day);

/* The once-a-second update. Returns true when something the screen shows
 * changed, so the app repaints on a change and not on a tick.
 *
 *   not set -> set    the date is learned: the view moves to its month and
 *                     selects it. This is the ordinary path on this board,
 *                     not an edge case - the clock is usually set some
 *                     seconds after boot.
 *   set -> not set    today is forgotten and no cell is marked; the month
 *                     on screen and the selection are left where they are
 *                     rather than yanked out from under the user.
 *   a new day         today moves. The view and the selection do not: the
 *                     selection is a date the user chose, and midnight is
 *                     not a reason to change it. */
bool cal_view_set_system_day(struct cal_view *v, int64_t system_day);

/* Browse. The selection does not follow: it is a date, not a position, so
 * stepping the month leaves it where it is and there is no 31 January to
 * clamp into February. Both work whether or not the system date is set. */
void cal_view_prev_month(struct cal_view *v);
void cal_view_next_month(struct cal_view *v);

/* Back to today, and select it. Does nothing when the system date is not
 * set - there is no today to go to, and the button is disabled to say so. */
void cal_view_today(struct cal_view *v);

/* Select a day of the month on screen. Ignores a day the displayed month
 * does not have, so an empty cell cannot select anything. Allowed while the
 * system date is not set: browsing stays usable. */
void cal_view_select(struct cal_view *v, int day_of_month);

/* The drawn month, CAL_CELLS cells, Monday-first. Every is_today is false
 * while the system date is not set, whatever month is being browsed. */
void cal_view_grid(const struct cal_view *v, struct cal_cell out[CAL_CELLS]);

/* "September 2026", the month on screen. */
void cal_view_month_text(const struct cal_view *v, char *out, size_t out_len);
/* "Saturday 12 September 2026", with " - Today" appended when the selected
 * day is today: the marks in the grid are a colour and an outline, and
 * DS v0.1 section 2 does not let colour carry meaning by itself. "" when
 * nothing is selected. */
void cal_view_selected_text(const struct cal_view *v, char *out, size_t out_len);
/* Room for the longest cal_view_selected_text, which is a date plus the
 * suffix. */
#define CAL_SELECTED_TEXT_MAX (CAL_DATE_TEXT_MAX + 12)

/* Whether the board knows the date. False puts the "Date not set" notice up
 * and disables Today. */
bool cal_view_date_set(const struct cal_view *v);

#endif
