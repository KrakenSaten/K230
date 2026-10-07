/*
 * RX LOG's ring, its mesh.rx reader and its words (apps/rift/rift_rxlog.h).
 *
 * No LVGL and no socket: events are handed to the reader as the service
 * would write them, and what comes out is read back the way the screen
 * reads it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_format.h"
#include "rift_rxlog.h"

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

/* 2026-10-05T12:43:08.000Z on the wall clock is 500000 ms on the monotonic
 * one, so a reception at mono 500412 is at 12:43:08.412. */
#define MONO0 500000LL
#define WALL0 1791204188000LL

static struct rift_rxlog g_log;

static int apply(struct rift_rxlog *log, const char *json)
{
    cJSON *data = cJSON_Parse(json);
    int rc = rift_rxlog_apply(log, data, MONO0 + 1000, WALL0 + 1000);

    cJSON_Delete(data);
    return rc;
}

/* An event with the fields every reception has, each field of `extra`
 * (an object's members, without the braces) replacing or adding one. */
static int frame(struct rift_rxlog *log, long seq, long mono, const char *extra)
{
    char buf[2048];
    cJSON *base;
    cJSON *over;
    cJSON *item;
    int rc;

    snprintf(buf, sizeof(buf),
             "{\"v\":1,\"seq\":%ld,\"mono_ms\":%ld,\"bytes\":42,\"rssi_dbm\":-71,\"snr_db\":9,"
             "\"verdict\":\"new\",\"type\":\"group_text\",\"type_code\":5,\"route\":\"flood\","
             "\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":0,"
             "\"hash\":\"3c9a000000000001\",\"dup\":1,\"relayed\":false,\"own\":false}",
             seq, mono);
    base = cJSON_Parse(buf);
    snprintf(buf, sizeof(buf), "{%s}", extra);
    over = cJSON_Parse(buf);
    if (!base || !over) {
        fprintf(stderr, "fixture does not parse: %s\n", buf);
        exit(2);
    }
    for (item = over->child; item; item = item->next) {
        cJSON *copy = cJSON_Duplicate(item, 1);

        if (cJSON_GetObjectItemCaseSensitive(base, item->string)) {
            cJSON_ReplaceItemInObjectCaseSensitive(base, item->string, copy);
        } else {
            cJSON_AddItemToObject(base, item->string, copy);
        }
    }
    rc = rift_rxlog_apply(log, base, MONO0 + 1000, WALL0 + 1000);
    cJSON_Delete(base);
    cJSON_Delete(over);
    return rc;
}

static const char *line(void (*fmt)(const struct rift_rx_entry *, char *, size_t),
                        const struct rift_rx_entry *e)
{
    static char buf[RIFT_RXLOG_LINE_MAX];

    fmt(e, buf, sizeof(buf));
    return buf;
}

static void test_a_reception(void)
{
    const struct rift_rx_entry *e;
    char state[24];

    rift_rxlog_init(&g_log);
    check("a channel message is taken",
          frame(&g_log, 7, MONO0 + 412,
                "\"path_hops\":4,\"path_hex\":\"6e677473\",\"channel_hash\":\"a7\","
                "\"channel_known\":true,\"decoded\":\"channel\",\"channel_name\":\"Public\","
                "\"sender\":\"Anna\",\"text\":\"Kommer opp om 10 min \xF0\x9F\x91\x8D\"") == 0);
    e = rift_rxlog_at(&g_log, 0);
    check("it is the newest entry", e != NULL && g_log.count == 1);
    if (!e) {
        return;
    }
    rift_rxlog_state_word(e, state, sizeof(state));
    check("RX", strcmp(state, "RX") == 0);
    check("MSG", strcmp(rift_rxlog_type_word(e), "MSG") == 0);
    check("at its time of day, to the millisecond",
          strcmp(line(rift_rxlog_fmt_time, e), "12:43:08.412") == 0);
    check("CH:A7", strcmp(line(rift_rxlog_fmt_channel, e), "CH:A7") == 0);
    check("H:3C9A, the packet hash", strcmp(line(rift_rxlog_fmt_hash, e), "H:3C9A") == 0);
    check("42B", strcmp(line(rift_rxlog_fmt_size, e), "42B") == 0);
    check("RSSI with a real minus", strcmp(line(rift_rxlog_fmt_rssi, e), "RSSI:" RIFT_MINUS "71") == 0);
    check("SNR:9", strcmp(line(rift_rxlog_fmt_snr, e), "SNR:9") == 0);
    check("the whole path", strcmp(line(rift_rxlog_fmt_path, e), "PATH  6E > 67 > 74 > 73") == 0);
    check("the channel and the claimed sender",
          strcmp(line(rift_rxlog_fmt_who, e), "#Public Anna?:") == 0);
    check("and the text, emoji and all",
          strcmp(line(rift_rxlog_fmt_what, e), "\"Kommer opp om 10 min \xF0\x9F\x91\x8D\"") == 0);
    check("graded good", rift_rxlog_rssi_grade(e) == 2 && rift_rxlog_snr_grade(e) == 2);
}

