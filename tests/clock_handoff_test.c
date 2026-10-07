/*
 * The restart handoff: what a shell hands the shell that replaces it.
 *
 * An orientation change ends the process and not the boot (DS §21.2,
 * restart_in_place() in ui/shell/shell.c is an execv), and this file is the
 * codec and the rules that carry a running stopwatch, a running countdown
 * and a snooze across that seam. Both clocks are injected, so every instant
 * below is exact and nothing sleeps.
 *
 * The seam itself - a real process being replaced by a real process - is
 * tests/clock_restart_test.c. This one is the decisions.
 *
 * Runs against a temporary POCKETOS_RUNTIME_DIR, so it never touches the
 * real one.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock_store.h"

#include "clock_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int checks;
static char root[128];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* A reading of both clocks, the way the shell's tick takes one. */
static struct clock_now unset_at(int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.mono_ms = mono_ms;
    return n;
}

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

static int handoff_exists(void)
{
    char path[256];

    if (clock_handoff_path(path, sizeof(path)) != 0) {
        return 0;
    }
    return access(path, F_OK) == 0;
}

/* Write a handoff file by hand, for the files no writer of ours would
 * produce. */
static void plant(const char *body)
{
    char path[256];
    char dir[256];
    char cmd[512];
    FILE *f;

    snprintf(dir, sizeof(dir), "%s", clock_handoff_dir());
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0) {
        return;
    }
    if (clock_handoff_path(path, sizeof(path)) != 0) {
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fputs(body, f);
    fclose(f);
}

/* The engine as the next shell finds it before the handoff: initialised, and
 * with the alarms the settings file gave it. They are all at 07:00 and after,
 * so a step at 06:00 can make the wall clock real without any of them having
 * had their moment yet. */
static void with_alarms(struct clock_engine *e, int count)
{
    int i;

    clock_engine_init(e);
    for (i = 0; i < count; i++) {
        clock_alarm_add(e, 7, i * 10, CLOCK_REPEAT_DAILY, NULL, NULL);
    }
}

/* The board learns what time it is, which on this hardware happens once per
 * boot and is a step of its own: the engine settles which alarms have already
 * had their moment today on the first valid reading, and only then is a step
 * at an alarm's own minute the alarm ringing rather than that rule firing
 * (clock_engine.c). Every run the shell has starts this way. */
static void wall_becomes_real(struct clock_engine *e)
{
    struct clock_now dawn = at(20260918, 6, 0, 5, 900000);

    clock_engine_step(e, &dawn);
}

/* ---- the three things a rotation used to end ---------------------------- */

static void test_running_stopwatch(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);   /* 30 s in */
    struct clock_now back = at(20260918, 9, 0, 5, 131500);      /* 1.5 s later */

    wipe();
    with_alarms(&before, 0);
    clock_sw_start(&before, &start);
    check("the stopwatch is running before the restart",
          clock_sw_elapsed_ms(&before, &exit_at) == 30000);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);
    check("and there is a file", handoff_exists());

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("the stopwatch is still running", after.sw.state == CLOCK_SW_RUNNING);
    /* The whole point: the monotonic clock did not stop for the exec, so the
     * elapsed time includes the restart and is the wall's own answer. */
    check("and its elapsed time counts the restart in",
          clock_sw_elapsed_ms(&after, &back) == 31500);
    check("the handoff is consumed", !handoff_exists());
    check("so a second start finds none", clock_handoff_load(&after, &back) == 1);
}

static void test_paused_stopwatch(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now paused = at(20260918, 9, 0, 5, 112345);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);
    struct clock_now back = at(20260918, 9, 1, 5, 190000); /* a minute later */

    wipe();
    with_alarms(&before, 0);
    clock_sw_start(&before, &start);
    clock_sw_pause(&before, &paused);
    check("paused at the moment it was paused",
          clock_sw_elapsed_ms(&before, &exit_at) == 12345);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("it is still paused", after.sw.state == CLOCK_SW_PAUSED);
    /* A paused stopwatch must not have counted the restart, or the pause was
     * not a pause. */
    check("and has not moved", clock_sw_elapsed_ms(&after, &back) == 12345);
}

