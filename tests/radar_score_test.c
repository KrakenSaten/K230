/*
 * PocketRadar scoring test: what an engagement is worth, what the streak
 * multiplier does, what mistakes cost, and the lifetime record.
 *
 * The numbers are asserted as numbers rather than as inequalities. The
 * scoring model is small enough to state exactly, and a change to it is a
 * change to the game, so it should have to be written down twice.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_score.h"

#include <stdio.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void check_value(const char *name, int32_t got, int32_t want)
{
    if (got != want) {
        printf("     got %d, expected %d\n", (int)got, (int)want);
    }
    check(name, got == want);
}

static void test_init(void)
{
    struct radar_score s;

    radar_score_init(&s);
    check("a new score is empty",
          s.points == 0 && s.streak == 0 && s.best_streak == 0 &&
          s.engaged == 0 && s.mistakes == 0 && s.missed == 0);
    check("a new run starts with the sector intact",
          s.integrity == RADAR_INTEGRITY_MAX && !radar_score_spent(&s));
}

static void test_value(void)
{
    /* Base values, on the last tick of a track's life, with no streak. */
    check_value("a normal target on its last tick is worth its base",
                radar_score_value(RADAR_CLASS_NORMAL, 0, 100, 0), 100);
    check_value("a fast target on its last tick is worth its base",
                radar_score_value(RADAR_CLASS_FAST, 0, 100, 0), 150);
    check_value("a high-value target on its last tick is worth its base",
                radar_score_value(RADAR_CLASS_HIGH_VALUE, 0, 100, 0), 300);

    /* Response bonus: the whole of it at full life. */
    check_value("a normal target worked at once is worth half as much again",
                radar_score_value(RADAR_CLASS_NORMAL, 100, 100, 0), 150);
    check_value("a high-value target worked at once carries the same bonus",
                radar_score_value(RADAR_CLASS_HIGH_VALUE, 100, 100, 0), 450);
    check_value("the bonus falls with the life left",
                radar_score_value(RADAR_CLASS_NORMAL, 50, 100, 0), 125);

    /* The streak multiplier, in its 0.2 steps, and its ceiling. */
    check_value("the first engagement carries no multiplier",
                radar_score_value(RADAR_CLASS_NORMAL, 100, 100, 0), 150);
    check_value("one on the streak is 1.2",
                radar_score_value(RADAR_CLASS_NORMAL, 100, 100, 1), 180);
    check_value("five on the streak is 2.0",
                radar_score_value(RADAR_CLASS_NORMAL, 100, 100, 5), 300);
    check_value("the multiplier stops at 2.0",
                radar_score_value(RADAR_CLASS_NORMAL, 100, 100, 50), 300);

    check_value("a decoy is worth nothing to engage",
                radar_score_value(RADAR_CLASS_DECOY, 100, 100, 3), 0);
    check_value("an out-of-range class is worth nothing",
                radar_score_value(RADAR_CLASS_COUNT, 100, 100, 0), 0);
    check_value("a track with no lifetime is worth nothing",
                radar_score_value(RADAR_CLASS_NORMAL, 0, 0, 0), 0);
    check_value("more life left than a track ever had is capped",
                radar_score_value(RADAR_CLASS_NORMAL, 500, 100, 0), 150);
    check_value("a negative lifetime is refused",
                radar_score_value(RADAR_CLASS_NORMAL, -1, 100, 0), 0);
}

static void test_hit(void)
{
    struct radar_score s;
    int32_t first;
    int32_t second;

    radar_score_init(&s);
    first = radar_score_hit(&s, RADAR_CLASS_NORMAL, 100, 100);
    check_value("the first hit scores its plain value", first, 150);
    check("the first hit starts the streak", s.streak == 1 && s.best_streak == 1);
    check("the first hit counts", s.engaged == 1 && s.points == 150);
    check("a hit costs no integrity", s.integrity == RADAR_INTEGRITY_MAX);

    second = radar_score_hit(&s, RADAR_CLASS_NORMAL, 100, 100);
    check_value("the second hit carries the streak", second, 180);
    check("the score is the sum", s.points == 330);
    check("the streak keeps count", s.streak == 2 && s.best_streak == 2);

    check_value("engaging a decoy through the hit path scores nothing",
                radar_score_hit(&s, RADAR_CLASS_DECOY, 100, 100), 0);
    check("a refused hit changes nothing",
          s.points == 330 && s.streak == 2 && s.engaged == 2);
}

static void test_foul_and_miss(void)
{
    struct radar_score s;
    int32_t cost;

    radar_score_init(&s);
    radar_score_hit(&s, RADAR_CLASS_HIGH_VALUE, 100, 100);   /* 450 */
    radar_score_hit(&s, RADAR_CLASS_HIGH_VALUE, 100, 100);   /* 540 */
    check("two high-value hits score 990", s.points == 990 && s.streak == 2);

    cost = radar_score_foul(&s);
    check_value("engaging a decoy costs a flat 150", cost, -150);
    check("a foul breaks the streak", s.streak == 0 && s.best_streak == 2);
    check("a foul is counted", s.mistakes == 1);
    check("a foul is taken off the score", s.points == 840);
    check("a foul costs sector integrity",
          s.integrity == RADAR_INTEGRITY_MAX - RADAR_INTEGRITY_FOUL);

    cost = radar_score_miss(&s);
    check_value("a leaker costs a flat 25", cost, -RADAR_SCORE_MISS_PENALTY);
    check("a leaker breaks the streak", s.streak == 0);
    check("a leaker is counted", s.missed == 1);
    check("a leaker costs more integrity than a foul",
          s.integrity == RADAR_INTEGRITY_MAX - RADAR_INTEGRITY_FOUL -
                         RADAR_INTEGRITY_MISS);

    /* The streak restarts from the bottom, and the best is remembered. */
    check_value("the streak restarts at 1.0",
                radar_score_hit(&s, RADAR_CLASS_NORMAL, 0, 100), 100);
    check("the best streak is remembered", s.best_streak == 2);
}

