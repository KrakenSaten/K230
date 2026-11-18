/*
 * PacketShark's shell-side model (apps/packetshark/psk_model.h), fed lines
 * as the helper writes them.
 *
 * What is checked: hello, state, stats, bin, error and note are applied and
 * say what changed; a new capture clears the last one's figures; bins of
 * skipped seconds are cleared, never left over; a window answer is taken
 * only for the newest request (a stale answer is dropped whole) and never
 * holds more than PSK_ROW_CACHE rows; damaged and unknown lines change
 * nothing; a helper that leaves by itself during a capture is UNEXPECTED,
 * one that leaves when idle is NORMAL; the exit texts.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_model.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;
static struct psk_model M;

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

static unsigned line(const char *l)
{
    struct ppc_evt e;

    ppc_evt_parse(l, &e);
    return psk_model_apply(&M, &e, 1000);
}

static void stats_line(char *buf, size_t n, uint64_t seen)
{
    struct ppc_counters c;

    memset(&c, 0, sizeof(c));
    c.seen = seen;
    c.stored = seen;
    c.elapsed_ms = seen * 10u;
    ppc_evt_format_stats(buf, n, &c);
}

int main(void)
{
    char buf[PPC_LINE_MAX];
    char t[64];
    int i;
    unsigned ch;

    setvbuf(stdout, NULL, _IOLBF, 0);
    psk_model_init(&M);
    psk_model_helper_started(&M, 0);
    check("hello", line("hello\t1\t0.4.0\tpcap\n") == PSK_CHANGED_HELLO && M.hello &&
                       strcmp(M.backend, "pcap") == 0);
    check("idle", line("state\tidle\tnone\t-\t0\t0") == PSK_CHANGED_STATE && M.state == PPC_STATE_IDLE &&
                      !psk_model_has_packets(&M));
    check("capturing", line("state\tcapturing\tnone\teth0\t100\t1") == PSK_CHANGED_STATE &&
                           M.state == PPC_STATE_CAPTURING && strcmp(M.iface, "eth0") == 0 && M.speed == 100 &&
                           M.clock_set && M.had_capture && psk_model_has_packets(&M));
    stats_line(buf, sizeof(buf), 500);
    check("stats", line(buf) == PSK_CHANGED_STATS && M.have_stats && M.c.seen == 500 && M.stats_ms == 1000);
    check("a bin", line("bin\t3\t1\t2\t3\t4\t5\t6\t7\t8\t9") == PSK_CHANGED_BIN && M.bins[3][7] == 8 &&
                       M.bin_held[3] == 9 && M.bin_next == 4);
    line("bin\t1\t9\t9\t9\t9\t9\t9\t9\t9\t9");
    check("an older bin fills its own slot, the newest stays", M.bins[1][0] == 9 && M.bin_next == 4);
    line("bin\t6\t1\t0\t0\t0\t0\t0\t0\t0\t0");
    check("skipped seconds 4 and 5 are empty", M.bins[4][0] == 0 && M.bins[5][0] == 0 && M.bins[6][0] == 1);
    line("bin\t500\t2\t0\t0\t0\t0\t0\t0\t0\t0");
    check("a jump past the ring clears it", M.bins[3][7] == 0 && M.bins[500 % PSK_BINS][0] == 2);
    check("error", line("error\tbusy\tanother capture is running") == PSK_CHANGED_ERROR &&
                       strcmp(M.err_code, "busy") == 0 && M.err_ms == 1000);
    check("note: caps and shed", line("note\tcaps\tdropped bnd=empty") == PSK_CHANGED_NOTE &&
                                     strcmp(M.caps, "dropped bnd=empty") == 0 &&
                                     line("note\tshed\t1") == PSK_CHANGED_NOTE && M.shed);
    check("damaged and unknown lines change nothing", line("stats\t1\t2") == 0 && line("graph\t1") == 0 &&
                                                          line("") == 0 && M.c.seen == 500);

    /* windows */
    M.want_req = 7;
    line("rows.begin\t6\t0\t0\t3\t1\t3");
    line("row\t1\t0\t0\tin\t0\tTCP\t60\t0\ta\tb\tstale");
    ch = line("rows.end\t6");
    check("an answer to an older request is dropped", ch == 0 && M.row_count == 0);
    line("rows.begin\t7\t0\t0\t60\t1\t60");
    for (i = 1; i <= 60; i++) {
        char r[200];

        snprintf(r, sizeof(r), "row\t%d\t%d\t0\tin\t0\tTCP\t60\t0\ta\tb\tinfo %d", i, i, i);
        line(r);
    }
    ch = line("rows.end\t7");
    check("the newest answer is taken, at most PSK_ROW_CACHE rows", ch == PSK_CHANGED_ROWS &&
                                                                        M.row_count == PSK_ROW_CACHE &&
                                                                        M.rows[0].seq == 1 && M.rows_last_seq == 60);
    check("rows outside an answer are ignored", line("row\t99\t0\t0\tin\t0\tTCP\t60\t0\ta\tb\tc") == 0 &&
                                                    M.row_count == PSK_ROW_CACHE);

    /* stop, then a new capture in a new helper */
    line("state\tstopped\tuser\teth0\t100\t1");
    check("stopped keeps its packets", M.state == PPC_STATE_STOPPED && psk_model_has_packets(&M));
    psk_model_helper_stopped(&M, PSK_EXIT_NORMAL, 0, 2000);
    check("a helper that leaves after STOP left normally", M.exit == PSK_EXIT_NORMAL && !M.running &&
                                                                !psk_model_has_packets(&M));
    check("its figures stay for the screen", M.c.seen == 500);
    psk_model_helper_started(&M, 3000);
    line("hello\t1\t0.4.0\tpcap");
    line("state\tidle\tnone\t-\t0\t0");
    line("state\tcapturing\tnone\teth0\t100\t1");
    check("a new capture starts from zero", M.c.seen == 0 && !M.have_stats && M.bins[6][0] == 0 &&
                                                M.bin_next == 0 && M.exit == PSK_EXIT_NONE);
    psk_model_helper_stopped(&M, PSK_EXIT_NORMAL, 0, 4000);
    check("a helper that leaves by itself mid-capture is UNEXPECTED", M.exit == PSK_EXIT_UNEXPECTED);

    check("exit texts", strcmp(psk_exit_text(PSK_EXIT_CRASHED, 139, t, sizeof(t)), "CRASHED (SIG 11)") == 0 &&
                            strcmp(psk_exit_text(PSK_EXIT_CRASHED, 137, t, sizeof(t)), "KILLED (SIG 9)") == 0 &&
                            strcmp(psk_exit_text(PSK_EXIT_HUNG, 0, t, sizeof(t)), "NOT RESPONDING") == 0 &&
                            strcmp(psk_exit_text(PSK_EXIT_START, 127, t, sizeof(t)), "HELPER MISSING") == 0 &&
                            strcmp(psk_exit_text(PSK_EXIT_PROTOCOL, 0, t, sizeof(t)), "PROTOCOL ERROR") == 0);
    {
        char b[32];

        check("count grouping", strcmp(psk_fmt_count(48417, b, sizeof(b)), "48 417") == 0 &&
                                    strcmp(psk_fmt_count(7, b, sizeof(b)), "7") == 0 &&
                                    strcmp(psk_fmt_count(1000000, b, sizeof(b)), "1 000 000") == 0);
        check("bytes", strcmp(psk_fmt_bytes(980, b, sizeof(b)), "980 B") == 0 &&
                           strcmp(psk_fmt_bytes(31400000, b, sizeof(b)), "31.4 MB") == 0 &&
                           strcmp(psk_fmt_bytes(123456, b, sizeof(b)), "123 KB") == 0);
        check("bits", strcmp(psk_fmt_bits(0, b, sizeof(b)), "0 BIT/S") == 0 &&
                          strcmp(psk_fmt_bits(3400000, b, sizeof(b)), "3.4 MBIT/S") == 0);
        check("elapsed", strcmp(psk_fmt_elapsed(7000, b, sizeof(b)), "00:07") == 0 &&
                             strcmp(psk_fmt_elapsed(253000, b, sizeof(b)), "04:13") == 0 &&
                             strcmp(psk_fmt_elapsed(3723000, b, sizeof(b)), "1:02:03") == 0);
    }
    {
        /* The graph's ladder (DS §59.5): h(n) = round(4 + 50 log10 n), 0 for
         * none, capped at the 208 px band. */
        static const uint64_t n[] = { 0, 1, 9, 10, 99, 100, 1000, 10000, 1000000 };
        static const int32_t h[] = { 0, 4, 52, 54, 104, 104, 154, 204, 208 };
        struct psk_graph_window w;
        int ok = 1;
        int mono = 1;
        uint64_t k;
        char b[64];

        for (i = 0; i < (int)(sizeof(n) / sizeof(n[0])); i++) {
            if (psk_graph_height_of(n[i]) != h[i]) {
                printf("     h(%llu) = %d, not %d\n", (unsigned long long)n[i], (int)psk_graph_height_of(n[i]),
                       (int)h[i]);
                ok = 0;
            }
        }
        check("the graph's ladder", ok);
        for (k = 1; k < 200000; k += k / 7 + 1) {
            if (psk_graph_height_of(k + 1) < psk_graph_height_of(k)) {
                mono = 0;
            }
        }
        check("the ladder never falls", mono);
        check("class hues and words", psk_class_hue(0) == 6 && psk_class_hue(6) == 3 && psk_class_hue(7) < 0 &&
                                          strcmp(psk_class_word(3), "DHCP") == 0 &&
                                          strcmp(psk_class_word(7), "OTHER") == 0);

        psk_model_init(&M);
        psk_model_helper_started(&M, 0);
        check("no capture yet: waiting", strcmp(psk_model_readout(&M, -1, b, sizeof(b)), "WAITING FOR PACKETS") == 0);
        line("state\tcapturing\tnone\teth0\t100\t1");
        stats_line(buf, sizeof(buf), 30);
        line(buf);
        line("bin\t0\t10\t5\t0\t0\t0\t0\t0\t5\t0");
        line("bin\t1\t0\t0\t0\t0\t0\t0\t0\t0\t0");
        line("bin\t2\t0\t0\t0\t0\t0\t0\t0\t0\t7");
        psk_model_graph(&M, &w);
        check("the window: the newest second at the right, earlier seconds unwatched",
              w.newest == 2 && w.watched[59] && w.watched[57] && !w.watched[56] && w.cls[57][0] == 10 &&
                  w.held[59] == 7);
        check("a second's readout names its largest class",
              strcmp(psk_model_readout(&M, 0, b, sizeof(b)), "00:00 · 20 PKT · TCP 50%") == 0);
        check("a quiet second", strcmp(psk_model_readout(&M, 1, b, sizeof(b)), "00:01 · 0 PKT") == 0);
        check("a held second (the newest, by default)",
              strcmp(psk_model_readout(&M, -1, b, sizeof(b)), "00:02 · HELD · 7 PASSED") == 0);
        check("the newest second's rates", psk_model_class_rate(&M, 0) == 0);
    }
    {
        uint8_t classes = PPC_CLASSES_ALL;
        char host[PPC_HOST_TEXT] = "";
        uint16_t port = 0;
        char b[48];

        psk_model_init(&M);
        psk_model_helper_started(&M, 0);
        line("state\tcapturing\tnone\teth0\t100\t1");
        M.want_req = 7;
        line("rows.begin\t7\t3\t40\t900\t1\t950\t44");
        line("row\t41\t0\t0\tin\t0\tTCP\t60\t0\ta\tb\tc\t1");
        check("a window: its view, ranks, anchor and the rows' flags",
              line("rows.end\t7") == PSK_CHANGED_ROWS && M.rows_filter_id == 3 && M.rows_first_rank == 40 &&
                  M.rows_match == 900 && M.rows_anchor == 44 && M.row_count == 1 && M.rows[0].flags == 1);
        M.want_pkt = 9;
        check("an older inspector answer is dropped",
              line("pkt.begin\t8\t5\t0\t1\t0\t0\tin\t0\tTCP\t60\t60\t0\t0") == 0 &&
                  line("layer\t0\t-\tFrame\t60 B") == 0 && line("pkt.end\t8") == 0 && !M.have_pkt);
        line("pkt.begin\t9\t41\t0\t900\t20000\t5000\tin\t0\tTCP\t60\t54\t0\t0");
        line("pkt.ends\t192.0.2.20\tPORT 443\thost:192.0.2.20\t192.0.2.10\t-\t-");
        line("layer\t0\t-\tFrame\t60 B");
        line("field\t0\tON THE WIRE\t60 bytes\t-\t0\t-");
        line("layer\t1\t0\tTCP\t443 > 49152");
        line("field\t1\tSOURCE PORT\t443 https\t34\t2\tport:443");
        line("field\t5\tORPHAN\tx\t-\t0\t-");
        line("hex\t0\t02 00 5e 10  00 01 02 00\t..^.....");
        check("a packet shows only when whole", !M.have_pkt);
        check("the packet: head, ends, layers, fields, hex; a field of no layer dropped",
              line("pkt.end\t9") == PSK_CHANGED_PACKET && M.have_pkt && M.pkt.head.seq == 41 &&
                  M.pkt.nlayers == 2 && M.pkt.nfields == 2 && M.pkt.field[1].off == 34 &&
                  strcmp(M.pkt.field[1].filter, "port:443") == 0 && M.pkt.layer[1].cls == 0 && M.pkt.nhex == 1 &&
                  strcmp(M.pkt.src.filter, "host:192.0.2.20") == 0 && M.pkt.dst.sub[0] == '\0');
        check("pinned: the count, and the packet's flag", line("pinned\t41\t1\t1") == PSK_CHANGED_PINS &&
                                                              M.pins == 1 && (M.pkt.head.flags & PPC_ROWF_PINNED));
        line("pinned\t41\t0\t0");
        check("unpinned", M.pins == 0 && !(M.pkt.head.flags & PPC_ROWF_PINNED));
        psk_model_helper_stopped(&M, PSK_EXIT_CRASHED, 128 + 11, 0);
        check("a helper gone takes the rows and the packet with it", M.row_count == 0 && !M.have_pkt && M.pins == 0);

        check("a row's time and gap", strcmp(psk_fmt_rel(252318430, b, sizeof(b)), "04:12.318430") == 0 &&
                                          strcmp(psk_fmt_rel(6012000001ull, b, sizeof(b)), "100:12.000001") == 0 &&
                                          strcmp(psk_fmt_gap(187554, b, sizeof(b)), "+0.187554") == 0);
        check("direction glyphs", strcmp(psk_dir_glyph("out"), "\xe2\x86\x91") == 0 &&
                                      strcmp(psk_dir_glyph("in"), "\xe2\x86\x93") == 0 &&
                                      strcmp(psk_dir_glyph("bcast"), "\xc2\xb7") == 0);
        check("filter tokens", psk_filter_token("port:443", &classes, host, sizeof(host), &port) == 0 && port == 443 &&
                                   psk_filter_token("host:2001:db8::a", &classes, host, sizeof(host), &port) == 0 &&
                                   strcmp(host, "2001:db8::a") == 0 &&
                                   psk_filter_token("class:4", &classes, host, sizeof(host), &port) == 0 &&
                                   classes == (1u << 4));
        check("and the ones refused", psk_filter_token("port:0", &classes, host, sizeof(host), &port) < 0 &&
                                          psk_filter_token("port:70000", &classes, host, sizeof(host), &port) < 0 &&
                                          psk_filter_token("class:8", &classes, host, sizeof(host), &port) < 0 &&
                                          psk_filter_token("host:", &classes, host, sizeof(host), &port) < 0 &&
                                          psk_filter_token("rm -rf", &classes, host, sizeof(host), &port) < 0);
    }
    /* M11.1: the diagnosis. */
    {
        struct ppc_diag d;
        struct psk_diag_line dl[PSK_DIAG_LINES];
        unsigned nl;
        unsigned k;
        int found;

        psk_model_init(&M);
        check("no diagnosis yet: nothing seen, OK", psk_diag_summary(&M, PSK_DIAG_DNS, t, sizeof(t)) == PSK_SEV_OK &&
                                                      strcmp(t, "NO QUERY SEEN YET") == 0 &&
                                                      psk_diag_summary(&M, PSK_DIAG_TCP, t, sizeof(t)) == PSK_SEV_OK);
        memset(&d, 0, sizeof(d));
        d.dns_queries = 20;
        d.dns_answered = 20;
        d.dns_rtt_sum_us = 20 * 23000u;
        d.dns_rtt_max_us = 140000;
        d.dns_rtt_max_seq = 77;
        d.tcp_attempts = 5;
        d.tcp_connected = 5;
        d.tcp_timed = 5;
        d.tcp_hs_sum_us = 5 * 41000u;
        d.tcp_hs_max_us = 60000;
        d.tcp_hs_max_seq = 99;
        d.tcp_segments = 10000;
        d.tcp_retrans = 2;
        d.tcp_rst_closed = 3;
        ppc_evt_format_diag(buf, sizeof(buf), &d);
        check("the diag line fills the model", line(buf) == PSK_CHANGED_DIAG && M.have_diag && M.d.dns_queries == 20);
        check("all answered: OK, answered and how fast",
              psk_diag_summary(&M, PSK_DIAG_DNS, t, sizeof(t)) == PSK_SEV_OK &&
                  strcmp(t, "20 ANSWERED \xc2\xb7 AVG 23 MS") == 0);
        check("two retransmissions in ten thousand and resets after close are not a fault: a note at most",
              psk_diag_summary(&M, PSK_DIAG_TCP, t, sizeof(t)) == PSK_SEV_NOTE &&
                  strcmp(t, "5 CONNECTED \xc2\xb7 AVG 41 MS") == 0);
        nl = psk_diag_lines(&M, PSK_DIAG_TCP, dl, PSK_DIAG_LINES);
        found = 0;
        for (k = 0; k < nl; k++) {
            if (strstr(dl[k].text, "RESETS AFTER CLOSE") && dl[k].sev == PSK_SEV_OK && dl[k].go == PSK_GO_LIST &&
                dl[k].diag == PPC_DIAG_TCP_RST_CLOSED) {
                found |= 1;
            }
            if (strstr(dl[k].text, "2 RETRANSMISSIONS") && dl[k].sev == PSK_SEV_NOTE && dl[k].signals == PPC_SIG_RETRANS &&
                strncmp(dl[k].why, "2 had passed here before.", 25) == 0) {
                found |= 2;
            }
        }
        check("TCP lines: the summary opens the slowest handshake; resets after close OK, a few retransmissions a note",
              nl == 3 && dl[0].go == PSK_GO_OPEN && dl[0].seq == 99 && strcmp(dl[0].text, "5 OF 5 CONNECTIONS OPENED") == 0 &&
                  found == 3);

        d.dns_nxdomain = 2;
        d.dns_noreply = 3;
        d.dns_late = 1;
        d.dns_orphan = 4;
        d.tcp_refused = 1;
        d.tcp_aborted = 2;
        d.tcp_retrans = 200;
        d.tcp_retrans_unseen = 10;
        ppc_evt_format_diag(buf, sizeof(buf), &d);
        line(buf);
        check("unanswered queries: CHECK, the problem first",
              psk_diag_summary(&M, PSK_DIAG_DNS, t, sizeof(t)) == PSK_SEV_WARN &&
                  strncmp(t, "3 NO REPLY \xc2\xb7 2 FAILED \xc2\xb7 21 ANSWERED", 36) == 0);
        nl = psk_diag_lines(&M, PSK_DIAG_DNS, dl, PSK_DIAG_LINES);
        found = 0;
        for (k = 0; k < nl; k++) {
            if (strstr(dl[k].text, "NAME NOT FOUND") && dl[k].sev == PSK_SEV_NOTE && dl[k].signals == PPC_SIG_DNS_FAIL) {
                found |= 1;
            }
            if (strstr(dl[k].text, "NOT ANSWERED WITHIN 5 S") && dl[k].sev == PSK_SEV_WARN &&
                dl[k].signals == PPC_SIG_DNS_NOREPLY && strstr(dl[k].why, "Not proof of a DNS failure. 1 answered later.")) {
                found |= 2;
            }
            if (strstr(dl[k].text, "WITHOUT THEIR QUERY") && dl[k].diag == PPC_DIAG_DNS_ORPHAN && dl[k].sev == PSK_SEV_NOTE) {
                found |= 4;
            }
        }
        check("DNS lines: NXDOMAIN a note, unanswered a warning that is not proof, answers without a query a note",
              found == 7);
        check("aborted connections and many retransmissions: CHECK, refusals named",
              psk_diag_summary(&M, PSK_DIAG_TCP, t, sizeof(t)) == PSK_SEV_WARN && strstr(t, "2 ABORTED") &&
                  strstr(t, "1 REFUSED") && strstr(t, "210 RETRANS"));
        line("state\tcapturing\tnone\teth0\t100\t1");
        check("a new capture forgets the last one's diagnosis",
              (line("state\tidle\tnone\t-\t0\t0"), line("state\tcapturing\tnone\teth0\t100\t1"), !M.have_diag) &&
                  M.d.dns_queries == 0);
        check("durations: 0.4 MS, 23 MS, 1.2 S", strcmp(psk_fmt_ms(400, t, sizeof(t)), "0.4 MS") == 0 &&
                                                    strcmp(psk_fmt_ms(23400, t, sizeof(t)), "23 MS") == 0 &&
                                                    strcmp(psk_fmt_ms(1250000, t, sizeof(t)), "1.2 S") == 0);
        check("a diagnosis names its filter", strcmp(psk_diag_plural(PPC_DIAG_TCP_REFUSED), "REFUSED CONNECTIONS") == 0);
    }

    printf("psk_model_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
