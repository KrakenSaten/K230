/*
 * PacketShark's line protocol (core/pocketpcap/ppc_proto.h).
 *
 * What is checked: every command the app sends parses back to what was
 * meant; commands typed by hand with spaces parse too; every event the
 * helper sends parses back field for field; bad arguments, wrong field
 * counts, over-long lines, interface names that could be paths and numbers
 * out of range are refused (BAD), unknown words are ignored (NONE); free
 * text loses its tabs and newlines; a full stats line and a row with the
 * longest fields stay within PPC_LINE_MAX; the capture-filter presets.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_proto.h"

#include <stdio.h>
#include <string.h>

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

static void test_commands(void)
{
    char line[PPC_LINE_MAX];
    struct ppc_cmd c;

    check("start formats", ppc_cmd_format_start(line, sizeof(line), "eth0", PPC_KEEP_HEADERS, 0,
                                                PPC_CFILTER_NAMES, 900) > 0 &&
                               strcmp(line, "start\teth0\theaders\t0\tnames\t900") == 0);
    check("and parses back", ppc_cmd_parse(line, &c) == PPC_CMD_START && strcmp(c.iface, "eth0") == 0 &&
                                 c.keep == PPC_KEEP_HEADERS && c.promisc == 0 && c.cfilter == PPC_CFILTER_NAMES &&
                                 c.limit_s == 900);
    check("typed by hand with spaces", ppc_cmd_parse("  start eth0 full 1 all 0  \n", &c) == PPC_CMD_START &&
                                           c.keep == PPC_KEEP_FULL && c.promisc == 1 && c.limit_s == 0);
    check("an interface that is a path is refused at both ends",
          ppc_cmd_format_start(line, sizeof(line), "../x", PPC_KEEP_HEADERS, 0, 0, 0) < 0 &&
              ppc_cmd_parse("start ../x headers 0 all 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start abcdefghijklmnop headers 0 all 0", &c) == PPC_CMD_BAD);
    check("a bad keep, promisc, filter or limit is refused",
          ppc_cmd_parse("start eth0 most 0 all 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start eth0 headers 2 all 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start eth0 headers 0 bpf 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start eth0 headers 0 all -1", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start eth0 headers 0 all 99999999999", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("start eth0 headers 0 all", &c) == PPC_CMD_BAD);
    check("hold", ppc_cmd_format_hold(line, sizeof(line), 1) > 0 && ppc_cmd_parse(line, &c) == PPC_CMD_HOLD &&
                      c.on == 1 && ppc_cmd_parse("hold 0", &c) == PPC_CMD_HOLD && c.on == 0 &&
                      ppc_cmd_parse("hold 2", &c) == PPC_CMD_BAD && ppc_cmd_parse("hold", &c) == PPC_CMD_BAD);
    check("stop, discard, stats, quit", ppc_cmd_parse("stop", &c) == PPC_CMD_STOP &&
                                            ppc_cmd_parse("discard\n", &c) == PPC_CMD_DISCARD &&
                                            ppc_cmd_parse("stats", &c) == PPC_CMD_STATS &&
                                            ppc_cmd_parse("quit", &c) == PPC_CMD_QUIT &&
                                            ppc_cmd_parse("stop now", &c) == PPC_CMD_BAD);
    check("window by seq", ppc_cmd_format_window(line, sizeof(line), 7, 1234, 5, 6) > 0 &&
                               ppc_cmd_parse(line, &c) == PPC_CMD_WINDOW && c.req == 7 && !c.tail &&
                               c.seq == 1234 && c.before == 5 && c.after == 6);
    check("window at the tail", ppc_cmd_format_window(line, sizeof(line), 8, 0, 20, 0) > 0 &&
                                    ppc_cmd_parse(line, &c) == PPC_CMD_WINDOW && c.tail && c.before == 20);
    check("window with a bad anchor", ppc_cmd_parse("window 1 seq:0 1 1", &c) == PPC_CMD_BAD &&
                                          ppc_cmd_parse("window 1 middle 1 1", &c) == PPC_CMD_BAD);
    check("an unknown word is ignored, an empty line too",
          ppc_cmd_parse("filter 1 255", &c) == PPC_CMD_NONE && ppc_cmd_parse("", &c) == PPC_CMD_NONE &&
              ppc_cmd_parse("   \n", &c) == PPC_CMD_NONE);
    {
        char longline[PPC_LINE_MAX + 50];

        memset(longline, 'a', sizeof(longline) - 1);
        longline[sizeof(longline) - 1] = '\0';
        check("an over-long line is refused", ppc_cmd_parse(longline, &c) == PPC_CMD_BAD);
    }
}

static void test_events(void)
{
    char line[PPC_LINE_MAX];
    struct ppc_evt e;
    struct ppc_counters c;
    struct ppc_evt_row row;
    uint32_t bins[PPC_CLASS_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int i;
    int ok = 1;

    check("hello", ppc_evt_format_hello(line, sizeof(line), "0.4.0", "pcap") > 0 &&
                       ppc_evt_parse(line, &e) == PPC_EVT_HELLO && e.u.hello.proto == PPC_PROTO_VERSION &&
                       strcmp(e.u.hello.version, "0.4.0") == 0 && strcmp(e.u.hello.backend, "pcap") == 0);
    check("state", ppc_evt_format_state(line, sizeof(line), PPC_STATE_STOPPED, PPC_REASON_LINK, "eth0", 100, 1) > 0 &&
                       ppc_evt_parse(line, &e) == PPC_EVT_STATE && e.u.state.state == PPC_STATE_STOPPED &&
                       e.u.state.reason == PPC_REASON_LINK && strcmp(e.u.state.iface, "eth0") == 0 &&
                       e.u.state.speed == 100 && e.u.state.clock_set == 1);
    check("state without an interface", ppc_evt_format_state(line, sizeof(line), PPC_STATE_IDLE, PPC_REASON_NONE,
                                                             NULL, 0, 0) > 0 &&
                                            ppc_evt_parse(line, &e) == PPC_EVT_STATE && e.u.state.iface[0] == 0);
    memset(&c, 0, sizeof(c));
    c.elapsed_ms = 123456;
    c.seen = 18446744073709551615ull;
    c.stored = 2;
    c.held = 3;
    c.kdrop = 4;
    c.ifdrop = 5;
    c.shed = 6;
    c.bytes = 7;
    c.pps = 8;
    c.bps = 9;
    c.first_seq = 10;
    c.last_seq = 4294967295u;
    c.match = 12;
    c.fill_permille = 1000;
    c.ui_skipped = 14;
    for (i = 0; i < PPC_CLASS_COUNT; i++) {
        c.cls[i] = 18446744073709551615ull - (uint64_t)i;
    }
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        c.sig[i] = 4294967295u;
    }
    check("the largest stats line fits", ppc_evt_format_stats(line, sizeof(line), &c) > 0 && strlen(line) < PPC_LINE_MAX);
    check("and parses back field for field", ppc_evt_parse(line, &e) == PPC_EVT_STATS &&
                                                 memcmp(&e.u.stats, &c, sizeof(c)) == 0);
    check("bin", ppc_evt_format_bin(line, sizeof(line), 42, bins, 9) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_BIN &&
                     e.u.bin.second == 42 && e.u.bin.cls[7] == 8 && e.u.bin.held == 9);
    check("rows.begin and rows.end",
          ppc_evt_format_rows_begin(line, sizeof(line), 3, 0, 5, 100, 6, 105) > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_ROWS_BEGIN && e.u.rows.req == 3 && e.u.rows.first_rank == 5 &&
              e.u.rows.last_seq == 105 && ppc_evt_format_rows_end(line, sizeof(line), 3) > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_ROWS_END && e.u.rows.req == 3);
    memset(&row, 0, sizeof(row));
    row.seq = 4294967295u;
    row.rel_us = 18446744073709551615ull;
    row.gap_us = 1;
    snprintf(row.dir, sizeof(row.dir), "bcast");
    row.cls = 7;
    snprintf(row.word, sizeof(row.word), "DHCPv6");
    row.len = 65535;
    row.signal = 65535;
    memset(row.src, 'f', sizeof(row.src) - 1);
    memset(row.dst, 'e', sizeof(row.dst) - 1);
    memset(row.info, 'i', sizeof(row.info) - 1);
    check("the longest row fits", ppc_evt_format_row(line, sizeof(line), &row) > 0 && strlen(line) < PPC_LINE_MAX);
    check("and parses back", ppc_evt_parse(line, &e) == PPC_EVT_ROW && memcmp(&e.u.row, &row, sizeof(row)) == 0);
    snprintf(row.info, sizeof(row.info), "a\tb\nc");
    check("tabs and newlines in text are made harmless", ppc_evt_format_row(line, sizeof(line), &row) > 0 &&
                                                             ppc_evt_parse(line, &e) == PPC_EVT_ROW &&
                                                             strcmp(e.u.row.info, "a.b.c") == 0);
    check("error with text", ppc_evt_format_error(line, sizeof(line), "busy", "another\tcapture") > 0 &&
                                 ppc_evt_parse(line, &e) == PPC_EVT_ERROR && strcmp(e.u.error.code, "busy") == 0 &&
                                 strcmp(e.u.error.text, "another.capture") == 0);
    check("note, bye", ppc_evt_format_note(line, sizeof(line), "caps", "dropped bnd=empty") > 0 &&
                           ppc_evt_parse(line, &e) == PPC_EVT_NOTE && strcmp(e.u.note.value, "dropped bnd=empty") == 0 &&
                           ppc_evt_format_bye(line, sizeof(line)) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_BYE);
    {
        static const char *const bad[] = {
            "hello\t1\t0.4.0",                       /* a field short */
            "state\tflying\tnone\t-\t0\t0",          /* unknown state */
            "state\tidle\tnone\t../x\t0\t0",         /* not an interface */
            "state\tidle\tnone\t-\t0\t2",            /* clock_set not 0/1 */
            "stats\t1\t2",                           /* far too short */
            "bin\t1\t2\t3",                          /* too short */
            "row\t1\t2\t3\tin\t9\tTCP\t60\t0\ta\tb\tc", /* class 9 */
            "row\t-1\t2\t3\tin\t0\tTCP\t60\t0\ta\tb\tc",
            "rows.begin\t1\t2\t3",
            "bye\tnow",
            "error\tonly-code",
        };
        unsigned k;

        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            if (ppc_evt_parse(bad[k], &e) != PPC_EVT_BAD) {
                ok = 0;
                printf("     accepted: %s\n", bad[k]);
            }
        }
        check("damaged lines are refused", ok);
    }
    check("an unknown event is ignored", ppc_evt_parse("graph\t1\t2", &e) == PPC_EVT_NONE &&
                                             ppc_evt_parse("", &e) == PPC_EVT_NONE);
    check("a buffer too small is refused, not overrun", ppc_evt_format_hello(line, 8, "0.4.0", "pcap") == -1 &&
                                                            line[0] == '\0');
}

