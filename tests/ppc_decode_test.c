/*
 * The PacketShark decoder and row formatter (core/pocketpcap/ppc_decode.h,
 * ppc_format.h), on frames built in memory.
 *
 * What is checked: each protocol's class, word, ports, flags and sentence;
 * Ethernet, 802.1Q and QinQ; IPv4 options and fragments; IPv6 extension
 * headers and fragments; TCP and UDP; ARP, ICMP and ICMPv6; the payload
 * offset and the HEADERS rule (a plain-text HTTP payload is never kept, the
 * whole DHCP message is); a frame cut at every length never makes the
 * decoder read past it (the test owns every byte after the cut and poisons
 * it); DNS names with compression, a pointer loop and a forward pointer;
 * sanitized text; and a million decoded frames that allocate nothing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_format.h"
#include "pocketpcap/ppc_source.h"
#include "ppc_testframes.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- counting allocations ----------------------------------------------------- *
 *
 * The decoder must not allocate. Counted by interposing malloc while a flag
 * is up; skipped under the sanitizers, which own malloc themselves. */
#if defined(__SANITIZE_ADDRESS__)
#define COUNT_ALLOC 0
#else
#define COUNT_ALLOC 1
static volatile int counting;
static volatile unsigned long allocations;
extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t a, size_t b);
extern void *__libc_realloc(void *p, size_t n);

void *malloc(size_t n)
{
    if (counting) {
        allocations++;
    }
    return __libc_malloc(n);
}

void *calloc(size_t a, size_t b)
{
    if (counting) {
        allocations++;
    }
    return __libc_calloc(a, b);
}

void *realloc(void *p, size_t n)
{
    if (counting) {
        allocations++;
    }
    return __libc_realloc(p, n);
}
#endif

static struct fb F;

static uint32_t decode(struct ppc_record *r)
{
    struct ppc_frame f = { F.b, F.n, F.n, 0, PPC_PKT_UNKNOWN };

    return ppc_decode(&f, r);
}

static void row_of(const struct ppc_record *r, struct ppc_row_text *t)
{
    ppc_format_row(r, F.b, F.n, t);
}

/* ---- the protocols ------------------------------------------------------------- */

static void test_tcp_http(void)
{
    static const char payload[] = "GET /private?password=hunter2 HTTP/1.1\r\n\r\n";
    struct ppc_record r;
    struct ppc_row_text t;
    uint32_t ip;
    uint32_t pay;
    uint32_t keep;

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 1);
    fb_tcp(&F, 50100, 80, 101, 901, 0x18);
    fb_put(&F, payload, (uint32_t)strlen(payload));
    fb_ip4_end(&F, ip);
    pay = decode(&r);
    row_of(&r, &t);
    check("TCP: class, word, ports, flags", r.cls == PPC_CLASS_TCP && strcmp(t.word, "TCP") == 0 &&
                                                r.sport == 50100 && r.dport == 80 && r.tcp_flags == 0x18);
    check("TCP: the payload starts after 14 + 20 + 20", pay == 54);
    keep = ppc_store_len(&r, pay, F.n, PPC_KEEP_HEADERS);
    check("HEADERS keeps no byte of an HTTP payload (S4)", keep == 54 &&
                                                               !memmem(F.b, keep, "hunter2", 7));
    check("FULL keeps it all", ppc_store_len(&r, pay, F.n, PPC_KEEP_FULL) == F.n);
    check("the row text names no payload", strstr(t.info, "hunter2") == NULL &&
                                               strcmp(t.info, "50100 > 80 [PSH, ACK] Seq=101 Ack=901") == 0);
    check("addresses as dotted quads", strcmp(t.src, "192.0.2.10") == 0 && strcmp(t.dst, "192.0.2.20") == 0);
}

static void test_dhcp_kept_whole(void)
{
    static const uint8_t zero4[4] = { 0 };
    static const uint8_t bc4[4] = { 255, 255, 255, 255 };
    struct ppc_record r;
    struct ppc_row_text t;
    uint32_t ip;
    uint32_t udp;
    uint32_t pay;

    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, zero4, bc4, 0, 1);
    udp = fb_udp(&F, 68, 67);
    fb_u8(&F, 1);
    fb_u8(&F, 1);
    fb_u8(&F, 6);
    fb_u8(&F, 0);
    fb_u32(&F, 0xdeadbeefu);
    fb_zero(&F, 228);
    fb_u32(&F, 0x63825363u);
    fb_u8(&F, 53);
    fb_u8(&F, 1);
    fb_u8(&F, 3);
    fb_u8(&F, 255);
    fb_udp4_end(&F, ip, udp);
    pay = decode(&r);
    row_of(&r, &t);
    check("DHCP: class and word", r.cls == PPC_CLASS_DHCP && strcmp(t.word, "DHCP") == 0);
    check("DHCP: the message type is read past byte 282 (option 53 = Request)", (r.b & 0xffu) == 3 &&
                                                                                    r.a == 0xdeadbeefu);
    check("DHCP: the sentence", strcmp(t.info, "DHCP Request, transaction 0xdeadbeef") == 0);
    check("HEADERS keeps the whole DHCP message", ppc_store_len(&r, pay, F.n, PPC_KEEP_HEADERS) == F.n);
    check("broadcast by address", (r.flags & PPC_F_BROADCAST) != 0);
}

