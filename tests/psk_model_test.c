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
    printf("psk_model_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
