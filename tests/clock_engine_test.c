/*
 * PocketClock timekeeping, at every instant that matters and none of them
 * real.
 *
 * Nothing here sleeps. Both clocks are handed to the engine as numbers, so a
 * day, a midnight crossing, a clock correction and a nine-minute snooze all
 * take the same zero seconds to test. That is the point of the engine taking
 * a struct clock_now instead of reading one.
 *
 * The alert seam is checked at the end, in the same binary: it is the other
 * piece of PocketClock with no I/O in it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_alert.h"
#include "clock_engine.h"

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

/* A fake reading. day is a plain number here; the app builds it from the
 * local date, and the engine only ever compares two of them. */
static struct clock_now at(int64_t day, int hour, int minute, int wday,
                           int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.wall.valid = true;
    n.wall.day = day;
    n.wall.hour = hour;
    n.wall.minute = minute;
    n.wall.wday = wday;
    n.wall.epoch = CLOCK_WALL_VALID_FROM;
    n.mono_ms = mono_ms;
    return n;
}

/* A board that has just booted and does not know the time. */
static struct clock_now unset(int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.wall.valid = false;
    n.mono_ms = mono_ms;
    return n;
}

/* Step the engine the way the app does: over and over. If a rule depends on
 * how often it was called, this is what finds out. */
static void step_many(struct clock_engine *e, struct clock_now now, int times)
{
    int i;

    for (i = 0; i < times; i++) {
        clock_engine_step(e, &now);
    }
}

/* The engine deliberately suppresses alarms that were already due at the
 * moment the clock first became valid (test_validity below). So a test that
 * wants an alarm to ring has to let the clock become real before its time,
 * which is what the device does too: it is set once, early, and then runs. */
static void clock_becomes_valid(struct clock_engine *e, int64_t day, int hour,
                                int minute, int wday, int64_t mono_ms)
{
    struct clock_now n = at(day, hour, minute, wday, mono_ms);

    clock_engine_step(e, &n);
}

static void test_validity(void)
{
    struct clock_engine e;
    struct clock_now now;

    clock_engine_init(&e);
    check("a fresh engine has no alarms", clock_alarm_count(&e) == 0);
    check("and is not ringing", e.ringing == CLOCK_RING_NONE);

    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    /* A board with no RTC boots at the epoch. An alarm must not decide that
     * fifty-five years have gone by and every one of them is overdue. */
    now = unset(1000);
    step_many(&e, now, 20);
    check("nothing rings while the clock is not set", e.ringing == CLOCK_RING_NONE);
    check("and no alarm is marked as having rung",
          clock_alarm_at(&e, 0)->fired_day == CLOCK_DAY_NEVER);

    /* The time is set, to 09:00 - after the alarm. It did not ring while we
     * had no idea what time it was, and it must not ring now either. */
    now = at(20260911, 9, 0, 5, 2000);
    step_many(&e, now, 20);
    check("an alarm the clock jumped past does not fire retroactively",
          e.ringing == CLOCK_RING_NONE);

    /* The next day it is due again, and it does ring. */
    now = at(20260912, 7, 30, 6, 90000000);
    clock_engine_step(&e, &now);
    check("the same alarm fires the next day", e.ringing == CLOCK_RING_ALARM);
    check("and it is the right one", e.ringing_alarm == 0);
}

static void test_validity_before_the_alarm(void)
{
    struct clock_engine e;
    struct clock_now now;

    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    step_many(&e, unset(0), 5);
    /* The clock is set to 06:00, before the alarm: it is still ahead of us
     * today and stays armed. */
    now = at(20260911, 6, 0, 5, 1000);
    step_many(&e, now, 5);
    check("an alarm still ahead today survives the clock being set",
          e.ringing == CLOCK_RING_NONE);
    now = at(20260911, 7, 30, 5, 5400000);
    clock_engine_step(&e, &now);
    check("and rings at its time", e.ringing == CLOCK_RING_ALARM);
}