static void test_duplicates(void)
{
    const struct rift_rx_entry *e[4];
    char s1[24];
    char s2[24];

    rift_rxlog_init(&g_log);
    frame(&g_log, 1, MONO0 + 412,
          "\"channel_hash\":\"a7\",\"channel_known\":true,\"decoded\":\"channel\","
          "\"channel_name\":\"Public\",\"sender\":\"Anna\",\"text\":\"hei\"");
    frame(&g_log, 2, MONO0 + 527,
          "\"dup\":2,\"verdict\":\"duplicate\",\"rssi_dbm\":-76,\"snr_db\":7,"
          "\"path_hops\":2,\"path_hex\":\"6e67\",\"channel_hash\":\"a7\",\"channel_known\":true");
    frame(&g_log, 3, MONO0 + 801,
          "\"dup\":3,\"verdict\":\"duplicate\",\"rssi_dbm\":-110,\"snr_db\":-7.25,"
          "\"channel_hash\":\"a7\",\"channel_known\":true");
    check("three receptions are three entries, not one", g_log.count == 3 &&
                                                              rift_rxlog_count(&g_log, RIFT_RXF_ALL) == 3);
    check("newest first", rift_rxlog_window(&g_log, RIFT_RXF_ALL, 0, e, 4) == 3);
    rift_rxlog_state_word(e[1], s1, sizeof(s1));
    rift_rxlog_state_word(e[0], s2, sizeof(s2));
    check("DUP #2", strcmp(s1, "DUP #2") == 0);
    check("DUP #3", strcmp(s2, "DUP #3") == 0);
    check("each keeps its own signal", e[1]->rssi_dbm == -76.0f && e[0]->snr_db == -7.25f);
    check("and its own path", strcmp(line(rift_rxlog_fmt_path, e[1]), "PATH  6E > 67") == 0 &&
                                  strcmp(line(rift_rxlog_fmt_path, e[0]), "PATH  0 HOP") == 0);
    check("the repeat shows the text the first copy decoded",
          strcmp(line(rift_rxlog_fmt_what, e[1]), "\"hei\"") == 0 &&
              (e[1]->flags & RIFT_RXF_TEXT_BORROWED));
    check("with its sender", strcmp(line(rift_rxlog_fmt_who, e[0]), "#Public Anna?:") == 0);
    check("one decimal where there is one", strcmp(line(rift_rxlog_fmt_snr, e[0]), "SNR:" RIFT_MINUS "7.2") == 0 ||
                                                strcmp(line(rift_rxlog_fmt_snr, e[0]), "SNR:" RIFT_MINUS "7.3") == 0);
    check("weak is weak", rift_rxlog_rssi_grade(e[0]) == 0 && rift_rxlog_snr_grade(e[0]) == 0);
    check("good is good", rift_rxlog_rssi_grade(e[1]) == 2 && rift_rxlog_snr_grade(e[1]) == 2);
    {
        struct rift_rx_entry fair = *e[1];

        fair.rssi_dbm = -95.0f;
        fair.snr_db = 0.0f;
        check("and fair is fair", rift_rxlog_rssi_grade(&fair) == 1 && rift_rxlog_snr_grade(&fair) == 1);
        fair.flags &= ~(RIFT_RXF_HAVE_RSSI | RIFT_RXF_HAVE_SNR);
        check("not measured is no grade", rift_rxlog_rssi_grade(&fair) == -1 &&
                                              rift_rxlog_snr_grade(&fair) == -1);
    }
    check("DUP shows only the repeats", rift_rxlog_count(&g_log, RIFT_RXF_DUP) == 2);
    check("MSG shows all three", rift_rxlog_count(&g_log, RIFT_RXF_MSG) == 3);
    check("ADV and CTRL none", rift_rxlog_count(&g_log, RIFT_RXF_ADV) == 0 &&
                                   rift_rxlog_count(&g_log, RIFT_RXF_CTRL) == 0);

    /* The copy MeshCore decoded can be reported after its repeat (a weak
     * first copy waits out its receive delay): both still end up readable,
     * and in the order they were heard. */
    rift_rxlog_init(&g_log);
    frame(&g_log, 2, MONO0 + 900, "\"dup\":2,\"verdict\":\"new\",\"channel_hash\":\"a7\","
                                  "\"channel_known\":true,\"hash\":\"aaaa000000000002\"");
    frame(&g_log, 1, MONO0 + 100, "\"dup\":1,\"verdict\":\"duplicate\",\"channel_hash\":\"a7\","
                                  "\"channel_known\":true,\"hash\":\"aaaa000000000002\","
                                  "\"decoded\":\"channel\",\"channel_name\":\"Public\","
                                  "\"sender\":\"Ola\",\"text\":\"sent\"");
    check("a late report goes in where it was heard",
          rift_rxlog_at(&g_log, 1) && rift_rxlog_at(&g_log, 1)->seq == 1 &&
              rift_rxlog_at(&g_log, 0)->seq == 2);
    check("and lends its text to the repeat already shown",
          strcmp(rift_rxlog_at(&g_log, 0)->text, "sent") == 0);
}

