/*
 * TROPO (apps/rift/rift_tropo.h): the detector ported from RIFT on the
 * T-Deck - 20 hops, five within 15 minutes, held 20 minutes after the last -
 * fed bare and then through the mesh.rx reader exactly as the app feeds it.
 *
 * Every time here is on the monotonic clock the service stamps with, and the
 * detector has no clock of its own: "now" is passed in, so expiry is tested
 * at the millisecond it falls due rather than around it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_rxlog.h"
#include "rift_tropo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    failed += !ok;
}

#define MIN (60LL * 1000)
#define HOUR (60 * MIN)
/* An hour after boot, so nothing here sits at the clock's zero. */
#define T0 (HOUR)

/* A packet hash, distinct for every n. */
static const uint8_t *hash_of(unsigned n)
{
    static uint8_t h[64][RIFT_TROPO_HASH_LEN];
    uint8_t *p = h[n % 64];

    memset(p, 0, RIFT_TROPO_HASH_LEN);
    p[0] = 0xA5;
    p[6] = (uint8_t)(n >> 8);
    p[7] = (uint8_t)n;
    return p;
}

/* A deep flood, as the app would hand it on: handled, not ours, hashed. */
static enum rift_tropo_verdict deep(struct rift_tropo *t, int64_t rx, int hops, unsigned id,
                                    int64_t now)
{
    return rift_tropo_step(t, rx, hops, 1, 1, 0, hash_of(id), now);
}

static void test_what_is_evidence(void)
{
    struct rift_tropo t;
    int i;

    rift_tropo_init(&t);
    check("an ordinary depth is the baseline, not evidence",
          deep(&t, T0, 19, 1, T0) == RIFT_TROPO_SHALLOW);
    check("twenty hops is deep", deep(&t, T0, 20, 2, T0) == RIFT_TROPO_CANDIDATE);
    check("the no-path sentinel's 63 is not a measurement",
          deep(&t, T0, 63, 3, T0) == RIFT_TROPO_IMPLAUSIBLE_PATH);
    check("and neither is 62", deep(&t, T0, 62, 4, T0) == RIFT_TROPO_IMPLAUSIBLE_PATH);
    check("61 is", deep(&t, T0, 61, 5, T0) == RIFT_TROPO_CANDIDATE);
    check("a direct packet's path is hops to go, not hops come",
          rift_tropo_step(&t, T0, 40, 0, 1, 0, hash_of(6), T0) == RIFT_TROPO_NOT_FLOOD);
    check("a frame MeshCore never handled is not evidence",
          rift_tropo_step(&t, T0, 40, 1, 0, 0, hash_of(7), T0) == RIFT_TROPO_REJECTED);
    check("nor is this node's own packet heard back",
          rift_tropo_step(&t, T0, 40, 1, 1, 1, hash_of(8), T0) == RIFT_TROPO_OWN);
    check("nor a frame that cannot be told from its copies",
          rift_tropo_step(&t, T0, 40, 1, 1, 0, NULL, T0) == RIFT_TROPO_NO_HASH);
    check("nor one with no path", rift_tropo_step(&t, T0, -1, 1, 1, 0, hash_of(9), T0) ==
                                      RIFT_TROPO_NO_PATH);
    check("so two candidates and no opening", t.cand_count == 2 && !t.have_opening);
    check("NULL is nothing", rift_tropo_step(NULL, T0, 40, 1, 1, 0, hash_of(1), T0) ==
                                 RIFT_TROPO_NOT_FRAME &&
                                 rift_tropo_observe(&t, NULL, T0) == RIFT_TROPO_NOT_FRAME &&
                                 !rift_tropo_active(NULL, T0));
    rift_tropo_read(NULL, T0, NULL);

    rift_tropo_init(&t);
    for (i = 0; i < 50; i++) {
        deep(&t, T0 + i, 3, 100 + (unsigned)i, T0 + i);
    }
    check("fifty shallow floods open nothing", !t.have_opening && t.cand_count == 0);
}