/* Setting an alarm for a time that has already gone by today means the next
 * one, not this instant. Nothing about a clock is more startling than an
 * alarm that goes off while your finger is still on the Add button. */
static void test_added_in_the_past(void)
{
    struct clock_engine e;
    struct clock_now now = at(20260911, 6, 39, 5, 1000);
    struct clock_now no_clock = unset(1000);

    clock_engine_init(&e);
    clock_becomes_valid(&e, 20260911, 6, 0, 5, 500);
    clock_alarm_add(&e, 6, 30, CLOCK_REPEAT_DAILY, "nine minutes ago", &now);
    step_many(&e, now, 20);
    check("an alarm added for earlier today does not go off at once",
          e.ringing == CLOCK_RING_NONE);
    check("it is marked as done for today",
          clock_alarm_at(&e, 0)->fired_day == 20260911);
    step_many(&e, at(20260912, 6, 30, 6, 90000000), 5);
    check("and rings the next day", e.ringing == CLOCK_RING_ALARM);

    /* The same minute counts as gone by: an alarm for right now is not what
     * anyone means either. */
    clock_engine_init(&e);
    clock_becomes_valid(&e, 20260911, 6, 0, 5, 500);
    clock_alarm_add(&e, 6, 39, CLOCK_REPEAT_DAILY, "right now", &now);
    step_many(&e, now, 20);
    check("nor does one added for this very minute",
          e.ringing == CLOCK_RING_NONE);

    /* Later today is a different matter: that one is armed. */
    clock_engine_init(&e);
    clock_becomes_valid(&e, 20260911, 6, 0, 5, 500);
    clock_alarm_add(&e, 6, 40, CLOCK_REPEAT_DAILY, "in a minute", &now);
    step_many(&e, now, 5);
    check("an alarm added for later today stays armed",
          e.ringing == CLOCK_RING_NONE);
    check("with nothing recorded against it",
          clock_alarm_at(&e, 0)->fired_day == CLOCK_DAY_NEVER);
    step_many(&e, at(20260911, 6, 40, 5, 61000), 5);
    check("and rings when it comes round", e.ringing == CLOCK_RING_ALARM);

    /* With no clock to compare against, nothing is suppressed: the engine
     * settles it on the first valid reading instead. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 6, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    check("no reference time leaves the alarm armed",
          clock_alarm_at(&e, 0)->fired_day == CLOCK_DAY_NEVER);
    clock_alarm_add(&e, 6, 30, CLOCK_REPEAT_DAILY, NULL, &no_clock);
    check("and neither does an unset one",
          clock_alarm_at(&e, 1)->fired_day == CLOCK_DAY_NEVER);
}

static void test_fires_once(void)
{
    struct clock_engine e;
    int i;

    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, "Wake", NULL);
    clock_becomes_valid(&e, 20260911, 0, 0, 5, 500);

    /* The app steps this ten times a second. Every one of those steps sees
     * 07:30, and exactly one of them may ring. */
    step_many(&e, at(20260911, 7, 30, 5, 1000), 600);
    check("the alarm is ringing", e.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&e, NULL);
    check("acknowledging stops it", e.ringing == CLOCK_RING_NONE);

    /* Still 07:30 for another thirty seconds, and still the same minute a
     * moment later. It must stay quiet. */
    for (i = 0; i < 300; i++) {
        struct clock_now n = at(20260911, 7, 30, 5, 2000 + i * 100);

        clock_engine_step(&e, &n);
    }
    step_many(&e, at(20260911, 7, 31, 5, 100000), 50);
    step_many(&e, at(20260911, 23, 59, 5, 200000), 50);
    check("and never rings twice in the same day", e.ringing == CLOCK_RING_NONE);
}

