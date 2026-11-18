/*
 * PacketShark's inspector dissector (core/pocketpcap/ppc_dissect.h),
 * milestone M6.
 *
 * What is checked, on frames built here (documentation addresses only):
 *   - the layers of TCP with options and a payload HEADERS did not keep, of
 *     DNS over UDP with its question and answer, of DHCP with its options,
 *     of ARP, ICMP echo, ICMPv6 neighbour solicitation, two VLAN tags, an
 *     IPv6 extension header, a non-first fragment and a runt;
 *   - each field's value, the bytes it came from and the filter it stands
 *     for (a port, a host, a class; none for an IP packet's Ethernet MACs,
 *     which its record does not keep);
 *   - what was not stored is said, never read ("1448 B · not stored");
 *   - every cut of every frame: no field points past the stored bytes, no
 *     more than PPC_DS_LAYERS layers or PPC_DS_ITEMS items, every text
 *     printable (the two marks aside) and terminated;
 *   - the two ends and the hex lines.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_dissect.h"
#include "pocketpcap/ppc_format.h"
#include "pocketpcap/ppc_view.h"
#include "ppc_testframes.h"

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

static struct fb F;

/* What one walk handed over. */
struct seen {
    int layers;
    int items;
    char layer[PPC_DS_LAYERS][PPC_DS_NAME];
    int layer_cls[PPC_DS_LAYERS];
    char layer_text[PPC_DS_LAYERS][PPC_DS_TEXT];
    struct ppc_ds_item item[PPC_DS_ITEMS + 8];
    int nitems;
    uint32_t n;      /* the stored bytes the walk was given */
    int bad;         /* a field past the bytes, a text that is not clean */
    const struct ppc_record *r; /* the record the walk was given */
};

static struct seen S;

/* MAC filters offered over the whole run, and those that do not match their
 * own packet (FILTER BY would show a list without it). */
static int mac_filters;
static int mac_misses;

static int clean(const char *s, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < cap && p[i]) {
        if (p[i] == 0xc2 && p[i + 1] == 0xb7) {
            i += 2;
        } else if (p[i] == 0xe2 && p[i + 1] == 0x86 && p[i + 2] == 0x92) {
            i += 3;
        } else if (p[i] < 0x20 || p[i] >= 0x7f) {
            return 0;
        } else {
            i++;
        }
    }
    return i < cap;
}

static void on_item(void *ctx, const struct ppc_ds_item *it)
{
    struct seen *s = ctx;

    s->items++;
    if (!clean(it->name, sizeof(it->name)) || !clean(it->text, sizeof(it->text)) ||
        !clean(it->filter, sizeof(it->filter))) {
        s->bad++;
    }
    if (it->off >= 0 && ((uint32_t)it->off > s->n || it->len > s->n - (uint32_t)it->off)) {
        s->bad++;
    }
    if (strncmp(it->filter, "host:", 5) == 0 && s->r) {
        struct ppc_filter f;

        memset(&f, 0, sizeof(f));
        if (ppc_filter_host(it->filter + 5, f.host) == 6) {
            f.host_len = 6;
            mac_filters++;
            if (!ppc_filter_match(&f, s->r)) {
                mac_misses++;
                printf("     %s %s: %s does not match its own packet\n", it->name, it->text, it->filter);
            }
        }
    }
    if (it->kind == PPC_DS_LAYER) {
        if (s->layers < PPC_DS_LAYERS) {
            snprintf(s->layer[s->layers], sizeof(s->layer[0]), "%s", it->name);
            snprintf(s->layer_text[s->layers], sizeof(s->layer_text[0]), "%s", it->text);
            s->layer_cls[s->layers] = it->cls;
        }
        s->layers++;
    }
    if (s->nitems < (int)(sizeof(s->item) / sizeof(s->item[0]))) {
        s->item[s->nitems++] = *it;
    }
}

/* Decode the frame as it arrived, then dissect the first n bytes (what the
 * HEADERS or FULL rule stored, or any cut). */
static void dissect(uint32_t n, struct ppc_record *r)
{
    struct ppc_frame f = { F.b, F.n, F.n, 0, PPC_PKT_UNKNOWN };

    ppc_decode(&f, r);
    memset(&S, 0, sizeof(S));
    S.n = n;
    S.r = r;
    ppc_dissect(r, F.b, n, on_item, &S);
}