static void test_five_in_fifteen(void)
{
    struct rift_tropo t;
    struct rift_tropo_view v;
    int i;

    rift_tropo_init(&t);
    for (i = 0; i < RIFT_TROPO_NEEDED - 1; i++) {
        check("each of the first four is a candidate",
              deep(&t, T0 + i * MIN, 24, (unsigned)i, T0 + i * MIN) == RIFT_TROPO_CANDIDATE);
    }
    rift_tropo_read(&t, T0 + 4 * MIN, &v);
    check("four is not an opening", v.level == RIFT_TROPO_NONE && v.candidates == 4);
    check("the fifth opens it, once",
          deep(&t, T0 + 4 * MIN, 31, 4, T0 + 4 * MIN) == RIFT_TROPO_OPENED && t.openings == 1);
    rift_tropo_read(&t, T0 + 4 * MIN, &v);
    check("detected, active, with its evidence and its peak",
          v.level == RIFT_TROPO_DETECTED && v.active && v.evidence == 5 && v.peak_hops == 31 &&
              v.span_ms == 4 * MIN && v.candidates == 0);
    check("a sixth during it extends it, and is not a second opening",
          deep(&t, T0 + 6 * MIN, 27, 5, T0 + 6 * MIN) == RIFT_TROPO_EXTENDED && t.openings == 1 &&
              t.evidence == 6 && t.last_ms == T0 + 6 * MIN);

    /* Spread too thin: one every four minutes is never five in fifteen. */
    rift_tropo_init(&t);
    for (i = 0; i < 12; i++) {
        deep(&t, T0 + i * 4 * MIN, 30, 200 + (unsigned)i, T0 + i * 4 * MIN);
    }
    check("one deep packet every four minutes never opens", !t.have_opening &&
                                                             t.cand_count == 4);

    /* The window slides: 0, 4, 8, 12 and 16 minutes is not five in fifteen
     * (0 and 16 are sixteen apart), but one more at 17 makes 4..17 five. */
    rift_tropo_init(&t);
    for (i = 0; i < 5; i++) {
        deep(&t, T0 + i * 4 * MIN, 22, 300 + (unsigned)i, T0 + i * 4 * MIN);
    }
    check("five spanning sixteen minutes is not an opening", !t.have_opening && t.cand_count == 4);
    check("one more inside fifteen of the last four is",
          deep(&t, T0 + 17 * MIN, 22, 305, T0 + 17 * MIN) == RIFT_TROPO_OPENED &&
              t.first_ms == T0 + 4 * MIN && t.evidence == 5);

    /* Exactly fifteen minutes apart is still within the window. */
    rift_tropo_init(&t);
    deep(&t, T0, 22, 400, T0);
    for (i = 1; i < 4; i++) {
        deep(&t, T0 + i * MIN, 22, 400 + (unsigned)i, T0 + i * MIN);
    }
    check("the window's edge is inside it",
          deep(&t, T0 + 15 * MIN, 22, 404, T0 + 15 * MIN) == RIFT_TROPO_OPENED);
}

