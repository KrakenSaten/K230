/*
 * The seam itself: one process, replaced by another, with the clock still
 * running.
 *
 * WHY THIS TEST EXECS. An orientation change does not restart the device and
 * does not even restart the shell as a new process: restart_in_place() in
 * ui/shell/shell.c calls execv, which keeps the pid and the boot and throws
 * away everything in memory. A test that only called clock_runtime_deinit()
 * and clock_runtime_init() again would prove nothing about that, because a
 * static that deinit happened to leave alone would carry the answer across
 * and the test would pass on a bug. So this one really does it: every
 * scenario below starts a child, the child sets some state up and execs
 * itself, and the second image - which has never seen the first, whose BSS
 * the kernel has just zeroed, and whose only inheritance is the boot, the
 * environment and the filesystem - has to find that state again.
 *
 * The rules and the arithmetic are tests/clock_handoff_test.c, where both
 * clocks are injected and nothing is approximate. This file is about the
 * boundary, so it runs on the real monotonic clock: what it asserts exactly
 * is every instant that must not move, and what it asserts in bounds is the
 * elapsed time, which really does grow by however long the exec took.
 *
 * Runs against a temporary POCKETOS_RUNTIME_DIR and POCKETOS_STATE_DIR, one
 * pair per scenario, so it never touches the real ones.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock_runtime.h"

#include "clock_store.h"
#include "clock_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* An exec is fast, but this runs on whatever the host is doing at the time.
 * Anything under this is "the restart"; anything over it is a stopwatch that
 * has been restarted rather than resumed, which is the bug. */
#define RESTART_SLACK_MS 30000

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

static void check_i64(const char *what, int64_t got, int64_t want)
{
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: got %lld, want %lld\n", what, (long long)got,
               (long long)want);
    }
}

/* Somewhere at or after want, and not so far after it that the thing was
 * started again rather than carried on. */
static void check_carried(const char *what, int64_t got, int64_t want)
{
    checks++;
    if (got < want || got > want + RESTART_SLACK_MS) {
        failed++;
        printf("FAIL %s: got %lld, want %lld..%lld\n", what, (long long)got,
               (long long)want, (long long)(want + RESTART_SLACK_MS));
    }
}

/* ---- passing a number from one process image to the next ---------------- */

static void put_i64(const char *name, int64_t v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    setenv(name, buf, 1);
}

static int64_t get_i64(const char *name)
{
    const char *v = getenv(name);

    return v ? (int64_t)strtoll(v, NULL, 10) : -1;
}

/* Every stage adds its own tally to one file, so the parent can report the
 * checks its children made rather than one line each. */
static void record(void)
{
    const char *path = getenv("CLOCK_RESTART_RESULT");
    FILE *f;

    if (!path || !(f = fopen(path, "ab"))) {
        return;
    }
    fprintf(f, "%d %d\n", checks, failed);
    fclose(f);
}

/* The same trick restart_in_place() uses: this binary, again, in this
 * process. It does not return. */
static void exec_self(const char *scenario)
{
    char exe[4096];
    char *argv[4];
    ssize_t n;

    record();
    fflush(NULL);
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        printf("FAIL cannot find my own binary to exec\n");
        exit(1);
    }
    exe[n] = '\0';
    argv[0] = exe;
    argv[1] = (char *)scenario;
    argv[2] = (char *)"b";
    argv[3] = NULL;
    execv(exe, argv);
    printf("FAIL exec failed\n");
    exit(1);
}

/* ---- what the first image does ------------------------------------------ */

/* A reading of the real clocks, backdated by up to back_ms. The board's
 * monotonic clock starts at the boot and not at this test, so on a host that
 * has only just come up there may be less than that to go back: the instant
 * actually used is written down and handed to the next image, which asserts
 * against it rather than against what was asked for. */
static struct clock_now backdated(int64_t back_ms)
{
    struct clock_now t;

    clock_runtime_read();
    t = *clock_runtime_now();
    if (back_ms > t.mono_ms) {
        back_ms = t.mono_ms;
    }
    t.mono_ms -= back_ms;
    return t;
}

