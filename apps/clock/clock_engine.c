/*
 * PocketClock timekeeping. See clock_engine.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_engine.h"

#include <stdio.h>
#include <string.h>

void clock_engine_init(struct clock_engine *e)
{
    int i;

    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
    for (i = 0; i < CLOCK_MAX_ALARMS; i++) {
        e->alarms[i].fired_day = CLOCK_DAY_NEVER;
    }
    e->ringing = CLOCK_RING_NONE;
    e->ringing_alarm = -1;
    e->snooze_alarm = -1;
}

/* ---- alarms ------------------------------------------------------------ */

int clock_alarm_count(const struct clock_engine *e)
{
    return e ? e->alarm_count : 0;
}

const struct clock_alarm *clock_alarm_at(const struct clock_engine *e, int index)
{
    if (!e || index < 0 || index >= e->alarm_count) {
        return NULL;
    }
    return &e->alarms[index];
}

/* Minutes since local midnight, for comparing an alarm with the clock. */
static int minutes_of(int hour, int minute)
{
    return hour * 60 + minute;
}

int clock_alarm_add(struct clock_engine *e, int hour, int minute,
                    enum clock_repeat repeat, const char *label,
                    const struct clock_now *now)
{
    struct clock_alarm *a;
    int index;

    if (!e || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    if ((int)repeat < 0 || (int)repeat >= CLOCK_REPEAT_COUNT) {
        return -1;
    }
    if (e->alarm_count >= CLOCK_MAX_ALARMS) {
        return -1;
    }
    index = e->alarm_count++;
    a = &e->alarms[index];
    memset(a, 0, sizeof(*a));
    a->enabled = true;
    a->hour = (uint8_t)hour;
    a->minute = (uint8_t)minute;
    a->repeat = (uint8_t)repeat;
    /* A new alarm has never fired, whatever the alarm that used to occupy
     * this slot had done - unless its time has already gone by today, in
     * which case the owner meant the next one. Setting 06:30 at 06:39 must
     * not go off in the same breath as the Add button. */
    a->fired_day = CLOCK_DAY_NEVER;
    if (now && now->wall.valid &&
        minutes_of(hour, minute) <= minutes_of(now->wall.hour, now->wall.minute)) {
        a->fired_day = now->wall.day;
    }
    if (label) {
        snprintf(a->label, sizeof(a->label), "%s", label);
    }
    return index;
}

/* The alarm an index referred to has gone or moved down one; keep the two
 * references that carry an index pointing at the same alarm, or at nothing. */
static void reindex(int *ref, int removed)
{
    if (*ref == removed) {
        *ref = -1;
    } else if (*ref > removed) {
        (*ref)--;
    }
}

bool clock_alarm_remove(struct clock_engine *e, int index)
{
    int i;

    if (!e || index < 0 || index >= e->alarm_count) {
        return false;
    }
    for (i = index; i + 1 < e->alarm_count; i++) {
        e->alarms[i] = e->alarms[i + 1];
    }
    e->alarm_count--;
    memset(&e->alarms[e->alarm_count], 0, sizeof(e->alarms[0]));
    e->alarms[e->alarm_count].fired_day = CLOCK_DAY_NEVER;

    reindex(&e->ringing_alarm, index);
    reindex(&e->snooze_alarm, index);
    if (e->ringing == CLOCK_RING_ALARM && e->ringing_alarm < 0) {
        e->ringing = CLOCK_RING_NONE;
    }
    if (e->snooze_alarm < 0) {
        e->snooze_until = 0;
    }
    return true;
}

bool clock_alarm_set_enabled(struct clock_engine *e, int index, bool enabled)
{
    if (!e || index < 0 || index >= e->alarm_count) {
        return false;
    }
    e->alarms[index].enabled = enabled;
    if (enabled) {
        return true;
    }
    /* Switching an alarm off must also stop it ringing, and must not leave a
     * snooze behind that would bring it back nine minutes later. */
    if (e->ringing == CLOCK_RING_ALARM && e->ringing_alarm == index) {
        e->ringing = CLOCK_RING_NONE;
        e->ringing_alarm = -1;
    }
    if (e->snooze_alarm == index) {
        e->snooze_alarm = -1;
        e->snooze_until = 0;
    }
    return true;
}

static bool repeat_covers_day(uint8_t repeat, int wday)
{
    switch (repeat) {
    case CLOCK_REPEAT_ONCE:
    case CLOCK_REPEAT_DAILY:
        return true;
    case CLOCK_REPEAT_WEEKDAYS:
        return wday >= 1 && wday <= 5; /* Monday to Friday */
    default:
        return false;
    }
}

void clock_alarm_acknowledge(struct clock_engine *e, const struct clock_now *now)
{
    int idx;

    (void)now;
    if (!e || e->ringing != CLOCK_RING_ALARM) {
        return;
    }
    idx = e->ringing_alarm;
    e->ringing = CLOCK_RING_NONE;
    e->ringing_alarm = -1;
    e->snooze_alarm = -1;
    e->snooze_until = 0;
    /* "Once" has now happened. */
    if (idx >= 0 && idx < e->alarm_count &&
        e->alarms[idx].repeat == CLOCK_REPEAT_ONCE) {
        e->alarms[idx].enabled = false;
    }
}

void clock_alarm_snooze(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->ringing != CLOCK_RING_ALARM) {
        return;
    }
    e->snooze_alarm = e->ringing_alarm;
    /* Monotonic, so correcting the wall clock cannot stretch or cut it. */
    e->snooze_until = now->mono_ms + CLOCK_SNOOZE_MS;
    e->ringing = CLOCK_RING_NONE;
    e->ringing_alarm = -1;
}

