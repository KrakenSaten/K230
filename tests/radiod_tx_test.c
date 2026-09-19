/*
 * radiod's transmit state machine, lease and clocks, driven directly against
 * the mock backend: no socket, no daemon, no poll loop.
 *
 * Most of what is checked here is the absence of something, and those are
 * hard to see from the outside. A completion that arrives twice looks like a
 * completion. A completion that never arrives looks like a slow one. A
 * transmit whose payload pointer died looks fine until the bytes on the air
 * are the wrong ones. So the cases below are written as the mutations they
 * would catch, and each one says what going wrong would look like in the
 * field.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "lease.h"
#include "radio_backend.h"
#include "tx.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failed;

static void check(const char *label, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", label);
    failed += !ok;
}

/* ---- harness ------------------------------------------------------------ */

struct recorder {
    int calls;
    struct radio_tx_done last;
};

static void on_done(const struct radio_tx_done *d, void *user)
{
    struct recorder *r = user;

    r->calls++;
    r->last = *d;
}

struct rig {
    struct radio_backend be;
    struct radio_tx tx;
    struct recorder rec;
};

static void rig_up(struct rig *g)
{
    char err[128] = "";

    memset(g, 0, sizeof(*g));
    g->be.ops = &radio_backend_mock_ops;
    if (g->be.ops->init(&g->be, err, sizeof(err)) < 0) {
        printf("FAIL mock init: %s\n", err);
        failed++;
        return;
    }
    /* The EU868 default profile radiod starts with; SF7 BW125 CR4/5 makes a
     * 10-byte packet 41.216 ms, the value tests/airtime_test.c pins. */
    g->be.profile.frequency_mhz = 869.525;
    g->be.profile.bandwidth_khz = 125.0;
    g->be.profile.spreading_factor = 7;
    g->be.profile.coding_rate = 5;
    g->be.profile.sync_word = 0x12;
    g->be.profile.preamble_length = 8;
    g->be.profile.tx_power_dbm = 2;
    g->be.profile.crc = true;
    radio_tx_init(&g->tx, &g->be, on_done, &g->rec);
}

static void rig_down(struct rig *g)
{
    g->be.ops->shutdown(&g->be);
}

static void knob(struct rig *g, const char *key, int value)
{
    if (g->be.ops->debug_set(&g->be, key, value) < 0) {
        printf("FAIL unknown mock knob %s\n", key);
        failed++;
    }
}