static void test_laps(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 140000);
    struct clock_now back = at(20260918, 9, 0, 5, 141000);
    int i;

    wipe();
    with_alarms(&before, 0);
    clock_sw_start(&before, &start);
    for (i = 1; i <= CLOCK_MAX_LAPS; i++) {
        struct clock_now lap = at(20260918, 9, 0, 5, 100000 + i * 1000);

        clock_sw_lap(&before, &lap);
    }
    check("a full lap list before the restart", before.sw.lap_count == CLOCK_MAX_LAPS);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("every lap came across", after.sw.lap_count == CLOCK_MAX_LAPS);
    for (i = 0; i < CLOCK_MAX_LAPS; i++) {
        if (after.sw.laps[i] != (int64_t)(i + 1) * 1000) {
            check("and each is the time it was recorded at", 0);
            return;
        }
    }
    check("and each is the time it was recorded at", 1);
}

static void test_running_countdown(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);
    struct clock_now back = at(20260918, 9, 0, 5, 132000);

    wipe();
    with_alarms(&before, 0);
    clock_timer_set(&before, 0, 5, 0); /* five minutes */
    clock_timer_start(&before, &start);
    check("the deadline is five minutes from the start",
          before.timer.deadline_mono == 400000);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("the countdown is still running", after.timer.state == CLOCK_TIMER_RUNNING);
    /* A deadline is a monotonic instant, and the instant did not move. */
    check("against the same deadline", after.timer.deadline_mono == 400000);
    check("so the time left is the time left",
          clock_timer_remaining_ms(&after, &back) == 268000);
    check("and the duration it was set for came with it",
          after.timer.duration_ms == 300000);
}

static void test_paused_countdown(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now paused = at(20260918, 9, 0, 5, 160000); /* a minute in */
    struct clock_now exit_at = at(20260918, 9, 0, 5, 190000);
    struct clock_now back = at(20260918, 9, 5, 5, 460000);   /* much later */

    wipe();
    with_alarms(&before, 0);
    clock_timer_set(&before, 0, 5, 0);
    clock_timer_start(&before, &start);
    clock_timer_pause(&before, &paused);
    check("four minutes left when it was paused", before.timer.remaining_ms == 240000);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("it is still paused", after.timer.state == CLOCK_TIMER_PAUSED);
    /* Four and a half minutes of real time went by while it was paused. A
     * paused countdown that had counted any of it would be a bug. */
    check("with the same time left on it",
          clock_timer_remaining_ms(&after, &back) == 240000);
}

static void test_countdown_that_ended_during_the_restart(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 118000);  /* 2 s to go */
    struct clock_now back = at(20260918, 9, 0, 5, 123000);     /* 3 s past it */

    wipe();
    with_alarms(&before, 0);
    clock_timer_set(&before, 0, 0, 20);
    clock_timer_start(&before, &start);
    check("running when the shell went", before.timer.state == CLOCK_TIMER_RUNNING);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 0);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    /* Never RUNNING with a deadline already behind it: that would put a
     * countdown on screen that could only show zero until the next step
     * noticed. It is over, and it comes back over. */
    check("a countdown that ended in the gap comes back expired",
          after.timer.state == CLOCK_TIMER_EXPIRED);
    check("with nothing left on it", after.timer.remaining_ms == 0);
    check("and nothing is ringing yet", after.ringing == CLOCK_RING_NONE);

    /* The engine's own step is what rings it, exactly as it would have done
     * had nothing restarted. */
    clock_engine_step(&after, &back);
    check("the first step rings it", after.ringing == CLOCK_RING_TIMER);
    check("and it is still expired", after.timer.state == CLOCK_TIMER_EXPIRED);
}