static void test_repeats(void)
{
    struct rift_tropo t;
    int i;

    rift_tropo_init(&t);
    for (i = 0; i < 10; i++) {
        deep(&t, T0 + i * 1000, 30, 7, T0 + i * 1000);
    }
    check("one packet heard ten times is one piece of evidence",
          t.cand_count == 1 && t.repeats == 9 && !t.have_opening);
    /* Five copies of each of four packets, interleaved, over two routes. */
    rift_tropo_init(&t);
    for (i = 0; i < 20; i++) {
        rift_tropo_step(&t, T0 + i * 1000, 25 + i % 3, 1, 1, 0, hash_of((unsigned)(i % 4)),
                        T0 + i * 1000);
    }
    check("twenty receptions of four packets is four, and no opening",
          t.cand_count == 4 && t.repeats == 16 && !t.have_opening);
    check("a fifth distinct one opens it",
          deep(&t, T0 + 30000, 25, 99, T0 + 30000) == RIFT_TROPO_OPENED);
    check("a repeat of evidence during the opening adds nothing",
          deep(&t, T0 + 31000, 40, 99, T0 + 31000) == RIFT_TROPO_REPEAT && t.evidence == 5 &&
              t.peak_hops < 40);
    /* The seen ring is bounded. */
    rift_tropo_init(&t);
    for (i = 0; i < 3 * RIFT_TROPO_SEEN; i++) {
        uint8_t h[RIFT_TROPO_HASH_LEN] = { 0x11, 0, 0, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };

        rift_tropo_step(&t, T0 + i * 1000, 30, 1, 1, 0, h, T0 + i * 1000);
    }
    check("the hashes it remembers are bounded", t.seen_count == RIFT_TROPO_SEEN);
}

static void test_hold_and_lifecycle(void)
{
    struct rift_tropo t;
    struct rift_tropo_view v;
    int64_t last;
    int i;

    rift_tropo_init(&t);
    for (i = 0; i < 5; i++) {
        deep(&t, T0 + i * MIN, 26, 500 + (unsigned)i, T0 + i * MIN);
    }
    last = T0 + 4 * MIN;
    check("active a millisecond before the hold runs out",
          rift_tropo_active(&t, last + RIFT_TROPO_HOLD_MS - 1));
    check("and not at the millisecond it does, with nothing having run",
          !rift_tropo_active(&t, last + RIFT_TROPO_HOLD_MS));
    rift_tropo_read(&t, last + RIFT_TROPO_HOLD_MS - 1, &v);
    check("the hold left is what is left of twenty minutes", v.active && v.closes_in_ms == 1);
    rift_tropo_read(&t, last + 30 * MIN, &v);
    check("ended, still TROPO DETECTED in its first hour",
          !v.active && v.level == RIFT_TROPO_DETECTED && v.age_ms == 30 * MIN);
    rift_tropo_read(&t, last + HOUR - 1, &v);
    check("red to the last millisecond of the hour", v.level == RIFT_TROPO_DETECTED);
    rift_tropo_read(&t, last + HOUR, &v);
    check("RECENT TROPO from the hour", v.level == RIFT_TROPO_RECENT);
    rift_tropo_read(&t, last + 5 * HOUR - 1, &v);
    check("until five hours", v.level == RIFT_TROPO_RECENT);
    rift_tropo_read(&t, last + 5 * HOUR, &v);
    check("LAST TROPO from five hours", v.level == RIFT_TROPO_LAST);
    rift_tropo_read(&t, last + 50 * HOUR, &v);
    check("and stays LAST, with its peak and evidence",
          v.level == RIFT_TROPO_LAST && v.peak_hops == 26 && v.evidence == 5);

    /* After it closed, one deep packet is a candidate and nothing more. */
    check("a lone deep packet after it closed does not open a new one",
          deep(&t, last + HOUR, 45, 600, last + HOUR) == RIFT_TROPO_CANDIDATE && t.openings == 1);
    rift_tropo_read(&t, last + HOUR + MIN, &v);
    check("and the age is still the opening's, not that packet's",
          v.level == RIFT_TROPO_RECENT && v.age_ms == HOUR + MIN && v.peak_hops == 26 &&
              v.candidates == 1);
    for (i = 1; i < 5; i++) {
        deep(&t, last + HOUR + i * MIN, 33, 600 + (unsigned)i, last + HOUR + i * MIN);
    }
    rift_tropo_read(&t, last + HOUR + 4 * MIN, &v);
    check("four more make a second opening, measured from its own evidence",
          t.openings == 2 && v.level == RIFT_TROPO_DETECTED && v.active && v.age_ms == 0 &&
              v.peak_hops == 45 && v.evidence == 5 && v.openings == 2);

    /* Evidence that keeps coming keeps it open. */
    rift_tropo_init(&t);
    for (i = 0; i < 5; i++) {
        deep(&t, T0 + i * 1000, 21, 700 + (unsigned)i, T0 + i * 1000);
    }
    for (i = 1; i <= 6; i++) {
        deep(&t, T0 + i * 15 * MIN, 21, 710 + (unsigned)i, T0 + i * 15 * MIN);
    }
    rift_tropo_read(&t, T0 + 90 * MIN + 19 * MIN, &v);
    check("an opening fed every fifteen minutes is still the first one",
          v.active && t.openings == 1 && v.evidence == 11 && v.span_ms == 90 * MIN);
}