static uint32_t stored(enum ppc_keep keep)
{
    struct ppc_frame f = { F.b, F.n, F.n, 0, PPC_PKT_UNKNOWN };
    struct ppc_record r;
    uint32_t pay = ppc_decode(&f, &r);

    return ppc_store_len(&r, pay, F.n, keep);
}

static const struct ppc_ds_item *field(const char *layer, const char *name)
{
    int i;
    int want = -1;

    for (i = 0; i < S.layers && i < PPC_DS_LAYERS; i++) {
        if (strcmp(S.layer[i], layer) == 0) {
            want = i;
        }
    }
    for (i = 0; i < S.nitems; i++) {
        if (S.item[i].kind == PPC_DS_FIELD && S.item[i].layer == want && strcmp(S.item[i].name, name) == 0) {
            return &S.item[i];
        }
    }
    return NULL;
}

static int is(const char *layer, const char *name, const char *text)
{
    const struct ppc_ds_item *it = field(layer, name);

    if (!it || strcmp(it->text, text) != 0) {
        printf("     %s %s: '%s', expected '%s'\n", layer, name, it ? it->text : "(none)", text);
        return 0;
    }
    return 1;
}

static int layers_are(const char *const *names, int n)
{
    int i;

    if (S.layers != n) {
        printf("     %d layers, expected %d\n", S.layers, n);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(S.layer[i], names[i]) != 0) {
            printf("     layer %d '%s', expected '%s'\n", i, S.layer[i], names[i]);
            return 0;
        }
    }
    return 1;
}

/* ---- frames ----------------------------------------------------------------------- */

static void tcp_frame(void)
{
    uint32_t ip;
    int i;

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 0x3a1c);
    /* fb_tcp writes a 20-byte header; this one carries 12 bytes of options. */
    fb_u16(&F, 443);
    fb_u16(&F, 51234);
    fb_u32(&F, 1449);
    fb_u32(&F, 518);
    fb_u8(&F, 8 << 4);
    fb_u8(&F, 0x10);
    fb_u16(&F, 501);
    fb_u16(&F, 0x1234);
    fb_u16(&F, 0);
    fb_u8(&F, 1);
    fb_u8(&F, 1);
    fb_u8(&F, 8);
    fb_u8(&F, 10);
    fb_u32(&F, 1);
    fb_u32(&F, 2);
    for (i = 0; i < 1448; i++) {
        fb_u8(&F, (uint32_t)i);
    }
    fb_ip4_end(&F, ip);
}

static void test_tcp(void)
{
    static const char *const want[] = { "Frame", "Ethernet", "IPv4", "TCP" };
    struct ppc_record r;
    const struct ppc_ds_item *it;

    tcp_frame();
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("TCP: Frame, Ethernet, IPv4, TCP", layers_are(want, 4) && S.bad == 0);
    check("TCP: the mockup's fields", is("TCP", "SOURCE PORT", "443 https") && is("TCP", "DESTINATION PORT", "51234") &&
                                          is("TCP", "SEQUENCE", "1449") && is("TCP", "ACKNOWLEDGMENT", "518") &&
                                          is("TCP", "HEADER LENGTH", "32 bytes") && is("TCP", "FLAGS", "ACK") &&
                                          is("TCP", "WINDOW", "501") && is("TCP", "OPTIONS", "NOP NOP TS"));
    check("TCP: the payload HEADERS left out is said, not read",
          is("TCP", "PAYLOAD", "1448 B \xc2\xb7 not stored") && field("TCP", "PAYLOAD")->off < 0);
    it = field("TCP", "SOURCE PORT");
    check("a port field: its two bytes, and the filter on it", it && it->off == 34 && it->len == 2 &&
                                                                    strcmp(it->filter, "port:443") == 0);
    check("TCP's summary as the mockup's", strcmp(S.layer_text[3], "443 \xe2\x86\x92 51234 \xc2\xb7 ACK") == 0 &&
                                              S.layer_cls[3] == PPC_CLASS_TCP);
    check("IPv4: summary, flags, the hosts and their filters",
          strcmp(S.layer_text[2], "TTL 64 \xc2\xb7 DF \xc2\xb7 ID 0x3A1C") == 0 && is("IPv4", "FLAGS", "DF") &&
              is("IPv4", "SOURCE", "192.0.2.10") && strcmp(field("IPv4", "SOURCE")->filter, "host:192.0.2.10") == 0 &&
              field("IPv4", "DESTINATION")->off == 30);
    check("IPv4 PROTOCOL filters on the class", strcmp(field("IPv4", "PROTOCOL")->filter, "class:0") == 0);
    check("an IP packet's MACs offer no filter (its record keeps none)",
          field("Ethernet", "SOURCE") && field("Ethernet", "SOURCE")->filter[0] == '\0');
    check("the frame layer from the record", is("Frame", "ON THE WIRE", "1514 bytes") && is("Frame", "STORED", "66 bytes"));
    dissect(stored(PPC_KEEP_FULL), &r);
    check("FULL: the payload is stored, and marked", is("TCP", "PAYLOAD", "1448 B \xc2\xb7 stored") &&
                                                         field("TCP", "PAYLOAD")->off == 66 &&
                                                         field("TCP", "PAYLOAD")->len == 1448);
    dissect(0, &r);
    check("bytes gone: the frame layer alone, summary only", S.layers == 1 &&
                                                                  is("Frame", "STORED", "0 bytes") && S.bad == 0);
}