/* ---- stopwatch --------------------------------------------------------- */

int64_t clock_sw_elapsed_ms(const struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now) {
        return 0;
    }
    if (e->sw.state == CLOCK_SW_RUNNING) {
        int64_t delta = now->mono_ms - e->sw.started_mono;

        /* A monotonic clock cannot go backwards; if one ever appears to,
         * hold the total rather than show a time running in reverse. */
        if (delta < 0) {
            delta = 0;
        }
        return e->sw.accumulated_ms + delta;
    }
    return e->sw.accumulated_ms;
}

void clock_sw_start(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->sw.state == CLOCK_SW_RUNNING) {
        return;
    }
    e->sw.started_mono = now->mono_ms;
    e->sw.state = CLOCK_SW_RUNNING;
}

void clock_sw_pause(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->sw.state != CLOCK_SW_RUNNING) {
        return;
    }
    e->sw.accumulated_ms = clock_sw_elapsed_ms(e, now);
    e->sw.state = CLOCK_SW_PAUSED;
}

void clock_sw_reset(struct clock_engine *e)
{
    if (!e) {
        return;
    }
    memset(&e->sw, 0, sizeof(e->sw));
    e->sw.state = CLOCK_SW_IDLE;
}

bool clock_sw_lap(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->sw.state != CLOCK_SW_RUNNING) {
        return false;
    }
    if (e->sw.lap_count >= CLOCK_MAX_LAPS) {
        return false;
    }
    e->sw.laps[e->sw.lap_count++] = clock_sw_elapsed_ms(e, now);
    return true;
}

/* ---- timer ------------------------------------------------------------- */

bool clock_timer_set(struct clock_engine *e, int hours, int minutes, int seconds)
{
    int64_t ms;

    if (!e || hours < 0 || minutes < 0 || seconds < 0) {
        return false;
    }
    ms = (int64_t)hours * 3600 + (int64_t)minutes * 60 + seconds;
    if (ms <= 0 || ms > CLOCK_TIMER_MAX_SECONDS) {
        return false;
    }
    ms *= 1000;
    e->timer.duration_ms = ms;
    e->timer.remaining_ms = ms;
    e->timer.state = CLOCK_TIMER_IDLE;
    e->timer.deadline_mono = 0;
    if (e->ringing == CLOCK_RING_TIMER) {
        e->ringing = CLOCK_RING_NONE;
    }
    return true;
}

bool clock_timer_start(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->timer.state == CLOCK_TIMER_RUNNING) {
        return false;
    }
    if (e->timer.state == CLOCK_TIMER_EXPIRED || e->timer.remaining_ms <= 0) {
        return false;
    }
    /* The deadline is a monotonic instant, not a wall-clock timestamp: a
     * countdown is a length of time, and setting the clock must not move it. */
    e->timer.deadline_mono = now->mono_ms + e->timer.remaining_ms;
    e->timer.state = CLOCK_TIMER_RUNNING;
    return true;
}

int64_t clock_timer_remaining_ms(const struct clock_engine *e,
                                 const struct clock_now *now)
{
    int64_t left;

    if (!e || !now) {
        return 0;
    }
    if (e->timer.state != CLOCK_TIMER_RUNNING) {
        return e->timer.remaining_ms;
    }
    left = e->timer.deadline_mono - now->mono_ms;
    return left > 0 ? left : 0;
}

void clock_timer_pause(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now || e->timer.state != CLOCK_TIMER_RUNNING) {
        return;
    }
    e->timer.remaining_ms = clock_timer_remaining_ms(e, now);
    e->timer.state = CLOCK_TIMER_PAUSED;
}

void clock_timer_cancel(struct clock_engine *e)
{
    if (!e) {
        return;
    }
    if (e->ringing == CLOCK_RING_TIMER) {
        e->ringing = CLOCK_RING_NONE;
    }
    e->timer.state = CLOCK_TIMER_IDLE;
    e->timer.remaining_ms = e->timer.duration_ms;
    e->timer.deadline_mono = 0;
}

void clock_timer_acknowledge(struct clock_engine *e)
{
    if (!e || e->timer.state != CLOCK_TIMER_EXPIRED) {
        return;
    }
    if (e->ringing == CLOCK_RING_TIMER) {
        e->ringing = CLOCK_RING_NONE;
    }
    e->timer.state = CLOCK_TIMER_IDLE;
    e->timer.remaining_ms = e->timer.duration_ms;
}

/* ---- the step ---------------------------------------------------------- */