static void test_repeat(void)
{
    struct clock_engine e;

    /* Weekdays: Monday is wday 1, Saturday is 6, Sunday is 0. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 6, 0, CLOCK_REPEAT_WEEKDAYS, "Work", NULL);
    clock_becomes_valid(&e, 20260912, 5, 0, 6, 500);

    step_many(&e, at(20260912, 6, 0, 6, 1000), 10); /* Saturday */
    check("a weekday alarm is silent on Saturday", e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260913, 6, 0, 0, 2000), 10); /* Sunday */
    check("and on Sunday", e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260914, 6, 0, 1, 3000), 10); /* Monday */
    check("and rings on Monday", e.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&e, NULL);
    step_many(&e, at(20260918, 6, 0, 5, 4000), 10); /* Friday */
    check("and on Friday", e.ringing == CLOCK_RING_ALARM);
    check("a repeating alarm stays enabled", clock_alarm_at(&e, 0)->enabled);

    /* Once: it rings, and acknowledging switches it off for good. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 6, 0, CLOCK_REPEAT_ONCE, NULL, NULL);
    clock_becomes_valid(&e, 20260911, 5, 0, 5, 500);
    step_many(&e, at(20260911, 6, 0, 5, 1000), 10);
    check("a one-shot alarm rings", e.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&e, NULL);
    check("and switches itself off", !clock_alarm_at(&e, 0)->enabled);
    step_many(&e, at(20260912, 6, 0, 6, 90000000), 10);
    check("so it does not ring the next day", e.ringing == CLOCK_RING_NONE);
}

static void test_midnight(void)
{
    struct clock_engine e;

    clock_engine_init(&e);
    clock_alarm_add(&e, 0, 5, CLOCK_REPEAT_DAILY, "Late", NULL);
    clock_becomes_valid(&e, 20260911, 22, 0, 5, 500);

    /* 23:59 on the 11th: the alarm is at 00:05, which has not come round. */
    step_many(&e, at(20260911, 23, 59, 5, 1000), 10);
    check("an alarm just after midnight is quiet the evening before",
          e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260912, 0, 5, 6, 400000), 10);
    check("and rings once the date has rolled over", e.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&e, NULL);
    step_many(&e, at(20260912, 23, 59, 6, 900000), 10);
    check("and not again later that day", e.ringing == CLOCK_RING_NONE);
}

static void test_clock_jumps(void)
{
    struct clock_engine e;

    /* Forward: the clock is corrected from 07:00 to 08:00, past a 07:30
     * alarm that never got its minute. It is late, but it is today's alarm
     * and it rings. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    step_many(&e, at(20260911, 7, 0, 5, 1000), 5);
    check("quiet before its time", e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260911, 8, 0, 5, 2000), 5);
    check("a forward jump past the alarm still rings it",
          e.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&e, NULL);

    /* Backward, within the same day: the alarm has already rung today, so
     * passing 07:30 a second time must not ring it again. */
    step_many(&e, at(20260911, 7, 0, 5, 3000), 5);
    step_many(&e, at(20260911, 7, 30, 5, 4000), 5);
    step_many(&e, at(20260911, 9, 0, 5, 5000), 5);
    check("a backward jump does not ring an alarm that already rang today",
          e.ringing == CLOCK_RING_NONE);
}

static void test_snooze(void)
{
    struct clock_engine e;
    struct clock_now now;

    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    clock_becomes_valid(&e, 20260911, 7, 0, 5, 500);

    now = at(20260911, 7, 30, 5, 1000);
    clock_engine_step(&e, &now);
    check("ringing", e.ringing == CLOCK_RING_ALARM);
    clock_alarm_snooze(&e, &now);
    check("snoozing stops the ring", e.ringing == CLOCK_RING_NONE);
    check("and remembers which alarm", e.snooze_alarm == 0);

    /* One second short of nine minutes: still quiet. */
    step_many(&e, at(20260911, 7, 38, 5, 1000 + CLOCK_SNOOZE_MS - 1000), 10);
    check("a snooze does not end early", e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260911, 7, 39, 5, 1000 + CLOCK_SNOOZE_MS), 10);
    check("and rings again when it is up", e.ringing == CLOCK_RING_ALARM);
    check("still the same alarm", e.ringing_alarm == 0);

    /* It fires once, not on every step afterwards. */
    clock_alarm_acknowledge(&e, NULL);
    step_many(&e, at(20260911, 7, 45, 5, 1000 + CLOCK_SNOOZE_MS + 400000), 50);
    check("and the snooze does not come back", e.ringing == CLOCK_RING_NONE);
}