static void test_dns(void)
{
    static const char *const want[] = { "Frame", "Ethernet", "IPv4", "UDP", "DNS" };
    struct ppc_record r;
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 17, IP_DNS, IP_A, 0, 7);
    udp = fb_udp(&F, 53, 40000);
    fb_dns(&F, 0x1a2b, 0x8180, "api.example.net", 1, 1);
    fb_udp4_end(&F, ip, udp);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("DNS: five layers, the DNS one in the DNS hue", layers_are(want, 5) && S.layer_cls[4] == PPC_CLASS_DNS &&
                                                              S.bad == 0);
    check("DNS: id, flags, counts, the question and the answer",
          is("DNS", "TRANSACTION ID", "0x1a2b") && is("DNS", "FLAGS", "response \xc2\xb7 no error") &&
              is("DNS", "QUESTIONS", "1") && is("DNS", "ANSWERS", "1") && is("DNS", "QUERY", "api.example.net A") &&
              is("DNS", "ANSWER", "A 198.51.100.7 \xc2\xb7 TTL 300"));
    check("UDP: ports, length, the payload kept whole", is("UDP", "SOURCE PORT", "53 domain") &&
                                                            strcmp(field("UDP", "SOURCE PORT")->filter, "port:53") == 0 &&
                                                            is("UDP", "PAYLOAD", "49 B \xc2\xb7 stored"));
}

static void test_dhcp(void)
{
    static const uint8_t zero4[4] = { 0 };
    static const uint8_t bc4[4] = { 255, 255, 255, 255 };
    struct ppc_record r;
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, zero4, bc4, 0, 1);
    udp = fb_udp(&F, 68, 67);
    fb_u8(&F, 1);
    fb_u8(&F, 1);
    fb_u8(&F, 6);
    fb_u8(&F, 0);
    fb_u32(&F, 0x5e10122bu);
    fb_zero(&F, 20);
    fb_put(&F, MAC_A, 6);
    fb_zero(&F, 10 + 192);
    fb_u32(&F, 0x63825363u);
    fb_u8(&F, 53);
    fb_u8(&F, 1);
    fb_u8(&F, 1);
    fb_u8(&F, 12);
    fb_u8(&F, 7);
    fb_put(&F, "printer", 7);
    fb_u8(&F, 50);
    fb_u8(&F, 4);
    fb_put(&F, IP_B, 4);
    fb_u8(&F, 255);
    fb_udp4_end(&F, ip, udp);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("DHCP: the message and its options", S.bad == 0 && is("DHCP", "OPERATION", "1 request") &&
                                                   is("DHCP", "TRANSACTION ID", "0x5e10122b") &&
                                                   is("DHCP", "CLIENT MAC", "02:00:00:00:00:0a") &&
                                                   is("DHCP", "MESSAGE TYPE", "1 Discover") &&
                                                   is("DHCP", "HOST NAME", "printer") &&
                                                   is("DHCP", "REQUESTED IP", "192.0.2.20"));
    check("DHCP: the client MAC filters (a MAC in an IP packet's message: no)",
          field("DHCP", "CLIENT MAC")->filter[0] == '\0' &&
              strcmp(field("DHCP", "REQUESTED IP")->filter, "host:192.0.2.20") == 0);
}