static void step_alarms(struct clock_engine *e, const struct clock_now *now)
{
    const struct clock_wall *w = &now->wall;
    int i;

    /* A snooze is monotonic, so it survives anything the wall clock does,
     * including never being set at all. It is handled before the validity
     * check for exactly that reason. */
    if (e->snooze_until != 0 && e->snooze_alarm >= 0 &&
        e->snooze_alarm < e->alarm_count && now->mono_ms >= e->snooze_until) {
        if (e->ringing == CLOCK_RING_NONE &&
            e->alarms[e->snooze_alarm].enabled) {
            e->ringing = CLOCK_RING_ALARM;
            e->ringing_alarm = e->snooze_alarm;
        }
        e->snooze_until = 0;
    }

    if (!w->valid) {
        /* Without a real wall clock an alarm time means nothing, so nothing
         * fires. Recording that we were invalid is what makes the transition
         * below possible. */
        e->wall_was_valid = false;
        return;
    }

    /* The moment the clock becomes real - which on this hardware is every
     * boot, once something sets the time - every alarm whose time already
     * passed today would otherwise go off at once. Those alarms did not
     * ring while we had no idea what time it was, so they are marked done
     * for today; alarms still ahead of us today stay armed. */
    if (!e->wall_was_valid) {
        int nowmin = minutes_of(w->hour, w->minute);

        for (i = 0; i < e->alarm_count; i++) {
            struct clock_alarm *a = &e->alarms[i];

            if (minutes_of(a->hour, a->minute) <= nowmin) {
                a->fired_day = w->day;
            }
        }
        e->wall_was_valid = true;
    }

    if (e->ringing != CLOCK_RING_NONE) {
        return; /* one thing rings at a time */
    }

    for (i = 0; i < e->alarm_count; i++) {
        struct clock_alarm *a = &e->alarms[i];

        if (!a->enabled || !repeat_covers_day(a->repeat, w->wday)) {
            continue;
        }
        if (a->fired_day == w->day) {
            continue; /* already rang today: this is what stops it ringing
                       * again on every step through the same minute */
        }
        if (minutes_of(w->hour, w->minute) < minutes_of(a->hour, a->minute)) {
            continue; /* not yet */
        }
        a->fired_day = w->day;
        e->ringing = CLOCK_RING_ALARM;
        e->ringing_alarm = i;
        return;
    }
}

static void step_timer(struct clock_engine *e, const struct clock_now *now)
{
    if (e->timer.state != CLOCK_TIMER_RUNNING) {
        return;
    }
    if (now->mono_ms < e->timer.deadline_mono) {
        return;
    }
    /* EXPIRED is a state of its own, so this happens once however often the
     * engine is stepped afterwards. */
    e->timer.state = CLOCK_TIMER_EXPIRED;
    e->timer.remaining_ms = 0;
    if (e->ringing == CLOCK_RING_NONE) {
        e->ringing = CLOCK_RING_TIMER;
        e->ringing_alarm = -1;
    }
}

void clock_engine_step(struct clock_engine *e, const struct clock_now *now)
{
    if (!e || !now) {
        return;
    }
    /* The timer first: a countdown the user is watching should not wait
     * behind an alarm that is about to claim the ringing slot. */
    step_timer(e, now);
    step_alarms(e, now);
}

/* ---- formatting -------------------------------------------------------- */

void clock_format_elapsed(int64_t ms, char *out, size_t out_len)
{
    int64_t total_cs;
    int64_t h, m, s, cs;

    if (!out || out_len == 0) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    total_cs = ms / 10;
    cs = total_cs % 100;
    s = (total_cs / 100) % 60;
    m = (total_cs / 6000) % 60;
    h = total_cs / 360000;
    if (h > 0) {
        snprintf(out, out_len, "%lld:%02lld:%02lld.%02lld", (long long)h,
                 (long long)m, (long long)s, (long long)cs);
    } else {
        snprintf(out, out_len, "%02lld:%02lld.%02lld", (long long)m,
                 (long long)s, (long long)cs);
    }
}

void clock_format_remaining(int64_t ms, char *out, size_t out_len)
{
    int64_t total_s;
    int64_t h, m, s;

    if (!out || out_len == 0) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    /* Rounded up, so a countdown shows "1" for the whole of its last second
     * and reaches "0" exactly when it is over rather than a second early. */
    total_s = (ms + 999) / 1000;
    s = total_s % 60;
    m = (total_s / 60) % 60;
    h = total_s / 3600;
    if (h > 0) {
        snprintf(out, out_len, "%lld:%02lld:%02lld", (long long)h,
                 (long long)m, (long long)s);
    } else {
        snprintf(out, out_len, "%02lld:%02lld", (long long)m, (long long)s);
    }
}

const char *clock_repeat_name(enum clock_repeat repeat)
{
    switch (repeat) {
    case CLOCK_REPEAT_DAILY:
        return "Daily";
    case CLOCK_REPEAT_WEEKDAYS:
        return "Weekdays";
    case CLOCK_REPEAT_ONCE:
    default:
        return "Once";
    }
}