static void test_snooze_is_monotonic(void)
{
    struct clock_engine e;
    struct clock_now now;

    /* The wall clock is set by an hour in the middle of a snooze. A snooze
     * is a length of time, so it must be exactly as long as it was. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    clock_becomes_valid(&e, 20260911, 7, 0, 5, 500);
    now = at(20260911, 7, 30, 5, 1000);
    clock_engine_step(&e, &now);
    clock_alarm_snooze(&e, &now);

    step_many(&e, at(20260911, 8, 30, 5, 1000 + 60000), 10);
    check("an hour on the wall clock does not end a snooze",
          e.ringing == CLOCK_RING_NONE);
    step_many(&e, at(20260911, 8, 39, 5, 1000 + CLOCK_SNOOZE_MS), 10);
    check("nine monotonic minutes do", e.ringing == CLOCK_RING_ALARM);

    /* And a snooze survives the wall clock going away entirely, which is
     * what a time-sync correction to zero would look like. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    clock_becomes_valid(&e, 20260911, 7, 0, 5, 500);
    now = at(20260911, 7, 30, 5, 1000);
    clock_engine_step(&e, &now);
    clock_alarm_snooze(&e, &now);
    step_many(&e, unset(1000 + CLOCK_SNOOZE_MS), 5);
    check("a snooze rings even if the clock has been lost",
          e.ringing == CLOCK_RING_ALARM);
}

static void test_alarm_list(void)
{
    struct clock_engine e;

    clock_engine_init(&e);
    check("add returns the index", clock_alarm_add(&e, 6, 0, CLOCK_REPEAT_ONCE, "a", NULL) == 0);
    check("and the next one", clock_alarm_add(&e, 7, 0, CLOCK_REPEAT_ONCE, "b", NULL) == 1);
    check("and the next", clock_alarm_add(&e, 8, 0, CLOCK_REPEAT_ONCE, "c", NULL) == 2);
    check("three alarms", clock_alarm_count(&e) == 3);

    check("an impossible hour is refused",
          clock_alarm_add(&e, 24, 0, CLOCK_REPEAT_ONCE, NULL, NULL) == -1);
    check("an impossible minute is refused",
          clock_alarm_add(&e, 7, 60, CLOCK_REPEAT_ONCE, NULL, NULL) == -1);
    check("an unknown repeat is refused",
          clock_alarm_add(&e, 7, 0, (enum clock_repeat)9, NULL, NULL) == -1);
    check("nothing was added by any of those", clock_alarm_count(&e) == 3);

    /* Removing the middle one closes the gap, so no index means nothing. */
    check("remove", clock_alarm_remove(&e, 1));
    check("two alarms", clock_alarm_count(&e) == 2);
    check_str("the first is untouched", clock_alarm_at(&e, 0)->label, "a");
    check_str("and the third moved down", clock_alarm_at(&e, 1)->label, "c");
    check("an index past the end is not an alarm", clock_alarm_at(&e, 2) == NULL);
    check("and cannot be removed", !clock_alarm_remove(&e, 2));

    while (clock_alarm_count(&e) < CLOCK_MAX_ALARMS) {
        clock_alarm_add(&e, 9, 0, CLOCK_REPEAT_ONCE, NULL, NULL);
    }
    check("the list fills up", clock_alarm_count(&e) == CLOCK_MAX_ALARMS);
    check("and refuses one more",
          clock_alarm_add(&e, 10, 0, CLOCK_REPEAT_ONCE, NULL, NULL) == -1);

    /* A label longer than the field is cut, not written past. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 6, 0, CLOCK_REPEAT_ONCE,
                    "a label far longer than the field can hold", NULL);
    check("a long label is truncated",
          strlen(clock_alarm_at(&e, 0)->label) == CLOCK_LABEL_MAX - 1);
}

static void test_disable_and_remove_stop_the_ring(void)
{
    struct clock_engine e;
    struct clock_now now;

    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 0, CLOCK_REPEAT_DAILY, "one", NULL);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, "two", NULL);
    clock_becomes_valid(&e, 20260911, 6, 0, 5, 500);

    now = at(20260911, 7, 30, 5, 1000);
    clock_engine_step(&e, &now);
    check("the earlier alarm rings first", e.ringing_alarm == 0);
    clock_alarm_set_enabled(&e, 0, false);
    check("switching it off stops the ring", e.ringing == CLOCK_RING_NONE);
    clock_engine_step(&e, &now);
    check("and the other one takes its turn", e.ringing_alarm == 1);

    /* Snooze it, then delete it: the snooze must go with it. */
    clock_alarm_snooze(&e, &now);
    check("snoozed", e.snooze_alarm == 1);
    clock_alarm_remove(&e, 1);
    check("deleting the alarm cancels its snooze", e.snooze_alarm == -1);
    step_many(&e, at(20260911, 7, 40, 5, 1000 + CLOCK_SNOOZE_MS), 10);
    check("and nothing rings later", e.ringing == CLOCK_RING_NONE);

    /* Removing an alarm below a snoozing one must move the snooze with it,
     * not leave it pointing at whatever shifted into that slot. */
    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 0, CLOCK_REPEAT_DAILY, "zero", NULL);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, "one", NULL);
    clock_becomes_valid(&e, 20260911, 6, 0, 5, 500);
    now = at(20260911, 7, 30, 5, 1000);
    clock_engine_step(&e, &now);     /* index 0 rings */
    clock_alarm_acknowledge(&e, &now);
    clock_engine_step(&e, &now);     /* index 1 rings */
    check("the second alarm is ringing", e.ringing_alarm == 1);
    clock_alarm_snooze(&e, &now);
    clock_alarm_remove(&e, 0);
    check("the snooze follows the alarm down", e.snooze_alarm == 0);
    check_str("and it is still the same alarm", clock_alarm_at(&e, 0)->label, "one");
}