static void test_dns(void)
{
    struct ppc_record r;
    struct ppc_row_text t;
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 17, IP_DNS, IP_A, 0, 1);
    udp = fb_udp(&F, 53, 51000);
    fb_dns(&F, 0xbeef, 0x8183, "printer.example.net", 1, 0);
    fb_udp4_end(&F, ip, udp);
    decode(&r);
    row_of(&r, &t);
    check("DNS: class, info, id and rcode",
          r.cls == PPC_CLASS_DNS && r.info == PPC_INFO_DNS && (r.a >> 16) == 0xbeef && (r.a & 0xfu) == 3);
    check("DNS: NXDOMAIN reads as a sentence",
          strcmp(t.info, "Standard query response 0xbeef A printer.example.net No such name") == 0);
    /* The same response, its name no longer stored: no name, no guess. */
    ppc_format_row(&r, NULL, 0, &t);
    check("DNS without its bytes: the record's sentence only",
          strcmp(t.info, "Standard query response 0xbeef No such name") == 0);
}

static void test_dns_names(void)
{
    char out[PPC_NAME_TEXT];
    uint32_t next = 0;
    /* 12 bytes of header, "example.net" at 12, "www" + pointer to 12 at 25 */
    static const uint8_t m[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                 3, 'n', 'e', 't', 0, 3, 'w', 'w', 'w', 0xc0, 12 };
    static const uint8_t loop[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 'a', 0xc0, 12 };
    static const uint8_t fwd[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 14, 1, 'b', 0 };
    static const uint8_t ctl[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 'a', '\t', '\n', 2, 0xc3, 0x7f, 0 };
    static const uint8_t cut[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 'a', 'b' };

    check("a plain name", ppc_dns_name(m, sizeof(m), 12, out, sizeof(out), &next) == 0 &&
                              strcmp(out, "example.net") == 0 && next == 25);
    check("a compressed name", ppc_dns_name(m, sizeof(m), 25, out, sizeof(out), &next) == 0 &&
                                   strcmp(out, "www.example.net") == 0 && next == 31);
    check("a pointer loop is refused", ppc_dns_name(loop, sizeof(loop), 12, out, sizeof(out), NULL) != 0);
    check("a forward pointer is refused", ppc_dns_name(fwd, sizeof(fwd), 12, out, sizeof(out), NULL) != 0);
    check("tabs, newlines and high bytes become dots",
          ppc_dns_name(ctl, sizeof(ctl), 12, out, sizeof(out), NULL) == 0 && strcmp(out, "a.....") == 0);
    check("a label running out of the bytes is refused", ppc_dns_name(cut, sizeof(cut), 12, out, sizeof(out), NULL) != 0);
    {
        /* 200 labels of one byte: more than the limit of 127 */
        static uint8_t many[12 + 400 + 1];
        int i;

        memset(many, 0, sizeof(many));
        for (i = 0; i < 200; i++) {
            many[12 + i * 2] = 1;
            many[12 + i * 2 + 1] = 'x';
        }
        check("too many labels is refused", ppc_dns_name(many, sizeof(many), 12, out, sizeof(out), NULL) != 0);
    }
    {
        char small[8];

        check("a short output buffer is cut, terminated",
              ppc_dns_name(m, sizeof(m), 25, small, sizeof(small), NULL) == 0 && strlen(small) == 7);
    }
}

static void test_vlan_qinq_ipv6(void)
{
    struct ppc_record r;
    struct ppc_row_text t;
    uint32_t ip;

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x88a8);
    fb_vlan(&F, 100, 0x8100);
    fb_vlan(&F, 20, 0x86dd);
    ip = fb_ip6(&F, 0, IP6_A, IP6_B);
    fb_u8(&F, 60); /* hop-by-hop, then destination options, then TCP */
    fb_u8(&F, 0);
    fb_zero(&F, 6);
    fb_u8(&F, 6);
    fb_u8(&F, 1); /* 16 bytes */
    fb_zero(&F, 14);
    fb_tcp(&F, 443, 50000, 1, 2, 0x12);
    fb_ip6_end(&F, ip);
    check("QinQ + two extension headers: TCP found at 14+8+40+8+16", decode(&r) == 14 + 8 + 40 + 8 + 16 + 20);
    row_of(&r, &t);
    check("QinQ: VLAN and IPv6 flags", (r.flags & PPC_F_VLAN) && (r.flags & PPC_F_IPV6) && r.cls == PPC_CLASS_TCP);
    check("IPv6 addresses compressed", strcmp(t.src, "2001:db8::a") == 0 && strcmp(t.dst, "2001:db8::b") == 0);
}

