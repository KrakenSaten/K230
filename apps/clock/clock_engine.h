/*
 * PocketClock timekeeping: alarms, stopwatch, timer, and what counts as
 * knowing the time at all.
 *
 * Pure. No LVGL, no I/O, no syscalls - every clock reading is handed in, so
 * every rule below can be tested without waiting for a real second to pass
 * (tests/clock_lint.sh).
 *
 * THE TWO CLOCKS ARE NOT INTERCHANGEABLE, and keeping them apart is most of
 * the work here:
 *
 *   - Alarms are wall-clock. They mean "07:30", which is a statement about
 *     the world, so they must follow the wall clock when it is corrected.
 *   - The stopwatch and the timer are monotonic. They mean "ninety seconds
 *     from when I pressed start", which is a statement about elapsed time,
 *     and must be immune to the wall clock moving.
 *
 * On this hardware that distinction has teeth. The K230 has no
 * battery-backed RTC (docs/hardware/T-DISPLAY-K230.md: "no RTC"), so every
 * boot starts at the epoch and jumps to the real time only if something sets
 * it. An alarm that trusted the wall clock blindly would fire fifty-five
 * years late on the first tick after boot.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETCLOCK_ENGINE_H
#define POCKETCLOCK_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_MAX_ALARMS 8
#define CLOCK_LABEL_MAX 24
#define CLOCK_MAX_LAPS 20

/* Wall-clock readings before this are not a time, they are a boot with no
 * RTC. 2024-01-01T00:00:00Z; any plausible real time is far above it, and
 * the epoch this hardware starts from is far below. */
#define CLOCK_WALL_VALID_FROM 1704067200LL

/* Snooze is nine minutes, the interval every bedside clock has used since
 * the 1950s, and it is deliberately not configurable. It counts on the
 * monotonic clock so that correcting the wall clock cannot lengthen or
 * shorten a snooze already running. */
#define CLOCK_SNOOZE_MS (9 * 60 * 1000)

/* The longest countdown the timer accepts, one second short of a day. A
 * longer wait is an alarm, which is what the wall clock is for. */
#define CLOCK_TIMER_MAX_SECONDS 86399

enum clock_repeat {
    CLOCK_REPEAT_ONCE = 0,
    CLOCK_REPEAT_DAILY,
    CLOCK_REPEAT_WEEKDAYS /* Monday to Friday */
};
#define CLOCK_REPEAT_COUNT 3

enum clock_sw_state {
    CLOCK_SW_IDLE = 0,
    CLOCK_SW_RUNNING,
    CLOCK_SW_PAUSED
};

enum clock_timer_state {
    CLOCK_TIMER_IDLE = 0,
    CLOCK_TIMER_RUNNING,
    CLOCK_TIMER_PAUSED,
    CLOCK_TIMER_EXPIRED
};

/* What is ringing, if anything. */
enum clock_ring {
    CLOCK_RING_NONE = 0,
    CLOCK_RING_ALARM,
    CLOCK_RING_TIMER
};

/* The local wall clock, already broken down by the caller. The engine does
 * no timezone arithmetic: clock_time.c calls localtime_r and hands the
 * result here, which keeps every rule below testable without a TZ database. */
struct clock_wall {
    bool valid;      /* false when the wall clock is not a real time */
    int64_t epoch;   /* seconds since 1970, UTC */
    int64_t day;     /* local date as one number; compared for equality only */
    int hour;        /* 0-23 local */
    int minute;      /* 0-59 local */
    int second;      /* 0-59 local */
    int wday;        /* 0 = Sunday .. 6 = Saturday, local */
};

/* One reading of both clocks, taken together. */
struct clock_now {
    struct clock_wall wall;
    int64_t mono_ms; /* monotonic milliseconds; only differences matter */
};

/* No real local date is negative, so this cannot collide with one. */
#define CLOCK_DAY_NEVER (-1)

struct clock_alarm {
    bool enabled;
    uint8_t hour;   /* 0-23 */
    uint8_t minute; /* 0-59 */
    uint8_t repeat; /* enum clock_repeat */
    char label[CLOCK_LABEL_MAX];
    /* The local day this alarm last fired on, so one alarm fires once a day
     * however often the engine is stepped. CLOCK_DAY_NEVER means never. */
    int64_t fired_day;
    /* Monotonic ms at which this alarm's snooze is up, 0 when it is not
     * snoozed. Each alarm keeps its own, so snoozing one never costs another
     * its snooze, and a snooze that is up while something else is ringing
     * stays here until the ring is free (DS §18.6). */
    int64_t snooze_until;
};

struct clock_stopwatch {
    uint8_t state;          /* enum clock_sw_state */
    int64_t started_mono;   /* when the current run began */
    int64_t accumulated_ms; /* everything before the current run */
    int64_t laps[CLOCK_MAX_LAPS]; /* cumulative elapsed at each lap */
    int lap_count;
};