static void test_arp_icmp(void)
{
    struct ppc_record r;
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
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("ARP: who has, with the addresses and their filters",
          S.bad == 0 && is("ARP", "OPERATION", "1 request") && is("ARP", "SENDER IP", "192.0.2.10") &&
              is("ARP", "TARGET IP", "192.0.2.20") &&
              strcmp(field("ARP", "SENDER MAC")->filter, "host:02:00:00:00:00:0a") == 0 &&
              is("Ethernet", "TYPE", "0x0806 ARP") && strcmp(field("Ethernet", "SOURCE")->filter,
                                                             "host:02:00:00:00:00:0a") == 0);
    check("ARP: the broadcast Ethernet destination offers no filter (the record keeps the ARP MACs)",
          is("Ethernet", "DESTINATION", "ff:ff:ff:ff:ff:ff") && field("Ethernet", "DESTINATION")->filter[0] == '\0');

    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x88b5);
    fb_pad(&F);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("no network layer: the record keeps the Ethernet ends, so the broadcast destination filters",
          r.l3 == PPC_L3_OTHER && field("Ethernet", "DESTINATION") &&
              strcmp(field("Ethernet", "DESTINATION")->filter, "host:ff:ff:ff:ff:ff:ff") == 0);

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 1, IP_A, IP_B, 0, 9);
    fb_u8(&F, 8);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_u16(&F, 0x5e10);
    fb_u16(&F, 7);
    fb_zero(&F, 32);
    fb_ip4_end(&F, ip);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("ICMP echo: type, identifier, sequence", S.bad == 0 && is("ICMP", "TYPE", "8 Echo request") &&
                                                       is("ICMP", "IDENTIFIER", "24080") &&
                                                       is("ICMP", "SEQUENCE", "7"));

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 58, IP6_A, IP6_B);
    fb_u8(&F, 135);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_u32(&F, 0);
    fb_put(&F, IP6_B, 16);
    fb_ip6_end(&F, ip);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("ICMPv6 neighbour solicitation: the target and its filter",
          S.bad == 0 && is("ICMPv6", "TYPE", "135 Neighbor solicitation") && is("ICMPv6", "TARGET", "2001:db8::b") &&
              strcmp(field("ICMPv6", "TARGET")->filter, "host:2001:db8::b") == 0 &&
              is("IPv6", "SOURCE", "2001:db8::a"));
}

static void test_vlan_ext_frag_runt(void)
{
    struct ppc_record r;
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x88a8);
    fb_vlan(&F, 100, 0x8100);
    fb_vlan(&F, 200, 0x86dd);
    ip = fb_ip6(&F, 0, IP6_A, IP6_B);
    fb_u8(&F, 17);
    fb_u8(&F, 0);
    fb_zero(&F, 6);
    udp = fb_udp(&F, 5353, 5353);
    fb_dns(&F, 0, 0, "printer.local", 12, 0);
    fb_udp_end(&F, udp);
    fb_ip6_end(&F, ip);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("two VLAN tags, a hop-by-hop header, mDNS", S.bad == 0 && field("Ethernet", "VLAN") &&
                                                          is("IPv6", "EXTENSION", "Hop-by-hop \xc2\xb7 8 bytes") &&
                                                          is("mDNS", "QUERY", "printer.local PTR"));

    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 185, 0x99);
    fb_zero(&F, 100);
    fb_ip4_end(&F, ip);
    dissect(stored(PPC_KEEP_HEADERS), &r);
    check("a non-first fragment: its offset, no transport layer",
          S.bad == 0 && is("Fragment", "OFFSET", "1480 bytes") && field("UDP", "SOURCE PORT") == NULL);

    fb_reset(&F);
    fb_put(&F, MAC_A, 6);
    fb_put(&F, MAC_B, 4);
    dissect(F.n, &r);
    check("a runt: the frame layer only", S.layers == 1 && S.bad == 0);
}

