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

/* The findings of a frame, without its diagnosis (M7's checks). */
static uint16_t judge(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq)
{
    return (uint16_t)ppc_sig_findings(ppc_signals_frame(sg, r, tcp_len, seq));
}

static uint32_t late_seq[8];
static uint32_t late_n;

static uint16_t late_bit[8];

static void on_late(void *ctx, uint32_t seq, uint16_t bit)
{
    (void)ctx;
    if (late_n < 8) {
        late_bit[late_n] = bit;
    }
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
    check("a SYN is no finding yet, and waits", judge(&sg, &r, 0, 1) == 0 && sg.waiting == 1);
    r = tcp(0, 50000, 9000, PPC_TCP_SYN | PPC_TCP_ACK, 10);
    check("the SYN ACK answers it", judge(&sg, &r, 0, 2) == 0 && sg.waiting == 0);
    r = tcp(1, 50000, 1001, PPC_TCP_ACK | PPC_TCP_PSH, 20);
    check("new data is no finding", judge(&sg, &r, 100, 3) == 0);
    r = tcp(1, 50000, 1101, PPC_TCP_ACK, 21);
    check("a bare ACK is no finding", judge(&sg, &r, 0, 4) == 0);
    r = tcp(1, 50000, 1001, PPC_TCP_ACK | PPC_TCP_PSH, 220);
    check("the same bytes again are a retransmission", judge(&sg, &r, 100, 5) == PPC_SIG_RETRANS);
    r = tcp(1, 50000, 1051, PPC_TCP_ACK, 221);
    check("bytes inside what was seen are a retransmission", judge(&sg, &r, 50, 6) == PPC_SIG_RETRANS);
    r = tcp(1, 50000, 1100, PPC_TCP_ACK, 5000);
    check("a keep-alive (the last byte again) is not", judge(&sg, &r, 1, 7) == 0);
    r = tcp(1, 50000, 1101, PPC_TCP_ACK, 5001);
    check("data that runs on past what was seen is not", judge(&sg, &r, 200, 8) == 0);
    r = tcp(0, 50000, 9001, PPC_TCP_ACK, 5002);
    check("the other direction is judged on its own", judge(&sg, &r, 300, 9) == 0);
    r = tcp(0, 50000, 9301, PPC_TCP_RST | PPC_TCP_ACK, 5003);
    check("a reset is a finding", judge(&sg, &r, 0, 10) == PPC_SIG_RST);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 6000);
    check("a new SYN on the same ports starts again", judge(&sg, &r, 0, 11) == 0 && sg.waiting == 1);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 7000);
    check("the same SYN again is a retransmission, and the first still waits",
          judge(&sg, &r, 0, 12) == PPC_SIG_RETRANS && sg.waiting == 1);

    late_n = 0;
    check("it has not waited long enough at 2.9 s", ppc_signals_expire(&sg, 8900000000ull, on_late, NULL) == 0);
    check("at 3 s the first SYN is reported, once",
          ppc_signals_expire(&sg, 9000000000ull, on_late, NULL) == 1 && late_n == 1 && late_seq[0] == 11 &&
              sg.waiting == 0 && ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == 0);
    r = tcp(1, 50000, 77000, PPC_TCP_SYN, 9500);
    check("a third copy does not wait again", judge(&sg, &r, 0, 13) == PPC_SIG_RETRANS && sg.waiting == 0);

    r = tcp(1, 50001, 5, PPC_TCP_SYN, 9600);
    judge(&sg, &r, 0, 14);
    ppc_signals_blind(&sg);
    late_n = 0;
    check("a SYN waiting when frames pass unseen is never reported",
          sg.waiting == 0 && ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == 0 && late_n == 0);

    /* Sequence numbers wrap. */
    r = tcp(1, 50002, 0xffffff00u, PPC_TCP_ACK, 9700);
    judge(&sg, &r, 200, 15);
    r = tcp(1, 50002, 0xffffffc8u, PPC_TCP_ACK, 9701);
    check("data across the wrap of the sequence space is new", judge(&sg, &r, 200, 16) == 0);
    r = tcp(1, 50002, 0xffffff00u, PPC_TCP_ACK, 9702);
    check("and what came before the wrap is a retransmission", judge(&sg, &r, 200, 17) == PPC_SIG_RETRANS);

    r = tcp(1, 50003, 1, PPC_TCP_ACK, 9800);
    r.flags |= PPC_F_NONFIRST;
    check("a later fragment is never judged", judge(&sg, &r, 100, 18) == 0);
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
        judge(&sg, &r, 0, i + 1);
        if (sg.waiting > 16) {
            ok = 0;
        }
    }
    check("20,000 connections through 16 entries: never more waiting than entries, the oldest replaced",
          ok && sg.replaced > 19000);
    check("no table: a reset is still seen, nothing else", (ppc_signals_free(&sg), 1));
    r = tcp(1, 1, 1, PPC_TCP_RST, 0);
    check("a reset needs no table", judge(&sg, &r, 0, 1) == PPC_SIG_RST);
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
    check("NXDOMAIN is a DNS failure", judge(&sg, &r, 0, 1) == PPC_SIG_DNS_FAIL);
    r.a = 0x12340000u | 0x8180u;
    check("NOERROR is not", judge(&sg, &r, 0, 2) == 0);
    r.a = 0x12340000u | 0x0103u;
    check("a query is not, whatever its low bits", judge(&sg, &r, 0, 3) == 0);
    r.a = 0x12340000u | 0x8183u;
    r.app = PPC_APP_NBNS;
    check("a NetBIOS answer is not", judge(&sg, &r, 0, 4) == 0);
    r.app = PPC_APP_DNS;
    r.flags = PPC_F_MALFORMED;
    check("a malformed packet is not judged", judge(&sg, &r, 0, 5) == 0);

    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV4;
    r.info = PPC_INFO_ICMP;
    r.a = 3u << 8 | 3u;
    check("ICMP destination unreachable is a finding", judge(&sg, &r, 0, 6) == PPC_SIG_UNREACH);
    r.a = 8u << 8;
    check("an echo request is not", judge(&sg, &r, 0, 7) == 0);
    r.info = PPC_INFO_ICMP6;
    r.a = 1u << 8;
    check("ICMPv6 type 1 is", judge(&sg, &r, 0, 8) == PPC_SIG_UNREACH);
    r.a = 135u << 8;
    check("a neighbour solicitation is not", judge(&sg, &r, 0, 9) == 0);
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
        flagged += ppc_sig_findings(r.signal) != 0;
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