static void test_presets(void)
{
    char buf[160];

    check("EVERYTHING is no filter", ppc_cfilter_expr(PPC_CFILTER_ALL, NULL, buf, sizeof(buf))[0] == '\0');
    check("NOT MY SSH names the address when there is one",
          strcmp(ppc_cfilter_expr(PPC_CFILTER_NOSELF, "192.0.2.10", buf, sizeof(buf)),
                 "not (tcp port 22 and host 192.0.2.10)") == 0);
    check("and the port alone when there is none",
          strcmp(ppc_cfilter_expr(PPC_CFILTER_NOSELF, NULL, buf, sizeof(buf)), "not tcp port 22") == 0);
    check("ids round trip", ppc_cfilter_parse(ppc_cfilter_id(PPC_CFILTER_IP)) == PPC_CFILTER_IP &&
                                ppc_cfilter_parse("nonsense") == -1 && ppc_cfilter_parse(NULL) == -1);
    check("interface names", ppc_iface_ok("eth0") && ppc_iface_ok("wlan1") && !ppc_iface_ok("") &&
                                 !ppc_iface_ok(".") && !ppc_iface_ok("..") && !ppc_iface_ok("a/b") &&
                                 !ppc_iface_ok("a b") && !ppc_iface_ok("x\t") && !ppc_iface_ok(NULL));
}