static const uint8_t PAYLOAD[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

/* ---- the two clocks ----------------------------------------------------- */

/* Relabelling CLOCK_REALTIME as monotonic costs one character and passes any
 * test that only checks the field is present and increasing - a wall clock
 * increases too, right up to the moment NTP sets it. On this board that
 * happens on every boot, because it starts in 1970. So the check is that the
 * two functions read genuinely different clocks. */
static void test_clocks(void)
{
    uint64_t wall = radio_now_ms();
    uint64_t mono = radio_mono_ms();
    uint64_t prev;
    int i;

    /* The wall clock counts from 1970 and the monotonic one from boot, so
     * the gap between them is the wall-clock time the machine booted. Any
     * date after 2020 is more than 1.5e12 ms; a machine up for nineteen
     * years would be needed to close that gap, and this would then be the
     * least of its problems. */
    check("radio_now_ms is a wall clock and radio_mono_ms is not",
          wall > mono && wall - mono > 1500000000000ULL);
    check("radio_mono_ms is uptime, not an epoch", mono < 1000000000000ULL);

    prev = radio_mono_ms();
    for (i = 0; i < 2000; i++) {
        uint64_t now = radio_mono_ms();

        if (now < prev) {
            check("radio_mono_ms never runs backwards", 0);
            return;
        }
        prev = now;
    }
    check("radio_mono_ms never runs backwards", 1);
}

/* A monotonic clock passes 2^32 ms after 49.7 days of uptime. Everything on
 * the way to a client has to carry that: the C type, and the JSON number it
 * is printed as. A uint32_t or an int anywhere in the chain turns a
 * timestamp into one about 49 days in the past, and a protocol daemon
 * subtracting two of them across the wrap gets an interval of weeks where it
 * should have milliseconds.
 *
 * The upper bound is the JSON printer's, not the type's. cJSON prints with
 * %1.15g and keeps that if it round-trips within a tolerance that scales
 * with the magnitude, so integers of more than 15 significant digits can
 * come back off by one: 2^53-1 prints as 9.00719925474099e+15, which is
 * 9007199254740990. That bound is 10^15 ms, or about 31700 years of uptime,
 * and it is recorded here so it is a known limit rather than a surprise.
 * The values that matter - anything a board can actually reach - are exact,
 * and that is what is asserted. */
static void test_wide_timestamps(void)
{
    static const uint64_t values[] = {
        4294967295ULL,        /* 2^32 - 1, 49.7 days */
        4294967296ULL,        /* 2^32 */
        4294967296ULL + 12345ULL,
        1099511627776ULL,     /* 2^40, about 34 years of uptime */
        999999999999999ULL,   /* 15 digits, about 31700 years: still exact */
    };
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        char label[96];
        char expect[32];
        char *text;
        int ok;

        cJSON_AddNumberToObject(o, "mono_ms", (double)values[i]);
        text = cJSON_PrintUnformatted(o);
        snprintf(expect, sizeof(expect), "%" PRIu64, values[i]);
        ok = text && strstr(text, expect) != NULL;
        snprintf(label, sizeof(label), "mono_ms %s survives the wire intact", expect);
        check(label, ok);
        if (!ok && text) {
            printf("     got %s\n", text);
        }
        /* And back again: a consumer reading it must get the same integer. */
        if (text) {
            cJSON *back = cJSON_Parse(text);
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(back, "mono_ms");

            snprintf(label, sizeof(label), "mono_ms %s reads back unchanged", expect);
            check(label, v && (uint64_t)v->valuedouble == values[i]);
            cJSON_Delete(back);
        }
        free(text);
        cJSON_Delete(o);
    }

    /* The C type is 64 bits the whole way, above the printer's limit too.
     * Nothing truncates to 32 bits on the way in or out. */
    {
        uint64_t huge = 18446744073709551615ULL;   /* UINT64_MAX */
        struct radio_rx_packet pkt;

        memset(&pkt, 0, sizeof(pkt));
        pkt.mono_ms = huge;
        check("the monotonic field itself is 64 bits wide", pkt.mono_ms == huge);
        pkt.mono_ms = 4294967296ULL;
        check("and a value just past 2^32 is not folded to zero",
              pkt.mono_ms == 4294967296ULL && (uint32_t)pkt.mono_ms == 0);
    }
}

/* ---- transmit: identity and acceptance ---------------------------------- */

static void test_ids(void)
{
    struct rig g;
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;

    rig_up(&g);
    check("nothing is in flight to begin with", !radio_tx_active(&g.tx));
    check("and no id is claimed", radio_tx_active_id(&g.tx) == 0);

    check("the first transmit is accepted",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 7, true, &a) == 0);
    check("its id is 1, so 0 can mean nothing", a == 1);
    check("it is now in flight", radio_tx_active(&g.tx) && radio_tx_active_id(&g.tx) == 1);
    check("and it remembers who asked", radio_tx_active_client(&g.tx) == 7);

    /* One at a time, and the second request is refused rather than queued,
     * dropped or allowed to overwrite the first. A protocol daemon that
     * gets an error knows its packet did not go out; one whose packet is
     * silently discarded does not. */
    check("a second transmit while one is in flight is refused",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 8, true, &b) == -EBUSY);
    check("and the refusal did not disturb the first",
          radio_tx_active_id(&g.tx) == 1 && radio_tx_active_client(&g.tx) == 7);

    radio_tx_run(&g.tx);
    check("the first completed", !radio_tx_active(&g.tx) && g.rec.calls == 1);

    check("the next transmit is accepted",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 7, true, &c) == 0);
    check("and gets a fresh id, never a reused one", c == 2);
    radio_tx_run(&g.tx);

    check("a zero-length payload is refused",
          radio_tx_submit(&g.tx, PAYLOAD, 0, 7, true, NULL) == -EINVAL);
    check("and one past the chip's maximum is too",
          radio_tx_submit(&g.tx, PAYLOAD, RADIO_MAX_PAYLOAD + 1, 7, true, NULL) == -EINVAL);
    check("a refused submit leaves nothing in flight", !radio_tx_active(&g.tx));
    rig_down(&g);
}