static void test_snooze(void)
{
    struct clock_engine before, after;
    struct clock_now ring = at(20260918, 7, 0, 5, 1000000);
    struct clock_now exit_at = at(20260918, 7, 1, 5, 1060000);
    struct clock_now back = at(20260918, 7, 1, 5, 1062000);
    struct clock_now due = at(20260918, 7, 9, 5, 1540000); /* nine minutes on */

    wipe();
    with_alarms(&before, 2);
    wall_becomes_real(&before);
    /* The alarm rings, and the owner snoozes it. */
    clock_engine_step(&before, &ring);
    check("an alarm is ringing", before.ringing == CLOCK_RING_ALARM);
    clock_alarm_snooze(&before, &ring);
    check("the snooze is nine minutes on the monotonic clock",
          before.alarms[0].snooze_until == 1000000 + CLOCK_SNOOZE_MS);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 2);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("the snooze is on the alarm it belongs to",
          after.alarms[0].snooze_until == 1000000 + CLOCK_SNOOZE_MS);
    check("and not on any other", after.alarms[1].snooze_until == 0);
    clock_engine_step(&after, &back);
    check("it does not come back early", after.ringing == CLOCK_RING_NONE);
    clock_engine_step(&after, &due);
    check("it comes back when it is up", after.ringing == CLOCK_RING_ALARM);
    check("as the alarm it was", after.ringing_alarm == 0);
}

static void test_snooze_that_came_due_during_the_restart(void)
{
    struct clock_engine before, after;
    struct clock_now ring = at(20260918, 7, 0, 5, 1000000);
    struct clock_now exit_at = at(20260918, 7, 8, 5, 1530000);
    struct clock_now back = at(20260918, 7, 10, 5, 1600000); /* past the snooze */

    wipe();
    with_alarms(&before, 1);
    wall_becomes_real(&before);
    clock_engine_step(&before, &ring);
    clock_alarm_snooze(&before, &ring);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 1);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("nothing rings before a step", after.ringing == CLOCK_RING_NONE);
    /* A snooze needs no settling of its own: the engine already rings one
     * that is up, however long it has been up. */
    clock_engine_step(&after, &back);
    check("a snooze that came due in the gap rings on the first step",
          after.ringing == CLOCK_RING_ALARM);
    check("and is cleared", after.alarms[0].snooze_until == 0);
}

static void test_ringing_alarm(void)
{
    struct clock_engine before, after;
    struct clock_now ring = at(20260918, 7, 0, 5, 1000000);
    struct clock_now exit_at = at(20260918, 7, 0, 5, 1002000);
    struct clock_now back = at(20260918, 7, 0, 5, 1004000);

    wipe();
    with_alarms(&before, 2);
    wall_becomes_real(&before);
    clock_engine_step(&before, &ring);
    check("the second alarm is not the one ringing", before.ringing_alarm == 0);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 2);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    /* An alarm going off while the base is attached must not be answered by
     * the shell quietly going away. */
    check("the alarm is still ringing", after.ringing == CLOCK_RING_ALARM);
    check("and it is the same alarm", after.ringing_alarm == 0);
}

/* ---- what carrying "already rang today" is for -------------------------- */

static void test_fired_today_survives(void)
{
    struct clock_engine before, after;
    struct clock_now ring = at(20260918, 7, 0, 5, 1000000);
    struct clock_now exit_at = at(20260918, 7, 1, 5, 1060000);
    struct clock_now back = at(20260918, 7, 1, 5, 1062000);
    struct clock_now later = at(20260918, 23, 0, 5, 1900000);

    wipe();
    with_alarms(&before, 1);
    wall_becomes_real(&before);
    clock_engine_step(&before, &ring);
    check("it rings", before.ringing == CLOCK_RING_ALARM);
    clock_alarm_acknowledge(&before, &ring);
    check("and is marked as done for today", before.alarms[0].fired_day == 20260918);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 1);
    check("the next shell takes it", clock_handoff_load(&after, &back) == 0);
    check("it is still done for today", after.alarms[0].fired_day == 20260918);
    clock_engine_step(&after, &back);
    clock_engine_step(&after, &later);
    check("so it does not ring twice", after.ringing == CLOCK_RING_NONE);
}