static void test_fragments(void)
{
    struct ppc_record r;
    uint32_t ip;
    uint32_t pay;

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 185, 7);
    fb_put(&F, "secret-fragment-payload", 23);
    fb_ip4_end(&F, ip);
    pay = decode(&r);
    check("IPv4 non-first fragment: no transport header read", (r.flags & PPC_F_NONFIRST) &&
                                                                    r.info == PPC_INFO_FRAGMENT &&
                                                                    r.a == 1480 && r.sport == 0);
    check("and none of its bytes kept", ppc_store_len(&r, pay, F.n, PPC_KEEP_HEADERS) == 34);
}

static void test_arp_icmp(void)
{
    struct ppc_record r;
    struct ppc_row_text t;
    uint32_t ip;

    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x0806);
    fb_u16(&F, 1);
    fb_u16(&F, 0x0800);
    fb_u8(&F, 6);
    fb_u8(&F, 4);
    fb_u16(&F, 1);
    fb_put(&F, MAC_A, 6);
    fb_put(&F, IP_A, 4);
    fb_zero(&F, 6);
    fb_put(&F, IP_B, 4);
    fb_pad(&F);
    decode(&r);
    row_of(&r, &t);
    check("ARP: who-has", r.cls == PPC_CLASS_ARP && strcmp(t.info, "Who has 192.0.2.20? Tell 192.0.2.10") == 0 &&
                              strcmp(t.src, "02:00:00:00:00:0a") == 0 && strcmp(t.dst, "broadcast") == 0);

    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 1, IP_B, IP_A, 0, 1);
    fb_u8(&F, 3);
    fb_u8(&F, 1);
    fb_zero(&F, 6);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    decode(&r);
    row_of(&r, &t);
    check("ICMP host unreachable", r.cls == PPC_CLASS_ICMP && r.a == 0x0301 && strcmp(t.info, "host unreachable") == 0);
}

static void test_outgoing_and_runt(void)
{
    struct ppc_record r;
    struct ppc_frame f;
    uint8_t b[10] = { 0 };

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x88cc);
    fb_zero(&F, 46);
    f.data = F.b;
    f.cap_len = F.n;
    f.wire_len = F.n;
    f.ts_ns = 0;
    f.pkttype = PPC_PKT_OUTGOING;
    ppc_decode(&f, &r);
    check("the source's direction is kept", (r.flags & PPC_F_OUT) && strcmp(ppc_proto_word(&r), "LLDP") == 0);
    f.data = b;
    f.cap_len = 10;
    f.wire_len = 10;
    check("a runt: 10 bytes kept, nothing read past", ppc_decode(&f, &r) == 10 && r.info == PPC_INFO_RUNT &&
                                                          (r.flags & PPC_F_MALFORMED));
    f.data = NULL;
    f.cap_len = 100;
    check("no data at all is a runt, not a crash", ppc_decode(&f, &r) == 0 && r.info == PPC_INFO_RUNT);
    f.data = F.b;
    f.cap_len = F.n;
    f.wire_len = 60000;
    ppc_decode(&f, &r);
    check("longer than a frame: GRO and truncated", (r.flags & PPC_F_GRO) && (r.flags & PPC_F_TRUNCATED));
}

/* Every cut of a few frames, with the bytes after the cut poisoned and the
 * buffer ending exactly at the cut on the heap, so a read past it is found
 * by ASan in packetshark-san-test and by the poison here. */