/* The whole point of the asynchronous path: the packet is still on the air
 * after the submit returns. A backend that completed inside submit would
 * pass every check about tx_done and still be synchronous. */
static void test_async_returns_first(void)
{
    struct rig g;
    uint64_t id = 0;
    uint64_t began;
    uint64_t took;
    int steps = 0;

    rig_up(&g);
    knob(&g, "tx_delay_ms", 120);

    began = radio_mono_ms();
    check("submit is accepted",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 1, true, &id) == 0);
    check("and it has not completed when submit returns", g.rec.calls == 0);
    check("the transmit is still in flight", radio_tx_active(&g.tx));

    /* The first step hands it to the backend and must not wait for it
     * either, so the service loop gets back to its clients. */
    check("the first step starts it without finishing it", radio_tx_step(&g.tx) == 1);
    check("still no completion", g.rec.calls == 0);

    /* Bounded by the clock, not by a count: how many times a processor can
     * ask in 120 ms is not this test's business. */
    while (radio_tx_step(&g.tx)) {
        steps++;
        if (radio_mono_ms() - began > 5000) {
            break;
        }
    }
    took = radio_mono_ms() - began;
    check("it completed eventually", g.rec.calls == 1);
    check("it took more than one poll to get there", steps > 0);
    check("and it really was on the air for the time the backend asked for",
          took >= 120 && took < 5000);
    check("and the completion is the transmit that was submitted",
          g.rec.last.tx_id == id && g.rec.last.ok && g.rec.last.bytes == sizeof(PAYLOAD));
    rig_down(&g);
}

/* The path the SX1262 takes. RadioLib's transmit() does not return until the
 * packet has left, so there is no asynchronous half to use; the state
 * machine falls back to send(). Without this the only transmit path with
 * host coverage would be the one the real hardware does not use. */
static void test_blocking_fallback(void)
{
    struct rig g;
    uint64_t id = 0;

    rig_up(&g);
    knob(&g, "tx_async", 0);
    knob(&g, "tx_delay_ms", 500);   /* ignored by the blocking path */

    check("a backend without an asynchronous transmit still accepts one",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 1, true, &id) == 0);
    check("and it has not run inside submit either", g.rec.calls == 0);
    check("one step runs it to completion", radio_tx_step(&g.tx) == 0);
    check("exactly one completion", g.rec.calls == 1);
    check("reported as a success", g.rec.last.ok && g.rec.last.result == RADIO_TX_OK);
    check("with the airtime the profile gives",
          g.rec.last.airtime_ms > 41.2 && g.rec.last.airtime_ms < 41.3);
    check("declining the asynchronous path is not an error", g.rec.last.error[0] == '\0');
    rig_down(&g);
}

/* ---- transmit: exactly one completion, on every path -------------------- */

struct outcome_case {
    const char *name;
    const char *knob;
    bool async;
    bool expect_transmitted;
    bool expect_rx;
    enum radio_tx_result expect_result;
};