static void test_stopwatch(void)
{
    struct clock_engine e;
    struct clock_now now;
    struct clock_now later;

    clock_engine_init(&e);
    now = unset(0);
    check_i64("an idle stopwatch reads zero", clock_sw_elapsed_ms(&e, &now), 0);

    now = unset(10000);
    clock_sw_start(&e, &now);
    now = unset(10000 + 1234);
    check_i64("it counts monotonic milliseconds", clock_sw_elapsed_ms(&e, &now), 1234);

    /* The wall clock leaping about must not touch it. That is the whole
     * reason the stopwatch does not look at the wall clock at all. */
    now = at(20260911, 3, 0, 5, 10000 + 2000);
    check_i64("a wall-clock jump does not change the elapsed time",
              clock_sw_elapsed_ms(&e, &now), 2000);

    clock_sw_pause(&e, &now);
    later = unset(99999999);
    check_i64("pausing freezes it", clock_sw_elapsed_ms(&e, &later), 2000);
    clock_sw_pause(&e, &later);
    check_i64("and pausing a paused stopwatch changes nothing",
              clock_sw_elapsed_ms(&e, &later), 2000);

    now = unset(50000);
    clock_sw_start(&e, &now);
    now = unset(50000 + 500);
    check_i64("resuming adds to the total", clock_sw_elapsed_ms(&e, &now), 2500);
    clock_sw_start(&e, &now);
    check_i64("starting one that is already running does not restart it",
              clock_sw_elapsed_ms(&e, &now), 2500);

    check("a lap is recorded", clock_sw_lap(&e, &now));
    check_i64("at the elapsed time", e.sw.laps[0], 2500);
    now = unset(50000 + 1500);
    clock_sw_lap(&e, &now);
    check_i64("laps are cumulative, not deltas", e.sw.laps[1], 3500);
    check("two laps", e.sw.lap_count == 2);

    while (clock_sw_lap(&e, &now)) {
        /* fill it */
    }
    check("the lap list stops at its size", e.sw.lap_count == CLOCK_MAX_LAPS);

    clock_sw_reset(&e);
    check_i64("reset clears the time", clock_sw_elapsed_ms(&e, &now), 0);
    check("and the laps", e.sw.lap_count == 0);
    check("a lap while stopped is refused", !clock_sw_lap(&e, &now));

    /* Twenty-five days of milliseconds is past what 32 bits would hold. */
    clock_engine_init(&e);
    now = unset(0);
    clock_sw_start(&e, &now);
    now = unset(2200000000LL);
    check_i64("a long run does not wrap", clock_sw_elapsed_ms(&e, &now),
              2200000000LL);
}