/* ---- M11.1: the diagnosis ---------------------------------------------------------- */

static unsigned diag_of(uint16_t s)
{
    return ppc_sig_diag(s);
}

/* A TCP record with its acknowledgement number. */
static struct ppc_record tcpa(int out, uint16_t cport, uint32_t seq, uint32_t ack, uint8_t flags, uint64_t at_us)
{
    struct ppc_record r = tcp(out, cport, seq, flags, 0);

    r.b = ack;
    r.rel_ns = at_us * 1000ull;
    return r;
}

/* One DNS message between 192.0.2.1:cport and 192.0.2.53:53. */
static struct ppc_record dnsm(int answer, uint16_t cport, uint16_t id, unsigned rcode, uint64_t at_ms)
{
    struct ppc_record r;

    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV4;
    r.l4 = 17;
    r.info = PPC_INFO_DNS;
    r.app = PPC_APP_DNS;
    r.cls = PPC_CLASS_DNS;
    r.src[0] = r.dst[0] = 192;
    r.src[2] = r.dst[2] = 2;
    r.src[3] = answer ? 53 : 1;
    r.dst[3] = answer ? 1 : 53;
    r.sport = answer ? 53 : cport;
    r.dport = answer ? cport : 53;
    r.a = (uint32_t)id << 16 | (answer ? 0x8180u | rcode : 0x0100u);
    r.b = 1u << 16;
    r.rel_ns = at_ms * 1000000ull;
    return r;
}