static void test_one_completion_per_submit(void)
{
    static const struct outcome_case cases[] = {
        { "a clean transmit",              NULL,                true,  true,  true,  RADIO_TX_OK },
        { "a transmit that fails",         "tx_fail",           true,  false, true,  RADIO_TX_FAILED },
        { "a refusal before transmitting", "tx_fail_begin",     true,  false, true,  RADIO_TX_FAILED },
        { "receive lost after the packet", "tx_rx_fails_after", true,  true,  false, RADIO_TX_RX_RESUME_FAILED },
        { "a clean blocking transmit",     NULL,                false, true,  true,  RADIO_TX_OK },
        { "a blocking transmit that fails","tx_fail",           false, false, true,  RADIO_TX_FAILED },
        { "blocking, receive lost after",  "tx_rx_fails_after", false, true,  false, RADIO_TX_RX_RESUME_FAILED },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct outcome_case *tc = &cases[i];
        struct rig g;
        char label[160];
        int extra;

        rig_up(&g);
        if (!tc->async) {
            knob(&g, "tx_async", 0);
        }
        if (tc->knob) {
            knob(&g, tc->knob, 1);
        }
        if (radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 3, true, NULL) != 0) {
            snprintf(label, sizeof(label), "%s is accepted", tc->name);
            check(label, 0);
            rig_down(&g);
            continue;
        }
        radio_tx_run(&g.tx);

        snprintf(label, sizeof(label), "%s completes exactly once", tc->name);
        check(label, g.rec.calls == 1);
        snprintf(label, sizeof(label), "%s reports transmitted=%d", tc->name,
                 tc->expect_transmitted);
        check(label, g.rec.last.transmitted == tc->expect_transmitted);
        snprintf(label, sizeof(label), "%s reports rx_resumed=%d", tc->name, tc->expect_rx);
        check(label, g.rec.last.rx_resumed == tc->expect_rx);
        snprintf(label, sizeof(label), "%s reports result %d", tc->name, (int)tc->expect_result);
        check(label, g.rec.last.result == tc->expect_result);
        snprintf(label, sizeof(label), "%s: ok is transmitted and receiving, nothing else",
                 tc->name);
        check(label, g.rec.last.ok == (tc->expect_transmitted && tc->expect_rx));
        snprintf(label, sizeof(label), "%s carries a reason when it is not ok", tc->name);
        check(label, g.rec.last.ok || g.rec.last.error[0] != '\0');

        /* The loop calls step again on the very next turn. It must not find
         * work in a finished job and announce it a second time: a duplicate
         * would be counted twice in the airtime statistics and answer a
         * question the submitter has already had answered. */
        extra = g.rec.calls;
        radio_tx_step(&g.tx);
        radio_tx_step(&g.tx);
        radio_tx_run(&g.tx);
        snprintf(label, sizeof(label), "%s: stepping a finished job adds nothing", tc->name);
        check(label, g.rec.calls == extra);
        rig_down(&g);
    }
}

/* A transmit that fails must not leave the machine holding the radio: the
 * next one has to be accepted. */
static void test_failure_frees_the_slot(void)
{
    struct rig g;

    rig_up(&g);
    knob(&g, "tx_fail_begin", 1);
    check("a transmit the backend refuses is accepted first",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 1, true, NULL) == 0);
    radio_tx_run(&g.tx);
    check("it failed", g.rec.calls == 1 && !g.rec.last.ok);
    check("and the slot is free again", !radio_tx_active(&g.tx));
    knob(&g, "tx_fail_begin", 0);
    check("so the next transmit is accepted",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 1, true, NULL) == 0);
    radio_tx_run(&g.tx);
    check("and succeeds", g.rec.calls == 2 && g.rec.last.ok);
    rig_down(&g);
}

/* ---- payload lifetime ---------------------------------------------------- */

/* The bytes on the air have to be the bytes that were asked for, and on the
 * asynchronous path the request they arrived in is long gone by the time the
 * radio reads them. The mock keeps the pointer it was handed and compares
 * the memory behind it at completion, so a state machine that passed a
 * pointer into a parse buffer - or reused its own slot for the next packet -
 * fails here instead of transmitting whatever was at the address by then.
 *
 * The caller's buffer is deliberately destroyed after submit: it is the
 * caller's own again the moment submit returns, and nothing may still be
 * reading it. */
static void test_payload_lifetime(void)
{
    struct rig g;
    uint8_t scratch[10];

    rig_up(&g);
    knob(&g, "tx_delay_ms", 60);
    memcpy(scratch, PAYLOAD, sizeof(scratch));

    check("a transmit from a caller's buffer is accepted",
          radio_tx_submit(&g.tx, scratch, sizeof(scratch), 1, true, NULL) == 0);
    /* The caller reuses its buffer immediately, as it is entitled to. */
    memset(scratch, 0xa5, sizeof(scratch));
    radio_tx_run(&g.tx);

    check("it completed", g.rec.calls == 1);
    check("and the payload the backend saw was never disturbed",
          g.rec.last.ok && g.rec.last.result == RADIO_TX_OK);
    if (!g.rec.last.ok) {
        printf("     %s\n", g.rec.last.error);
    }

    /* And the check itself has teeth: point the mock at a buffer that really
     * does change and it must notice. Without this, the case above would
     * pass on a mock that compared nothing. */
    {
        struct radio_channel unused;
        uint8_t live[10];
        char err[128] = "";
        int rc;

        memset(&unused, 0, sizeof(unused));
        memcpy(live, PAYLOAD, sizeof(live));
        rc = g.be.ops->tx_begin(&g.be, live, sizeof(live), err, sizeof(err));
        check("the backend takes a transmit directly", rc == 0);
        memset(live, 0x5a, sizeof(live));
        rc = g.be.ops->tx_poll(&g.be, err, sizeof(err));
        while (rc == 0) {
            rc = g.be.ops->tx_poll(&g.be, err, sizeof(err));
        }
        check("a payload that changes while on the air is caught", rc == -EFAULT);
    }
    rig_down(&g);
}

