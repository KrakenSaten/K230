/*
 * Power & Sleep decisions (ui/shell/power_policy.h): the options a stepper
 * walks, what is stored and what is refused, and when the screen goes off
 * and the lock comes down.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "power_policy.h"

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
    } else {
        printf("ok   %s\n", what);
    }
}

static int label_is(int seconds, const char *want)
{
    char buf[16];

    power_option_label(seconds, buf, sizeof(buf));
    return strcmp(buf, want) == 0;
}

int main(void)
{
    struct power_policy p = { 0, 0 };
    int s = -7;
    int i;

    /* ---- the options ---- */
    check("screen off: 30 s, 1, 2, 5, 10 min and never",
          power_option_count(POWER_TIMER_SCREEN) == 6 && power_option_at(POWER_TIMER_SCREEN, 0) == 30 &&
              power_option_at(POWER_TIMER_SCREEN, 4) == 600 && power_option_at(POWER_TIMER_SCREEN, 5) == 0);
    check("lock: 1, 2, 5, 10, 30 min and never",
          power_option_count(POWER_TIMER_LOCK) == 6 && power_option_at(POWER_TIMER_LOCK, 0) == 60 &&
              power_option_at(POWER_TIMER_LOCK, 4) == 1800 && power_option_at(POWER_TIMER_LOCK, 5) == 0);
    check("an index past the end is no option", power_option_at(POWER_TIMER_SCREEN, 6) == -1 &&
                                                    power_option_at(POWER_TIMER_SCREEN, -1) == -1);
    for (i = 1; i < power_option_count(POWER_TIMER_SCREEN) - 1; i++) {
        if (power_option_at(POWER_TIMER_SCREEN, i) <= power_option_at(POWER_TIMER_SCREEN, i - 1)) {
            break;
        }
    }
    check("screen options grow up to never", i == power_option_count(POWER_TIMER_SCREEN) - 1);
    check("never is an option of both", power_option_index(POWER_TIMER_SCREEN, 0) == 5 &&
                                             power_option_index(POWER_TIMER_LOCK, 0) == 5);
    check("45 s is not an option", power_option_index(POWER_TIMER_SCREEN, 45) == -1);
    check("30 s is not a lock option (the lock waits at least a minute)",
          power_option_index(POWER_TIMER_LOCK, 30) == -1);

    /* ---- stepping ---- */
    check("+ from 1 min is 2 min", power_option_step(POWER_TIMER_SCREEN, 60, 1) == 120);
    check("- from 2 min is 1 min", power_option_step(POWER_TIMER_SCREEN, 120, -1) == 60);
    check("+ from 10 min is never", power_option_step(POWER_TIMER_SCREEN, 600, 1) == 0);
    check("+ at never stays never", power_option_step(POWER_TIMER_SCREEN, 0, 1) == 0);
    check("- from never is 10 min", power_option_step(POWER_TIMER_SCREEN, 0, -1) == 600);
    check("- at 30 s stays 30 s", power_option_step(POWER_TIMER_SCREEN, 30, -1) == 30);
    check("a value that is no option steps to the first", power_option_step(POWER_TIMER_LOCK, 45, 1) == 60);
    check("lock + from 10 min is 30 min", power_option_step(POWER_TIMER_LOCK, 600, 1) == 1800);

    /* ---- labels ---- */
    check("labels: Never, 30 s, 1 min, 30 min",
          label_is(0, "Never") && label_is(30, "30 s") && label_is(60, "1 min") && label_is(1800, "30 min"));

    /* ---- stored values ---- */
    check("a stored option is read", power_parse_setting(POWER_TIMER_SCREEN, "120", &s) == 0 && s == 120);
    s = -7;
    check("0 is never", power_parse_setting(POWER_TIMER_LOCK, "0", &s) == 0 && s == 0);
    s = -7;
    check("not an option: refused, nothing set", power_parse_setting(POWER_TIMER_SCREEN, "45", &s) < 0 && s == -7);
    check("a sign is refused", power_parse_setting(POWER_TIMER_SCREEN, "+60", &s) < 0);
    check("a space is refused", power_parse_setting(POWER_TIMER_SCREEN, "60 ", &s) < 0);
    check("empty and NULL are refused",
          power_parse_setting(POWER_TIMER_SCREEN, "", &s) < 0 && power_parse_setting(POWER_TIMER_SCREEN, NULL, &s) < 0);
    check("a long number is refused", power_parse_setting(POWER_TIMER_LOCK, "00000060", &s) < 0);
    check("a lock time is not a screen time", power_parse_setting(POWER_TIMER_SCREEN, "1800", &s) < 0);

    /* ---- what is due ---- */
    check("never and never: nothing, however long", power_policy_due(&p, 86400000u, false, false, false) == 0);
    p.screen_off_s = 60;
    p.lock_s = 300;
    check("before a minute: nothing", power_policy_due(&p, 59999u, false, false, false) == 0);
    check("at a minute: the screen", power_policy_due(&p, 60000u, false, false, false) == POWER_DO_SCREEN_OFF);
    check("an off screen is not turned off again", power_policy_due(&p, 61000u, false, true, false) == 0);
    check("at five minutes: the lock as well",
          power_policy_due(&p, 300000u, false, false, false) == (POWER_DO_SCREEN_OFF | POWER_DO_LOCK));
    check("a locked device is not locked again", power_policy_due(&p, 300000u, false, true, true) == 0);
    check("held awake: nothing at all", power_policy_due(&p, 999999u, true, false, false) == 0);
    p.screen_off_s = 0;
    check("screen never, lock in 5 min: only the lock",
          power_policy_due(&p, 300000u, false, false, false) == POWER_DO_LOCK);
    check("no policy: nothing", power_policy_due(NULL, 999999u, false, false, false) == 0);

    printf("power_policy_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