static void test_alarm_in_the_restart_minute_is_not_swallowed(void)
{
    struct clock_engine before, after, control;
    struct clock_now armed = at(20260918, 6, 59, 5, 1000000);
    struct clock_now exit_at = at(20260918, 7, 0, 5, 1001000);
    struct clock_now back = at(20260918, 7, 0, 5, 1003000);

    wipe();
    /* The shell is up with the clock already real, and goes down inside the
     * very minute the alarm is due. */
    with_alarms(&before, 1); /* 07:00 */
    clock_engine_step(&before, &armed);
    check("nothing has rung yet", before.alarms[0].fired_day == CLOCK_DAY_NEVER);
    check("and the run knows the wall clock is real", before.wall_was_valid);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    /* Without the handoff this is a boot as far as the engine can tell, and
     * the boot rule marks every alarm whose minute has gone by as done -
     * which is right after a power cut and wrong 300 ms after an exec. */
    with_alarms(&control, 1);
    clock_engine_step(&control, &back);
    check("a fresh engine would have swallowed it",
          control.ringing == CLOCK_RING_NONE &&
              control.alarms[0].fired_day == 20260918);

    with_alarms(&after, 1);
    check("the next shell takes the handoff", clock_handoff_load(&after, &back) == 0);
    check("which says the clock was already real", after.wall_was_valid);
    clock_engine_step(&after, &back);
    check("so the alarm still rings", after.ringing == CLOCK_RING_ALARM);
}

/* ---- nothing to say, and stale handoffs --------------------------------- */

static void test_an_idle_clock_writes_nothing(void)
{
    struct clock_engine e;
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);

    wipe();
    with_alarms(&e, 0);
    check("an untouched engine has nothing to hand on",
          clock_handoff_save(&e, &exit_at) == 1);
    check("and leaves no file", !handoff_exists());

    /* Alarms alone are not a reason either: they are in the settings file,
     * and until the board knows what time it is there is nothing this run
     * knows about them that the next one will not work out for itself. */
    with_alarms(&e, 3);
    check("nor do alarms on a board that does not know the time",
          clock_handoff_save(&e, &exit_at) == 1);
    check("still no file", !handoff_exists());

    /* Once it does know, it knows which of them have already had their
     * moment today, and that does not survive on its own. */
    wall_becomes_real(&e);
    check("but alarms and a real wall clock do",
          clock_handoff_save(&e, &exit_at) == 0);
    check("and there is a file", handoff_exists());
    check("cleared again", clock_handoff_clear() == 0 && !handoff_exists());
    clock_engine_init(&e);

    /* A duration that was set but never started is the settings file's, and
     * clock_store_load() rebuilds it: this run knows nothing extra. */
    clock_timer_set(&e, 0, 3, 0);
    check("nor a countdown that was set and left alone",
          clock_handoff_save(&e, &exit_at) == 1);
    check("and no file", !handoff_exists());
}

static void test_a_stale_handoff_is_cleared(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now first = at(20260918, 9, 0, 5, 130000);
    struct clock_now second = at(20260918, 9, 1, 5, 190000);
    struct clock_now back = at(20260918, 9, 1, 5, 191000);

    wipe();
    with_alarms(&before, 0);
    clock_sw_start(&before, &start);
    check("one exit hands a running stopwatch on",
          clock_handoff_save(&before, &first) == 0);

    /* The owner resets it, and the shell goes down again without anything
     * having been written in between. The first handoff must not come back. */
    clock_sw_reset(&before);
    check("the next exit has nothing to hand on",
          clock_handoff_save(&before, &second) == 1);
    check("and took the old one away", !handoff_exists());

    with_alarms(&after, 0);
    check("so the next shell finds none", clock_handoff_load(&after, &back) == 1);
    check("and its stopwatch is idle", after.sw.state == CLOCK_SW_IDLE);
}

/* ---- files this reader must refuse -------------------------------------- */

static void refuse_n(int alarms, const char *what, const char *body)
{
    struct clock_engine e;
    struct clock_now back = at(20260918, 9, 0, 5, 500000);
    int i;

    wipe();
    plant(body);
    with_alarms(&e, alarms);
    check(what, clock_handoff_load(&e, &back) == -1);
    /* A refused handoff leaves the engine exactly as the settings file left
     * it, and is not left to be refused again at every start. */
    checks++;
    if (e.sw.state != CLOCK_SW_IDLE || e.timer.state != CLOCK_TIMER_IDLE ||
        e.ringing != CLOCK_RING_NONE || handoff_exists()) {
        failed++;
        printf("FAIL %s: left something behind\n", what);
        return;
    }
    for (i = 0; i < alarms; i++) {
        if (e.alarms[i].snooze_until != 0 ||
            e.alarms[i].fired_day != CLOCK_DAY_NEVER) {
            failed++;
            printf("FAIL %s: left something on alarm %d\n", what, i);
            return;
        }
    }
}