static void test_floor(void)
{
    struct radar_score s;
    int32_t cost;

    /* A penalty larger than the score is reported in full but cannot drive
     * the player into a hole. */
    radar_score_init(&s);
    cost = radar_score_foul(&s);
    check_value("a foul from nothing still reports its full cost", cost, -150);
    check("the score does not go below zero", s.points == 0);

    radar_score_hit(&s, RADAR_CLASS_NORMAL, 0, 100);   /* 100 */
    cost = radar_score_foul(&s);
    check_value("the cost does not shrink to fit the score", cost, -150);
    check("the score floors rather than wrapping", s.points == 0);
}

static void test_integrity(void)
{
    struct radar_score s;
    int i;

    /* Five leakers end a run: that is the number the constants encode. */
    radar_score_init(&s);
    for (i = 0; i < RADAR_INTEGRITY_MAX / RADAR_INTEGRITY_MISS - 1; i++) {
        radar_score_miss(&s);
        check("the sector holds while integrity remains", !radar_score_spent(&s));
    }
    radar_score_miss(&s);
    check("the last leaker spends the sector",
          radar_score_spent(&s) && s.integrity == 0);

    /* It floors rather than wrapping: uint16_t would otherwise become
     * enormous and the run would never end. */
    radar_score_miss(&s);
    check("integrity floors at zero", s.integrity == 0 && radar_score_spent(&s));

    radar_score_init(&s);
    for (i = 0; i < RADAR_INTEGRITY_MAX / RADAR_INTEGRITY_FOUL; i++) {
        radar_score_foul(&s);
    }
    check("fouls alone can spend the sector too", radar_score_spent(&s));
}

static void test_record(void)
{
    struct radar_record r;
    struct radar_score s;

    radar_record_init(&r);
    check("a new record is empty",
          r.best_score == 0 && r.best_streak == 0 && r.best_level == 0 &&
          r.runs == 0 && r.engaged == 0 && r.mistakes == 0);

    radar_score_init(&s);
    radar_score_hit(&s, RADAR_CLASS_NORMAL, 100, 100);   /* 150 */
    radar_score_hit(&s, RADAR_CLASS_NORMAL, 100, 100);   /* 180 */
    radar_score_foul(&s);
    check("the first run is a new best", radar_record_note_run(&r, &s, 4) == 1);
    check("the record takes the run",
          r.best_score == 180 && r.best_streak == 2 && r.best_level == 4 &&
          r.runs == 1 && r.engaged == 2 && r.mistakes == 1);

    /* A worse run adds to the counters but not to the bests. */
    radar_score_init(&s);
    radar_score_hit(&s, RADAR_CLASS_NORMAL, 0, 100);     /* 100 */
    check("a worse run is not a new best", radar_record_note_run(&r, &s, 2) == 0);
    check("the bests are unchanged",
          r.best_score == 180 && r.best_streak == 2 && r.best_level == 4);
    check("the counters keep adding up",
          r.runs == 2 && r.engaged == 3 && r.mistakes == 1);

    /* A better run in only one respect updates only that. */
    radar_score_init(&s);
    radar_score_hit(&s, RADAR_CLASS_HIGH_VALUE, 100, 100);
    radar_score_hit(&s, RADAR_CLASS_HIGH_VALUE, 100, 100);
    radar_score_hit(&s, RADAR_CLASS_HIGH_VALUE, 100, 100);
    check("a better run is a new best", radar_record_note_run(&r, &s, 1) == 1);
    check("a longer streak is taken", r.best_streak == 3);
    check("a lower level does not lower the best", r.best_level == 4);

    /* A run that scored nothing is still a run. */
    radar_score_init(&s);
    check("a scoreless run is not a new best", radar_record_note_run(&r, &s, 0) == 0);
    check("a scoreless run is still counted", r.runs == 4);
}

static void test_null_is_safe(void)
{
    check("NULL is safe",
          radar_score_hit(NULL, RADAR_CLASS_NORMAL, 1, 1) == 0 &&
          radar_score_foul(NULL) == 0 && radar_score_miss(NULL) == 0 &&
          !radar_score_spent(NULL) &&
          radar_record_note_run(NULL, NULL, 1) == 0);
    radar_score_init(NULL);
    radar_record_init(NULL);
}

int main(void)
{
    test_init();
    test_value();
    test_hit();
    test_foul_and_miss();
    test_floor();
    test_integrity();
    test_record();
    test_null_is_safe();

    printf("radar_score_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