static void test_direct_and_unreadable(void)
{
    const struct rift_rx_entry *e;

    rift_rxlog_init(&g_log);
    frame(&g_log, 1, MONO0, "\"type\":\"text\",\"type_code\":2,\"route\":\"direct\","
                            "\"path_hops\":1,\"path_hex\":\"4d\",\"dest_hash\":\"73\","
                            "\"for_us\":true,\"src_hash\":\"6e\",\"decoded\":\"direct\","
                            "\"sender\":\"Anna\",\"recipient\":\"Mstr_k230\",\"text\":\"hei\","
                            "\"sender_public_key\":\"" "0123456789abcdef0123456789abcdef"
                            "0123456789abcdef0123456789abcdef" "\"");
    e = rift_rxlog_at(&g_log, 0);
    check("a direct message", e && e->decode == RIFT_RXD_DIRECT);
    if (!e) {
        return;
    }
    check("DM sender -> recipient, no claim mark",
          strcmp(line(rift_rxlog_fmt_who, e), "DM Anna \xE2\x86\x92 Mstr_k230:") == 0);
    check("a direct packet's path is the route still to go",
          strcmp(line(rift_rxlog_fmt_path, e), "ROUTE 4D") == 0);
    check("coloured by the sender's key, as its conversation is",
          (e->flags & RIFT_RXF_HAVE_KEY) &&
              e->sender_ident == rift_ident_hash("0123456789abcdef0123456789abcdef"
                                                 "0123456789abcdef0123456789abcdef"));
    check("CH:-- where there is no channel", strcmp(line(rift_rxlog_fmt_channel, e), "CH:--") == 0);

    frame(&g_log, 2, MONO0 + 1, "\"type\":\"text\",\"type_code\":2,\"dest_hash\":\"b2\","
                                "\"for_us\":false,\"src_hash\":\"4d\",\"hash\":\"bbbb000000000003\"");
    e = rift_rxlog_at(&g_log, 0);
    check("a packet for somebody else is encrypted, and says between whom",
          strcmp(line(rift_rxlog_fmt_what, e), "[ENCRYPTED" RIFT_SEP "4D \xE2\x86\x92 B2]") == 0);
    check("with nobody named", line(rift_rxlog_fmt_who, e)[0] == '\0');

    frame(&g_log, 3, MONO0 + 2, "\"channel_hash\":\"11\",\"channel_known\":false,"
                                "\"hash\":\"cccc000000000004\"");
    check("a channel this node does not hold",
          strcmp(line(rift_rxlog_fmt_what, rift_rxlog_at(&g_log, 0)),
                 "[ENCRYPTED" RIFT_SEP "NO KEY FOR THIS CHANNEL]") == 0);
    frame(&g_log, 4, MONO0 + 3, "\"channel_hash\":\"a7\",\"channel_known\":true,"
                                "\"hash\":\"dddd000000000005\"");
    check("a held channel that did not decode is unreadable",
          strcmp(line(rift_rxlog_fmt_what, rift_rxlog_at(&g_log, 0)), "[UNREADABLE]") == 0);

    frame(&g_log, 5, MONO0 + 4, "\"type\":\"control\",\"type_code\":11,\"route\":\"direct\","
                                "\"control_flags\":146,\"hash\":\"eeee000000000006\"");
    e = rift_rxlog_at(&g_log, 0);
    check("a discovery response is DISC", strcmp(rift_rxlog_type_word(e), "DISC") == 0);
    check("and has nothing to read: no third line", line(rift_rxlog_fmt_what, e)[0] == '\0');
    check("CTRL shows it", rift_rxlog_matches(e, RIFT_RXF_CTRL) && !rift_rxlog_matches(e, RIFT_RXF_MSG));

    frame(&g_log, 6, MONO0 + 5, "\"type\":\"advert\",\"type_code\":4,\"decoded\":\"advert\","
                                "\"sender\":\"HYTTA\",\"src_hash\":\"9f\","
                                "\"hash\":\"ffff000000000007\"");
    e = rift_rxlog_at(&g_log, 0);
    check("an advert is ADV, named", strcmp(rift_rxlog_type_word(e), "ADV") == 0 &&
                                         strcmp(line(rift_rxlog_fmt_who, e), "ADV HYTTA") == 0);

    /* Rejected before MeshCore: only what the service could know. */
    check("an unparsed frame is taken", apply(&g_log, "{\"v\":1,\"seq\":8,\"mono_ms\":500010,"
                                                      "\"bytes\":5,\"rssi_dbm\":-88,\"snr_db\":2,"
                                                      "\"verdict\":\"rejected\",\"reject\":\"unparsed\","
                                                      "\"header\":21,\"type\":\"group_text\","
                                                      "\"type_code\":5,\"route\":\"flood\"}") == 0);
    e = rift_rxlog_at(&g_log, 0);
    check("REJ", e && e->state == RIFT_RXS_REJECTED);
    check("with no hash and no path", strcmp(line(rift_rxlog_fmt_hash, e), "H:--") == 0 &&
                                          strcmp(line(rift_rxlog_fmt_path, e), "PATH  --") == 0);
    check("and says why", strcmp(line(rift_rxlog_fmt_what, e),
                                 "[REJECTED" RIFT_SEP "NOT A VALID FRAME]") == 0);
    apply(&g_log, "{\"v\":1,\"seq\":9,\"mono_ms\":500011,\"bytes\":30,\"verdict\":\"rejected\","
                  "\"reject\":\"queue_full\"}");
    e = rift_rxlog_at(&g_log, 0);
    check("a dropped frame with no signal says so", e && e->state == RIFT_RXS_REJECTED &&
                                                        strcmp(line(rift_rxlog_fmt_rssi, e), "RSSI:--") == 0 &&
                                                        strcmp(rift_rxlog_type_word(e), "UNK") == 0);
}

