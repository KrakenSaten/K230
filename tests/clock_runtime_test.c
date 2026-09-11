/*
 * The clock runtime: the one engine the shell owns, and the behaviour that
 * only exists because it is not the app's.
 *
 * The whole point of this module is that an alarm rings with PocketClock
 * shut, so that is what is tested here - no app, no LVGL, just the runtime
 * being stepped the way the shell's tick steps it, with the clock injected
 * so 07:30 arrives immediately.
 *
 * Runs against a temporary POCKETOS_STATE_DIR, so it never touches a real
 * store.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_runtime.h"

#include "clock_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int checks;
static char root[128];
static int ring_changes;

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

static void on_ring_change(void)
{
    ring_changes++;
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* A reading the shell's tick might have taken. */
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

static struct clock_now unset_at(int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.mono_ms = mono_ms;
    return n;
}

/* The shell's tick, repeated. Every one of these is a chance to get the
 * "fires once" rule wrong. */
static void tick(struct clock_now now, int times)
{
    int i;

    for (i = 0; i < times; i++) {
        clock_runtime_step_at(&now);
    }
}

static void fresh(void)
{
    clock_runtime_deinit();
    ring_changes = 0;
    clock_runtime_init(on_ring_change);
}

static void test_init_and_load(void)
{
    struct clock_engine *e;

    wipe();
    clock_runtime_deinit();
    check("an empty store is not a failure", clock_runtime_init(NULL) == 1);
    check("and gives an empty engine", clock_alarm_count(clock_runtime_engine()) == 0);
    check("with nothing ringing",
          clock_runtime_engine()->ringing == CLOCK_RING_NONE);
    check("and a clock already read", clock_runtime_now()->mono_ms > 0);

    /* What the shell does at boot: load what the app saved last time. */
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, "Wake up", NULL);
    clock_timer_set(e, 0, 2, 0);
    check("saving works", clock_runtime_save() == 0);

    clock_runtime_deinit();
    check("a second start loads them", clock_runtime_init(NULL) == 0);
    check("one alarm", clock_alarm_count(clock_runtime_engine()) == 1);
    check_str("the one that was saved",
              clock_alarm_at(clock_runtime_engine(), 0)->label, "Wake up");
    check("and the timer duration",
          clock_runtime_engine()->timer.duration_ms == 120000);

    /* init is idempotent: the shell calls it once, but the alert sheet is
     * built before it and must not be able to wipe the engine. */
    check("calling init again returns the same answer", clock_runtime_init(NULL) == 0);
    check("and keeps the alarms", clock_alarm_count(clock_runtime_engine()) == 1);
}

/* The behaviour this whole module exists for. */
static void test_fires_with_no_app(void)
{
    struct clock_engine *e;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, "Wake up", NULL);
    clock_runtime_save();

    /* The clock becomes real before the alarm is due, as it does on a board
     * whose time is set shortly after boot. */
    tick(at(20260911, 7, 0, 5, 1000), 5);
    check("nothing rings before its time",
          e->ringing == CLOCK_RING_NONE);
    check("and the shell has not been told anything", ring_changes == 0);

    /* Nobody has opened PocketClock. The shell keeps ticking. */
    tick(at(20260911, 7, 30, 5, 1800000), 60);
    check("the alarm rings with no app in sight", e->ringing == CLOCK_RING_ALARM);
    check("and the shell was told exactly once", ring_changes == 1);
    check("it is the right alarm", e->ringing_alarm == 0);

    /* Sixty more ticks of the same minute must not ring it again. */
    tick(at(20260911, 7, 30, 5, 1830000), 60);
    check("and stays the one ring", ring_changes == 1);

    /* Stop, the way the shell's alert does it. */
    clock_runtime_stop_ringing();
    check("stopping it stops it", e->ringing == CLOCK_RING_NONE);
    check("and told the shell", ring_changes == 2);
    tick(at(20260911, 7, 30, 5, 1860000), 60);
    tick(at(20260911, 7, 31, 5, 1900000), 60);
    tick(at(20260911, 23, 59, 5, 2000000), 60);
    check("an acknowledged alarm does not come back the same day",
          e->ringing == CLOCK_RING_NONE);
    check("and the shell heard nothing more", ring_changes == 2);

    tick(at(20260912, 7, 30, 6, 90000000), 5);
    check("it rings again the next day", e->ringing == CLOCK_RING_ALARM);
    check("once", ring_changes == 3);
}

