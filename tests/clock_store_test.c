/*
 * PocketClock persistence: what survives a reboot, what deliberately does
 * not, and what happens to a file that is not ours.
 *
 * Runs against a temporary POCKETOS_STATE_DIR, so it never touches a real
 * store.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want);
    }
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* Nothing to remove on the first call. */
    }
}

/* Write the settings file directly, to make states the app cannot. */
static void plant(const char *text)
{
    char path[256];
    FILE *f;

    mkdir(root, 0755);
    snprintf(path, sizeof(path), "%s/clock", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/clock/%s", root, CLOCK_STORE_FILE);
    f = fopen(path, "wb");
    if (!f) {
        printf("FAIL could not plant %s\n", path);
        failed++;
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static int count_files(void)
{
    char path[256];
    DIR *d;
    struct dirent *e;
    int n = 0;

    snprintf(path, sizeof(path), "%s/clock", root);
    d = opendir(path);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void test_paths(void)
{
    char path[256];

    check("the store lives under the state directory",
          strstr(clock_store_dir(), root) == clock_store_dir());
    check("in its own subdirectory",
          strstr(clock_store_dir(), CLOCK_STORE_SUBDIR) != NULL);
    check("the path is built", clock_store_path(path, sizeof(path)) == 0);
    check("and ends in the file name",
          strstr(path, CLOCK_STORE_FILE) != NULL);
    check("a buffer that is too small is refused, not overrun",
          clock_store_path(path, 4) == -1);
}

static void test_round_trip(void)
{
    struct clock_engine a;
    struct clock_engine b;
    const struct clock_alarm *al;

    wipe();
    clock_engine_init(&a);
    check("no file yet is not a failure", clock_store_load(&a) == 1);
    check("and leaves an empty engine", clock_alarm_count(&a) == 0);

    clock_alarm_add(&a, 7, 30, CLOCK_REPEAT_DAILY, "Wake up", NULL);
    clock_alarm_add(&a, 6, 0, CLOCK_REPEAT_WEEKDAYS, "Tog til Ås", NULL);
    clock_alarm_add(&a, 22, 45, CLOCK_REPEAT_ONCE, NULL, NULL);
    clock_alarm_set_enabled(&a, 2, false, NULL);
    clock_timer_set(&a, 0, 3, 20);

    check("saving works", clock_store_save(&a) == 0);
    check("and leaves exactly one file behind", count_files() == 1);

    clock_engine_init(&b);
    check("loading works", clock_store_load(&b) == 0);
    check("three alarms", clock_alarm_count(&b) == 3);

    al = clock_alarm_at(&b, 0);
    check("the first is at its time", al->hour == 7 && al->minute == 30);
    check("with its repeat", al->repeat == CLOCK_REPEAT_DAILY);
    check_str("and its label", al->label, "Wake up");
    check("and it is on", al->enabled);

    al = clock_alarm_at(&b, 1);
    check_str("a label with a space and a non-ASCII letter survives",
              al->label, "Tog til Ås");
    check("weekdays survives", al->repeat == CLOCK_REPEAT_WEEKDAYS);

    al = clock_alarm_at(&b, 2);
    check("an alarm with no label comes back with none", al->label[0] == '\0');
    check("and a switched-off alarm stays off", !al->enabled);
    check("the order is the order they were added",
          clock_alarm_at(&b, 0)->hour == 7 && clock_alarm_at(&b, 1)->hour == 6);

    check("the timer duration survives", b.timer.duration_ms == 200000);
    check("as the time remaining", b.timer.remaining_ms == 200000);
    check("and it is not running", b.timer.state == CLOCK_TIMER_IDLE);
}

static void test_what_is_not_stored(void)
{
    struct clock_engine a;
    struct clock_engine b;
    struct clock_now now;

    wipe();
    clock_engine_init(&a);
    clock_alarm_add(&a, 7, 30, CLOCK_REPEAT_DAILY, NULL, NULL);

    /* Ring it, so the alarm carries a fired_day and the engine a ringing
     * state, and run the stopwatch and a countdown. */
    memset(&now, 0, sizeof(now));
    now.wall.valid = true;
    now.wall.day = 20260911;
    now.wall.hour = 7;
    now.wall.minute = 0;
    now.wall.wday = 5;
    now.mono_ms = 1000;
    clock_engine_step(&a, &now);
    now.wall.minute = 30;
    clock_engine_step(&a, &now);
    check("it is ringing", a.ringing == CLOCK_RING_ALARM);
    check("and marked as having rung today",
          clock_alarm_at(&a, 0)->fired_day == 20260911);
    clock_sw_start(&a, &now);
    clock_timer_set(&a, 0, 5, 0);
    clock_timer_start(&a, &now);

    check("saving works anyway", clock_store_save(&a) == 0);
    clock_engine_init(&b);
    check("loading works", clock_store_load(&b) == 0);

    /* Whether an alarm already rang is a fact about today, and today does
     * not survive a power cycle; the engine works it out again. */
    check("fired_day is not restored",
          clock_alarm_at(&b, 0)->fired_day == CLOCK_DAY_NEVER);
    check("nothing is ringing after a load", b.ringing == CLOCK_RING_NONE);
    check("no alarm is snoozing", clock_alarm_at(&b, 0)->snooze_until == 0);
    check("the stopwatch is not running", b.sw.state == CLOCK_SW_IDLE);
    check("and reads zero", b.sw.accumulated_ms == 0);
    check("the timer is not running", b.timer.state == CLOCK_TIMER_IDLE);
    check("but its duration is a setting and came back",
          b.timer.duration_ms == 300000);
    check("with nothing counted off it", b.timer.remaining_ms == 300000);
}

static void test_bad_files(void)
{
    struct clock_engine e;

    /* Every one of these must be refused whole. A settings file that is
     * half-understood would produce alarms nobody set. */
    plant("");
    clock_engine_init(&e);
    check("an empty file is refused", clock_store_load(&e) == -1);
    check("and adds no alarms", clock_alarm_count(&e) == 0);

    plant("pocketclock 2\ntimer 0\n");
    clock_engine_init(&e);
    check("a later format is refused rather than misread",
          clock_store_load(&e) == -1);

    plant("something else\n");
    clock_engine_init(&e);
    check("so is a file that is not ours", clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 7 30 0 ok\nnonsense 4\n");
    clock_engine_init(&e);
    check("an unknown keyword is refused", clock_store_load(&e) == -1);
    check("and the alarm before it is not kept", clock_alarm_count(&e) == 0);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 24 00 0 late\n");
    clock_engine_init(&e);
    check("an impossible hour is refused", clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 7 60 0 late\n");
    clock_engine_init(&e);
    check("an impossible minute is refused", clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 7 30 9 late\n");
    clock_engine_init(&e);
    check("an unknown repeat is refused", clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 7 x 0 late\n");
    clock_engine_init(&e);
    check("a field that is not a number is refused, not read as zero",
          clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\nalarm 1 7\n");
    clock_engine_init(&e);
    check("a short line is refused", clock_store_load(&e) == -1);

    plant(CLOCK_STORE_MAGIC "\ntimer 999999\n");
    clock_engine_init(&e);
    check("a duration the engine would refuse is refused here too",
          clock_store_load(&e) == -1);

    /* Nine alarms: one more than the engine holds. */
    plant(CLOCK_STORE_MAGIC "\n"
          "alarm 1 1 0 0 a\nalarm 1 2 0 0 b\nalarm 1 3 0 0 c\n"
          "alarm 1 4 0 0 d\nalarm 1 5 0 0 e\nalarm 1 6 0 0 f\n"
          "alarm 1 7 0 0 g\nalarm 1 8 0 0 h\nalarm 1 9 0 0 i\n");
    clock_engine_init(&e);
    check("more alarms than the engine holds is refused",
          clock_store_load(&e) == -1);
    check("and none of them are kept", clock_alarm_count(&e) == 0);

    /* A label longer than a line, which a truncating reader would silently
     * shorten into a different label. */
    {
        char big[512];
        int n;

        n = snprintf(big, sizeof(big), "%s\nalarm 1 7 30 0 ", CLOCK_STORE_MAGIC);
        memset(big + n, 'x', 300);
        big[n + 300] = '\n';
        big[n + 301] = '\0';
        plant(big);
        clock_engine_init(&e);
        check("a line longer than the reader is refused, not truncated",
              clock_store_load(&e) == -1);
    }

    /* And a good file still loads after all of that. */
    plant(CLOCK_STORE_MAGIC "\ntimer 60\nalarm 0 7 30 1 Wake\n");
    clock_engine_init(&e);
    check("a good file loads", clock_store_load(&e) == 0);
    check("one alarm", clock_alarm_count(&e) == 1);
    check("switched off", !clock_alarm_at(&e, 0)->enabled);
    check("daily", clock_alarm_at(&e, 0)->repeat == CLOCK_REPEAT_DAILY);
    check_str("with its label", clock_alarm_at(&e, 0)->label, "Wake");
    check("and a minute on the timer", e.timer.duration_ms == 60000);

    /* Blank lines are ignored rather than treated as an error: a file that
     * was hand-edited is still a file we wrote the rest of. */
    plant(CLOCK_STORE_MAGIC "\n\nalarm 1 7 30 0 ok\n\n");
    clock_engine_init(&e);
    check("blank lines are skipped", clock_store_load(&e) == 0);
    check("and the alarm is there", clock_alarm_count(&e) == 1);
}

static void test_overwrite_is_atomic(void)
{
    struct clock_engine a;
    struct clock_engine b;

    wipe();
    clock_engine_init(&a);
    clock_alarm_add(&a, 7, 30, CLOCK_REPEAT_DAILY, "first", NULL);
    clock_store_save(&a);

    clock_alarm_remove(&a, 0);
    clock_alarm_add(&a, 8, 0, CLOCK_REPEAT_ONCE, "second", NULL);
    check("saving over an existing file works", clock_store_save(&a) == 0);
    check("and still leaves exactly one file", count_files() == 1);

    clock_engine_init(&b);
    clock_store_load(&b);
    check("one alarm", clock_alarm_count(&b) == 1);
    check_str("and it is the new one", clock_alarm_at(&b, 0)->label, "second");

    /* Saving nothing is a real state: the owner deleted the last alarm. */
    clock_engine_init(&a);
    check("an empty engine saves", clock_store_save(&a) == 0);
    clock_engine_init(&b);
    clock_alarm_add(&b, 9, 0, CLOCK_REPEAT_ONCE, "stale", NULL);
    check("and loads", clock_store_load(&b) == 0);
    check("as no alarms at all", clock_alarm_count(&b) == 0);
}

static void test_labels(void)
{
    check("no label is storable", clock_label_is_storable(NULL));
    check("an empty one is", clock_label_is_storable(""));
    check("a plain one is", clock_label_is_storable("Wake up"));
    check("a non-ASCII one is", clock_label_is_storable("Tog til Ås"));
    check("a newline is not, it would end the line",
          !clock_label_is_storable("two\nlines"));
    check("nor is a carriage return", !clock_label_is_storable("two\rlines"));
    check("nor a tab", !clock_label_is_storable("a\tb"));
    check("nor a longer label than the field",
          !clock_label_is_storable("123456789012345678901234567890"));
    check("the longest one that fits is",
          clock_label_is_storable("12345678901234567890123"));
}

static void test_null(void)
{
    check("loading into nothing is refused", clock_store_load(NULL) == -1);
    check("saving nothing is refused", clock_store_save(NULL) == -1);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/pocketclock-test-%d", (int)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);

    test_paths();
    test_round_trip();
    test_what_is_not_stored();
    test_bad_files();
    test_overwrite_is_atomic();
    test_labels();
    test_null();

    wipe();
    printf("clock_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