static void test_long_path(void)
{
    char json[512];
    char hex[129];
    char want[400];
    size_t at = 0;
    int i;

    for (i = 0; i < 32; i++) {
        snprintf(hex + 4 * i, sizeof(hex) - (size_t)(4 * i), "%02x%02x", 0xA0 + i, i);
        at += (size_t)snprintf(want + at, sizeof(want) - at, "%s%02X%02X", i == 0 ? "PATH  " : " > ",
                               0xA0 + i, i);
    }
    rift_rxlog_init(&g_log);
    snprintf(json, sizeof(json), "\"path_hash_size\":2,\"path_hops\":32,\"path_hex\":\"%s\"", hex);
    check("a 64-byte path is taken", frame(&g_log, 1, MONO0, json) == 0);
    check("every hop is printed, none elided",
          strcmp(line(rift_rxlog_fmt_path, rift_rxlog_at(&g_log, 0)), want) == 0 &&
              strstr(want, RIFT_ELLIPSIS) == NULL);

    rift_rxlog_init(&g_log);
    frame(&g_log, 1, MONO0, "\"type\":\"trace\",\"type_code\":9,\"route\":\"direct\","
                            "\"path_kind\":\"snr\",\"path_hops\":2,\"path_hex\":\"26f2\"");
    check("a TRACE prints the SNRs it collected",
          strcmp(line(rift_rxlog_fmt_path, rift_rxlog_at(&g_log, 0)), "SNRS  +9.50 > -3.50") == 0);
}

