/*
 * Host tests for pocketpcap's findings (core/pocketpcap/ppc_signals.h, M7):
 * each of the five, what must not be one, the table's bound, and the late
 * mark in the store with the filter that shows it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_signals.h"
#include "pocketpcap/ppc_source.h"
#include "pocketpcap/ppc_store.h"
#include "pocketpcap/ppc_view.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

/* One TCP record between 192.0.2.10:cport and 192.0.2.20:443. */
static struct ppc_record tcp(int out, uint16_t cport, uint32_t seq, uint8_t flags, uint64_t at_ms)
{
    struct ppc_record r;

    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV4;
    r.l4 = 6;
    r.info = PPC_INFO_TCP;
    r.cls = PPC_CLASS_TCP;
    r.src[0] = r.dst[0] = 192;
    r.src[2] = r.dst[2] = 2;
    r.src[3] = out ? 10 : 20;
    r.dst[3] = out ? 20 : 10;
    r.sport = out ? cport : 443;
    r.dport = out ? 443 : cport;
    r.a = seq;
    r.tcp_flags = flags;
    r.rel_ns = at_ms * 1000000ull;
    return r;
}

static uint32_t late_seq[8];
static uint32_t late_n;

static void on_late(void *ctx, uint32_t seq)
{
    (void)ctx;
    if (late_n < 8) {
        late_seq[late_n] = seq;
    }
    late_n++;
}

static void test_tcp(void)
{
    struct ppc_signals sg;
    struct ppc_record r;

    check("the table allocates", ppc_signals_init(&sg, 64) == 0);

    r = tcp(1, 50000, 1000, PPC_TCP_SYN, 0);
    check("a SYN is no finding yet, and waits", ppc_signals_frame(&sg, &r, 0, 1) == 0 && sg.waiting == 1);
    r = tcp(0, 50000, 9000, PPC_TCP_SYN | PPC_TCP_ACK, 10);
    check("the SYN ACK answers it", ppc_signals_frame(&sg, &r, 0, 2) == 0 && sg.waiting == 0);
    r = tcp(1, 50000, 1001, PPC_TCP_ACK | PPC_TCP_PSH, 20);
    check("new data is no finding", ppc_signals_frame(&sg, &r, 100, 3) == 0);
    r = tcp(1, 50000, 1101, PPC_TCP_ACK, 21);
    check("a bare ACK is no finding", ppc_signals_frame(&sg, &r, 0, 4) == 0);
    r = tcp(1, 50000, 1001, PPC_TCP_ACK | PPC_TCP_PSH, 220);
    check("the same bytes again are a retransmission", ppc_signals_frame(&sg, &r, 100, 5) == PPC_SIG_RETRANS);
    r = tcp(1, 50000, 1051, PPC_TCP_ACK, 221);
    check("bytes inside what was seen are a retransmission", ppc_signals_frame(&sg, &r, 50, 6) == PPC_SIG_RETRANS);
    r = tcp(1, 50000, 1100, PPC_TCP_ACK, 5000);
    check("a keep-alive (the last byte again) is not", ppc_signals_frame(&sg, &r, 1, 7) == 0);
    r = tcp(1, 50000, 1101, PPC_TCP_ACK, 5001);
    check("data that runs on past what was seen is not", ppc_signals_frame(&sg, &r, 200, 8) == 0);
    r = tcp(0, 50000, 9001, PPC_TCP_ACK, 5002);
    check("the other direction is judged on its own", ppc_signals_frame(&sg, &r, 300, 9) == 0);
    r = tcp(0, 50000, 9301, PPC_TCP_RST | PPC_TCP_ACK, 5003);
    check("a reset is a finding", ppc_signals_frame(&sg, &r, 0, 10) == PPC_SIG_RST);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 6000);
    check("a new SYN on the same ports starts again", ppc_signals_frame(&sg, &r, 0, 11) == 0 && sg.waiting == 1);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 7000);
    check("the same SYN again is a retransmission, and the first still waits",
          ppc_signals_frame(&sg, &r, 0, 12) == PPC_SIG_RETRANS && sg.waiting == 1);

    late_n = 0;
    check("it has not waited long enough at 2.9 s", ppc_signals_expire(&sg, 8900000000ull, on_late, NULL) == 0);
    check("at 3 s the first SYN is reported, once",
          ppc_signals_expire(&sg, 9000000000ull, on_late, NULL) == 1 && late_n == 1 && late_seq[0] == 11 &&
              sg.waiting == 0 && ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == 0);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 9500);
    check("a third copy does not wait again", ppc_signals_frame(&sg, &r, 0, 13) == PPC_SIG_RETRANS && sg.waiting == 0);

    r = tcp(1, 50001, 5, PPC_TCP_SYN, 9600);
    ppc_signals_frame(&sg, &r, 0, 14);
    ppc_signals_blind(&sg);
    late_n = 0;
    check("a SYN waiting when frames pass unseen is never reported",
          sg.waiting == 0 && ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == 0 && late_n == 0);

    /* Sequence numbers wrap. */
    r = tcp(1, 50002, 0xffffff00u, PPC_TCP_ACK, 9700);
    ppc_signals_frame(&sg, &r, 200, 15);
    r = tcp(1, 50002, 0xffffffc8u, PPC_TCP_ACK, 9701);
    check("data across the wrap of the sequence space is new", ppc_signals_frame(&sg, &r, 200, 16) == 0);
    r = tcp(1, 50002, 0xffffff00u, PPC_TCP_ACK, 9702);
    check("and what came before the wrap is a retransmission", ppc_signals_frame(&sg, &r, 200, 17) == PPC_SIG_RETRANS);

    r = tcp(1, 50003, 1, PPC_TCP_ACK, 9800);
    r.flags |= PPC_F_NONFIRST;
    check("a later fragment is never judged", ppc_signals_frame(&sg, &r, 100, 18) == 0);
    ppc_signals_free(&sg);
}