static void test_clock(void)
{
    struct rift_tropo t;
    struct rift_tropo_view v;
    int i;

    /* A reception reported late goes in where it was received. */
    rift_tropo_init(&t);
    deep(&t, T0 + 10 * MIN, 23, 800, T0 + 10 * MIN);
    deep(&t, T0 + 11 * MIN, 23, 801, T0 + 11 * MIN);
    deep(&t, T0 + 2 * MIN, 23, 802, T0 + 12 * MIN);
    check("a late report is put back in order", t.cand_count == 3 && t.cand[0].mono_ms == T0 + 2 * MIN);
    deep(&t, T0 + 12 * MIN, 23, 803, T0 + 12 * MIN);
    check("and counts towards the window it was received in",
          deep(&t, T0 + 13 * MIN, 23, 804, T0 + 13 * MIN) == RIFT_TROPO_OPENED &&
              t.first_ms == T0 + 2 * MIN);
    /* Too old to be evidence of now. */
    rift_tropo_init(&t);
    check("a reception older than the window is stale",
          deep(&t, T0, 30, 900, T0 + 16 * MIN) == RIFT_TROPO_STALE && t.cand_count == 0 &&
              t.stale == 1);
    /* A stamp ahead of this clock is taken as now. */
    rift_tropo_init(&t);
    deep(&t, T0 + HOUR, 30, 901, T0);
    check("a stamp from the future is now", t.cand_count == 1 && t.cand[0].mono_ms == T0);
    /* The baseline: the deepest ordinary path over this quarter and the last. */
    rift_tropo_init(&t);
    rift_tropo_read(&t, T0, &v);
    check("no baseline before anything is heard", !v.have_base);
    deep(&t, T0, 0, 1, T0);
    rift_tropo_read(&t, T0, &v);
    check("a zero-hop flood is a baseline of zero", v.have_base && v.base_hops == 0);
    deep(&t, T0 + 1000, 7, 2, T0 + 1000);
    deep(&t, T0 + 2000, 4, 3, T0 + 2000);
    rift_tropo_read(&t, T0 + 2000, &v);
    check("the deepest of them", v.have_base && v.base_hops == 7);
    deep(&t, T0 + RIFT_TROPO_WINDOW_MS, 3, 4, T0 + RIFT_TROPO_WINDOW_MS);
    rift_tropo_read(&t, T0 + RIFT_TROPO_WINDOW_MS, &v);
    check("still the deepest of this quarter and the last", v.base_hops == 7);
    deep(&t, T0 + 2 * RIFT_TROPO_WINDOW_MS, 2, 5, T0 + 2 * RIFT_TROPO_WINDOW_MS);
    rift_tropo_read(&t, T0 + 2 * RIFT_TROPO_WINDOW_MS, &v);
    check("and forgotten after that", v.base_hops == 3);
    rift_tropo_read(&t, T0 + 5 * RIFT_TROPO_WINDOW_MS, &v);
    check("a baseline nothing has refreshed lately is not shown", !v.have_base);
    for (i = 0; i < 4; i++) {
        check("level words", rift_tropo_level_word((enum rift_tropo_level)i)[0] != '\0');
    }
    check("the words the panel leads with",
          strcmp(rift_tropo_level_word(RIFT_TROPO_DETECTED), "TROPO DETECTED") == 0 &&
              strcmp(rift_tropo_level_word(RIFT_TROPO_RECENT), "RECENT TROPO") == 0 &&
              strcmp(rift_tropo_level_word(RIFT_TROPO_LAST), "LAST TROPO") == 0 &&
              strcmp(rift_tropo_level_word(RIFT_TROPO_NONE), "NO TROPO SEEN") == 0);
}