static void test_malformed(void)
{
    static const char *const bad[] = {
        "[]",
        "{}",
        "{\"v\":2,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\"}",
        "{\"v\":1,\"seq\":-1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":256,\"verdict\":\"new\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"maybe\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"rssi_dbm\":\"-70\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"type_code\":16,\"route\":\"flood\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"type_code\":5}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"path_kind\":\"hops\","
        "\"path_hash_size\":1,\"path_hops\":3,\"path_hex\":\"6e67\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"path_kind\":\"hops\","
        "\"path_hash_size\":4,\"path_hops\":1,\"path_hex\":\"6e676767\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"path_kind\":\"hops\","
        "\"path_hash_size\":1,\"path_hops\":1,\"path_hex\":\"zz\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"hash\":\"3c9a\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"channel_hash\":\"a\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"rejected\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"decoded\":\"direct\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"decoded\":\"channel\","
        "\"text\":5}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"decoded\":\"advert\","
        "\"sender\":\"x\",\"sender_public_key\":\"abc\"}",
        "{\"v\":1,\"seq\":1,\"mono_ms\":1,\"bytes\":1,\"verdict\":\"new\",\"dup\":0}",
    };
    size_t i;
    int refused = 0;

    rift_rxlog_init(&g_log);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        refused += apply(&g_log, bad[i]) != 0;
    }
    check("every malformed event is refused", refused == (int)(sizeof(bad) / sizeof(bad[0])));
    check("counted, and nothing half-added",
          g_log.malformed == (unsigned)refused && g_log.count == 0);
    check("a NULL event is refused too", rift_rxlog_apply(&g_log, NULL, 0, 0) != 0);
    {
        /* A name longer than its field, and one cut inside a character:
         * bounded, and still UTF-8. */
        char json[1024];
        char name[200];

        memset(name, 'x', sizeof(name));
        memcpy(name + 30, "\xC3\xA5\xC3\xA5", 4);
        name[sizeof(name) - 1] = '\0';
        snprintf(json, sizeof(json), "\"decoded\":\"advert\",\"sender\":\"%s\"", name);
        check("an over-long name is taken", frame(&g_log, 1, MONO0, json) == 0);
        check("and cut on a character", strlen(rift_rxlog_at(&g_log, 0)->sender) <= 32 &&
                                            rift_utf8_valid(rift_rxlog_at(&g_log, 0)->sender));
    }
}