/* ---- the submitter disappears ------------------------------------------- */

static void test_client_gone(void)
{
    struct rig g;
    uint64_t id = 0;

    rig_up(&g);
    knob(&g, "tx_delay_ms", 60);
    check("a client submits a transmit",
          radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 42, true, &id) == 0);
    radio_tx_step(&g.tx);

    check("forgetting a client that submitted nothing does nothing",
          !radio_tx_forget_client(&g.tx, 43));
    check("the submitter is still recorded", radio_tx_active_client(&g.tx) == 42);
    check("forgetting the real submitter is acknowledged",
          radio_tx_forget_client(&g.tx, 42));
    /* The radio is mid-packet. Stopping now would leave the transceiver
     * keyed with nothing to bring it back, so it finishes. */
    check("the transmit is still in flight", radio_tx_active(&g.tx));
    check("and no longer belongs to anyone", radio_tx_active_client(&g.tx) == 0);

    radio_tx_run(&g.tx);
    check("it still completed", g.rec.calls == 1 && g.rec.last.ok);
    check("its completion names no client", g.rec.last.client_id == 0);
    check("and it is still the transmit that was submitted", g.rec.last.tx_id == id);
    rig_down(&g);
}

/* ---- rapid traffic ------------------------------------------------------- */

/* Nothing lost, nothing counted twice, nothing left in flight. Volume goes
 * through the blocking path, which is the one real hardware uses and the one
 * that does not wait for a simulated airtime; the asynchronous path is
 * covered for pace by test_async_returns_first and for correctness by the
 * outcome table, and gets a smaller batch here. */
static void test_rapid_blocking(void)
{
    struct rig g;
    int accepted = 0;
    int refused = 0;
    int i;

    rig_up(&g);
    knob(&g, "tx_async", 0);
    for (i = 0; i < 500; i++) {
        uint8_t body[3];
        uint64_t id = 0;

        body[0] = (uint8_t)i;
        body[1] = (uint8_t)(i >> 8);
        body[2] = 0x5a;
        if (radio_tx_submit(&g.tx, body, sizeof(body), 1, true, &id) == 0) {
            accepted++;
            if (id != (uint64_t)accepted) {
                check("ids stay in step with acceptances", 0);
                rig_down(&g);
                return;
            }
        } else {
            refused++;
        }
        radio_tx_run(&g.tx);
    }
    check("every rapid submit was accepted", accepted == 500 && refused == 0);
    check("and every one of them completed exactly once", g.rec.calls == 500);
    check("with nothing left in flight", !radio_tx_active(&g.tx));
    rig_down(&g);
}

static void test_rapid_async(void)
{
    struct rig g;
    int i;
    int ok = 1;

    rig_up(&g);
    for (i = 0; i < 60; i++) {
        uint64_t id = 0;

        if (radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 1, true, &id) != 0 ||
            id != (uint64_t)(i + 1)) {
            ok = 0;
            break;
        }
        /* And a second one arriving while the first is still going. */
        if (radio_tx_submit(&g.tx, PAYLOAD, sizeof(PAYLOAD), 2, true, NULL) != -EBUSY) {
            ok = 0;
            break;
        }
        radio_tx_run(&g.tx);
    }
    check("back-to-back asynchronous transmits keep their order and their ids", ok);
    check("and each produced one completion", g.rec.calls == 60);
    check("with nothing left in flight", !radio_tx_active(&g.tx));
    rig_down(&g);
}