/* ---- through the mesh.rx reader ------------------------------------------- */

static struct rift_rxlog g_log;

/* One mesh.rx event, path_hops relays of one byte each; each member of
 * `extra` (",\"key\":value" ...) replaces or adds one field. */
static int rx(long seq, int64_t mono, const char *route, int hops, unsigned id, const char *extra,
              int64_t now)
{
    char path[2 * 63 + 1];
    char buf[1024];
    cJSON *data;
    cJSON *over;
    cJSON *item;
    int i;
    int rc;

    for (i = 0; i < hops; i++) {
        snprintf(path + 2 * i, 3, "%02x", (unsigned)(0x10 + i));
    }
    path[2 * hops] = '\0';
    snprintf(buf, sizeof(buf),
             "{\"v\":1,\"seq\":%ld,\"mono_ms\":%lld,\"bytes\":60,\"verdict\":\"new\","
             "\"type\":\"group_text\",\"type_code\":5,\"route\":\"%s\",\"path_kind\":\"hops\","
             "\"path_hash_size\":1,\"path_hops\":%d%s%s%s,\"hash\":\"a5a5a5a5a5a5%04x\",\"dup\":1}",
             seq, (long long)mono, route, hops, hops ? ",\"path_hex\":\"" : "", hops ? path : "",
             hops ? "\"" : "", id);
    data = cJSON_Parse(buf);
    snprintf(buf, sizeof(buf), "{%s}", extra[0] == ',' ? extra + 1 : extra);
    over = cJSON_Parse(buf);
    if (!data || !over) {
        fprintf(stderr, "fixture does not parse: %s\n", buf);
        exit(2);
    }
    for (item = over->child; item; item = item->next) {
        cJSON *copy = cJSON_Duplicate(item, 1);

        if (cJSON_GetObjectItemCaseSensitive(data, item->string)) {
            cJSON_ReplaceItemInObjectCaseSensitive(data, item->string, copy);
        } else {
            cJSON_AddItemToObject(data, item->string, copy);
        }
    }
    cJSON_Delete(over);
    rc = rift_rxlog_apply(&g_log, data, now, 1791204188000LL);
    cJSON_Delete(data);
    return rc;
}

