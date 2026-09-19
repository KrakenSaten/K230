/*
 * meshcored: the transmit identity map (services/meshcored/tx_map.c).
 *
 * The awkward cases radiod's asynchronous transmit makes possible, each one
 * asserted rather than assumed away:
 *
 *   - a completion for a transmit this service never had (stale tx_id);
 *   - a second completion for one it did (duplicate tx_done);
 *   - a submission whose reply never comes, because the connection went;
 *   - a tx_id of zero, which a submission awaiting acceptance also carries,
 *     and which must not select it.
 *
 * Each of those, answered wrongly, ends the same way: an outcome attributed
 * to the wrong packet, and a retransmission of something that already went
 * out. Twice the airtime for a bookkeeping mistake.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "tx_map.h"

#include <stdio.h>

static int failed;
static int checks;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    failed += !ok;
}

static void test_happy_path(void)
{
    struct mcd_tx_map m;
    uint64_t s1;
    uint64_t done;

    mcd_tx_map_init(&m);
    check("a fresh map has nothing outstanding", mcd_tx_map_outstanding(&m) == 0);

    s1 = mcd_tx_map_submit(&m, 7, 109, 1000);
    check("a submission gets an id", s1 != 0);
    check("and is outstanding", mcd_tx_map_outstanding(&m) == 1);
    check("its byte count is remembered", mcd_tx_map_bytes(&m, s1) == 109);

    check("the reply is matched by request id",
          mcd_tx_map_accepted(&m, 7, 4242, 754.0, 1000) == s1);
    check("it is still outstanding after acceptance", mcd_tx_map_outstanding(&m) == 1);

    done = mcd_tx_map_completed(&m, 4242);
    check("the completion is matched by tx_id", done == s1);
    check("and the slot is free again", mcd_tx_map_outstanding(&m) == 0);
    check("the byte count is gone with it", mcd_tx_map_bytes(&m, s1) == -1);
}

static void test_ids_are_not_reused(void)
{
    struct mcd_tx_map m;
    uint64_t a;
    uint64_t b;

    mcd_tx_map_init(&m);
    a = mcd_tx_map_submit(&m, 1, 10, 0);
    mcd_tx_map_accepted(&m, 1, 100, 0.0, 0);
    mcd_tx_map_completed(&m, 100);
    b = mcd_tx_map_submit(&m, 2, 10, 0);
    check("a submit id is never handed out twice", a != b);
    check("and it goes up", b > a);
}

static void test_duplicate_and_stale(void)
{
    struct mcd_tx_map m;
    uint64_t s1;

    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 1, 20, 0);
    mcd_tx_map_accepted(&m, 1, 555, 0.0, 0);
    check("the first completion matches", mcd_tx_map_completed(&m, 555) == s1);
    check("a second completion for the same tx_id matches nothing",
          mcd_tx_map_completed(&m, 555) == 0);

    /* And the same, with another transmit in flight: the duplicate must not
     * be attributed to whatever happens to be outstanding now. This is the
     * case that would report the wrong packet as sent. */
    {
        uint64_t s2 = mcd_tx_map_submit(&m, 2, 30, 0);

        mcd_tx_map_accepted(&m, 2, 777, 0.0, 0);
        check("a stale completion does not steal the current transmit",
              mcd_tx_map_completed(&m, 555) == 0);
        check("and the current one still completes normally",
              mcd_tx_map_completed(&m, 777) == s2);
    }

    check("a completion for a tx_id nobody was given matches nothing",
          mcd_tx_map_completed(&m, 999999) == 0);
}

static void test_zero_tx_id(void)
{
    struct mcd_tx_map m;

    mcd_tx_map_init(&m);
    mcd_tx_map_submit(&m, 1, 20, 0);
    /* Awaiting acceptance: tx_id is still 0. A tx_done carrying 0 - a
     * malformed event, or one whose field was missing - must not select it,
     * or a transmit that had not been accepted would be completed. */
    check("a tx_done with tx_id 0 does not match a submission awaiting acceptance",
          mcd_tx_map_completed(&m, 0) == 0);
    check("which is still outstanding", mcd_tx_map_outstanding(&m) == 1);
}

static void test_refusal(void)
{
    struct mcd_tx_map m;
    uint64_t s1;

    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 3, 40, 0);
    check("a refusal is matched by request id", mcd_tx_map_refused(&m, 3) == s1);
    check("and frees the slot", mcd_tx_map_outstanding(&m) == 0);
    check("a second refusal matches nothing", mcd_tx_map_refused(&m, 3) == 0);
    check("a refused submission has no tx_id to complete",
          mcd_tx_map_completed(&m, 0) == 0);
}

static void test_acceptance_of_the_wrong_request(void)
{
    struct mcd_tx_map m;

    mcd_tx_map_init(&m);
    mcd_tx_map_submit(&m, 5, 10, 0);
    check("an acceptance for another request id matches nothing",
          mcd_tx_map_accepted(&m, 6, 123, 0.0, 0) == 0);
    check("the submission is untouched", mcd_tx_map_outstanding(&m) == 1);
    check("a completion for that other tx_id matches nothing",
          mcd_tx_map_completed(&m, 123) == 0);
}