/* Every legal length, at both ends of the range. */
static void test_lengths(void)
{
    struct rig g;
    uint8_t body[RADIO_MAX_PAYLOAD];
    size_t lens[] = { 1, 2, 16, 109, 254, RADIO_MAX_PAYLOAD };
    size_t i;
    int ok = 1;

    rig_up(&g);
    memset(body, 0x3c, sizeof(body));
    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        int before = g.rec.calls;

        if (radio_tx_submit(&g.tx, body, lens[i], 1, true, NULL) != 0) {
            ok = 0;
            break;
        }
        radio_tx_run(&g.tx);
        if (g.rec.calls != before + 1 || !g.rec.last.ok || g.rec.last.bytes != lens[i]) {
            ok = 0;
            break;
        }
    }
    check("every legal payload length transmits, up to the 255-byte maximum", ok);
    rig_down(&g);
}

/* ---- the lease ----------------------------------------------------------- */

static void test_lease(void)
{
    struct radio_lease l;
    uint64_t first = 0;
    uint64_t again = 0;
    uint64_t second = 0;

    radio_lease_init(&l);
    check("nothing holds the lease to begin with", !radio_lease_held(&l));
    /* Opt-in: with no lease taken, every client may do everything, which is
     * what every caller written before the lease existed expects. */
    check("and every client may use the radio",
          radio_lease_permits(&l, 1) && radio_lease_permits(&l, 2));

    check("a client takes the lease",
          radio_lease_acquire(&l, 1, "meshcored", 1000, &first) == 0);
    check("it has an identity", first == 1);
    check("it is held by that client", radio_lease_is_owner(&l, 1));
    check("the label is kept", strcmp(radio_lease_owner(&l), "meshcored") == 0);
    check("and when it was taken", radio_lease_since(&l) == 1000);

    /* A daemon that is not sure whether its reconnect kept the lease asks
     * again. Refusing that would make recovery harder than it needs to be. */
    check("the holder may acquire again",
          radio_lease_acquire(&l, 1, "meshcored", 2000, &again) == 0);
    check("and it is the same lease, not a new one", again == first);
    check("its start time did not move", radio_lease_since(&l) == 1000);

    check("another client is refused",
          radio_lease_acquire(&l, 2, "meshtasticd", 3000, &second) == -EBUSY);
    check("the holder is unchanged", radio_lease_is_owner(&l, 1));
    check("the other client may not use the radio", !radio_lease_permits(&l, 2));
    check("the holder still may", radio_lease_permits(&l, 1));

    /* Releasing a lease somebody else is relying on is exactly the accident
     * the lease exists to prevent. */
    check("a client that does not hold it cannot release it",
          radio_lease_release(&l, 2) == -EPERM);
    check("and it is still held", radio_lease_held(&l) && radio_lease_is_owner(&l, 1));

    check("the holder releases it", radio_lease_release(&l, 1) == 0);
    check("it is free", !radio_lease_held(&l));
    check("releasing again is refused rather than ignored",
          radio_lease_release(&l, 1) == -EPERM);
    check("and everybody may use the radio again",
          radio_lease_permits(&l, 1) && radio_lease_permits(&l, 2));

    check("the other client may now take it",
          radio_lease_acquire(&l, 2, "meshtasticd", 4000, &second) == 0);
    /* A fresh identity, so a client cannot mistake the lease it holds now
     * for the one it held before. */
    check("with a new identity", second != first);

    check("the owner disconnecting releases it", radio_lease_client_gone(&l, 2));
    check("it is free again", !radio_lease_held(&l));
    check("a client that did not hold it disconnecting changes nothing",
          !radio_lease_client_gone(&l, 9));

    check("client 0 is not an identity and cannot hold a lease",
          radio_lease_acquire(&l, 0, "nobody", 5000, NULL) == -EINVAL);
    radio_lease_acquire(&l, 3, NULL, 6000, NULL);
    check("a lease with no label still names something",
          radio_lease_owner(&l)[0] != '\0');
    check("and an unidentified client is still not its owner",
          !radio_lease_is_owner(&l, 0));
}

int main(void)
{
    test_clocks();
    test_wide_timestamps();
    test_ids();
    test_async_returns_first();
    test_blocking_fallback();
    test_one_completion_per_submit();
    test_failure_frees_the_slot();
    test_payload_lifetime();
    test_client_gone();
    test_rapid_blocking();
    test_rapid_async();
    test_lengths();
    test_lease();

    printf("radiod_tx_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