static void a_stopwatch(void)
{
    struct clock_engine *e = clock_runtime_engine();
    struct clock_now t = backdated(2000);

    clock_runtime_step_at(&t);
    clock_sw_start(e, clock_runtime_now());
    check("the stopwatch is running before the exec", e->sw.state == CLOCK_SW_RUNNING);
    put_i64("CLOCK_RESTART_T0", e->sw.started_mono);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_stopwatch(void)
{
    struct clock_engine *e = clock_runtime_engine();
    int64_t t0 = get_i64("CLOCK_RESTART_T0");

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    check("the stopwatch is still running", e->sw.state == CLOCK_SW_RUNNING);
    /* The instant it started is an instant, and an exec does not move one. */
    check_i64("it started when it started", e->sw.started_mono, t0);
    check("nothing was accumulated behind it", e->sw.accumulated_ms == 0);
    /* And it has been running the whole time, the exec included. */
    check_carried("its elapsed time carried across the exec",
                  clock_sw_elapsed_ms(e, clock_runtime_now()),
                  clock_runtime_now()->mono_ms - t0);
    check("which is at least the time before the exec",
          clock_sw_elapsed_ms(e, clock_runtime_now()) >= 2000 ||
              t0 == 0);
}

static void a_paused_stopwatch(void)
{
    struct clock_engine *e = clock_runtime_engine();
    struct clock_now start = backdated(4000);
    struct clock_now pause = backdated(1500);

    clock_runtime_step_at(&start);
    clock_sw_start(e, clock_runtime_now());
    clock_runtime_step_at(&pause);
    clock_sw_pause(e, clock_runtime_now());
    check("it is paused before the exec", e->sw.state == CLOCK_SW_PAUSED);
    put_i64("CLOCK_RESTART_T0", e->sw.accumulated_ms);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_paused_stopwatch(void)
{
    struct clock_engine *e = clock_runtime_engine();
    int64_t elapsed = get_i64("CLOCK_RESTART_T0");

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    check("it is still paused", e->sw.state == CLOCK_SW_PAUSED);
    /* Exactly, not within anything: a paused stopwatch that counted the
     * restart was not paused. */
    check_i64("and has not moved across the exec",
              clock_sw_elapsed_ms(e, clock_runtime_now()), elapsed);
}

static void a_countdown(void)
{
    struct clock_engine *e = clock_runtime_engine();
    struct clock_now t = backdated(2000);

    clock_runtime_step_at(&t);
    check("a ten-minute countdown is set", clock_timer_set(e, 0, 10, 0));
    check("and started", clock_timer_start(e, clock_runtime_now()));
    put_i64("CLOCK_RESTART_T0", e->timer.deadline_mono);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_countdown(void)
{
    struct clock_engine *e = clock_runtime_engine();
    int64_t deadline = get_i64("CLOCK_RESTART_T0");
    int64_t left;

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    check("the countdown is still running", e->timer.state == CLOCK_TIMER_RUNNING);
    /* A deadline is a monotonic instant. The process went; the instant did
     * not. */
    check_i64("against the same deadline", e->timer.deadline_mono, deadline);
    check_i64("for the duration it was set for", e->timer.duration_ms, 600000);
    left = clock_timer_remaining_ms(e, clock_runtime_now());
    check_i64("and the time left is the time to the deadline", left,
              deadline - clock_runtime_now()->mono_ms);
    check("which is less than it was before the exec", left < 598000);
    clock_runtime_step();
    check("a step does not end it early", e->timer.state == CLOCK_TIMER_RUNNING);
    check("and nothing is ringing", e->ringing == CLOCK_RING_NONE);
}

static void a_countdown_expiring(void)
{
    struct clock_engine *e = clock_runtime_engine();
    struct clock_now t = backdated(3000);

    clock_runtime_step_at(&t);
    check("a one-second countdown is set", clock_timer_set(e, 0, 0, 1));
    check("and started", clock_timer_start(e, clock_runtime_now()));
    check("it is running when the process goes",
          e->timer.state == CLOCK_TIMER_RUNNING);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_countdown_expiring(void)
{
    struct clock_engine *e = clock_runtime_engine();

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    /* Its second was up while the process was being replaced. It comes back
     * over, never running against a deadline that has already gone. */
    check("the countdown comes back expired", e->timer.state == CLOCK_TIMER_EXPIRED);
    check("with nothing left on it",
          clock_timer_remaining_ms(e, clock_runtime_now()) == 0);
    check("and is not ringing until the shell steps", e->ringing == CLOCK_RING_NONE);
    clock_runtime_step();
    check("the first step rings it", e->ringing == CLOCK_RING_TIMER);
    check("and it stays expired until it is answered",
          e->timer.state == CLOCK_TIMER_EXPIRED);
    clock_runtime_stop_ringing();
    check("answering it puts it away", e->ringing == CLOCK_RING_NONE);
    check("and leaves the duration for next time", e->timer.duration_ms == 1000);
}

/* An alarm set for the minute this test is running in, so the wall clock it
 * rings against is the host's own and the second image, a few milliseconds
 * later, is on the same day and the same minute. A synthetic date here would
 * make every check below depend on what today happens to be.
 *
 * It takes two steps, because the first one is the board learning what time
 * it is: the engine marks every alarm whose minute has already gone by as
 * done for today, since it cannot have rung while the board had no idea
 * (clock_engine.c). So the run is warmed up one minute earlier - by the
 * calendar and not by subtraction, so the hour, the day and the week all
 * roll over properly - and only the second step is the alarm going off. */
static int arm_for_now(struct clock_engine *e, const char *label)
{
    struct clock_now warm;
    struct clock_now ring;
    int index;

    clock_runtime_read();
    ring = *clock_runtime_now();
    check("the host knows what time it is", ring.wall.valid);
    if (!ring.wall.valid) {
        return -1;
    }
    index = clock_alarm_add(e, ring.wall.hour, ring.wall.minute,
                            CLOCK_REPEAT_DAILY, label, NULL);
    check("an alarm is added", index >= 0);
    if (index < 0) {
        return -1;
    }
    warm = ring;
    warm.mono_ms = backdated(2000).mono_ms;
    clock_wall_from_epoch(&warm.wall, ring.wall.epoch - 60);
    clock_runtime_step_at(&warm);
    check("nothing rings a minute before it is due", e->ringing == CLOCK_RING_NONE);
    clock_runtime_step_at(&ring);
    return index;
}

/* The alarm the snooze belongs to, and the control case with it: alarms are
 * in the settings file and have always survived, and must go on doing so
 * unchanged. */
static void a_snooze(void)
{
    struct clock_engine *e = clock_runtime_engine();

    /* Off, so that whatever time of day this test runs at, there is exactly
     * one alarm that can ring and it is the one being snoozed. */
    check("a second alarm is added", clock_alarm_add(e, 8, 0, CLOCK_REPEAT_ONCE,
                                                     "Later", NULL) == 0);
    check("and switched off", clock_alarm_set_enabled(e, 0, false, NULL));
    if (arm_for_now(e, "Wake up") != 1) {
        return;
    }
    check("the alarm to ring is the second", clock_alarm_count(e) == 2);
    check("and saved the way alarms always are", clock_runtime_save() == 0);

    check("it rings", e->ringing == CLOCK_RING_ALARM);
    check("and it is the one that was armed", e->ringing_alarm == 1);
    clock_runtime_snooze();
    check("and snoozes", e->ringing == CLOCK_RING_NONE);
    check("on the alarm that was ringing", e->alarms[1].snooze_until != 0);
    check("and on no other", e->alarms[0].snooze_until == 0);
    put_i64("CLOCK_RESTART_T0", e->alarms[1].snooze_until);
    put_i64("CLOCK_RESTART_T1", e->alarms[1].fired_day);
    put_i64("CLOCK_RESTART_T2", e->alarms[1].hour * 60 + e->alarms[1].minute);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_snooze(void)
{
    struct clock_engine *e = clock_runtime_engine();
    int64_t due = get_i64("CLOCK_RESTART_T0");
    int64_t fired = get_i64("CLOCK_RESTART_T1");
    int64_t when = get_i64("CLOCK_RESTART_T2");

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    /* The control: the alarms came out of the settings file, exactly as they
     * did before any of this existed. */
    check("both alarms are back", clock_alarm_count(e) == 2);
    check("the one that was switched off is off and still 08:00",
          !e->alarms[0].enabled && e->alarms[0].hour == 8 &&
              e->alarms[0].minute == 0 &&
              strcmp(e->alarms[0].label, "Later") == 0);
    check("its repeat came back too", e->alarms[0].repeat == CLOCK_REPEAT_ONCE);
    check("and the other is on, at its minute, with its label",
          e->alarms[1].enabled &&
              e->alarms[1].hour * 60 + e->alarms[1].minute == when &&
              strcmp(e->alarms[1].label, "Wake up") == 0);
    check("and its repeat", e->alarms[1].repeat == CLOCK_REPEAT_DAILY);

    /* And the snooze, which is not in that file and used to end here. */
    check_i64("the snooze is due when it was due", e->alarms[1].snooze_until, due);
    check("it is on the alarm it belongs to", e->alarms[0].snooze_until == 0);
    check_i64("and the alarm is still done for today", e->alarms[1].fired_day, fired);
    clock_runtime_step();
    check("nothing rings while it waits", e->ringing == CLOCK_RING_NONE);
    check("and the snooze is still waiting", e->alarms[1].snooze_until == due);
}

static void a_ringing(void)
{
    struct clock_engine *e = clock_runtime_engine();

    if (arm_for_now(e, NULL) != 0) {
        return;
    }
    check("and saved", clock_runtime_save() == 0);
    check("it is ringing when the process goes", e->ringing == CLOCK_RING_ALARM);
    check("the handoff is written", clock_runtime_handoff_save() == 0);
}

static void b_ringing(void)
{
    struct clock_engine *e = clock_runtime_engine();

    check("the handoff was taken", clock_runtime_handoff_result() == 0);
    /* An alarm going off while the keyboard base is being attached must not
     * be answered by the shell quietly going away with it. */
    check("the alarm is still ringing", e->ringing == CLOCK_RING_ALARM);
    check("and it is the alarm that was ringing", e->ringing_alarm == 0);
    clock_runtime_step();
    check("a step leaves it ringing", e->ringing == CLOCK_RING_ALARM);
    clock_runtime_stop_ringing();
    check("and Stop stops it", e->ringing == CLOCK_RING_NONE);
}

/* Nothing worth handing on: the ordinary exit, which must leave no file and
 * must not resurrect anything. */
static void a_idle(void)
{
    char path[256];

    check("an idle clock hands nothing on", clock_runtime_handoff_save() == 1);
    check("and leaves no file", clock_handoff_path(path, sizeof(path)) == 0 &&
                                    access(path, F_OK) != 0);
}

static void b_idle(void)
{
    struct clock_engine *e = clock_runtime_engine();

    check("the next image finds no handoff", clock_runtime_handoff_result() == 1);
    check("its stopwatch is idle", e->sw.state == CLOCK_SW_IDLE);
    check("its countdown is idle", e->timer.state == CLOCK_TIMER_IDLE);
    check("and nothing is ringing", e->ringing == CLOCK_RING_NONE);
}

/* A handoff that cannot be read. The engine must be exactly what the
 * settings file made it, and the file must be gone. */
static void a_damaged(void)
{
    struct clock_engine *e = clock_runtime_engine();
    char path[256];
    FILE *f;

    check("an alarm is added", clock_alarm_add(e, 6, 45, CLOCK_REPEAT_WEEKDAYS,
                                               NULL, NULL) == 0);
    check("and saved", clock_runtime_save() == 0);
    clock_sw_start(e, clock_runtime_now());
    check("a stopwatch is running", e->sw.state == CLOCK_SW_RUNNING);
    check("the handoff is written", clock_runtime_handoff_save() == 0);

    /* Something ate the middle of it. */
    check("the handoff can be found", clock_handoff_path(path, sizeof(path)) == 0);
    f = fopen(path, "wb");
    check("and damaged", f != NULL);
    if (f) {
        fputs(CLOCK_HANDOFF_MAGIC "\nmono 0\nwall 1\nsw 1 0\n", f);
        fclose(f);
    }
}

static void b_damaged(void)
{
    struct clock_engine *e = clock_runtime_engine();
    char path[256];

    check("the damaged handoff is refused", clock_runtime_handoff_result() == -1);
    check("the alarm is still there", clock_alarm_count(e) == 1);
    check("and is the one that was saved",
          e->alarms[0].hour == 6 && e->alarms[0].minute == 45);
    check("the stopwatch is idle rather than half restored",
          e->sw.state == CLOCK_SW_IDLE && e->sw.accumulated_ms == 0);
    check("the countdown too", e->timer.state == CLOCK_TIMER_IDLE);
    check("nothing is ringing", e->ringing == CLOCK_RING_NONE);
    /* And it is not left behind to be refused again at every start for the
     * rest of the boot. */
    check("the damaged file is gone", clock_handoff_path(path, sizeof(path)) == 0 &&
                                          access(path, F_OK) != 0);
}

/* ---- the scenarios ------------------------------------------------------ */

struct scenario {
    const char *name;
    void (*a)(void);
    void (*b)(void);
};

static const struct scenario scenarios[] = {
    { "stopwatch", a_stopwatch, b_stopwatch },
    { "paused", a_paused_stopwatch, b_paused_stopwatch },
    { "countdown", a_countdown, b_countdown },
    { "expiring", a_countdown_expiring, b_countdown_expiring },
    { "snooze", a_snooze, b_snooze },
    { "ringing", a_ringing, b_ringing },
    { "idle", a_idle, b_idle },
    { "damaged", a_damaged, b_damaged },
};
#define SCENARIO_COUNT ((int)(sizeof(scenarios) / sizeof(scenarios[0])))

static int run_stage(const char *name, const char *stage)
{
    int i;

    for (i = 0; i < SCENARIO_COUNT; i++) {
        if (strcmp(scenarios[i].name, name) != 0) {
            continue;
        }
        clock_runtime_init(NULL);
        if (strcmp(stage, "a") == 0) {
            scenarios[i].a();
            if (failed) {
                record();
                return 1; /* no point exec'ing into a check that cannot pass */
            }
            exec_self(name); /* does not return */
        }
        scenarios[i].b();
        record();
        return failed ? 1 : 0;
    }
    printf("FAIL unknown scenario %s\n", name);
    return 1;
}

/* ---- the parent --------------------------------------------------------- */

static char root[128];

static void sh(const char *fmt, const char *arg)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), fmt, arg);
    if (system(cmd) != 0) {
        /* nothing there yet, or already gone */
    }
}

/* Add what a child counted to what this process is reporting, so the total
 * is the checks that were actually made and not one per scenario. */
static void absorb(const char *path)
{
    FILE *f = fopen(path, "rb");
    int c, x;

    if (!f) {
        return;
    }
    while (fscanf(f, "%d %d", &c, &x) == 2) {
        checks += c;
        failed += x;
    }
    fclose(f);
}

static int drive(void)
{
    int i;

    for (i = 0; i < SCENARIO_COUNT; i++) {
        char run[192], state[192], result[224];
        pid_t pid;
        int status = -1;

        snprintf(run, sizeof(run), "%s/%s/run", root, scenarios[i].name);
        snprintf(state, sizeof(state), "%s/%s/state", root, scenarios[i].name);
        snprintf(result, sizeof(result), "%s/%s/result", root, scenarios[i].name);
        sh("mkdir -p '%s'", run);
        sh("mkdir -p '%s'", state);
        setenv("POCKETOS_RUNTIME_DIR", run, 1);
        setenv("POCKETOS_STATE_DIR", state, 1);
        setenv("CLOCK_RESTART_RESULT", result, 1);

        pid = fork();
        if (pid < 0) {
            check("a child could be started", 0);
            continue;
        }
        if (pid == 0) {
            char exe[4096];
            char *argv[4];
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

            if (n <= 0) {
                _exit(1);
            }
            exe[n] = '\0';
            argv[0] = exe;
            argv[1] = (char *)scenarios[i].name;
            argv[2] = (char *)"a";
            argv[3] = NULL;
            execv(exe, argv);
            _exit(1);
        }
        if (waitpid(pid, &status, 0) != pid) {
            status = -1;
        }
        absorb(result);
        checks++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed++;
            printf("FAIL %s did not survive the exec (status %d)\n",
                   scenarios[i].name, status);
        }
    }
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    int rc;

    if (argc >= 3) {
        return run_stage(argv[1], argv[2]);
    }
    snprintf(root, sizeof(root), "/tmp/pocketclock-restart-%d", (int)getpid());
    sh("rm -rf '%s'", root);
    rc = drive();
    sh("rm -rf '%s'", root);
    printf("clock_restart_test: %d checks across %d real execs, %d failure(s)\n",
           checks, SCENARIO_COUNT, failed);
    return rc;
}