static void test_through_the_reader(void)
{
    struct rift_tropo_view v;
    struct rift_tropo *t = &g_log.tropo;
    int64_t now = T0;
    long seq = 1;
    int i;

    rift_rxlog_init(&g_log);
    check("a deep flood is read and counted", rx(seq++, now, "flood", 25, 1, "", now) == 0 &&
                                                  t->cand_count == 1);
    check("a transport flood counts too", rx(seq++, now + 1, "transport_flood", 21, 2, "", now + 1) == 0 &&
                                              t->cand_count == 2);
    rx(seq++, now + 2, "direct", 30, 3, "", now + 2);
    check("a direct packet's remaining route does not", t->cand_count == 2);
    rx(seq++, now + 3, "flood", 30, 4, ",\"own\":true", now + 3);
    check("an own echo does not", t->cand_count == 2);
    rx(seq++, now + 4, "flood", 30, 1, ",\"dup\":2", now + 4);
    rx(seq++, now + 5, "flood", 33, 1, ",\"verdict\":\"duplicate\",\"dup\":3", now + 5);
    check("copies of a packet already counted do not", t->cand_count == 2 && t->repeats == 2);
    rx(seq++, now + 6, "flood", 30, 5, ",\"verdict\":\"rejected\",\"reject\":\"queue_full\"", now + 6);
    rx(seq++, now + 7, "flood", 30, 6, ",\"verdict\":\"unresolved\"", now + 7);
    check("a frame MeshCore never handled does not", t->cand_count == 2);
    {
        /* A TRACE: its path bytes are SNRs, whatever their count. */
        char buf[512];
        cJSON *data;

        snprintf(buf, sizeof(buf),
                 "{\"v\":1,\"seq\":%ld,\"mono_ms\":%lld,\"bytes\":60,\"verdict\":\"new\","
                 "\"type\":\"trace\",\"type_code\":9,\"route\":\"flood\",\"path_kind\":\"snr\","
                 "\"path_hash_size\":1,\"path_hops\":24,\"path_hex\":\"%s\","
                 "\"hash\":\"a5a5a5a5a5a5beef\",\"dup\":1}",
                 seq++, (long long)(now + 8), "101112131415161718191a1b1c1d1e1f2021222324252627");
        data = cJSON_Parse(buf);
        rift_rxlog_apply(&g_log, data, now + 8, 1791204188000LL);
        cJSON_Delete(data);
        check("nor does a TRACE", g_log.count == 9 && t->cand_count == 2);
    }
    rx(seq++, now + 9, "flood", 4, 7, "", now + 9);
    check("an ordinary flood is the baseline", t->cand_count == 2);
    /* The service goes away and comes back; RX LOG is cleared: neither is a
     * reason to forget what was heard. */
    rift_rxlog_service_lost(&g_log, now + 10);
    rift_rxlog_service_answered(&g_log, NULL, now + 11);
    rift_rxlog_clear(&g_log);
    check("a reconnect and a CLEAR keep the candidates", t->cand_count == 2 && g_log.count == 0);
    for (i = 0; i < 3; i++) {
        rx(seq++, now + 2 * MIN + i, "flood", 28 + i, 20 + (unsigned)i, "", now + 2 * MIN + i);
    }
    rift_tropo_read(t, now + 3 * MIN, &v);
    check("five distinct deep floods through the reader open it",
          v.level == RIFT_TROPO_DETECTED && v.active && v.evidence == 5 && v.peak_hops == 30 &&
              v.have_base && v.base_hops == 4);
    /* A full ring still feeds TROPO: it is fed before an entry is filed. */
    rift_rxlog_init(&g_log);
    for (i = 0; i < RIFT_RXLOG_MAX + 10; i++) {
        rx(seq++, now + 1000 + i, "flood", 1, 3000 + (unsigned)i, "", now + 1000 + i);
    }
    for (i = 0; i < 5; i++) {
        rx(seq++, now - 1000 + i, "flood", 25, 50 + (unsigned)i, "", now + 5000);
    }
    check("a reception the full ring drops is still heard by TROPO",
          g_log.evicted > 0 && t->openings == 1);
}

/* The bounded ring of counted hashes, and what a copy of a packet whose hash
 * has left it can and cannot do. */
