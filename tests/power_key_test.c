/*
 * The power key's press timing (ui/shell/power_key.h): short and long
 * presses, the threshold on both sides, the release after a long press,
 * repeats and duplicate events, and a press ended by lost input.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "power_key.h"

#include <stdio.h>

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

/* Poll every 25 ms from..to (inclusive of to), as the shell's timer would,
 * and count the long presses reported. */
static int polls(struct power_key *k, uint32_t from, uint32_t to)
{
    int longs = 0;
    uint32_t t;

    for (t = from; (int32_t)(to - t) >= 0; t += 25) {
        longs += power_key_poll(k, t) == POWER_KEY_LONG;
    }
    return longs;
}

int main(void)
{
    struct power_key k;
    enum power_key_press r;
    uint32_t base;
    int n;

    /* ---- short presses ---- */
    power_key_init(&k);
    check("idle: up, nothing to report", !power_key_is_down(&k) && power_key_poll(&k, 5000) == POWER_KEY_NONE);
    check("a press reports nothing yet", power_key_input(&k, 1, 1000) == POWER_KEY_NONE && power_key_is_down(&k));
    n = polls(&k, 1000, 1250);
    check("a 250 ms press is short", n == 0 && power_key_input(&k, 0, 1250) == POWER_KEY_SHORT);
    check("and leaves the key up", !power_key_is_down(&k) && k.shorts == 1 && k.longs == 0);

    /* ---- the threshold ---- */
    power_key_init(&k);
    power_key_input(&k, 1, 2000);
    check("1 ms under the threshold: no long yet", polls(&k, 2000, 2000 + POWER_KEY_LONG_MS - 1) == 0);
    check("a release 1 ms under the threshold is short",
          power_key_input(&k, 0, 2000 + POWER_KEY_LONG_MS - 1) == POWER_KEY_SHORT);

    power_key_init(&k);
    power_key_input(&k, 1, 2000);
    check("at the threshold, still held: the long press, once",
          power_key_poll(&k, 2000 + POWER_KEY_LONG_MS) == POWER_KEY_LONG);
    check("the long press is not reported again while held",
          polls(&k, 2000 + POWER_KEY_LONG_MS, 2000 + 4900) == 0 && k.longs == 1);
    check("the release after a long press means nothing", power_key_input(&k, 0, 2000 + 4900) == POWER_KEY_NONE);
    check("no short press was counted for it", k.shorts == 0 && !power_key_is_down(&k));

    /* A hold the loop never got to poll: the release past the threshold is
     * the long press, not a short one. */
    power_key_init(&k);
    power_key_input(&k, 1, 3000);
    check("an unpolled release past the threshold is the long press",
          power_key_input(&k, 0, 3000 + POWER_KEY_LONG_MS + 300) == POWER_KEY_LONG && k.shorts == 0);

    /* ---- one long press per hold, a new one per new hold ---- */
    power_key_init(&k);
    power_key_input(&k, 1, 100);
    n = polls(&k, 100, 1600);
    power_key_input(&k, 0, 1600);
    power_key_input(&k, 1, 1700);
    n += polls(&k, 1700, 3000);
    power_key_input(&k, 0, 3000);
    check("two holds: two long presses", n == 2 && k.longs == 2 && k.shorts == 0);

    /* ---- repeated short presses ---- */
    power_key_init(&k);
    n = 0;
    for (base = 10000; base < 10000 + 5 * 300; base += 300) {
        power_key_input(&k, 1, base);
        polls(&k, base, base + 150);
        n += power_key_input(&k, 0, base + 150) == POWER_KEY_SHORT;
    }
    check("five quick presses are five short presses", n == 5 && k.shorts == 5 && k.longs == 0);

    /* ---- repeats and duplicates ---- */
    power_key_init(&k);
    power_key_input(&k, 1, 500);
    r = power_key_input(&k, 2, 700);
    check("an auto-repeat is ignored", r == POWER_KEY_NONE && k.ignored == 1);
    r = power_key_input(&k, 1, 900);
    check("a second press without a release is ignored", r == POWER_KEY_NONE && k.ignored == 2);
    check("and does not restart the hold", power_key_poll(&k, 500 + POWER_KEY_LONG_MS) == POWER_KEY_LONG);
    power_key_input(&k, 0, 1600);
    check("a release with no press is ignored",
          power_key_input(&k, 0, 1700) == POWER_KEY_NONE && k.ignored == 3 && k.shorts == 0);
    check("an unknown value is ignored", power_key_input(&k, 7, 1800) == POWER_KEY_NONE && !power_key_is_down(&k));
    check("a repeat with no press before it starts no hold",
          power_key_input(&k, 2, 1900) == POWER_KEY_NONE && !power_key_is_down(&k) &&
              power_key_poll(&k, 1900 + 2 * POWER_KEY_LONG_MS) == POWER_KEY_NONE);

    /* ---- lost input ---- */
    power_key_init(&k);
    power_key_input(&k, 1, 100);
    power_key_lost(&k);
    check("input lost mid-press: up, no action", !power_key_is_down(&k) && k.lost == 1);
    check("no long press after the loss", polls(&k, 100, 3000) == 0);
    check("nor a short one from a late release", power_key_input(&k, 0, 3000) == POWER_KEY_NONE);

    power_key_init(&k);
    power_key_input(&k, 1, 100);
    power_key_poll(&k, 100 + POWER_KEY_LONG_MS);
    power_key_lost(&k);
    check("lost after the long press: up, not counted as a lost press", !power_key_is_down(&k) && k.lost == 0);

    /* Back again with the key found down: when it went down is unknown, so
     * the whole hold is swallowed. */
    power_key_init(&k);
    power_key_resync(&k, true);
    check("found down after a loss: swallowed", k.state == POWER_KEY_SWALLOW);
    check("a swallowed hold never turns long", polls(&k, 0, 5000) == 0);
    check("and its release is nothing", power_key_input(&k, 0, 5000) == POWER_KEY_NONE && k.shorts == 0);
    check("the next press is a press again", power_key_input(&k, 1, 6000) == POWER_KEY_NONE &&
                                                 power_key_input(&k, 0, 6100) == POWER_KEY_SHORT);

    /* Events dropped by the kernel, the release among them. */
    power_key_init(&k);
    power_key_input(&k, 1, 100);
    power_key_resync(&k, false);
    check("a release lost in a drop: up, no action", !power_key_is_down(&k) && k.lost == 1 && k.shorts == 0);

    /* Dropped events while the key stays down: the press goes on. */
    power_key_init(&k);
    power_key_input(&k, 1, 100);
    power_key_resync(&k, true);
    check("still down after a drop: the same press", k.state == POWER_KEY_DOWN &&
                                                         power_key_poll(&k, 100 + POWER_KEY_LONG_MS) == POWER_KEY_LONG);

    /* ---- the clock wrapping ---- */
    power_key_init(&k);
    power_key_input(&k, 1, 0xFFFFFF00u);
    check("a hold across the clock's wrap is still timed",
          power_key_poll(&k, 0xFFFFFF00u + 999u) == POWER_KEY_NONE &&
              power_key_poll(&k, 0xFFFFFF00u + POWER_KEY_LONG_MS) == POWER_KEY_LONG);
    power_key_init(&k);
    power_key_input(&k, 1, 5000);
    check("a time before the press is no time, not a long press", power_key_poll(&k, 4000) == POWER_KEY_NONE &&
                                                                     power_key_input(&k, 0, 4000) == POWER_KEY_SHORT);

    check("the threshold is well under the kernel's 5 s power-off", POWER_KEY_LONG_MS <= 2000);
    check("state names", power_key_state_name(POWER_KEY_UP)[0] == 'u' &&
                             power_key_state_name(POWER_KEY_SWALLOW)[0] == 's');

    printf("power_key_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