static void test_dns(void)
{
    struct ppc_signals sg;
    struct ppc_record r;
    struct ppc_diag d;
    uint16_t s;
    uint32_t i;
    int ok;

    check("DNS: the tables allocate", ppc_signals_init(&sg, 64) == 0 && sg.dns_cap == PPC_DNS_QUERIES);
    r = dnsm(0, 33000, 0x1234, 0, 0);
    s = ppc_signals_frame(&sg, &r, 0, 1);
    check("DNS: a query is no finding, enters the table and waits",
          ppc_sig_findings(s) == 0 && diag_of(s) == PPC_DIAG_DNS_QUERY && sg.dns_waiting == 1 && ppc_signals_pending(&sg));
    r = dnsm(0, 33000, 0x1234, 0, 1000);
    check("DNS: the same query again is a retry, the wait starts over",
          diag_of(ppc_signals_frame(&sg, &r, 0, 2)) == PPC_DIAG_DNS_RETRY && sg.dns_waiting == 1);
    r = dnsm(1, 33000, 0x1234, 0, 1012);
    s = ppc_signals_frame(&sg, &r, 0, 3);
    ppc_signals_diag(&sg, &d);
    check("DNS: its answer is matched and timed from the latest copy (12 ms)",
          ppc_sig_findings(s) == 0 && diag_of(s) == PPC_DIAG_DNS_ANSWER && d.dns_answered == 1 &&
              d.dns_rtt_sum_us == 12000 && d.dns_rtt_max_us == 12000 && d.dns_rtt_max_seq == 3 && d.dns_waiting == 0 &&
              d.dns_queries == 1 && d.dns_retries == 1 && sg.dns_live == 0);
    r = dnsm(1, 33000, 0x1234, 0, 1013);
    check("DNS: a second copy of the answer has no query left: not seen", diag_of(ppc_signals_frame(&sg, &r, 0, 4)) ==
                                                                          PPC_DIAG_DNS_ORPHAN);
    r = dnsm(1, 33001, 0x9999, 0, 1100);
    check("DNS: an answer to a query this capture never saw", diag_of(ppc_signals_frame(&sg, &r, 0, 5)) ==
                                                              PPC_DIAG_DNS_ORPHAN);
    r = dnsm(0, 33002, 0x0002, 0, 2000);
    ppc_signals_frame(&sg, &r, 0, 6);
    r = dnsm(1, 33002, 0x0003, 0, 2001);
    check("DNS: an answer with another id is not the answer", diag_of(ppc_signals_frame(&sg, &r, 0, 7)) ==
                                                              PPC_DIAG_DNS_ORPHAN && sg.dns_waiting == 1);
    r = dnsm(1, 33003, 0x0002, 0, 2002);
    check("DNS: nor one to another port", diag_of(ppc_signals_frame(&sg, &r, 0, 8)) == PPC_DIAG_DNS_ORPHAN);

    late_n = 0;
    check("DNS: not called unanswered at 4.99 s", ppc_signals_expire(&sg, 6990000000ull, on_late, NULL) == 0);
    check("DNS: at 5 s it is, once, on the query's packet",
          ppc_signals_expire(&sg, 7000000000ull, on_late, NULL) == 1 && late_n == 1 && late_seq[0] == 6 &&
              late_bit[0] == PPC_SIG_DNS_NOREPLY && sg.dns_waiting == 0 &&
              ppc_signals_expire(&sg, 8000000000ull, on_late, NULL) == 0);
    r = dnsm(1, 33002, 0x0002, 0, 9000);
    s = ppc_signals_frame(&sg, &r, 0, 9);
    ppc_signals_diag(&sg, &d);
    check("DNS: an answer after that is a late answer, timed (7 s)",
          diag_of(s) == PPC_DIAG_DNS_LATE && d.dns_late == 1 && d.dns_noreply == 1 && d.dns_rtt_max_us == 7000000);

    r = dnsm(0, 33004, 0x0004, 0, 10000);
    ppc_signals_frame(&sg, &r, 0, 10);
    r = dnsm(1, 33004, 0x0004, 3, 10020);
    s = ppc_signals_frame(&sg, &r, 0, 11);
    check("DNS: NXDOMAIN is a failure and an answer", ppc_sig_findings(s) == PPC_SIG_DNS_FAIL &&
                                                          diag_of(s) == PPC_DIAG_DNS_ANSWER);
    r = dnsm(1, 33005, 0x0005, 2, 10030);
    ppc_signals_frame(&sg, &r, 0, 12);
    r = dnsm(1, 33005, 0x0006, 5, 10031);
    ppc_signals_frame(&sg, &r, 0, 13);
    r = dnsm(1, 33005, 0x0007, 4, 10032);
    ppc_signals_frame(&sg, &r, 0, 14);
    ppc_signals_diag(&sg, &d);
    check("DNS: response codes counted apart: NXDOMAIN, SERVFAIL, REFUSED, other",
          d.dns_nxdomain == 1 && d.dns_servfail == 1 && d.dns_refused == 1 && d.dns_other == 1);

    r = dnsm(0, 33006, 0x0008, 0, 11000);
    ppc_signals_frame(&sg, &r, 0, 15);
    ppc_signals_blind(&sg);
    late_n = 0;
    ppc_signals_diag(&sg, &d);
    check("DNS: a query waiting when frames pass unseen is forgotten, never called unanswered",
          sg.dns_waiting == 0 && ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == 0 && late_n == 0 &&
              d.dns_unjudged == 1 && d.blinded == 1);

    r = dnsm(0, 33007, 0x0009, 0, 12000);
    r.app = PPC_APP_MDNS;
    check("DNS: an mDNS query is not a transaction", diag_of(ppc_signals_frame(&sg, &r, 0, 16)) == PPC_DIAG_NONE);
    r.app = PPC_APP_LLMNR;
    check("DNS: nor an LLMNR one", diag_of(ppc_signals_frame(&sg, &r, 0, 17)) == PPC_DIAG_NONE && sg.dns_waiting == 0);

    r = dnsm(0, 33008, 0x000a, 0, 13000);
    r.l3 = PPC_L3_IPV6;
    memset(r.src, 0, 16);
    memset(r.dst, 0, 16);
    r.src[0] = r.dst[0] = 0x20;
    r.src[15] = 1;
    r.dst[15] = 0x53;
    ppc_signals_frame(&sg, &r, 0, 18);
    r = dnsm(1, 33008, 0x000a, 0, 13004);
    r.l3 = PPC_L3_IPV6;
    memset(r.src, 0, 16);
    memset(r.dst, 0, 16);
    r.src[0] = r.dst[0] = 0x20;
    r.src[15] = 0x53;
    r.dst[15] = 1;
    check("DNS: IPv6 queries and answers match", diag_of(ppc_signals_frame(&sg, &r, 0, 19)) == PPC_DIAG_DNS_ANSWER);

    /* An unanswered query is remembered for a late answer, then let go. */
    r = dnsm(0, 33009, 0x000b, 0, 20000);
    ppc_signals_frame(&sg, &r, 0, 20);
    ppc_signals_expire(&sg, 25000000000ull, NULL, NULL);
    check("DNS: called unanswered, still held", sg.dns_live == 1 && sg.dns_waiting == 0);
    ppc_signals_expire(&sg, 50000000000ull, NULL, NULL);
    check("DNS: let go 30 s after it went", sg.dns_live == 0 && !ppc_signals_pending(&sg));
    ppc_signals_free(&sg);

    /* The bound: 5,000 distinct queries through 1,024 entries. */
    ppc_signals_init(&sg, 16);
    ok = 1;
    for (i = 0; i < 5000; i++) {
        r = dnsm(0, (uint16_t)(1024 + i), (uint16_t)i, 0, i);
        ppc_signals_frame(&sg, &r, 0, i + 1);
        if (sg.dns_live > PPC_DNS_QUERIES || sg.dns_waiting > sg.dns_live) {
            ok = 0;
        }
    }
    ppc_signals_diag(&sg, &d);
    late_n = 0;
    check("DNS: 5,000 queries through 1,024 entries: never more held, the replaced forgotten unjudged",
          ok && sg.dns_live <= PPC_DNS_QUERIES && d.dns_unjudged + sg.dns_live == 5000);
    check("DNS: only the queries still held are called unanswered",
          ppc_signals_expire(&sg, 99000000000ull, on_late, NULL) == sg.dns_live);
    ppc_signals_free(&sg);
}

