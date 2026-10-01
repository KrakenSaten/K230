/*
 * PocketRadar vocabulary test: the class table, the name tables and the
 * integer scope geometry.
 *
 * The geometry checks are the load-bearing ones. Touch picking, spawning
 * and drift all run through radar_bearing_wrap(), radar_bearing_delta() and
 * radar_polar_dist2(), and all three have to behave across the 0/3600 seam
 * and stay inside 32 bits without a single floating-point operation.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "radar_types.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_class_table(void)
{
    int named = 1;
    int positive_ttl = 1;
    int targets = 0;
    int i;

    for (i = 0; i < RADAR_CLASS_COUNT; i++) {
        enum radar_class cls = (enum radar_class)i;

        named &= radar_class_name(cls)[0] != '?';
        positive_ttl &= radar_class_base_ttl(cls) > 0;
        targets += radar_class_is_target(cls);
    }
    check("every class has a name", named);
    check("every class has a lifetime", positive_ttl);
    check("exactly one class is not a target", targets == RADAR_CLASS_COUNT - 1);

    check("the decoy is not a target", !radar_class_is_target(RADAR_CLASS_DECOY));
    check("the decoy is the only class that costs points",
          radar_class_base_points(RADAR_CLASS_DECOY) < 0 &&
          radar_class_base_points(RADAR_CLASS_NORMAL) > 0 &&
          radar_class_base_points(RADAR_CLASS_FAST) > 0 &&
          radar_class_base_points(RADAR_CLASS_HIGH_VALUE) > 0);

    /* The shape the design asks for: fast is short-lived and worth more
     * than normal, high value is worth the most. A change that breaks the
     * ordering changes the game, so it has to be deliberate. */
    check("a fast contact is shorter-lived than a normal one",
          radar_class_base_ttl(RADAR_CLASS_FAST) < radar_class_base_ttl(RADAR_CLASS_NORMAL));
    check("a fast contact is worth more than a normal one",
          radar_class_base_points(RADAR_CLASS_FAST) >
          radar_class_base_points(RADAR_CLASS_NORMAL));
    check("a high-value contact is worth the most",
          radar_class_base_points(RADAR_CLASS_HIGH_VALUE) >
          radar_class_base_points(RADAR_CLASS_FAST));

    check("an out-of-range class is refused",
          radar_class_name(RADAR_CLASS_COUNT)[0] == '?' &&
          radar_class_base_ttl(RADAR_CLASS_COUNT) == 0 &&
          radar_class_base_points(RADAR_CLASS_COUNT) == 0 &&
          !radar_class_is_target(RADAR_CLASS_COUNT));
    check("a negative class is refused",
          radar_class_name((enum radar_class)-1)[0] == '?' &&
          !radar_class_is_target((enum radar_class)-1));
}

static void test_name_tables(void)
{
    int named = 1;
    int i;

    for (i = 0; i < RADAR_CONTACT_STATE_COUNT; i++) {
        named &= radar_contact_state_name((enum radar_contact_state)i)[0] != '?';
    }
    for (i = 0; i < RADAR_RUN_STATE_COUNT; i++) {
        named &= radar_run_state_name((enum radar_run_state)i)[0] != '?';
    }
    for (i = 0; i < RADAR_ENGAGE_COUNT; i++) {
        named &= radar_engage_name((enum radar_engage)i)[0] != '?';
    }
    for (i = 0; i < RADAR_EVENT_COUNT; i++) {
        named &= radar_event_name((enum radar_event_type)i)[0] != '?';
    }
    check("every state, outcome and event has a name", named);
    check("out-of-range names are refused",
          radar_contact_state_name(RADAR_CONTACT_STATE_COUNT)[0] == '?' &&
          radar_run_state_name(RADAR_RUN_STATE_COUNT)[0] == '?' &&
          radar_engage_name(RADAR_ENGAGE_COUNT)[0] == '?' &&
          radar_event_name(RADAR_EVENT_COUNT)[0] == '?');
}

static void test_bearing_wrap(void)
{
    int folded = 1;
    int b;

    for (b = -3 * RADAR_BEARING_MAX; b <= 3 * RADAR_BEARING_MAX; b++) {
        int w = radar_bearing_wrap(b);

        folded &= w >= 0 && w < RADAR_BEARING_MAX;
        folded &= (w - b) % RADAR_BEARING_MAX == 0;
    }
    check("any bearing folds into one turn", folded);
    check("the seam folds to zero",
          radar_bearing_wrap(RADAR_BEARING_MAX) == 0 &&
          radar_bearing_wrap(-RADAR_BEARING_MAX) == 0);
    check("a negative bearing folds forwards", radar_bearing_wrap(-1) == 3599);
}