static void refuse(const char *what, const char *body)
{
    refuse_n(1, what, body);
}

/* Every good file this reader accepts, so the refusals below are refusing
 * the one thing each of them changes and not something incidental. */
#define GOOD_HANDOFF \
    CLOCK_HANDOFF_MAGIC "\n" \
    "mono 400000\n" \
    "wall 1\n" \
    "alarms 1\n" \
    "alarm 0 0 -1\n" \
    "sw 1 300000 0 0\n" \
    "timer 0 0 0 0\n" \
    "ring 0 -1\n"

static void test_a_good_file_is_taken(void)
{
    struct clock_engine e;
    struct clock_now back = at(20260918, 9, 0, 5, 500000);

    wipe();
    plant(GOOD_HANDOFF);
    with_alarms(&e, 1);
    check("the file the refusals are built from is itself good",
          clock_handoff_load(&e, &back) == 0);
    check("and says what it says", e.sw.state == CLOCK_SW_RUNNING &&
                                       e.sw.started_mono == 300000);
}

static void test_refusals(void)
{
    refuse("an empty file is refused", "");
    refuse("a file with only a magic line is refused", CLOCK_HANDOFF_MAGIC "\n");
    refuse("a foreign magic line is refused",
           "pocketclock 1\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a later format is refused",
           "pocketclock-restart 2\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("an unknown keyword is refused",
           GOOD_HANDOFF "weather 3\n");
    refuse("a missing line is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\n");
    refuse("a repeated line is refused", GOOD_HANDOFF "ring 0 -1\n");
    refuse("a field that is not a number is refused",
           CLOCK_HANDOFF_MAGIC "\nmono later\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a line with a field too many is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000 400000\nwall 1\nalarms 1\n"
           "alarm 0 0 -1\nsw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a line with a field too few is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");

    /* The boot check. A monotonic clock that has gone backwards is a
     * different boot, and every instant in the file means nothing. */
    refuse("a handoff from further in the future than now is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 900000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");

    /* The alarm list has to be the one it was written against, or a snooze
     * would land on somebody else's alarm. */
    refuse("a handoff for more alarms than there are is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 2\nalarm 0 0 -1\n"
           "alarm 1 0 -1\nsw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a handoff for fewer is refused too",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 0\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    /* The count is checked against the list and the lines are checked against
     * the count, and each catches what the other does not: this one has the
     * right number of lines and the wrong count at the top. */
    refuse("a count that contradicts the lines under it is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 0\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    /* And this one has the right count and no lines at all. */
    refuse("a count with none of the lines it promised is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("an alarm line out of order is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 1 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    /* One alarm described twice and another not at all is the same number of
     * lines and would leave the second alarm silently un-restored. */
    refuse_n(2, "the same alarm described twice is refused",
             CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 2\n"
             "alarm 0 0 -1\nalarm 0 0 -1\nsw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("an alarm line before the count is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarm 0 0 -1\nalarms 1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");

    /* States that cannot have happened. Below the first one as well as above
     * the last: these fields are read as signed and stored as unsigned, so a
     * negative that was only checked against the top of the range would come
     * back as 255 - a state no engine rule has an answer for. */
    refuse("a stopwatch state the engine does not have is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 9 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a negative stopwatch state is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw -1 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a countdown state the engine does not have is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 7 60000 0 0\nring 0 -1\n");
    refuse("a negative countdown state is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer -1 60000 0 0\nring 0 -1\n");
    refuse("a ring the engine does not have is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 5 -1\n");
    refuse("a negative ring is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring -1 -1\n");
    refuse("a negative lap count is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 1 300000 0 -1\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("an idle stopwatch with time on it is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 5000 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a stopwatch that started after the exit is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 1 600000 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("more laps than the engine holds is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 1 300000 0 99\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("fewer lap lines than the count promised is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 1 300000 0 2\nlap 1000\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("more lap lines than the count promised is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 1 300000 0 1\nlap 1000\nlap 2000\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a lap before the stopwatch is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "lap 1000\nsw 1 300000 0 1\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a countdown running with no duration is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 1 0 500000 0\nring 0 -1\n");
    refuse("a countdown with more left than it was set for is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 2 60000 0 90000\nring 0 -1\n");
    refuse("a countdown longer than the timer accepts is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 2 90000000 0 0\nring 0 -1\n");
    refuse("an expired countdown with time left is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 3 60000 0 1000\nring 0 -1\n");
    refuse("a ring at an alarm that is not there is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 1 4\n");
    refuse("a ringing alarm with no index is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 1 -1\n");
    refuse("a ringing timer that names an alarm is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 2 0\n");
    refuse("a negative snooze is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 -5 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a day before never is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\nalarm 0 0 -2\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a negative reading is refused",
           CLOCK_HANDOFF_MAGIC "\nmono -1\nwall 1\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    refuse("a wall flag that is not a flag is refused",
           CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 2\nalarms 1\nalarm 0 0 -1\n"
           "sw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1\n");
}

static void test_a_long_line_is_refused(void)
{
    struct clock_engine e;
    struct clock_now back = at(20260918, 9, 0, 5, 500000);
    char body[1024];
    char pad[400];

    wipe();
    memset(pad, '0', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    /* A line longer than the reader's buffer arrives truncated, and a
     * truncated line is not the line that was written. */
    snprintf(body, sizeof(body),
             CLOCK_HANDOFF_MAGIC "\nmono 400000\nwall 1\nalarms 1\n"
             "alarm 0 0 -1\nsw 0 0 0 0\ntimer 0 0 0 0\nring 0 -1 %s\n",
             pad);
    plant(body);
    with_alarms(&e, 1);
    check("a line longer than the reader is refused",
          clock_handoff_load(&e, &back) == -1);
}

static void test_blank_lines(void)
{
    struct clock_engine e;
    struct clock_now back = at(20260918, 9, 0, 5, 500000);
    char body[2048];
    size_t n;
    int i;

    wipe();
    plant(CLOCK_HANDOFF_MAGIC "\nmono 400000\n\nwall 1\nalarms 1\n"
          "alarm 0 0 -1\n\nsw 1 300000 0 0\ntimer 0 0 0 0\nring 0 -1\n");
    with_alarms(&e, 1);
    check("a blank line between two good ones is nothing",
          clock_handoff_load(&e, &back) == 0);

    /* But they are still lines, and a file padded out with them is a file
     * far larger than this format can be. */
    wipe();
    n = (size_t)snprintf(body, sizeof(body), "%s", GOOD_HANDOFF);
    for (i = 0; i < 64 && n < sizeof(body) - 4; i++) {
        body[n++] = '\n';
    }
    body[n] = '\0';
    plant(body);
    with_alarms(&e, 1);
    check("a file with more lines than the format has is refused",
          clock_handoff_load(&e, &back) == -1);
}

/* ---- the wall clock moving underneath it -------------------------------- */

static void test_the_wall_clock_moves_during_the_restart(void)
{
    struct clock_engine before, after, control;
    struct clock_now armed = at(20260918, 6, 30, 5, 1000000);
    struct clock_now exit_at = at(20260918, 6, 31, 5, 1060000);
    /* Something set the clock forward an hour and a half while the process
     * was being replaced. */
    struct clock_now jumped = at(20260918, 8, 0, 5, 1062000);
    struct clock_now unset = unset_at(1062000);

    wipe();
    with_alarms(&before, 1); /* 07:00 */
    wall_becomes_real(&before);
    clock_engine_step(&before, &armed);
    check("the handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    /* The engine's own answer to a forward jump, with nothing restarting:
     * the alarm is late, but it is today's alarm and it rings. */
    with_alarms(&control, 1);
    wall_becomes_real(&control);
    clock_engine_step(&control, &armed);
    clock_engine_step(&control, &jumped);
    check("a forward jump rings a late alarm", control.ringing == CLOCK_RING_ALARM);

    with_alarms(&after, 1);
    check("the next shell takes the handoff", clock_handoff_load(&after, &jumped) == 0);
    clock_engine_step(&after, &jumped);
    check("and a restart across the same jump does the same",
          after.ringing == CLOCK_RING_ALARM);

    /* And the other way: the wall clock stops being a time at all. Nothing
     * fires, and the run says so again on its next reading. */
    wipe();
    with_alarms(&before, 1);
    wall_becomes_real(&before);
    clock_engine_step(&before, &armed);
    clock_alarm_snooze(&before, &armed); /* nothing ringing: no snooze set */
    check("the handoff is written again", clock_handoff_save(&before, &exit_at) == 0);

    with_alarms(&after, 1);
    check("it is taken", clock_handoff_load(&after, &unset) == 0);
    check("with the clock remembered as real", after.wall_was_valid);
    clock_engine_step(&after, &unset);
    check("a reading that is not a time fires nothing",
          after.ringing == CLOCK_RING_NONE);
    check("and the run stops claiming the clock is real", !after.wall_was_valid);
}

/* ---- a write that was interrupted --------------------------------------- */

static void test_a_stale_temporary_is_beside_the_point(void)
{
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);
    struct clock_now back = at(20260918, 9, 0, 5, 131000);
    char path[256];
    char tmp[300];
    FILE *f;

    wipe();
    with_alarms(&before, 0);
    clock_sw_start(&before, &start);
    check("a handoff is written", clock_handoff_save(&before, &exit_at) == 0);

    /* What a write interrupted half way through leaves: the temporary, and
     * never a half-written handoff, because the last step is a rename. The
     * reader never opens the temporary, so it cannot be mistaken for one. */
    check("the handoff can be found", clock_handoff_path(path, sizeof(path)) == 0);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    check("a stale temporary can be planted", f != NULL);
    if (f) {
        fputs("pocketclock-restart 1\nmono 4000\nsw 1 ", f);
        fclose(f);
    }

    with_alarms(&after, 0);
    check("the handoff is taken whole", clock_handoff_load(&after, &back) == 0);
    check("and is the one that was written", after.sw.state == CLOCK_SW_RUNNING &&
                                                 after.sw.started_mono == 100000);

    /* The next write replaces the temporary rather than tripping over it. */
    check("the next write goes through", clock_handoff_save(&after, &exit_at) == 0);
    check("and leaves no temporary of its own", access(tmp, F_OK) != 0);
}

/* ---- the control case: the settings file is untouched ------------------- */

static void test_the_settings_file_is_not_involved(void)
{
    char state[160];
    char cmd[256];
    struct clock_engine before, after;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now exit_at = at(20260918, 9, 0, 5, 130000);
    struct clock_now back = at(20260918, 9, 0, 5, 131000);
    char path[256];

    /* Deliberately not a name that contains the runtime one, so the two
     * checks at the end of this test mean something. */
    snprintf(state, sizeof(state), "/tmp/pocketclock-state-%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
    setenv("POCKETOS_STATE_DIR", state, 1);
    wipe();

    with_alarms(&before, 2);
    clock_timer_set(&before, 0, 2, 0);
    check("the alarms are saved the way they always were",
          clock_store_save(&before) == 0);
    clock_sw_start(&before, &start);
    check("and a handoff is written beside them",
          clock_handoff_save(&before, &exit_at) == 0);

    /* The next shell, doing what the shell does: the settings file first. */
    clock_engine_init(&after);
    check("the settings file loads exactly as before",
          clock_store_load(&after) == 0);
    check("two alarms", after.alarm_count == 2);
    check("and the duration", after.timer.duration_ms == 120000);
    check("with no stopwatch in it", after.sw.state == CLOCK_SW_IDLE);
    check("and nothing rung today", after.alarms[0].fired_day == CLOCK_DAY_NEVER);

    check("then the handoff", clock_handoff_load(&after, &back) == 0);
    check("which adds the stopwatch", after.sw.state == CLOCK_SW_RUNNING);
    check("and leaves the alarms alone", after.alarm_count == 2);

    /* The two files are two files, in two directories. */
    check("the handoff is not in the state directory",
          clock_store_path(path, sizeof(path)) == 0 &&
          strstr(path, root) == NULL);
    check("and the settings file is not in the runtime one",
          clock_handoff_path(path, sizeof(path)) == 0 &&
          strstr(path, state) == NULL);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state);
    if (system(cmd) != 0) {
        /* already gone */
    }
    unsetenv("POCKETOS_STATE_DIR");
}

static void test_an_old_store_still_works(void)
{
    char state[160];
    char cmd[256];
    char path[256];
    struct clock_engine e;
    struct clock_now back = at(20260918, 9, 0, 5, 500000);
    FILE *f;

    snprintf(state, sizeof(state), "/tmp/pocketclock-oldstore-%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf '%.60s' && mkdir -p '%.60s/clock'", state, state);
    if (system(cmd) != 0) {
        check("the old-store fixture could be made", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", state, 1);
    wipe();

    /* A clock.conf written by a build from before any of this existed. The
     * format did not change, and it must not have to. */
    if (clock_store_path(path, sizeof(path)) != 0 || !(f = fopen(path, "wb"))) {
        check("the old store could be written", 0);
        return;
    }
    fputs("pocketclock 1\ntimer 200\nalarm 1 7 30 1 Wake up\nalarm 0 22 45 0\n", f);
    fclose(f);

    clock_engine_init(&e);
    check("a store from before the handoff loads", clock_store_load(&e) == 0);
    check("with both alarms", e.alarm_count == 2);
    check("the first one on", e.alarms[0].enabled && e.alarms[0].hour == 7);
    check("its label intact", strcmp(e.alarms[0].label, "Wake up") == 0);
    check("the second one off", !e.alarms[1].enabled && e.alarms[1].hour == 22);
    check("and the duration", e.timer.duration_ms == 200000);
    check("and no handoff to go with it", clock_handoff_load(&e, &back) == 1);
    check("which changes nothing", e.alarm_count == 2 && e.sw.state == CLOCK_SW_IDLE);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state);
    if (system(cmd) != 0) {
        /* already gone */
    }
    unsetenv("POCKETOS_STATE_DIR");
}

/* ---- the write happens once, not on a tick ------------------------------ */

static void test_stepping_writes_nothing(void)
{
    struct clock_engine e;
    struct clock_now start = at(20260918, 9, 0, 5, 100000);
    struct clock_now out = at(20260918, 9, 10, 5, 700000);
    int i;

    wipe();
    with_alarms(&e, 2);
    clock_sw_start(&e, &start);
    clock_timer_set(&e, 0, 1, 0);
    clock_timer_start(&e, &start);

    /* Six hundred steps: ten minutes of the shell's tick, and ten seconds of
     * PocketClock's refresh. Not one of them may touch the filesystem. */
    for (i = 0; i < 600; i++) {
        struct clock_now n = at(20260918, 9, 0, 5, 100000 + i * 100);

        clock_engine_step(&e, &n);
        (void)clock_sw_elapsed_ms(&e, &n);
        (void)clock_timer_remaining_ms(&e, &n);
    }
    check("no amount of stepping writes a handoff", !handoff_exists());

    /* It takes the one call on the way out. */
    check("only the way out does", clock_handoff_save(&e, &out) == 0);
    check("and then there is one", handoff_exists());
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/pocketclock-handoff-%d", (int)getpid());
    setenv("POCKETOS_RUNTIME_DIR", root, 1);

    test_running_stopwatch();
    test_paused_stopwatch();
    test_laps();
    test_running_countdown();
    test_paused_countdown();
    test_countdown_that_ended_during_the_restart();
    test_snooze();
    test_snooze_that_came_due_during_the_restart();
    test_ringing_alarm();
    test_fired_today_survives();
    test_alarm_in_the_restart_minute_is_not_swallowed();
    test_an_idle_clock_writes_nothing();
    test_a_stale_handoff_is_cleared();
    test_a_good_file_is_taken();
    test_refusals();
    test_a_long_line_is_refused();
    test_blank_lines();
    test_the_wall_clock_moves_during_the_restart();
    test_a_stale_temporary_is_beside_the_point();
    test_the_settings_file_is_not_involved();
    test_an_old_store_still_works();
    test_stepping_writes_nothing();

    wipe();
    printf("clock_handoff_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