static void test_handshake(void)
{
    struct ppc_signals sg;
    struct ppc_record r;
    struct ppc_diag d;
    uint16_t s;

    ppc_signals_init(&sg, 64);
    /* A connection: SYN, SYN-ACK 10 ms later, ACK 0.5 ms after that. */
    r = tcpa(1, 40000, 1000, 0, PPC_TCP_SYN, 0);
    check("TCP: a SYN is an attempt", diag_of(ppc_signals_frame(&sg, &r, 0, 1)) == PPC_DIAG_TCP_SYN);
    r = tcpa(0, 40000, 9000, 1001, PPC_TCP_SYN | PPC_TCP_ACK, 10000);
    check("TCP: the SYN-ACK that acknowledges it answers it", diag_of(ppc_signals_frame(&sg, &r, 0, 2)) ==
                                                               PPC_DIAG_TCP_SYNACK);
    r = tcpa(1, 40000, 1001, 9001, PPC_TCP_ACK, 10500);
    s = ppc_signals_frame(&sg, &r, 0, 3);
    ppc_signals_diag(&sg, &d);
    check("TCP: the ACK of the SYN-ACK completes it, 10.5 ms after the SYN",
          diag_of(s) == PPC_DIAG_TCP_CONNECTED && d.tcp_attempts == 1 && d.tcp_connected == 1 && d.tcp_timed == 1 &&
              d.tcp_hs_sum_us == 10500 && d.tcp_hs_max_seq == 3);
    late_n = 0;
    ppc_signals_expire(&sg, 99000000000ull, on_late, NULL);
    check("TCP: an answered SYN is never called unanswered", late_n == 0);

    /* Retransmissions: seen, already acknowledged, and a gap. */
    r = tcpa(1, 40000, 1001, 9001, PPC_TCP_ACK | PPC_TCP_PSH, 20000);
    ppc_signals_frame(&sg, &r, 100, 4);
    r = tcpa(1, 40000, 1001, 9001, PPC_TCP_ACK | PPC_TCP_PSH, 220000);
    s = ppc_signals_frame(&sg, &r, 100, 5);
    check("TCP: data again, not yet acknowledged: a retransmission of what passed here",
          ppc_sig_findings(s) == PPC_SIG_RETRANS && diag_of(s) == PPC_DIAG_TCP_RETRANS);
    r = tcpa(0, 40000, 9001, 1101, PPC_TCP_ACK, 230000);
    ppc_signals_frame(&sg, &r, 0, 6);
    r = tcpa(1, 40000, 1001, 9001, PPC_TCP_ACK | PPC_TCP_PSH, 400000);
    s = ppc_signals_frame(&sg, &r, 100, 7);
    check("TCP: data again after the receiver acknowledged it: spurious",
          ppc_sig_findings(s) == PPC_SIG_RETRANS && diag_of(s) == PPC_DIAG_TCP_RETRANS_ACKED);
    /* 1101..1201 skipped, 1201..1301 seen; the gap filled 0.2 ms later. */
    r = tcpa(1, 40000, 1201, 9001, PPC_TCP_ACK, 500000);
    check("TCP: data after a gap is new data", ppc_sig_findings(ppc_signals_frame(&sg, &r, 100, 8)) == 0);
    r = tcpa(1, 40000, 1101, 9001, PPC_TCP_ACK, 500200);
    s = ppc_signals_frame(&sg, &r, 100, 9);
    check("TCP: the gap filled within the handshake's time is out of order, not a retransmission",
          ppc_sig_findings(s) == 0 && diag_of(s) == PPC_DIAG_TCP_OUT_OF_ORDER);
    r = tcpa(1, 40000, 1401, 9001, PPC_TCP_ACK, 600000);
    ppc_signals_frame(&sg, &r, 100, 10);
    r = tcpa(1, 40000, 1301, 9001, PPC_TCP_ACK, 650000);
    s = ppc_signals_frame(&sg, &r, 100, 11);
    check("TCP: a gap filled later than that: its original never came by",
          ppc_sig_findings(s) == PPC_SIG_RETRANS && diag_of(s) == PPC_DIAG_TCP_RETRANS_UNSEEN);
    r = tcpa(1, 40000, 1301, 9001, PPC_TCP_ACK, 660000);
    check("TCP: and the same bytes once more were seen: a plain retransmission",
          diag_of(ppc_signals_frame(&sg, &r, 100, 12)) == PPC_DIAG_TCP_RETRANS);

    /* Resets: aborted, after close, refused, never seen. */
    r = tcpa(0, 40000, 9001, 1501, PPC_TCP_RST | PPC_TCP_ACK, 700000);
    s = ppc_signals_frame(&sg, &r, 0, 13);
    check("TCP: a reset of an open connection is an abort", ppc_sig_findings(s) == PPC_SIG_RST &&
                                                            diag_of(s) == PPC_DIAG_TCP_ABORTED);
    r = tcpa(0, 40000, 9001, 1501, PPC_TCP_RST, 700100);
    check("TCP: a second reset comes after the first: after close", diag_of(ppc_signals_frame(&sg, &r, 0, 14)) ==
                                                                    PPC_DIAG_TCP_RST_CLOSED);
    r = tcpa(1, 40001, 5000, 0, PPC_TCP_SYN, 800000);
    ppc_signals_frame(&sg, &r, 0, 15);
    r = tcpa(0, 40001, 0, 5001, PPC_TCP_RST | PPC_TCP_ACK, 800400);
    s = ppc_signals_frame(&sg, &r, 0, 16);
    late_n = 0;
    ppc_signals_expire(&sg, 99000000000ull, on_late, NULL);
    check("TCP: a reset answering a SYN is a refusal, and the SYN was answered",
          ppc_sig_findings(s) == PPC_SIG_RST && diag_of(s) == PPC_DIAG_TCP_REFUSED && late_n == 0);
    r = tcpa(1, 40002, 7000, 0, PPC_TCP_ACK | PPC_TCP_PSH, 900000);
    ppc_signals_frame(&sg, &r, 10, 17);
    r = tcpa(1, 40002, 7010, 0, PPC_TCP_FIN | PPC_TCP_ACK, 900100);
    ppc_signals_frame(&sg, &r, 0, 18);
    r = tcpa(0, 40002, 1, 7011, PPC_TCP_RST, 900200);
    check("TCP: a reset after a FIN: after close", diag_of(ppc_signals_frame(&sg, &r, 0, 19)) == PPC_DIAG_TCP_RST_CLOSED);
    r = tcpa(0, 40003, 1, 0, PPC_TCP_RST, 900300);
    check("TCP: a reset of a connection never seen", diag_of(ppc_signals_frame(&sg, &r, 0, 20)) ==
                                                     PPC_DIAG_TCP_RST_UNSEEN);
    r = tcpa(1, 40002, 7000, 0, PPC_TCP_ACK | PPC_TCP_PSH, 900000);
    ppc_signals_diag(&sg, &d);
    check("TCP: each reset counted by kind", d.tcp_aborted == 1 && d.tcp_rst_closed == 2 && d.tcp_refused == 1 &&
                                                 d.tcp_rst_unseen == 1);
    check("TCP: the connection seen mid-stream is counted", d.tcp_midstream == 1);
    check("TCP: retransmissions counted by kind", d.tcp_retrans == 2 && d.tcp_retrans_acked == 1 &&
                                                      d.tcp_retrans_unseen == 1 && d.tcp_ooo == 1);

    /* The same SYN again; a SYN-ACK whose SYN was not seen. */
    r = tcpa(1, 40004, 100, 0, PPC_TCP_SYN, 1000000);
    ppc_signals_frame(&sg, &r, 0, 21);
    r = tcpa(1, 40004, 100, 0, PPC_TCP_SYN, 2000000);
    s = ppc_signals_frame(&sg, &r, 0, 22);
    check("TCP: the same SYN again", ppc_sig_findings(s) == PPC_SIG_RETRANS && diag_of(s) == PPC_DIAG_TCP_SYN_AGAIN);
    r = tcpa(0, 40004, 500, 101, PPC_TCP_SYN | PPC_TCP_ACK, 2010000);
    ppc_signals_frame(&sg, &r, 0, 23);
    r = tcpa(1, 40004, 101, 501, PPC_TCP_ACK, 2010100);
    ppc_signals_frame(&sg, &r, 0, 24);
    ppc_signals_diag(&sg, &d);
    check("TCP: a connection whose SYN was sent twice is timed from the first (1010.1 ms)",
          d.tcp_hs_max_us == 1010100 && d.tcp_hs_max_seq == 24);
    r = tcpa(0, 40005, 600, 77, PPC_TCP_SYN | PPC_TCP_ACK, 3000000);
    check("TCP: a SYN-ACK whose SYN was not seen: mid-stream", diag_of(ppc_signals_frame(&sg, &r, 0, 25)) ==
                                                               PPC_DIAG_TCP_MIDSTREAM);
    r = tcpa(1, 40005, 77, 601, PPC_TCP_ACK, 3000100);
    ppc_signals_frame(&sg, &r, 0, 26);
    ppc_signals_diag(&sg, &d);
    check("TCP: it completes, untimed", d.tcp_connected == 3 && d.tcp_timed == 2);

    /* Frames passing unseen in the middle of a handshake. */
    r = tcpa(1, 40006, 100, 0, PPC_TCP_SYN, 4000000);
    ppc_signals_frame(&sg, &r, 0, 27);
    ppc_signals_blind(&sg);
    r = tcpa(1, 40006, 101, 901, PPC_TCP_ACK, 4100000);
    check("TCP: a handshake whose middle passed unseen is not completed, nor timed",
          diag_of(ppc_signals_frame(&sg, &r, 0, 28)) == PPC_DIAG_NONE);

    /* One way only: three segments, no answer ever (asymmetric routing). */
    r = tcpa(0, 40007, 1, 1, PPC_TCP_ACK | PPC_TCP_PSH, 5000000);
    ppc_signals_frame(&sg, &r, 10, 29);
    r = tcpa(0, 40007, 11, 1, PPC_TCP_ACK | PPC_TCP_PSH, 5000100);
    ppc_signals_frame(&sg, &r, 10, 30);
    r = tcpa(0, 40007, 21, 1, PPC_TCP_ACK | PPC_TCP_PSH, 5000200);
    ppc_signals_frame(&sg, &r, 10, 31);
    ppc_signals_diag(&sg, &d);
    check("TCP: a connection seen one way only is counted as such", d.tcp_oneway >= 1 && d.tcp_flows >= 8);

    /* A connection with a long handshake: a fill sooner than that is reordering. */
    r = tcpa(1, 40008, 1000, 0, PPC_TCP_SYN, 6000000);
    ppc_signals_frame(&sg, &r, 0, 32);
    r = tcpa(0, 40008, 9000, 1001, PPC_TCP_SYN | PPC_TCP_ACK, 6040000);
    ppc_signals_frame(&sg, &r, 0, 33);
    r = tcpa(1, 40008, 1001, 9001, PPC_TCP_ACK, 6040100);
    ppc_signals_frame(&sg, &r, 0, 34);
    r = tcpa(0, 40008, 9101, 1001, PPC_TCP_ACK, 6100000);
    ppc_signals_frame(&sg, &r, 100, 35);
    r = tcpa(0, 40008, 9001, 1001, PPC_TCP_ACK, 6120000);
    check("TCP: a gap filled 20 ms on, within a 40 ms handshake, is out of order",
          diag_of(ppc_signals_frame(&sg, &r, 100, 36)) == PPC_DIAG_TCP_OUT_OF_ORDER);
    ppc_signals_free(&sg);
}

int main(void)
{
    test_tcp();
    test_bound();
    test_others();
    test_synth();
    test_dns();
    test_handshake();
    printf("ppc_signals_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