static void test_bearing_delta(void)
{
    int bounded = 1;
    int consistent = 1;
    int a;

    for (a = 0; a < RADAR_BEARING_MAX; a += 7) {
        int b;

        for (b = 0; b < RADAR_BEARING_MAX; b += 13) {
            int d = radar_bearing_delta(a, b);

            bounded &= d > -RADAR_BEARING_MAX / 2 && d <= RADAR_BEARING_MAX / 2;
            consistent &= radar_bearing_wrap(b + d) == a;
        }
    }
    check("a delta is the shortest way round", bounded);
    check("a delta added back reaches the target", consistent);

    check("a delta crosses the seam the short way",
          radar_bearing_delta(10, 3590) == 20 && radar_bearing_delta(3590, 10) == -20);
    check("the opposite bearing is the positive half turn",
          radar_bearing_delta(1800, 0) == 1800 && radar_bearing_delta(0, 1800) == 1800);
    check("a delta is zero for the same bearing",
          radar_bearing_delta(1234, 1234) == 0 &&
          radar_bearing_delta(1234, 1234 + RADAR_BEARING_MAX) == 0);
}

static void test_bearing_name(void)
{
    char buf[RADAR_BEARING_NAME_MAX];
    char tiny[2];

    check("a bearing reads as three digits",
          radar_bearing_name(450, buf, sizeof(buf)) == 0 && strcmp(buf, "045") == 0);
    check("north reads as 000",
          radar_bearing_name(0, buf, sizeof(buf)) == 0 && strcmp(buf, "000") == 0);
    check("the last decidegree still reads as 359",
          radar_bearing_name(3599, buf, sizeof(buf)) == 0 && strcmp(buf, "359") == 0);
    check("an unwrapped bearing is folded first",
          radar_bearing_name(-900, buf, sizeof(buf)) == 0 && strcmp(buf, "270") == 0);
    check("a short buffer is refused safely",
          radar_bearing_name(450, tiny, sizeof(tiny)) == -1 && strcmp(tiny, "?") == 0);
    check("a null buffer is refused", radar_bearing_name(450, NULL, 4) == -1);
}

static void test_polar_distance(void)
{
    int symmetric = 1;
    int a;

    check("a point is at no distance from itself",
          radar_polar_dist2(900, 500, 900, 500) == 0);
    check("a pure range difference is its square",
          radar_polar_dist2(900, 500, 900, 400) == 100 * 100);

    /* A quarter turn at the rim is a quarter circumference, so the arc
     * metric should report about 1000 * pi / 2 = 1570.8. The integer
     * division truncates that to 1570, which is the number
     * docs/apps/POCKETRADAR.md quotes; it is pinned here so the two cannot
     * drift apart. */
    {
        int32_t d2 = radar_polar_dist2(0, 1000, 900, 1000);
        int32_t arc = 0;

        while ((arc + 1) * (arc + 1) <= d2) {
            arc++;
        }
        if (arc < 1560 || arc > 1580) {
            printf("     quarter turn measured %d, expected about 1570.8\n", (int)arc);
        }
        check("a quarter turn at the rim is a quarter circumference",
              arc >= 1560 && arc <= 1580);
        check("the documented worst-case truncation is still 1570", arc == 1570);
    }

    /* The same angle subtends a shorter arc closer in, which is why a
     * contact near the hub is easier to pick. */
    check("the same angle is a shorter arc closer in",
          radar_polar_dist2(0, 200, 300, 200) < radar_polar_dist2(0, 900, 300, 900));

    for (a = 0; a < RADAR_BEARING_MAX; a += 37) {
        symmetric &= radar_polar_dist2(a, 700, 0, 300) == radar_polar_dist2(0, 300, a, 700);
    }
    check("the metric is symmetric in its arguments", symmetric);

    check("the seam is not a wall",
          radar_polar_dist2(3590, 900, 10, 900) == radar_polar_dist2(10, 900, 30, 900));

    /* Out-of-range input is clamped rather than allowed to overflow. */
    check("an out-of-range point is clamped",
          radar_polar_dist2(0, 99999, 0, -5) == RADAR_RANGE_MAX * RADAR_RANGE_MAX);
}

int main(void)
{
    test_class_table();
    test_name_tables();
    test_bearing_wrap();
    test_bearing_delta();
    test_bearing_name();
    test_polar_distance();

    printf("radar_types_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