static void test_timer(void)
{
    struct clock_engine e;
    struct clock_now now;
    struct clock_now zero;
    int i;

    clock_engine_init(&e);
    zero = unset(0);
    check("a zero duration is refused", !clock_timer_set(&e, 0, 0, 0));
    check("a negative one too", !clock_timer_set(&e, 0, -1, 0));
    check("and one longer than a day", !clock_timer_set(&e, 24, 0, 0));
    check("starting with nothing set is refused", !clock_timer_start(&e, &zero));

    check("ninety seconds", clock_timer_set(&e, 0, 1, 30));
    check_i64("is the remaining time before it starts",
              clock_timer_remaining_ms(&e, &zero), 90000);

    now = unset(5000);
    check("it starts", clock_timer_start(&e, &now));
    now = unset(5000 + 30000);
    check_i64("and counts down", clock_timer_remaining_ms(&e, &now), 60000);

    /* The wall clock is set backwards by a year. A countdown is elapsed
     * time; it does not care. */
    now = at(20250911, 12, 0, 4, 5000 + 60000);
    check_i64("a wall-clock jump does not move the deadline",
              clock_timer_remaining_ms(&e, &now), 30000);

    clock_timer_pause(&e, &now);
    now = unset(999999999);
    check_i64("pausing holds what was left",
              clock_timer_remaining_ms(&e, &now), 30000);
    clock_timer_start(&e, &now);
    now = unset(999999999 + 29999);
    check_i64("and resuming carries on from there",
              clock_timer_remaining_ms(&e, &now), 1);
    check("still running", e.timer.state == CLOCK_TIMER_RUNNING);
    check("and not ringing yet", e.ringing == CLOCK_RING_NONE);

    /* Step it far past the deadline, many times. It expires once. */
    now = unset(999999999 + 40000);
    for (i = 0; i < 200; i++) {
        clock_engine_step(&e, &now);
    }
    check("it expires", e.timer.state == CLOCK_TIMER_EXPIRED);
    check("and rings", e.ringing == CLOCK_RING_TIMER);
    check_i64("with nothing left", clock_timer_remaining_ms(&e, &now), 0);

    clock_timer_acknowledge(&e);
    check("acknowledging stops the ring", e.ringing == CLOCK_RING_NONE);
    check("and returns it to idle", e.timer.state == CLOCK_TIMER_IDLE);
    check_i64("with the duration it was set to",
              clock_timer_remaining_ms(&e, &now), 90000);
    for (i = 0; i < 200; i++) {
        clock_engine_step(&e, &now);
    }
    check("and it does not ring again", e.ringing == CLOCK_RING_NONE);

    /* Cancelling mid-run puts the whole duration back. */
    clock_timer_start(&e, &now);
    now = unset(999999999 + 80000);
    clock_timer_cancel(&e);
    check("cancel returns it to idle", e.timer.state == CLOCK_TIMER_IDLE);
    check_i64("with the full duration", clock_timer_remaining_ms(&e, &now), 90000);

    /* A long one: twenty-three hours, in milliseconds, past a 32-bit range. */
    clock_engine_init(&e);
    check("a long duration is accepted", clock_timer_set(&e, 23, 0, 0));
    now = unset(1000);
    clock_timer_start(&e, &now);
    now = unset(1000 + 82000000LL);
    check_i64("and counts down correctly", clock_timer_remaining_ms(&e, &now),
              23LL * 3600000 - 82000000LL);
}