static void test_bound(void)
{
    struct ppc_signals sg;
    struct ppc_record r;
    uint32_t i;
    int ok = 1;

    ppc_signals_init(&sg, 16);
    for (i = 0; i < 20000; i++) {
        r = tcp(1, (uint16_t)(1024 + i), i, PPC_TCP_SYN, i);
        ppc_signals_frame(&sg, &r, 0, i + 1);
        if (sg.waiting > 16) {
            ok = 0;
        }
    }
    check("20,000 connections through 16 entries: never more waiting than entries, the oldest replaced",
          ok && sg.replaced > 19000);
    check("no table: a reset is still seen, nothing else", (ppc_signals_free(&sg), 1));
    r = tcp(1, 1, 1, PPC_TCP_RST, 0);
    check("a reset needs no table", ppc_signals_frame(&sg, &r, 0, 1) == PPC_SIG_RST);
    check("a table of 3 is refused", ppc_signals_init(&sg, 3) != 0);
}

static void test_others(void)
{
    struct ppc_signals sg;
    struct ppc_record r;

    ppc_signals_init(&sg, 16);
    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV4;
    r.l4 = 17;
    r.info = PPC_INFO_DNS;
    r.app = PPC_APP_DNS;
    r.a = 0x12340000u | 0x8183u;
    check("NXDOMAIN is a DNS failure", ppc_signals_frame(&sg, &r, 0, 1) == PPC_SIG_DNS_FAIL);
    r.a = 0x12340000u | 0x8180u;
    check("NOERROR is not", ppc_signals_frame(&sg, &r, 0, 2) == 0);
    r.a = 0x12340000u | 0x0103u;
    check("a query is not, whatever its low bits", ppc_signals_frame(&sg, &r, 0, 3) == 0);
    r.a = 0x12340000u | 0x8183u;
    r.app = PPC_APP_NBNS;
    check("a NetBIOS answer is not", ppc_signals_frame(&sg, &r, 0, 4) == 0);
    r.app = PPC_APP_DNS;
    r.flags = PPC_F_MALFORMED;
    check("a malformed packet is not judged", ppc_signals_frame(&sg, &r, 0, 5) == 0);

    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV4;
    r.info = PPC_INFO_ICMP;
    r.a = 3u << 8 | 3u;
    check("ICMP destination unreachable is a finding", ppc_signals_frame(&sg, &r, 0, 6) == PPC_SIG_UNREACH);
    r.a = 8u << 8;
    check("an echo request is not", ppc_signals_frame(&sg, &r, 0, 7) == 0);
    r.info = PPC_INFO_ICMP6;
    r.a = 1u << 8;
    check("ICMPv6 type 1 is", ppc_signals_frame(&sg, &r, 0, 8) == PPC_SIG_UNREACH);
    r.a = 135u << 8;
    check("a neighbour solicitation is not", ppc_signals_frame(&sg, &r, 0, 9) == 0);
    ppc_signals_free(&sg);
}