static void test_once_survives_the_acknowledgement(void)
{
    struct clock_engine *e;
    struct clock_engine reloaded;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 6, 0, CLOCK_REPEAT_ONCE, "just once", NULL);
    clock_runtime_save();

    tick(at(20260911, 5, 0, 5, 1000), 2);
    tick(at(20260911, 6, 0, 5, 4600000), 10);
    check("a one-shot alarm rings", e->ringing == CLOCK_RING_ALARM);
    clock_runtime_stop_ringing();
    check("and switches itself off", !clock_alarm_at(e, 0)->enabled);

    /* That is a setting, and a setting that is not written down is lost at
     * the next power cut - which is how an alarm nobody set goes off. */
    clock_engine_init(&reloaded);
    check("acknowledging wrote it down", clock_store_load(&reloaded) == 0);
    check("and it is off on disk", !clock_alarm_at(&reloaded, 0)->enabled);
}

static void test_snooze_with_no_app(void)
{
    struct clock_engine *e;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    tick(at(20260911, 7, 0, 5, 1000), 2);
    tick(at(20260911, 7, 30, 5, 1800000), 2);
    check("ringing", e->ringing == CLOCK_RING_ALARM);
    clock_runtime_snooze();
    check("snoozing stops it", e->ringing == CLOCK_RING_NONE);
    check("and the shell was told", ring_changes == 2);

    tick(at(20260911, 7, 38, 5, 1800000 + CLOCK_SNOOZE_MS - 1000), 10);
    check("a snooze does not end early", e->ringing == CLOCK_RING_NONE);
    tick(at(20260911, 7, 39, 5, 1800000 + CLOCK_SNOOZE_MS), 10);
    check("and rings again when it is up", e->ringing == CLOCK_RING_ALARM);
    check("once", ring_changes == 3);
}

static void test_timer_with_no_app(void)
{
    struct clock_engine *e;
    struct clock_now now;

    wipe();
    fresh();
    e = clock_runtime_engine();
    check("a countdown is set", clock_timer_set(e, 0, 1, 30));
    now = unset_at(5000);
    check("and started", clock_timer_start(e, &now));

    /* PocketClock is closed from here on. The shell is the only thing
     * looking at the countdown. */
    tick(unset_at(5000 + 89000), 20);
    check("it has not finished", e->timer.state == CLOCK_TIMER_RUNNING);
    check("nothing is ringing", e->ringing == CLOCK_RING_NONE);

    tick(unset_at(5000 + 90000), 40);
    check("it finishes", e->timer.state == CLOCK_TIMER_EXPIRED);
    check("and rings", e->ringing == CLOCK_RING_TIMER);
    check("the shell was told once", ring_changes == 1);

    tick(unset_at(5000 + 200000), 40);
    check("and does not ring again", ring_changes == 1);

    clock_runtime_stop_ringing();
    check("stopping it stops it", e->ringing == CLOCK_RING_NONE);
    check("and returns it to idle", e->timer.state == CLOCK_TIMER_IDLE);
    check("with the duration it was set to", e->timer.remaining_ms == 90000);
    check("the shell was told", ring_changes == 2);

    /* A countdown is elapsed time on the monotonic clock, so it runs with
     * the wall clock never having been set at all - which is exactly the
     * state this board boots into. */
    check("none of that needed a wall clock", !clock_runtime_now()->wall.valid);
}

