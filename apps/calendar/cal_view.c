/*
 * PocketCalendar's state. See cal_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cal_view.h"

#include <stdio.h>
#include <string.h>

/* What the selected line says when the selected day is also today. The grid
 * says it with an accent and a dot; this says it in words (DS section 2). */
#define TODAY_SUFFIX " - Today"

static void go_to(struct cal_view *v, const struct cal_date *d)
{
    v->view_year = d->year;
    v->view_month = d->month;
    v->have_selection = true;
    v->selected = *d;
}

void cal_view_init(struct cal_view *v, int64_t system_day)
{
    struct cal_date today;

    if (!v) {
        return;
    }
    memset(v, 0, sizeof(*v));
    v->view_year = CAL_FALLBACK_YEAR;
    v->view_month = CAL_FALLBACK_MONTH;
    if (cal_date_from_day(system_day, &today)) {
        v->have_today = true;
        v->today = today;
        go_to(v, &today);
    }
}

bool cal_view_set_system_day(struct cal_view *v, int64_t system_day)
{
    struct cal_date today;

    if (!v) {
        return false;
    }
    if (!cal_date_from_day(system_day, &today)) {
        /* The clock is not set, or has stopped being believable. Drop today
         * so no cell is marked; leave the month on screen and the selection
         * alone, because the user may be reading them. */
        if (!v->have_today) {
            return false;
        }
        v->have_today = false;
        memset(&v->today, 0, sizeof(v->today));
        return true;
    }
    if (!v->have_today) {
        /* The board has just learned the date. */
        v->have_today = true;
        v->today = today;
        go_to(v, &today);
        return true;
    }
    if (cal_date_equal(&v->today, &today)) {
        return false;
    }
    /* A new day. Only the mark moves. */
    v->today = today;
    return true;
}

void cal_view_prev_month(struct cal_view *v)
{
    if (v) {
        cal_month_add(&v->view_year, &v->view_month, -1);
    }
}

void cal_view_next_month(struct cal_view *v)
{
    if (v) {
        cal_month_add(&v->view_year, &v->view_month, 1);
    }
}

void cal_view_today(struct cal_view *v)
{
    if (!v || !v->have_today) {
        return;
    }
    go_to(v, &v->today);
}

void cal_view_select(struct cal_view *v, int day_of_month)
{
    if (!v) {
        return;
    }
    if (day_of_month < 1 ||
        day_of_month > cal_days_in_month(v->view_year, v->view_month)) {
        return;
    }
    v->have_selection = true;
    v->selected.year = v->view_year;
    v->selected.month = v->view_month;
    v->selected.day = day_of_month;
}

void cal_view_grid(const struct cal_view *v, struct cal_cell out[CAL_CELLS])
{
    uint8_t days[CAL_CELLS];
    int i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(struct cal_cell) * CAL_CELLS);
    if (!v) {
        return;
    }
    cal_month_grid(v->view_year, v->view_month, days);
    for (i = 0; i < CAL_CELLS; i++) {
        struct cal_date d;

        out[i].day = days[i];
        if (days[i] == 0) {
            continue;
        }
        d.year = v->view_year;
        d.month = v->view_month;
        d.day = days[i];
        /* have_today is checked first and on every cell: while the date is
         * not set there is no today anywhere, in this month or any other. */
        out[i].is_today = v->have_today && cal_date_equal(&v->today, &d);
        out[i].is_selected = v->have_selection && cal_date_equal(&v->selected, &d);
    }
}

void cal_view_month_text(const struct cal_view *v, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!v) {
        return;
    }
    cal_format_month(v->view_year, v->view_month, out, out_len);
}

void cal_view_selected_text(const struct cal_view *v, char *out, size_t out_len)
{
    size_t len;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!v || !v->have_selection) {
        return;
    }
    cal_format_date(&v->selected, out, out_len);
    if (!v->have_today || !cal_date_equal(&v->selected, &v->today)) {
        return;
    }
    len = strlen(out);
    if (len == 0 || len + sizeof(TODAY_SUFFIX) > out_len) {
        return;
    }
    memcpy(out + len, TODAY_SUFFIX, sizeof(TODAY_SUFFIX));
}

bool cal_view_date_set(const struct cal_view *v)
{
    return v && v->have_today;
}