static void test_abandon(void)
{
    struct mcd_tx_map m;
    uint64_t out[MCD_TX_MAP_SLOTS];
    uint64_t a;
    uint64_t b;
    int n;

    mcd_tx_map_init(&m);
    a = mcd_tx_map_submit(&m, 1, 10, 0);
    b = mcd_tx_map_submit(&m, 2, 20, 0);
    mcd_tx_map_accepted(&m, 2, 900, 0.0, 0);

    n = mcd_tx_map_abandon_all(&m, out);
    check("both outstanding submissions are handed back", n == 2);
    check("the one still awaiting acceptance is among them", out[0] == a || out[1] == a);
    check("and so is the one in flight", out[0] == b || out[1] == b);
    check("the map is empty afterwards", mcd_tx_map_outstanding(&m) == 0);
    check("and the in-flight tx_id no longer matches anything",
          mcd_tx_map_completed(&m, 900) == 0);

    n = mcd_tx_map_abandon_all(&m, out);
    check("abandoning an empty map hands back nothing", n == 0);
}

static void test_full(void)
{
    struct mcd_tx_map m;
    int i;
    int taken = 0;

    mcd_tx_map_init(&m);
    for (i = 0; i < MCD_TX_MAP_SLOTS + 4; i++) {
        if (mcd_tx_map_submit(&m, 100 + i, 10, 0) != 0) {
            taken++;
        }
    }
    check("the table takes exactly its slots", taken == MCD_TX_MAP_SLOTS);
    check("and reports them outstanding", mcd_tx_map_outstanding(&m) == MCD_TX_MAP_SLOTS);
    /* A full table says no. It never overwrites a slot: the submission it
     * dropped would be one whose outcome somebody is waiting for. */
    check("a further submission is refused rather than overwriting one",
          mcd_tx_map_submit(&m, 999, 10, 0) == 0);
}

/* The case that made this deadline exist: a completion that never comes.
 * Without it the slot stays taken, every later submission is refused because
 * one is outstanding, and the node goes quiet for the rest of the session
 * with nothing in the log to explain it - which is exactly what
 * tests/meshcored_harness_test.sh found when it was first asked to drop a
 * radio.tx_done. */
static void test_deadline(void)
{
    struct mcd_tx_map m;
    uint64_t out[MCD_TX_MAP_SLOTS];
    uint64_t s1;
    uint64_t s2;

    /* Awaiting acceptance: a reply that never arrives. */
    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 1, 100, 1000);
    check("nothing expires before the acceptance deadline",
          mcd_tx_map_expire(&m, 1000 + MCD_TX_ACCEPT_DEADLINE_MS - 1, out) == 0);
    check("and it is still outstanding", mcd_tx_map_outstanding(&m) == 1);
    check("it expires at the deadline",
          mcd_tx_map_expire(&m, 1000 + MCD_TX_ACCEPT_DEADLINE_MS, out) == 1 &&
          out[0] == s1);
    check("and the slot is free, so the next transmit can go out",
          mcd_tx_map_outstanding(&m) == 0);
    check("an expired submission has no completion left to match",
          mcd_tx_map_completed(&m, 0) == 0);

    /* Accepted, then no completion. The deadline comes from the airtime
     * radiod reported, so a long packet gets a long bound. */
    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 1, 255, 0);
    mcd_tx_map_accepted(&m, 1, 77, 1500.0, 0);
    check("acceptance pushes the deadline past the acceptance one",
          mcd_tx_map_expire(&m, MCD_TX_ACCEPT_DEADLINE_MS + 1, out) == 0);
    check("and it holds for three times the airtime plus slack",
          mcd_tx_map_expire(&m, (uint64_t)(1500 * 3) + MCD_TX_FLIGHT_SLACK_MS - 1,
                            out) == 0);
    check("then expires",
          mcd_tx_map_expire(&m, (uint64_t)(1500 * 3) + MCD_TX_FLIGHT_SLACK_MS, out) == 1 &&
          out[0] == s1);

    /* A short packet still gets the floor, not a deadline of milliseconds. */
    mcd_tx_map_init(&m);
    mcd_tx_map_submit(&m, 1, 10, 0);
    mcd_tx_map_accepted(&m, 1, 88, 5.0, 0);
    check("a short transmit still gets the minimum deadline",
          mcd_tx_map_expire(&m, MCD_TX_MIN_FLIGHT_DEADLINE_MS - 1, out) == 0);
    check("and expires at it",
          mcd_tx_map_expire(&m, MCD_TX_MIN_FLIGHT_DEADLINE_MS, out) == 1);

    /* A completion that arrives before the deadline is the ordinary case and
     * must not be pre-empted by it. */
    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 1, 100, 0);
    mcd_tx_map_accepted(&m, 1, 99, 700.0, 0);
    check("a completion inside the deadline is matched", mcd_tx_map_completed(&m, 99) == s1);
    check("and nothing expires afterwards", mcd_tx_map_expire(&m, 1000000, out) == 0);

    /* Two submissions with different deadlines expire independently. */
    mcd_tx_map_init(&m);
    s1 = mcd_tx_map_submit(&m, 1, 100, 0);
    s2 = mcd_tx_map_submit(&m, 2, 100, 3000);
    check("the older one expires first",
          mcd_tx_map_expire(&m, MCD_TX_ACCEPT_DEADLINE_MS, out) == 1 && out[0] == s1);
    check("and the newer one is still outstanding", mcd_tx_map_outstanding(&m) == 1);
    check("until its own deadline",
          mcd_tx_map_expire(&m, 3000 + MCD_TX_ACCEPT_DEADLINE_MS, out) == 1 &&
          out[0] == s2);
    check("and then the table is empty", mcd_tx_map_outstanding(&m) == 0);
}

int main(void)
{
    test_happy_path();
    test_deadline();
    test_ids_are_not_reused();
    test_duplicate_and_stale();
    test_zero_tx_id();
    test_refusal();
    test_acceptance_of_the_wrong_request();
    test_abandon();
    test_full();
    printf("meshcored_txmap_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