static void test_eviction(void)
{
    struct rift_tropo t;
    struct rift_tropo_view v;
    uint8_t first[RIFT_TROPO_HASH_LEN];
    int64_t now = T0;
    int i;

    /* The ring alone: a copy of the first packet after 64 others is new to
     * it. That is the whole of the risk, and its bound: one more piece of
     * evidence, only after RIFT_TROPO_SEEN other distinct deep packets. */
    rift_tropo_init(&t);
    memcpy(first, hash_of(0), sizeof(first));
    deep(&t, now, 30, 0, now);
    for (i = 1; i <= RIFT_TROPO_SEEN; i++) {
        uint8_t h[RIFT_TROPO_HASH_LEN] = { 0x22, 0, 0, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };

        now += 20 * MIN; /* spread thin: no opening */
        rift_tropo_step(&t, now, 30, 1, 1, 0, h, now);
    }
    check("sixty-four thin deep packets later, no opening and the first has left the ring",
          !t.have_opening && t.seen_count == RIFT_TROPO_SEEN);
    check("told nothing by the service, the ring alone takes a copy of it as new",
          rift_tropo_step(&t, now + 1000, 30, 1, 1, 0, first, now + 1000) == RIFT_TROPO_CANDIDATE);
    check("but the service's word that it is a repeat is enough",
          rift_tropo_step_seen(&t, now + 2000, 30, 1, 1, 0, first, 1, now + 2000) ==
              RIFT_TROPO_REPEAT);

    /* A replay after an opening has closed: five old deep packets heard
     * again, every one evicted from the ring, every one a repeat to the
     * service. No second opening, and the first keeps its age. */
    rift_tropo_init(&t);
    now = T0;
    for (i = 0; i < 5; i++) {
        deep(&t, now + i * MIN, 28, 1000 + (unsigned)i, now + i * MIN);
    }
    check("an opening to replay", t.openings == 1);
    now += 4 * MIN;
    for (i = 0; i < RIFT_TROPO_SEEN; i++) {
        uint8_t h[RIFT_TROPO_HASH_LEN] = { 0x33, 0, 0, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };

        rift_tropo_step(&t, now + 1000 + i, 25, 1, 1, 0, h, now + 1000 + i);
    }
    now += 2 * HOUR;
    check("closed, with the five replayed hashes gone from the ring",
          !rift_tropo_active(&t, now) && !t.cand_count);
    for (i = 0; i < 5; i++) {
        check("each replayed copy is a repeat to the service, so to TROPO",
              rift_tropo_step_seen(&t, now + i * 1000, 28, 1, 1, 0, hash_of(1000 + (unsigned)i), 1,
                                   now + i * 1000) == RIFT_TROPO_REPEAT);
    }
    rift_tropo_read(&t, now + 5000, &v);
    check("and they open nothing: one opening, aged from its own evidence",
          t.openings == 1 && v.level == RIFT_TROPO_RECENT && v.candidates == 0);
}

/* The service's two words on a frame, as the reader takes them. */
static void test_service_repeats(void)
{
    struct rift_tropo *t = &g_log.tropo;
    int64_t now = T0;
    long seq = 5000;
    int i;

    rift_rxlog_init(&g_log);
    /* Sixty-four deep floods an hour apart: the ring full, no opening. */
    for (i = 0; i < RIFT_TROPO_SEEN + 1; i++) {
        now += HOUR;
        rx(seq++, now, "flood", 30, 0x4000 + (unsigned)i, "", now);
    }
    check("through the reader: the ring is full and nothing opened",
          t->seen_count == RIFT_TROPO_SEEN && !t->have_opening && t->cand_count == 1);
    rx(seq++, now + 1, "flood", 30, 0x4000, ",\"dup\":2", now + 1);
    check("a copy whose hash left the ring, with dup 2, is a repeat", t->cand_count == 1);
    rx(seq++, now + 2, "flood", 30, 0x4000, ",\"verdict\":\"duplicate\"", now + 2);
    check("and with MeshCore's duplicate verdict too", t->cand_count == 1);
    rx(seq++, now + 3, "flood", 30, 0x9999, ",\"dup\":2", now + 3);
    check("a deep copy of a packet first heard shallow is not new deep evidence",
          t->cand_count == 1 && t->repeats == 3);
    rx(seq++, now + 4, "flood", 30, 0x4000, "", now + 4);
    check("with neither word - a service that started again - the ring's bound is what "
          "remains: one candidate more, not an opening",
          t->cand_count == 2 && !t->have_opening);
}

int main(void)
{
    test_what_is_evidence();
    test_five_in_fifteen();
    test_repeats();
    test_hold_and_lifecycle();
    test_clock();
    test_through_the_reader();
    test_eviction();
    test_service_repeats();
    printf("rift_tropo_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