static void test_one_ring_at_a_time(void)
{
    struct clock_engine e;
    struct clock_now now;

    clock_engine_init(&e);
    clock_alarm_add(&e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    clock_timer_set(&e, 0, 0, 10);
    now = at(20260911, 7, 0, 5, 1000);
    clock_engine_step(&e, &now);
    clock_timer_start(&e, &now);

    /* Both come due in the same step. The timer claims the ring; the alarm
     * does not vanish, it simply waits its turn. */
    now = at(20260911, 7, 30, 5, 1000 + 10000);
    clock_engine_step(&e, &now);
    check("the timer rings", e.ringing == CLOCK_RING_TIMER);
    clock_timer_acknowledge(&e);
    clock_engine_step(&e, &now);
    check("and the alarm follows", e.ringing == CLOCK_RING_ALARM);
}

static void test_formatting(void)
{
    char buf[16];

    clock_format_elapsed(0, buf, sizeof(buf));
    check_str("zero elapsed", buf, "00:00.00");
    clock_format_elapsed(1234, buf, sizeof(buf));
    check_str("a second and a bit", buf, "00:01.23");
    clock_format_elapsed(59999, buf, sizeof(buf));
    check_str("just short of a minute", buf, "00:59.99");
    clock_format_elapsed(60000, buf, sizeof(buf));
    check_str("a minute", buf, "01:00.00");
    clock_format_elapsed(3600000, buf, sizeof(buf));
    check_str("an hour grows the field", buf, "1:00:00.00");
    clock_format_elapsed(-5, buf, sizeof(buf));
    check_str("a negative elapsed time reads zero", buf, "00:00.00");

    clock_format_remaining(0, buf, sizeof(buf));
    check_str("nothing left", buf, "00:00");
    clock_format_remaining(1, buf, sizeof(buf));
    check_str("a millisecond left still shows a second", buf, "00:01");
    clock_format_remaining(1000, buf, sizeof(buf));
    check_str("one second", buf, "00:01");
    clock_format_remaining(1001, buf, sizeof(buf));
    check_str("and a whisker more is two", buf, "00:02");
    clock_format_remaining(90000, buf, sizeof(buf));
    check_str("a minute and a half", buf, "01:30");
    clock_format_remaining(3600000, buf, sizeof(buf));
    check_str("an hour", buf, "1:00:00");

    check_str("once", clock_repeat_name(CLOCK_REPEAT_ONCE), "Once");
    check_str("daily", clock_repeat_name(CLOCK_REPEAT_DAILY), "Daily");
    check_str("weekdays", clock_repeat_name(CLOCK_REPEAT_WEEKDAYS), "Weekdays");
}

static void test_nulls(void)
{
    struct clock_engine e;
    struct clock_now now = unset(0);

    /* Every entry point takes a pointer the app could get wrong. None of
     * them may be the one that crashes the shell. */
    clock_engine_init(NULL);
    clock_engine_step(NULL, &now);
    clock_engine_init(&e);
    clock_engine_step(&e, NULL);
    check("no alarm from a null step", e.ringing == CLOCK_RING_NONE);
    check("count of nothing", clock_alarm_count(NULL) == 0);
    check("no alarm at any index of nothing", clock_alarm_at(NULL, 0) == NULL);
    check("nothing to add to",
          clock_alarm_add(NULL, 7, 0, CLOCK_REPEAT_ONCE, NULL, &now) == -1);
    check("nothing to remove from", !clock_alarm_remove(NULL, 0));
    check("nothing to enable", !clock_alarm_set_enabled(NULL, 0, true));
    clock_alarm_acknowledge(NULL, &now);
    clock_alarm_snooze(NULL, &now);
    clock_sw_start(NULL, &now);
    clock_sw_pause(NULL, &now);
    clock_sw_reset(NULL);
    check("no lap without a stopwatch", !clock_sw_lap(NULL, &now));
    check_i64("no elapsed time either", clock_sw_elapsed_ms(NULL, &now), 0);
    check("no timer to set", !clock_timer_set(NULL, 0, 1, 0));
    check("none to start", !clock_timer_start(NULL, &now));
    clock_timer_pause(NULL, &now);
    clock_timer_cancel(NULL);
    clock_timer_acknowledge(NULL);
    check_i64("and nothing remaining", clock_timer_remaining_ms(NULL, &now), 0);
    clock_format_elapsed(0, NULL, 16);
    clock_format_remaining(0, NULL, 16);
    check("and the engine is still sane", clock_alarm_count(&e) == 0);
}

/* ---- the alert seam ---------------------------------------------------- */

static int fake_begins;
static int fake_ends;
static enum clock_alert_kind fake_kind;

static void fake_begin(enum clock_alert_kind kind)
{
    fake_begins++;
    fake_kind = kind;
}

static void fake_end(void)
{
    fake_ends++;
}

static const struct clock_alert_backend fake = {
    .name = "fake",
    .channels = CLOCK_ALERT_SOUND | CLOCK_ALERT_VISUAL,
    .why = "a test",
    .begin = fake_begin,
    .end = fake_end,
};

static void test_alert(void)
{
    /* The shipped backend states what this board can do, and does not claim
     * a buzzer or an amplifier it has not got (clock_alert.h). */
    clock_alert_set_backend(NULL);
    check_str("the default backend is the screen", clock_alert_backend_name(),
              "screen");
    check("which is the only channel it has",
          clock_alert_channels() == CLOCK_ALERT_VISUAL);
    check("no sound is claimed", !(clock_alert_channels() & CLOCK_ALERT_SOUND));
    check("no haptics either", !(clock_alert_channels() & CLOCK_ALERT_HAPTIC));
    check("and it says why in words", strlen(clock_alert_why()) > 20);

    clock_alert_set_backend(&fake);
    check("a backend can be installed", clock_alert_channels() & CLOCK_ALERT_SOUND);
    check("nothing is alerting yet", !clock_alert_active());

    clock_alert_begin(CLOCK_ALERT_TIMER);
    check("beginning starts it", clock_alert_active());
    check("once", fake_begins == 1);
    check("and passes the kind through", fake_kind == CLOCK_ALERT_TIMER);

    /* The engine is stepped ten times a second while something rings, and
     * the view calls begin from that path. It must not restart the alert on
     * every one of those. */
    clock_alert_begin(CLOCK_ALERT_TIMER);
    clock_alert_begin(CLOCK_ALERT_ALARM);
    check("beginning again does nothing", fake_begins == 1);

    clock_alert_end();
    check("ending stops it", !clock_alert_active());
    check("once", fake_ends == 1);
    clock_alert_end();
    check("and ending again does nothing", fake_ends == 1);

    /* Swapping the backend out from under a running alert ends it first,
     * so the old one is never left holding something. */
    clock_alert_begin(CLOCK_ALERT_ALARM);
    clock_alert_set_backend(NULL);
    check("changing backend ends the alert", fake_ends == 2);
    check("and nothing is active", !clock_alert_active());
}

int main(void)
{
    test_validity();
    test_validity_before_the_alarm();
    test_added_in_the_past();
    test_fires_once();
    test_repeat();
    test_midnight();
    test_clock_jumps();
    test_snooze();
    test_snooze_is_monotonic();
    test_alarm_list();
    test_disable_and_remove_stop_the_ring();
    test_stopwatch();
    test_timer();
    test_one_ring_at_a_time();
    test_formatting();
    test_nulls();
    test_alert();
    printf("clock_engine_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
