/*
 * peerd replay window (services/peerd/peer_replay.h, ADR-017 section 6):
 * each rule by name, then a model check - a long random stream held against
 * a plain record of what was accepted.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_replay.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* Offer a frame: 1 when it was accepted. */
static int offer(struct pr_window *w, uint64_t epoch, uint64_t seq)
{
    enum pr_verdict v = pr_check(w, epoch, seq);

    if (v > 0) {
        pr_commit(w, epoch, seq);
        return 1;
    }
    return 0;
}

static void test_rules(void)
{
    struct pr_window w;
    int i;
    int ok;

    memset(&w, 0, sizeof(w));
    check("replay: epoch 0 is invalid", pr_check(&w, 0, 1) == PR_INVALID);
    check("replay: sequence 0 is invalid", pr_check(&w, 1, 0) == PR_INVALID);
    check("replay: the first frame is newer", pr_check(&w, 1, 1) == PR_NEWER);
    check("replay: checking changes nothing", w.epoch == 0 && w.top == 0 && w.map == 0);
    check("replay: it is accepted once", offer(&w, 1, 1) && !offer(&w, 1, 1));
    check("replay: the next is accepted", offer(&w, 1, 2) && w.top == 2);
    check("replay: a gap is fine, and what it skipped is still welcome",
          offer(&w, 1, 10) && pr_check(&w, 1, 5) == PR_WITHIN && offer(&w, 1, 5));
    check("replay: but only once", !offer(&w, 1, 5) && pr_check(&w, 1, 5) == PR_REPLAY);
    check("replay: one inside the window does not move its top", w.top == 10);
    check("replay: 63 behind the top is the oldest still accepted",
          offer(&w, 1, 100) && pr_check(&w, 1, 37) == PR_WITHIN && pr_check(&w, 1, 36) == PR_REPLAY);
    check("replay: a jump of exactly 64 clears the map and keeps the new top",
          offer(&w, 1, 164) && w.top == 164 && w.map == 1 && pr_check(&w, 1, 100) == PR_REPLAY &&
          pr_check(&w, 1, 101) == PR_WITHIN);
    check("replay: a jump of far more than 64 is no different",
          offer(&w, 1, 1000000) && w.map == 1 && !offer(&w, 1, 164));
    check("replay: an older epoch is refused whatever its sequence",
          offer(&w, 5, 3) && !offer(&w, 4, 999999999) && !offer(&w, 1, 1000001));
    check("replay: a newer epoch restarts the window", w.epoch == 5 && w.top == 3 && w.map == 1 &&
          pr_check(&w, 5, 1) == PR_WITHIN);
    check("replay: the largest values are handled",
          offer(&w, UINT64_MAX, UINT64_MAX) && !offer(&w, UINT64_MAX, UINT64_MAX) &&
          pr_check(&w, UINT64_MAX, UINT64_MAX - 63) == PR_WITHIN &&
          pr_check(&w, UINT64_MAX, UINT64_MAX - 64) == PR_REPLAY);

    memset(&w, 0, sizeof(w));
    ok = 1;
    for (i = 1; i <= 64; i++) {
        ok &= offer(&w, 2, (uint64_t)i);
    }
    check("replay: sixty-four in a row fill the map", ok && w.map == UINT64_MAX && w.top == 64);
    ok = 1;
    for (i = 1; i <= 64; i++) {
        ok &= !offer(&w, 2, (uint64_t)i);
    }
    check("replay: and none of them is accepted again", ok);

    pr_commit(&w, 2, 3);
    check("replay: committing a replay changes nothing", w.top == 64 && w.map == UINT64_MAX);
    pr_commit(&w, 0, 5);
    check("replay: nor does committing something invalid", w.epoch == 2 && w.top == 64);
}

static void test_restore(void)
{
    struct pr_window w;

    pr_restore(&w, 9, 500);
    check("restore: everything at or below the mark counts as seen",
          pr_check(&w, 9, 500) == PR_REPLAY && pr_check(&w, 9, 499) == PR_REPLAY &&
          pr_check(&w, 9, 437) == PR_REPLAY && pr_check(&w, 9, 1) == PR_REPLAY);
    check("restore: what follows the mark is accepted", pr_check(&w, 9, 501) == PR_NEWER);
    check("restore: an older epoch is refused, a newer one accepted from 1",
          pr_check(&w, 8, 100000) == PR_REPLAY && pr_check(&w, 10, 1) == PR_NEWER);
    pr_restore(&w, 0, 0);
    check("restore: no floor at all accepts a first frame", pr_check(&w, 1, 1) == PR_NEWER && w.map == 0);
    pr_restore(&w, 4, 0);
    check("restore: an epoch with no mark refuses only older epochs",
          pr_check(&w, 4, 1) == PR_NEWER && pr_check(&w, 3, 9) == PR_REPLAY);
}

/* A random stream of sequences around a moving point, against a record of
 * what was accepted: nothing is accepted twice, and nothing that should be
 * accepted (new and within 63 of the highest so far) is refused. */
static void test_model(void)
{
    enum { SPAN = 120000 };
    static uint8_t seen[SPAN];
    struct pr_window w;
    uint32_t rng = 12345;
    uint64_t highest = 0;
    int twice = 0;
    int wrongly_refused = 0;
    int wrongly_accepted = 0;
    int accepted = 0;
    int i;

    memset(&w, 0, sizeof(w));
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < 200000; i++) {
        uint64_t seq;
        int should;
        int got;

        rng = rng * 1664525u + 1013904223u;
        /* Mostly near the front, sometimes far back, sometimes a leap. */
        seq = highest + 1;
        switch ((rng >> 24) % 8) {
        case 0:
            seq = highest > 200 ? highest - (rng >> 8) % 200 : 1;
            break;
        case 1:
            /* now and then a leap, some of them past the whole window */
            seq = highest + 1 + ((rng >> 4) % 16 == 0 ? (rng >> 8) % 90 : (rng >> 8) % 3);
            break;
        default:
            seq = highest > 70 ? highest - 70 + (rng >> 8) % 75 : 1 + (rng >> 8) % 5;
            break;
        }
        if (seq == 0 || seq >= SPAN) {
            continue;
        }
        should = !seen[seq] && (seq > highest || highest - seq < PR_WINDOW);
        got = offer(&w, 3, seq);
        if (got && seen[seq]) {
            twice++;
        }
        wrongly_refused += should && !got;
        wrongly_accepted += !should && got;
        if (got) {
            seen[seq] = 1;
            accepted++;
            if (seq > highest) {
                highest = seq;
            }
        }
    }
    printf("     %d accepted of a stream of 200000, reaching sequence %llu\n", accepted,
           (unsigned long long)highest);
    check("model: nothing was accepted twice", twice == 0);
    check("model: nothing the rule accepts was refused", wrongly_refused == 0);
    check("model: nothing the rule refuses was accepted", wrongly_accepted == 0);
    check("model: and the stream did exercise it", accepted > 1000 && highest > 1000);
}

int main(void)
{
    test_rules();
    test_restore();
    test_model();
    printf("peer_replay_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