/* M5, M6: the view, the window's anchors, inspect, pin and their events. */
static void test_list_and_inspector(void)
{
    char line[PPC_LINE_MAX];
    struct ppc_cmd c;
    struct ppc_evt e;
    struct ppc_evt_pkt p;
    struct ppc_evt_end src;
    struct ppc_evt_end dst;
    struct ppc_evt_row row;
    char marks[64];

    check("view formats and parses", ppc_cmd_format_view(line, sizeof(line), 9, 2, 0x01, "192.0.2.10", 443, 1) > 0 &&
                                         strcmp(line, "view\t9\tlen\t1\t192.0.2.10\t443\t1") == 0 &&
                                         ppc_cmd_parse(line, &c) == PPC_CMD_VIEW && c.view_id == 9 && c.sort == 2 &&
                                         c.classes == 1 && strcmp(c.host, "192.0.2.10") == 0 && c.port == 443 &&
                                         c.pinned == 1);
    check("view with no host, by hand", ppc_cmd_parse("view 1 time ff - 0 0", &c) == PPC_CMD_VIEW && c.host[0] == '\0' &&
                                            c.classes == 0xff && c.sort == 0);
    check("a view's bad words are refused at both ends",
          ppc_cmd_format_view(line, sizeof(line), 1, 3, 0xff, NULL, 0, 0) < 0 &&
              ppc_cmd_format_view(line, sizeof(line), 1, 0, 0xff, "a b", 0, 0) < 0 &&
              ppc_cmd_parse("view 1 size ff - 0 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("view 1 time 100 - 0 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("view 1 time ff host;rm 0 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("view 1 time ff - 70000 0", &c) == PPC_CMD_BAD &&
              ppc_cmd_parse("view 1 time ff - 0 2", &c) == PPC_CMD_BAD);
    check("window at a rank and at a time",
          ppc_cmd_format_window_at(line, sizeof(line), 3, PPC_ANCHOR_RANK, 40, 15, 29) > 0 &&
              ppc_cmd_parse(line, &c) == PPC_CMD_WINDOW && c.anchor == PPC_ANCHOR_RANK && c.at == 40 &&
              ppc_cmd_format_window_at(line, sizeof(line), 4, PPC_ANCHOR_US, 4000000000ull, 0, 29) > 0 &&
              ppc_cmd_parse(line, &c) == PPC_CMD_WINDOW && c.anchor == PPC_ANCHOR_US && c.at == 4000000000ull &&
              ppc_cmd_format_window_at(line, sizeof(line), 5, PPC_ANCHOR_SEQ, 0, 1, 1) < 0);
    check("the tail and a seq say their anchor too", ppc_cmd_parse("window 1 tail 3 0", &c) == PPC_CMD_WINDOW &&
                                                         c.anchor == PPC_ANCHOR_TAIL &&
                                                         ppc_cmd_parse("window 1 seq:9 3 0", &c) == PPC_CMD_WINDOW &&
                                                         c.anchor == PPC_ANCHOR_SEQ && c.seq == 9);
    check("inspect and its steps", ppc_cmd_format_inspect(line, sizeof(line), 6, 48196, -1) > 0 &&
                                       ppc_cmd_parse(line, &c) == PPC_CMD_INSPECT && c.req == 6 && c.seq == 48196 &&
                                       c.step == -1 && ppc_cmd_parse("inspect 1 5 2", &c) == PPC_CMD_BAD &&
                                       ppc_cmd_parse("inspect 1 0 0", &c) == PPC_CMD_BAD);
    check("pin", ppc_cmd_format_pin(line, sizeof(line), 5, 1) > 0 && ppc_cmd_parse(line, &c) == PPC_CMD_PIN &&
                     c.seq == 5 && c.on == 1 && ppc_cmd_parse("pin 5 2", &c) == PPC_CMD_BAD);

    memset(&row, 0, sizeof(row));
    row.seq = 3;
    snprintf(row.dir, sizeof(row.dir), "in");
    snprintf(row.word, sizeof(row.word), "TCP");
    row.flags = PPC_ROWF_PINNED | PPC_ROWF_GONE;
    check("a row carries its flags", ppc_evt_format_row(line, sizeof(line), &row) > 0 &&
                                         ppc_evt_parse(line, &e) == PPC_EVT_ROW && e.u.row.flags == 3);
    check("a row of protocol 1 (no flags) still parses",
          ppc_evt_parse("row\t1\t0\t0\tin\t0\tTCP\t60\t0\ta\tb\tc", &e) == PPC_EVT_ROW && e.u.row.flags == 0);
    check("rows.begin with its anchor, and without",
          ppc_evt_format_rows_begin_at(line, sizeof(line), 2, 7, 10, 500, 1, 600, 14) > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_ROWS_BEGIN && e.u.rows.anchor_rank == 14 &&
              e.u.rows.filter_id == 7 && ppc_evt_parse("rows.begin\t2\t7\t10\t500\t1\t600", &e) == PPC_EVT_ROWS_BEGIN &&
              e.u.rows.anchor_rank == 10);

    memset(&p, 0, sizeof(p));
    p.req = 4;
    p.seq = 48196;
    p.rank = 12;
    p.match = 900;
    p.rel_us = 252318430;
    p.gap_us = 187554;
    snprintf(p.dir, sizeof(p.dir), "in");
    p.cls = PPC_CLASS_TCP;
    snprintf(p.word, sizeof(p.word), "TCP");
    p.wire_len = 1514;
    p.stored = 66;
    p.flags = PPC_ROWF_PINNED;
    check("pkt.begin round-trips", ppc_evt_format_pkt_begin(line, sizeof(line), &p) > 0 &&
                                       ppc_evt_parse(line, &e) == PPC_EVT_PKT_BEGIN && e.u.pkt.seq == 48196 &&
                                       e.u.pkt.rank == 12 && e.u.pkt.match == 900 && e.u.pkt.rel_us == 252318430 &&
                                       e.u.pkt.stored == 66 && e.u.pkt.flags == PPC_ROWF_PINNED &&
                                       strcmp(e.u.pkt.word, "TCP") == 0);
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    snprintf(src.addr, sizeof(src.addr), "203.0.113.42");
    snprintf(src.sub, sizeof(src.sub), "PORT 443 \xc2\xb7 HTTPS");
    snprintf(src.filter, sizeof(src.filter), "host:203.0.113.42");
    snprintf(dst.addr, sizeof(dst.addr), "192.0.2.20");
    check("pkt.ends: the mark in a sub line survives, an empty one comes back empty",
          ppc_evt_format_pkt_ends(line, sizeof(line), &src, &dst) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_PKT_ENDS &&
              strcmp(e.u.ends.src.sub, "PORT 443 \xc2\xb7 HTTPS") == 0 && e.u.ends.dst.sub[0] == '\0' &&
              e.u.ends.dst.filter[0] == '\0' && strcmp(e.u.ends.src.filter, "host:203.0.113.42") == 0);
    check("layer, with and without a class",
          ppc_evt_format_layer(line, sizeof(line), 3, PPC_CLASS_TCP, "TCP", "443 \xe2\x86\x92 51234 \xc2\xb7 ACK") > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_LAYER && e.u.layer.index == 3 && e.u.layer.cls == 0 &&
              strcmp(e.u.layer.text, "443 \xe2\x86\x92 51234 \xc2\xb7 ACK") == 0 &&
              ppc_evt_format_layer(line, sizeof(line), 0, -1, "Frame", "60 B") > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_LAYER && e.u.layer.cls == -1);
    check("field, with bytes and a filter, and without",
          ppc_evt_format_field(line, sizeof(line), 3, "SOURCE PORT", "443 https", 34, 2, "port:443") > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_FIELD && e.u.field.off == 34 && e.u.field.len == 2 &&
              strcmp(e.u.field.filter, "port:443") == 0 &&
              ppc_evt_format_field(line, sizeof(line), 0, "STORED", "66 bytes", -1, 9, NULL) > 0 &&
              ppc_evt_parse(line, &e) == PPC_EVT_FIELD && e.u.field.off == -1 && e.u.field.len == 0 &&
              e.u.field.filter[0] == '\0');
    check("hex, pkt.end and pinned", ppc_evt_format_hex(line, sizeof(line), 8, "5e 10 00 01  08 00 45 00", "^.....E.") > 0 &&
                                         ppc_evt_parse(line, &e) == PPC_EVT_HEX && e.u.hex.off == 8 &&
                                         strcmp(e.u.hex.ascii, "^.....E.") == 0 &&
                                         ppc_evt_format_pkt_end(line, sizeof(line), 4) > 0 &&
                                         ppc_evt_parse(line, &e) == PPC_EVT_PKT_END && e.u.pkt.req == 4 &&
                                         ppc_evt_format_pinned(line, sizeof(line), 48196, 1, 3) > 0 &&
                                         ppc_evt_parse(line, &e) == PPC_EVT_PINNED && e.u.pinned.on == 1 &&
                                         e.u.pinned.pins == 3);
    check("damaged inspector lines are refused", ppc_evt_parse("layer\t1\t9\tX\ty", &e) == PPC_EVT_BAD &&
                                                     ppc_evt_parse("field\t1\tN\tv\t70000\t1\t-", &e) == PPC_EVT_BAD &&
                                                     ppc_evt_parse("pkt.begin\t1\t2", &e) == PPC_EVT_BAD &&
                                                     ppc_evt_parse("pinned\t1\t2\t3", &e) == PPC_EVT_BAD);
    snprintf(marks, sizeof(marks), "a\xc2\xb7" "b\xe2\x86\x92" "c\xe2\x86\t\xc3\xa9\x01");
    ppc_sanitize_marks(marks);
    check("the marks survive, a broken mark and other bytes do not",
          strcmp(marks, "a\xc2\xb7" "b\xe2\x86\x92" "c......") == 0);
    snprintf(marks, sizeof(marks), "a\xc2\xb7" "b");
    ppc_sanitize(marks);
    check("packet text keeps no mark", strcmp(marks, "a..b") == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_commands();
    test_events();
    test_presets();
    test_list_and_inspector();
    printf("ppc_proto_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