static void test_ring_clear_pause(void)
{
    static struct rift_rxlog log;
    const struct rift_rx_entry *e;
    int i;

    rift_rxlog_init(&log);
    for (i = 1; i <= RIFT_RXLOG_MAX + 250; i++) {
        char extra[64];

        snprintf(extra, sizeof(extra), "\"hash\":\"%016x\"", i);
        frame(&log, i, MONO0 + i, extra);
    }
    check("the ring holds its bound", log.count == RIFT_RXLOG_MAX);
    check("the oldest went first", rift_rxlog_at(&log, RIFT_RXLOG_MAX - 1)->seq == 251 &&
                                       rift_rxlog_at(&log, 0)->seq == RIFT_RXLOG_MAX + 250);
    check("and are counted", log.evicted == 250 && log.total == RIFT_RXLOG_MAX + 250);
    check("counting across the wrap", rift_rxlog_count(&log, RIFT_RXF_ALL) == RIFT_RXLOG_MAX);

    /* PAUSE holds what is shown: the entry at the top stays where it is
     * while capture goes on above it. */
    log.paused = 1;
    log.top_uid = rift_rxlog_at(&log, 0)->uid;
    frame(&log, 2000, MONO0 + 2000, "\"hash\":\"00000000000007d0\"");
    frame(&log, 2001, MONO0 + 2001, "\"hash\":\"00000000000007d1\"");
    check("capture goes on while paused", rift_rxlog_at(&log, 0)->seq == 2001);
    check("and the held entry is two down", rift_rxlog_position(&log, RIFT_RXF_ALL, log.top_uid) == 2);
    e = rift_rxlog_find(&log, log.top_uid);
    check("found by its id", e && e->seq == RIFT_RXLOG_MAX + 250);

    rift_rxlog_mark(&log, "MESHCORED NOT ANSWERING", MONO0 + 3000, WALL0 + 3000);
    check("a mark is an entry, in every filter",
          rift_rxlog_at(&log, 0)->kind == RIFT_RX_MARK &&
              rift_rxlog_matches(rift_rxlog_at(&log, 0), RIFT_RXF_DUP) &&
              strcmp(line(rift_rxlog_fmt_what, rift_rxlog_at(&log, 0)), "MESHCORED NOT ANSWERING") == 0);

    log.malformed = 3;
    log.supported = 1;
    rift_rxlog_clear(&log);
    check("CLEAR empties the log", log.count == 0 && rift_rxlog_at(&log, 0) == NULL);
    check("and forgets what was held and selected", log.top_uid == 0 && log.selected_uid == 0);
    check("and nothing else", log.malformed == 3 && log.supported == 1 && log.paused == 1);
    frame(&log, 1, MONO0, "");
    check("and fills again", log.count == 1);
}

static void test_no_wall_clock(void)
{
    cJSON *d = cJSON_Parse("{\"v\":1,\"seq\":1,\"mono_ms\":3723456,\"bytes\":10,"
                           "\"verdict\":\"new\"}");

    rift_rxlog_init(&g_log);
    rift_rxlog_apply(&g_log, d, 3723456, 1000); /* a board that thinks it is 1970 */
    cJSON_Delete(d);
    check("with the wall clock unset, the time since boot",
          strcmp(line(rift_rxlog_fmt_time, rift_rxlog_at(&g_log, 0)), "01:02:03.456") == 0 &&
              !(rift_rxlog_at(&g_log, 0)->flags & RIFT_RXF_HAVE_WALL));
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    test_a_reception();
    test_duplicates();
    test_direct_and_unreadable();
    test_long_path();
    test_malformed();
    test_ring_clear_pause();
    test_no_wall_clock();
    printf("rift_rxlog_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
