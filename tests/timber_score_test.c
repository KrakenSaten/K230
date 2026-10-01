/*
 * PocketTimber scoring test: every number stated twice, once in
 * timber_score.h and once here, so a tuning change is a deliberate edit in
 * two places; the streak and its cap; the layer bonus; the record.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_score.h"

#include <stdio.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void test_values(void)
{
    check("base points rise with the class",
          timber_score_base(TIMBER_CLASS_FREE) == 60 && timber_score_base(TIMBER_CLASS_EASY) == 100 &&
          timber_score_base(TIMBER_CLASS_FIRM) == 180 && timber_score_base(TIMBER_CLASS_STUCK) == 300 &&
          timber_score_base(TIMBER_CLASS_COUNT) == 0);
    check("a free block from just under the top, jolted, tested, no streak, is its base",
          timber_score_value(TIMBER_CLASS_FREE, 0, 0, 1, 0) == 60);
    check("sixteen layers above add sixty-four percent",
          timber_score_value(TIMBER_CLASS_FREE, 16, 0, 1, 0) == 98);
    check("a clean pull is half as much again",
          timber_score_value(TIMBER_CLASS_FREE, 16, 1, 1, 0) == 147);
    check("an untested block is a quarter more",
          timber_score_value(TIMBER_CLASS_FREE, 16, 1, 0, 0) == 183);
    check("a streak of one adds a fifth",
          timber_score_value(TIMBER_CLASS_FREE, 16, 1, 0, 1) == 219);
    check("the streak stops doubling at five",
          timber_score_value(TIMBER_CLASS_FREE, 16, 1, 0, 5) == 366 &&
          timber_score_value(TIMBER_CLASS_FREE, 16, 1, 0, 9) == 366);
    check("the best pull in the game: a stuck bottom block, clean, untested, on a full streak",
          timber_score_value(TIMBER_CLASS_STUCK, 17, 1, 0, 5) == 1890);
    check("an out-of-range class is worth nothing",
          timber_score_value(TIMBER_CLASS_COUNT, 17, 1, 0, 5) == 0);
    check("negative depth and streak are treated as zero",
          timber_score_value(TIMBER_CLASS_EASY, -3, 0, 1, -2) == 100);
}

static void test_pulls(void)
{
    struct timber_score s;

    timber_score_init(&s);
    check("a fresh score is empty",
          s.points == 0 && s.streak == 0 && s.best_streak == 0 && s.pulls == 0 && s.clean == 0 &&
          s.layers_built == 0 && s.height == 0);
    check("a clean pull scores and starts a streak",
          timber_score_pull(&s, TIMBER_CLASS_FREE, 16, 1, 0) == 183 && s.points == 183 &&
          s.streak == 1 && s.best_streak == 1 && s.pulls == 1 && s.clean == 1);
    check("the next clean pull is worth the standing streak",
          timber_score_pull(&s, TIMBER_CLASS_FREE, 16, 1, 0) == 219 && s.points == 402 && s.streak == 2);
    /* FIRM 180, ten layers above: 252; jolted and tested add nothing; the
     * streak of two standing adds forty percent: 352. */
    check("a jolted pull still scores, on the standing streak, but ends it",
          timber_score_pull(&s, TIMBER_CLASS_FIRM, 10, 0, 1) == 352 && s.points == 754 &&
          s.streak == 0 && s.best_streak == 2 && s.pulls == 3 && s.clean == 2);
    check("a completed layer is a flat bonus",
          timber_score_layer(&s) == 250 && s.points == 1004 && s.layers_built == 1);
    timber_score_height(&s, 19);
    timber_score_height(&s, 18);
    check("the height keeps the tallest", s.height == 19);
    timber_score_height(&s, -1);
    timber_score_height(&s, 999);
    check("an impossible height is ignored", s.height == 19);
    check("NULL is safe",
          timber_score_pull(NULL, TIMBER_CLASS_FREE, 0, 1, 0) == 0 && timber_score_layer(NULL) == 0);
    timber_score_init(NULL);
    timber_score_height(NULL, 5);
}

static void test_record(void)
{
    struct timber_record r;
    struct timber_score s;

    timber_record_init(&r);
    timber_score_init(&s);
    timber_score_pull(&s, TIMBER_CLASS_STUCK, 17, 1, 0);
    timber_score_pull(&s, TIMBER_CLASS_STUCK, 17, 1, 0);
    timber_score_height(&s, 19);
    check("the first run is a best",
          timber_record_note_run(&r, &s) == 1 && r.best_score == (uint32_t)s.points &&
          r.best_height == 19 && r.best_streak == 2 && r.runs == 1 && r.pulls == 2);
    timber_score_init(&s);
    timber_score_pull(&s, TIMBER_CLASS_FREE, 0, 0, 1);
    check("a worse run is counted but is not a best",
          timber_record_note_run(&r, &s) == 0 && r.best_score > 60 && r.runs == 2 && r.pulls == 3 &&
          r.best_height == 19);
    check("NULL is safe",
          timber_record_note_run(NULL, &s) == 0 && timber_record_note_run(&r, NULL) == 0);
    timber_record_init(NULL);
}

int main(void)
{
    test_values();
    test_pulls();
    test_record();

    printf("timber_score_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
