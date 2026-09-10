/*
 * PocketTimber pull model test: tightness from seat and load, the class
 * thresholds, the per-class limits and stiction, and the jolt formula.
 *
 * The numbers are stated twice, once in timber_pull.h and once here, so a
 * tuning change is a deliberate edit in two places.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_pull.h"

#include <stdio.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_tightness(void)
{
    int seat;
    int load;
    int monotone_seat = 1;
    int monotone_load = 1;
    int in_range = 1;

    check("a wedged block under the whole tower is fully tight",
          timber_pull_tightness(0, TIMBER_LOAD_MAX) == 256);
    check("a wedged block with nothing on it is half tight",
          timber_pull_tightness(0, 0) == 128);
    check("a free block is never tight",
          timber_pull_tightness(255, 0) == 0 && timber_pull_tightness(255, TIMBER_LOAD_MAX) == 0);
    check("the default seat under full load is half tight",
          timber_pull_tightness(128, TIMBER_LOAD_MAX) == 128);
    for (load = 0; load <= TIMBER_LOAD_MAX; load++) {
        int32_t previous = 1 << 20;

        for (seat = 0; seat < 256; seat++) {
            int32_t t = timber_pull_tightness(seat, load);

            monotone_seat &= t <= previous;
            in_range &= t >= 0 && t <= 256;
            previous = t;
        }
    }
    for (seat = 0; seat < 256; seat++) {
        int32_t previous = -1;

        for (load = 0; load <= TIMBER_LOAD_MAX; load++) {
            int32_t t = timber_pull_tightness(seat, load);

            monotone_load &= t >= previous;
            previous = t;
        }
    }
    check("tightness never rises with a looser seat", monotone_seat);
    check("tightness never falls with more load", monotone_load);
    check("tightness stays within 0 and 1", in_range);
    check("seat and load outside their ranges are clamped",
          timber_pull_tightness(-5, 0) == timber_pull_tightness(0, 0) &&
          timber_pull_tightness(300, 0) == 0 &&
          timber_pull_tightness(0, -1) == 128 &&
          timber_pull_tightness(0, 999) == 256);
}

static void test_classes(void)
{
    check("a free seat is FREE under any load",
          timber_pull_class(255, 0) == TIMBER_CLASS_FREE &&
          timber_pull_class(255, TIMBER_LOAD_MAX) == TIMBER_CLASS_FREE);
    check("a wedged seat under full load is STUCK",
          timber_pull_class(0, TIMBER_LOAD_MAX) == TIMBER_CLASS_STUCK);
    check("a wedged seat with nothing on it is only FIRM",
          timber_pull_class(0, 0) == TIMBER_CLASS_FIRM);
    check("the default seat is FIRM under full load and EASY under none",
          timber_pull_class(128, TIMBER_LOAD_MAX) == TIMBER_CLASS_FIRM &&
          timber_pull_class(128, 0) == TIMBER_CLASS_EASY);
    check("the guaranteed loose seat is EASY even at the bottom",
          timber_pull_class(200, TIMBER_LOAD_MAX) == TIMBER_CLASS_EASY);
    /* The thresholds themselves: tightness at full load is (255-seat)
     * rounded, so seat 218 is 37 (FREE) and 217 is 38 (EASY). */
    check("the FREE / EASY threshold falls where the table says",
          timber_pull_class(218, TIMBER_LOAD_MAX) == TIMBER_CLASS_FREE &&
          timber_pull_class(217, TIMBER_LOAD_MAX) == TIMBER_CLASS_EASY);
    check("the EASY / FIRM threshold falls where the table says",
          timber_pull_class(166, TIMBER_LOAD_MAX) == TIMBER_CLASS_EASY &&
          timber_pull_class(165, TIMBER_LOAD_MAX) == TIMBER_CLASS_FIRM);
    /* Seat 90 is 165.6, which rounds up to the STUCK threshold. */
    check("the FIRM / STUCK threshold falls where the table says",
          timber_pull_class(91, TIMBER_LOAD_MAX) == TIMBER_CLASS_FIRM &&
          timber_pull_class(90, TIMBER_LOAD_MAX) == TIMBER_CLASS_STUCK);
}

static void test_limits(void)
{
    check("limits fall with tightness",
          timber_pull_limit(TIMBER_CLASS_FREE) > timber_pull_limit(TIMBER_CLASS_EASY) &&
          timber_pull_limit(TIMBER_CLASS_EASY) > timber_pull_limit(TIMBER_CLASS_FIRM) &&
          timber_pull_limit(TIMBER_CLASS_FIRM) > timber_pull_limit(TIMBER_CLASS_STUCK) &&
          timber_pull_limit(TIMBER_CLASS_STUCK) > 0);
    check("limits are the tuning table",
          timber_pull_limit(TIMBER_CLASS_FREE) == 110 && timber_pull_limit(TIMBER_CLASS_EASY) == 73 &&
          timber_pull_limit(TIMBER_CLASS_FIRM) == 40 && timber_pull_limit(TIMBER_CLASS_STUCK) == 22);
    check("an unknown class is treated as the tightest",
          timber_pull_limit(TIMBER_CLASS_COUNT) == timber_pull_limit(TIMBER_CLASS_STUCK) &&
          timber_pull_stiction(TIMBER_CLASS_COUNT) == timber_pull_stiction(TIMBER_CLASS_STUCK));
    check("loose blocks move at once, tight ones absorb travel first",
          timber_pull_stiction(TIMBER_CLASS_FREE) == 0 && timber_pull_stiction(TIMBER_CLASS_EASY) == 0 &&
          timber_pull_stiction(TIMBER_CLASS_FIRM) == 73 && timber_pull_stiction(TIMBER_CLASS_STUCK) == 128);
    check("a stuck block takes longer to break than a firm one",
          timber_pull_stiction(TIMBER_CLASS_STUCK) > timber_pull_stiction(TIMBER_CLASS_FIRM));
    check("the slip point is four fifths of a block", TIMBER_SLIP_AT == 614);
}

static void test_jolt(void)
{
    check("no excess is no jolt", timber_pull_jolt(0, 256) == 0 && timber_pull_jolt(-10, 256) == 0);
    check("a free block jolts a quarter of the excess", timber_pull_jolt(100, 0) == 25);
    check("a wedged block jolts one and a quarter times the excess",
          timber_pull_jolt(100, 256) == 125);
    check("the jolt grows with tightness",
          timber_pull_jolt(38, 128) > timber_pull_jolt(38, 0) &&
          timber_pull_jolt(38, 256) > timber_pull_jolt(38, 128));
    check("tightness outside its range is clamped",
          timber_pull_jolt(100, -1) == 25 && timber_pull_jolt(100, 999) == 125);
    check("the worked example from the rules holds: 38 over on a wedged block is 47",
          timber_pull_jolt(38, 256) == 47);
}

int main(void)
{
    test_tightness();
    test_classes();
    test_limits();
    test_jolt();

    printf("timber_pull_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