struct clock_timer {
    uint8_t state;            /* enum clock_timer_state */
    int64_t duration_ms;      /* what was set */
    int64_t deadline_mono;    /* while running */
    int64_t remaining_ms;     /* while idle or paused, and 0 at expiry */
};

struct clock_engine {
    /* Dense: alarms 0..alarm_count-1 are the alarms, in the order they were
     * added, and removing one closes the gap. The list is short and every
     * view walks it on every refresh, so an index that means nothing would
     * have to be filtered out in each of them instead of here, once. */
    struct clock_alarm alarms[CLOCK_MAX_ALARMS];
    int alarm_count;

    struct clock_stopwatch sw;
    struct clock_timer timer;

    uint8_t ringing;      /* enum clock_ring */
    int ringing_alarm;    /* index, when ringing an alarm; else -1 */

    bool wall_was_valid;  /* to notice the moment the clock becomes real */
};

/* ---- lifecycle --------------------------------------------------------- */

void clock_engine_init(struct clock_engine *e);

/* Advance the engine to this reading. Call it as often as the UI likes: the
 * result depends on the clocks, never on how many times it was called. */
void clock_engine_step(struct clock_engine *e, const struct clock_now *now);

/* ---- alarms ------------------------------------------------------------ */

/* Appends. Returns the index, or -1 when the list is full or the time is out
 * of range. label may be NULL.
 *
 * now is the reference for "has this time already gone by today". An alarm
 * set for 06:30 at 06:39 means tomorrow morning, not nine minutes ago, so it
 * is marked as done for today and arms itself for the next occurrence. Pass
 * NULL only when there is no meaningful today - loading from disk, where the
 * engine works it out on its first valid reading instead. */
int clock_alarm_add(struct clock_engine *e, int hour, int minute,
                    enum clock_repeat repeat, const char *label,
                    const struct clock_now *now);
/* Removes and closes the gap, so later indices move down by one. A snooze
 * the alarm had goes with it. */
bool clock_alarm_remove(struct clock_engine *e, int index);
/* Switch an alarm on or off. Off stops it ringing and cancels its snooze,
 * and no other alarm's.
 *
 * On is setting it, so now is the reference exactly as it is for
 * clock_alarm_add: an alarm switched on at or after its minute today means
 * its next occurrence and does not ring on the spot. Only the off-to-on
 * change counts. Pass NULL when there is no meaningful today. */
bool clock_alarm_set_enabled(struct clock_engine *e, int index, bool enabled,
                             const struct clock_now *now);
int clock_alarm_count(const struct clock_engine *e);
const struct clock_alarm *clock_alarm_at(const struct clock_engine *e, int index);

/* Stop the ringing alarm and cancel its snooze, if it had one; any other
 * alarm's snooze carries on. A one-shot alarm also switches itself off,
 * because "once" has happened. */
void clock_alarm_acknowledge(struct clock_engine *e, const struct clock_now *now);
/* Stop the ringing alarm for CLOCK_SNOOZE_MS, on a snooze of its own. Does
 * nothing unless an alarm is ringing. */
void clock_alarm_snooze(struct clock_engine *e, const struct clock_now *now);

/* ---- stopwatch --------------------------------------------------------- */

void clock_sw_start(struct clock_engine *e, const struct clock_now *now);
void clock_sw_pause(struct clock_engine *e, const struct clock_now *now);
void clock_sw_reset(struct clock_engine *e);
/* Records the elapsed time at this moment. False when the lap list is full
 * or the stopwatch is not running. */
bool clock_sw_lap(struct clock_engine *e, const struct clock_now *now);
int64_t clock_sw_elapsed_ms(const struct clock_engine *e, const struct clock_now *now);

/* ---- timer ------------------------------------------------------------- */

/* Arms the timer. False for a duration of zero or less. */
bool clock_timer_set(struct clock_engine *e, int hours, int minutes, int seconds);
bool clock_timer_start(struct clock_engine *e, const struct clock_now *now);
void clock_timer_pause(struct clock_engine *e, const struct clock_now *now);
void clock_timer_cancel(struct clock_engine *e);
int64_t clock_timer_remaining_ms(const struct clock_engine *e,
                                 const struct clock_now *now);
/* Dismiss the expired state. */
void clock_timer_acknowledge(struct clock_engine *e);

/* ---- formatting helpers (pure, and used by the views) ------------------ */

/* "MM:SS.hh", or "H:MM:SS.hh" once an hour has passed. out needs 16 bytes. */
void clock_format_elapsed(int64_t ms, char *out, size_t out_len);
/* "MM:SS" for a countdown, "H:MM:SS" once there is an hour on it. 16 bytes. */
void clock_format_remaining(int64_t ms, char *out, size_t out_len);
/* "Once", "Daily", "Weekdays". */
const char *clock_repeat_name(enum clock_repeat repeat);

#endif