static void test_every_cut(void)
{
    static struct fb frames[4];
    int k;
    int bad = 0;
    uint32_t ip;
    uint32_t udp;

    fb_reset(&frames[0]);
    fb_eth(&frames[0], MAC_B, MAC_A, 0x8100);
    fb_vlan(&frames[0], 1, 0x0800);
    ip = fb_ip4(&frames[0], 17, IP_A, IP_DNS, 0, 1);
    udp = fb_udp(&frames[0], 5000, 53);
    fb_dns(&frames[0], 1, 0x8180, "www.example.net", 1, 1);
    fb_udp4_end(&frames[0], ip, udp);
    fb_reset(&frames[1]);
    fb_eth(&frames[1], MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&frames[1], 44, IP6_A, IP6_B);
    fb_u8(&frames[1], 6);
    fb_u8(&frames[1], 0);
    fb_u16(&frames[1], 1);
    fb_u32(&frames[1], 9);
    fb_tcp(&frames[1], 1, 2, 3, 4, 0x02);
    fb_ip6_end(&frames[1], ip);
    fb_reset(&frames[2]);
    fb_eth(&frames[2], MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&frames[2], 17, IP_A, IP_B, 0, 1);
    udp = fb_udp(&frames[2], 68, 67);
    fb_u8(&frames[2], 1);
    fb_zero(&frames[2], 235);
    fb_u32(&frames[2], 0x63825363u);
    fb_u8(&frames[2], 53);
    fb_u8(&frames[2], 1);
    fb_u8(&frames[2], 1);
    fb_udp4_end(&frames[2], ip, udp);
    fb_reset(&frames[3]);
    fb_eth(&frames[3], MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&frames[3], 58, IP6_A, IP6_B);
    fb_u8(&frames[3], 135);
    fb_zero(&frames[3], 7);
    fb_put(&frames[3], IP6_B, 16);
    fb_ip6_end(&frames[3], ip);
    for (k = 0; k < 4; k++) {
        uint32_t cut;

        for (cut = 0; cut <= frames[k].n; cut++) {
            uint8_t *heap = malloc(cut ? cut : 1);
            struct ppc_frame f;
            struct ppc_record r;
            struct ppc_row_text t;
            uint32_t pay;
            uint32_t keep;

            memcpy(heap, frames[k].b, cut);
            f.data = heap;
            f.cap_len = cut;
            f.wire_len = frames[k].n;
            f.ts_ns = 0;
            f.pkttype = PPC_PKT_UNKNOWN;
            pay = ppc_decode(&f, &r);
            keep = ppc_store_len(&r, pay, cut, PPC_KEEP_HEADERS);
            ppc_format_row(&r, heap, keep, &t);
            if (pay > cut || keep > cut || (cut < frames[k].n && !(r.flags & PPC_F_TRUNCATED)) ||
                strlen(t.info) >= sizeof(t.info) || strchr(t.info, '\t') || strchr(t.info, '\n')) {
                bad++;
                printf("     frame %d cut at %u: payload %u keep %u flags 0x%x info '%s'\n", k, cut, pay, keep,
                       r.flags, t.info);
            }
            free(heap);
        }
    }
    check("every cut of four frames: in bounds, flagged, printable", bad == 0);
}

static void test_no_allocation(void)
{
    static uint8_t tpl[64][1600];
    static uint32_t len[64];
    struct ppc_record r;
    uint64_t sum = 0;
    unsigned i;

    for (i = 0; i < 64; i++) {
        len[i] = ppc_synth_frame(i, 3, tpl[i]);
    }
#if COUNT_ALLOC
    allocations = 0;
    counting = 1;
#endif
    for (i = 0; i < 1000000u; i++) {
        struct ppc_frame f = { tpl[i & 63u], len[i & 63u], len[i & 63u], i, PPC_PKT_UNKNOWN };

        sum += ppc_decode(&f, &r);
        sum += ppc_store_len(&r, 54, f.cap_len, PPC_KEEP_HEADERS);
    }
#if COUNT_ALLOC
    counting = 0;
    check("1,000,000 frames decoded with no allocation", allocations == 0 && sum > 0);
#else
    printf("note  allocation count skipped under the sanitizers (%llu)\n", (unsigned long long)(sum & 1u));
#endif
}

static void test_words(void)
{
    struct ppc_record r;
    unsigned c;
    int ok = 1;

    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        ok = ok && strcmp(ppc_class_name(c), "?") != 0;
    }
    check("every class has a name, out of range is ?", ok && strcmp(ppc_class_name(99), "?") == 0);
    memset(&r, 0, sizeof(r));
    r.l3 = PPC_L3_IPV6;
    r.info = PPC_INFO_ICMP6;
    r.a = 133u << 8;
    check("ICMPv6 133 is NDP", strcmp(ppc_proto_word(&r), "NDP") == 0);
    r.a = 143u << 8;
    check("ICMPv6 143 is MLD", strcmp(ppc_proto_word(&r), "MLD") == 0);
    {
        char s[] = "a\tb\nc\x01\x7f\xff";

        ppc_sanitize(s);
        check("sanitize", strcmp(s, "a.b.c...") == 0);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_tcp_http();
    test_dhcp_kept_whole();
    test_dns();
    test_dns_names();
    test_vlan_qinq_ipv6();
    test_fragments();
    test_arp_icmp();
    test_outgoing_and_runt();
    test_every_cut();
    test_no_allocation();
    test_words();
    printf("ppc_decode_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