/* The synthetic source carries every finding: decoded and judged as the
 * helper does, then the store's late mark and the filter. */
static void test_synth(void)
{
    static uint8_t b[1600];
    struct ppc_signals sg;
    struct ppc_store st;
    struct ppc_view v;
    struct ppc_filter f;
    uint32_t count[PPC_SIGNAL_COUNT] = { 0 };
    uint32_t flagged = 0;
    uint32_t pad_len;
    uint64_t n;
    int i;

    ppc_signals_init(&sg, PPC_SIG_FLOWS);
    ppc_store_init(&st, 65536, 1u << 22);
    ppc_view_init(&v, 65536);
    for (n = 0; n < 20000; n++) {
        struct ppc_frame fr;
        struct ppc_record r;
        uint32_t pay;

        fr.data = b;
        fr.cap_len = fr.wire_len = ppc_synth_frame(n, 7, b);
        fr.ts_ns = n * 1000000ull;
        fr.pkttype = PPC_PKT_UNKNOWN;
        pay = ppc_decode(&fr, &r);
        r.rel_ns = fr.ts_ns;
        r.signal = ppc_signals_frame(&sg, &r, r.info == PPC_INFO_TCP ? ppc_tcp_payload_len(b, fr.cap_len, pay) : 0,
                                     st.last_seq + 1u);
        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            count[i] += (r.signal >> i) & 1u;
        }
        flagged += r.signal != 0;
        ppc_store_add(&st, &r, b, ppc_store_len(&r, pay, fr.cap_len, PPC_KEEP_HEADERS));
    }
    check("the synthetic traffic carries retransmissions, resets, failed lookups and unreachables",
          count[0] > 10 && count[1] > 10 && count[2] > 10 && count[3] > 10);
    check("and no more than a few in a hundred packets are findings", flagged < 20000 / 25);
    late_n = 0;
    ppc_signals_expire(&sg, 20000ull * 1000000ull + PPC_SIG_SYN_WAIT_NS, on_late, NULL);
    check("its unanswered SYNs are reported late", late_n > 10);
    check("a late finding is marked on the stored packet",
          ppc_store_mark(&st, late_seq[0], PPC_SIG_SYN_UNANSWERED) == 0 &&
              (ppc_store_get(&st, late_seq[0])->signal & PPC_SIG_SYN_UNANSWERED));
    check("a packet that was never stored cannot be marked", ppc_store_mark(&st, 900000, PPC_SIG_RST) == -1);

    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    f.signals = PPC_SIG_RST;
    ppc_view_set(&v, &st, 1, PPC_SORT_TIME, &f);
    check("a view of resets holds exactly the resets", ppc_view_count(&v) == count[1]);
    f.signals = PPC_SIG_ALL;
    ppc_view_set(&v, &st, 2, PPC_SORT_TIME, &f);
    check("a view of every finding holds every marked packet", ppc_view_count(&v) == flagged + 1);
    f.signals = 0;
    ppc_view_set(&v, &st, 3, PPC_SORT_TIME, &f);
    check("no finding asked for: every packet", ppc_view_count(&v) == 20000);

    /* Ethernet padding is not payload: a 60-byte frame with a 40-byte datagram. */
    memset(b, 0, 60);
    b[12] = 0x08;
    b[14] = 0x45;
    b[17] = 40;
    pad_len = ppc_tcp_payload_len(b, 60, 54);
    check("padding after the IP datagram is not TCP payload", pad_len == 0);
    b[17] = 46;
    check("six bytes of payload are six", ppc_tcp_payload_len(b, 60, 54) == 6);
    check("a frame cut short gives none", ppc_tcp_payload_len(b, 20, 54) == 0 && ppc_tcp_payload_len(NULL, 0, 0) == 0);
    ppc_view_free(&v);
    ppc_store_free(&st);
    ppc_signals_free(&sg);
}

int main(void)
{
    test_tcp();
    test_bound();
    test_others();
    test_synth();
    printf("ppc_signals_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