/* Every cut of every frame: nothing read or pointed at past the bytes. */
static void test_every_cut(void)
{
    void (*builders[])(void) = { tcp_frame };
    struct ppc_record r;
    uint32_t n;
    int bad = 0;
    int over = 0;
    int k;
    int round;

    for (round = 0; round < 6; round++) {
        uint32_t ip;
        uint32_t udp;

        switch (round) {
        case 0:
            builders[0]();
            break;
        case 1:
            fb_reset(&F);
            fb_eth(&F, MAC_A, MAC_B, 0x0800);
            ip = fb_ip4(&F, 17, IP_DNS, IP_A, 0, 7);
            udp = fb_udp(&F, 53, 40000);
            fb_dns(&F, 0x1a2b, 0x8180, "api.example.net", 1, 1);
            fb_udp4_end(&F, ip, udp);
            break;
        case 2:
            fb_reset(&F);
            fb_eth(&F, MAC_B, MAC_A, 0x88a8);
            fb_vlan(&F, 100, 0x8100);
            fb_vlan(&F, 200, 0x86dd);
            ip = fb_ip6(&F, 0, IP6_A, IP6_B);
            fb_u8(&F, 58);
            fb_u8(&F, 0);
            fb_zero(&F, 6);
            fb_u8(&F, 135);
            fb_zero(&F, 7);
            fb_put(&F, IP6_B, 16);
            fb_ip6_end(&F, ip);
            break;
        case 3:
            fb_reset(&F);
            fb_eth(&F, MAC_BCAST, MAC_A, 0x0800);
            ip = fb_ip4(&F, 17, IP_A, IP_B, 0, 1);
            udp = fb_udp(&F, 68, 67);
            fb_u8(&F, 2);
            fb_u8(&F, 1);
            fb_u8(&F, 6);
            fb_zero(&F, 233);
            fb_u32(&F, 0x63825363u);
            for (k = 0; k < 40; k++) {
                fb_u8(&F, 12);
                fb_u8(&F, 3);
                fb_put(&F, "abc", 3);
            }
            fb_udp4_end(&F, ip, udp);
            break;
        case 4:
            fb_reset(&F);
            fb_eth(&F, MAC_BCAST, MAC_A, 0x0806);
            fb_u16(&F, 1);
            fb_u16(&F, 0x0800);
            fb_u8(&F, 6);
            fb_u8(&F, 4);
            fb_u16(&F, 2);
            fb_zero(&F, 20);
            break;
        default:
            /* TCP whose option lengths lie. */
            fb_reset(&F);
            fb_eth(&F, MAC_B, MAC_A, 0x0800);
            ip = fb_ip4(&F, 6, IP_A, IP_B, 0, 1);
            fb_u32(&F, 0x01bb1f90u);
            fb_zero(&F, 8);
            fb_u8(&F, 15 << 4);
            fb_u8(&F, 0x12);
            fb_zero(&F, 6);
            for (k = 0; k < 40; k++) {
                fb_u8(&F, k & 1 ? 0xff : 2);
            }
            fb_ip4_end(&F, ip);
            break;
        }
        for (n = 0; n <= F.n; n++) {
            dissect(n, &r);
            bad += S.bad;
            over += S.layers > PPC_DS_LAYERS || S.items > PPC_DS_ITEMS;
        }
    }
    check("every cut of six frames: no field past the stored bytes, every text clean", bad == 0);
    check("and never more layers or items than the shell keeps", over == 0);
}

static void test_ends_and_hex(void)
{
    struct ppc_record r;
    struct ppc_endpoint src;
    struct ppc_endpoint dst;
    char hex[PPC_HEX_TEXT];
    char ascii[PPC_ASCII_TEXT];
    uint8_t b[12] = { 0x02, 0x00, 0x5e, 0x10, 0x20, 0x14, 0x02, 0x00, 'E', 0x00, 0x7f, ' ' };

    tcp_frame();
    dissect(stored(PPC_KEEP_HEADERS), &r);
    ppc_endpoints(&r, &src, &dst);
    check("the ends: address, port and service, host filter",
          strcmp(src.addr, "192.0.2.10") == 0 && strcmp(src.sub, "PORT 443 \xc2\xb7 HTTPS") == 0 &&
              strcmp(src.filter, "host:192.0.2.10") == 0 && strcmp(dst.sub, "PORT 51234") == 0);
    ppc_hex_line(b, sizeof(b), 0, hex, sizeof(hex), ascii, sizeof(ascii));
    check("a hex line: eight bytes, a gap after four, printable ASCII",
          strcmp(hex, "02 00 5e 10  20 14 02 00") == 0 && strcmp(ascii, "..^. ...") == 0);
    ppc_hex_line(b, sizeof(b), 8, hex, sizeof(hex), ascii, sizeof(ascii));
    check("the last line: what is left", strcmp(hex, "45 00 7f 20") == 0 && strcmp(ascii, "E.. ") == 0);
    check("service names", strcmp(ppc_port_name(443), "https") == 0 && ppc_port_name(51234) == NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_tcp();
    test_dns();
    test_dhcp();
    test_arp_icmp();
    test_vlan_ext_frag_runt();
    test_every_cut();
    test_ends_and_hex();
    check("every MAC filter offered matches its own packet", mac_filters > 0 && mac_misses == 0);
    printf("ppc_dissect_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