static void test_no_wall_clock_no_alarm(void)
{
    struct clock_engine *e;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    /* A board that booted with no RTC and nothing to set the time. The
     * shell ticks all day and the alarm must stay silent. */
    tick(unset_at(1000), 500);
    tick(unset_at(90000000), 500);
    check("no wall clock means no alarm", e->ringing == CLOCK_RING_NONE);
    check("and the shell was never told otherwise", ring_changes == 0);

    /* The time is set, to after the alarm. It did not ring while nobody
     * knew the time and it must not ring in a burst now. */
    tick(at(20260911, 9, 0, 5, 90001000), 20);
    check("and none of them go off when the time arrives",
          e->ringing == CLOCK_RING_NONE);
    check("still nothing to tell the shell", ring_changes == 0);
}

static void test_wall_clock_jumps(void)
{
    struct clock_engine *e;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    tick(at(20260911, 7, 0, 5, 1000), 5);
    /* Corrected forward past the alarm: late, but it is today's alarm. */
    tick(at(20260911, 8, 0, 5, 1100), 5);
    check("a forward jump rings it", e->ringing == CLOCK_RING_ALARM);
    clock_runtime_stop_ringing();

    /* And corrected back again: it has already rung today. */
    tick(at(20260911, 7, 0, 5, 1200), 5);
    tick(at(20260911, 7, 30, 5, 1300), 5);
    tick(at(20260911, 9, 0, 5, 1400), 5);
    check("a backward jump does not ring it twice", e->ringing == CLOCK_RING_NONE);
    check("the shell heard one ring and one stop", ring_changes == 2);
}

static void test_read_does_not_step(void)
{
    struct clock_engine *e;

    wipe();
    fresh();
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    tick(at(20260911, 7, 0, 5, 1000), 2);

    /* The app's refresh calls this ten times a second while it is open. If
     * it advanced the engine, an alarm would be decided by two callers and
     * "the shell steps and nobody else" would be a comment, not a rule. */
    clock_runtime_read();
    clock_runtime_read();
    check("a read leaves the engine where it was", e->ringing == CLOCK_RING_NONE);
    check("and tells the shell nothing", ring_changes == 0);
    /* The injected reading above was at mono 1000; a machine that has been
     * up for more than a second reads something else, so this says the read
     * really did replace it. */
    check("but it does take a new reading", clock_runtime_now()->mono_ms != 1000);
    check("a real one", clock_runtime_now()->mono_ms > 1000);
}

static void test_null_callback(void)
{
    struct clock_engine *e;

    wipe();
    clock_runtime_deinit();
    clock_runtime_init(NULL);
    e = clock_runtime_engine();
    clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);
    tick(at(20260911, 7, 0, 5, 1000), 2);
    tick(at(20260911, 7, 30, 5, 1800000), 2);
    check("it rings without anyone listening", e->ringing == CLOCK_RING_ALARM);
    clock_runtime_stop_ringing();
    check("and stops", e->ringing == CLOCK_RING_NONE);

    clock_runtime_step_at(NULL);
    check("a step with no reading changes nothing", e->ringing == CLOCK_RING_NONE);

    /* Stopping and snoozing when nothing is ringing must be harmless: the
     * alert's buttons can be pressed on the way out. */
    clock_runtime_stop_ringing();
    clock_runtime_snooze();
    check("and neither does stopping nothing", e->ringing == CLOCK_RING_NONE);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/pocketclock-runtime-%d", (int)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);

    test_init_and_load();
    test_fires_with_no_app();
    test_once_survives_the_acknowledgement();
    test_snooze_with_no_app();
    test_timer_with_no_app();
    test_no_wall_clock_no_alarm();
    test_wall_clock_jumps();
    test_read_does_not_step();
    test_null_callback();

    clock_runtime_deinit();
    wipe();
    printf("clock_runtime_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
