/*
 * The PacketShark decoder fixtures: deterministic synthetic captures, one
 * .pcap per protocol family, written into a directory.
 *
 *   ppc_mkfixtures <dir>
 *
 * The fixtures are generated, not committed: what is committed is what the
 * decoder must print for each (tests/data/packetshark/<name>.expected,
 * checked by tests/pos_capture_test.sh), and this program, so the bytes are
 * reviewable as code. Documentation addresses and example.net names only
 * (ppc_testframes.h). No real capture is ever a fixture: the feasibility
 * study's eth0-real.pcap holds LAN metadata and stays outside the repository.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_pcapfile.h"
#include "ppc_testframes.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 2025-01-02T00:00:00Z: a set clock, so files read as real dates. */
#define BASE_NS (1735776000ull * 1000000000ull)

static int out_fd = -1;
static uint64_t out_ts;
static int failed;
static struct fb F;

static void begin(const char *dir, const char *name)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s.pcap", dir, name);
    out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0 || ppc_pcap_write_header(out_fd, 65535) != 0) {
        fprintf(stderr, "ppc_mkfixtures: cannot write %s\n", path);
        failed = 1;
    }
    out_ts = BASE_NS;
}

/* Write F as one record, cut to cap bytes (0: all of it). */
static void emit_cut(uint32_t cap)
{
    uint32_t wire = F.n;

    if (cap == 0 || cap > F.n) {
        cap = F.n;
    }
    if (out_fd >= 0 && ppc_pcap_write_record(out_fd, out_ts, F.b, cap, wire) != 0) {
        failed = 1;
    }
    out_ts += 1500000u; /* 1.5 ms */
}

static void emit(void)
{
    emit_cut(0);
}

static void end(void)
{
    if (out_fd >= 0) {
        close(out_fd);
    }
    out_fd = -1;
}

/* ---- DNS ------------------------------------------------------------------------- */

static void dns_frame(uint32_t id, uint32_t flags, const char *name, uint32_t type, int answer, int from_server)
{
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, from_server ? MAC_A : MAC_B, from_server ? MAC_B : MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, from_server ? IP_DNS : IP_A, from_server ? IP_A : IP_DNS, 0, id);
    udp = fb_udp(&F, from_server ? 53 : 51000, from_server ? 51000 : 53);
    fb_dns(&F, id, flags, name, type, answer);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
}

static void fixture_dns(const char *dir)
{
    uint32_t ip;
    uint32_t udp;

    begin(dir, "dns");
    dns_frame(0x1a2b, 0x0100, "www.example.net", 1, 0, 0);
    dns_frame(0x1a2b, 0x8180, "www.example.net", 1, 1, 1);
    dns_frame(0x2b3c, 0x0100, "printer.example.net", 28, 0, 0);
    dns_frame(0x2b3c, 0x8183, "printer.example.net", 28, 0, 1); /* NXDOMAIN */
    dns_frame(0x3c4d, 0x8182, "api.example.net", 1, 0, 1);      /* SERVFAIL */
    dns_frame(0x4d5e, 0x0100, "10.2.0.192.in-addr.arpa", 12, 0, 0);
    /* A question whose name is a label and a pointer back to itself: a
     * compression loop. The name must be refused, not followed forever. */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_DNS, 0, 7);
    udp = fb_udp(&F, 51001, 53);
    fb_u16(&F, 0x5e6f);
    fb_u16(&F, 0x0100);
    fb_u16(&F, 1);
    fb_zero(&F, 6);
    fb_u8(&F, 1);
    fb_u8(&F, 'a');
    fb_u16(&F, 0xc00c);
    fb_u16(&F, 1);
    fb_u16(&F, 1);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
    /* A forward pointer, which no encoder writes. */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_DNS, 0, 8);
    udp = fb_udp(&F, 51002, 53);
    fb_u16(&F, 0x6f70);
    fb_u16(&F, 0x0100);
    fb_u16(&F, 1);
    fb_zero(&F, 6);
    fb_u16(&F, 0xc020);
    fb_u16(&F, 1);
    fb_u16(&F, 1);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
    /* A header cut short: eight bytes of DNS. */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_DNS, 0, 9);
    udp = fb_udp(&F, 51003, 53);
    fb_zero(&F, 8);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
    end();
}

/* ---- DHCP ------------------------------------------------------------------------ */

static void dhcp_frame(uint32_t op, uint32_t type, int from_server)
{
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    static const uint8_t bcast4[4] = { 255, 255, 255, 255 };
    uint32_t ip;
    uint32_t udp;

    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, from_server ? MAC_B : MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, from_server ? IP_B : zero4, bcast4, 0, type);
    udp = fb_udp(&F, from_server ? 67 : 68, from_server ? 68 : 67);
    fb_u8(&F, op);
    fb_u8(&F, 1);
    fb_u8(&F, 6);
    fb_u8(&F, 0);
    fb_u32(&F, 0x3903f326u);
    fb_zero(&F, 8); /* secs, flags, ciaddr */
    if (from_server) {
        fb_put(&F, IP_A, 4); /* yiaddr */
    } else {
        fb_zero(&F, 4);
    }
    fb_zero(&F, 8);
    fb_put(&F, MAC_A, 6);
    fb_zero(&F, 10 + 64 + 128);
    fb_u32(&F, 0x63825363u);
    fb_u8(&F, 53);
    fb_u8(&F, 1);
    fb_u8(&F, type);
    fb_u8(&F, 12);
    fb_u8(&F, 6);
    fb_put(&F, "sensor", 6); /* a host name: kept, it is what the tool is for */
    fb_u8(&F, 255);
    fb_udp4_end(&F, ip, udp);
    emit();
}

static void fixture_dhcp(const char *dir)
{
    begin(dir, "dhcp");
    dhcp_frame(1, 1, 0);
    dhcp_frame(2, 2, 1);
    dhcp_frame(1, 3, 0);
    dhcp_frame(2, 5, 1);
    end();
}

/* ---- mDNS, LLMNR, NBNS, NTP, SSDP, IGMP, DHCPv6 ---------------------------------- */

static void fixture_local(const char *dir)
{
    static const uint8_t mdns4[4] = { 224, 0, 0, 251 };
    static const uint8_t llmnr4[4] = { 224, 0, 0, 252 };
    static const uint8_t ssdp4[4] = { 239, 255, 255, 250 };
    static const uint8_t nbns4[4] = { 192, 0, 2, 255 };
    static const uint8_t mcmac[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
    static const uint8_t igmp4[4] = { 224, 0, 0, 22 };
    static const uint8_t dhcp6[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2 };
    static const uint8_t ll6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a };
    uint32_t ip;
    uint32_t udp;
    const char *msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n\r\n";

    begin(dir, "local");
    fb_reset(&F);
    fb_eth(&F, mcmac, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, mdns4, 0, 1);
    udp = fb_udp(&F, 5353, 5353);
    fb_dns(&F, 0, 0, "_ipp._tcp.local", 12, 0);
    fb_udp4_end(&F, ip, udp);
    emit();
    fb_reset(&F);
    fb_eth(&F, mcmac, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, llmnr4, 0, 2);
    udp = fb_udp(&F, 52000, 5355);
    fb_dns(&F, 0x7a7a, 0, "nas", 1, 0);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, nbns4, 0, 3);
    udp = fb_udp(&F, 137, 137);
    fb_dns(&F, 0x8001, 0x0110, "EKFCEMEFFDEOEPCACACACACACACACAAA", 32, 0);
    fb_udp4_end(&F, ip, udp);
    emit();
    /* NTP client and server */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 0, 4);
    udp = fb_udp(&F, 123, 123);
    fb_u8(&F, 0x23); /* v4, client */
    fb_zero(&F, 47);
    fb_udp4_end(&F, ip, udp);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 17, IP_B, IP_A, 0, 5);
    udp = fb_udp(&F, 123, 123);
    fb_u8(&F, 0x24); /* v4, server */
    fb_u8(&F, 2);
    fb_zero(&F, 46);
    fb_udp4_end(&F, ip, udp);
    emit();
    /* SSDP M-SEARCH: its text is payload, kept only with FULL */
    fb_reset(&F);
    fb_eth(&F, mcmac, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, ssdp4, 0, 6);
    udp = fb_udp(&F, 53000, 1900);
    fb_put(&F, msearch, (uint32_t)strlen(msearch));
    fb_udp4_end(&F, ip, udp);
    emit();
    /* IGMPv2 report and v3 report */
    fb_reset(&F);
    fb_eth(&F, mcmac, MAC_A, 0x0800);
    ip = fb_ip4(&F, 2, IP_A, igmp4, 0, 7);
    fb_u8(&F, 0x16);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_put(&F, igmp4, 4);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    fb_reset(&F);
    fb_eth(&F, mcmac, MAC_A, 0x0800);
    ip = fb_ip4(&F, 2, IP_A, igmp4, 0, 8);
    fb_u8(&F, 0x22);
    fb_zero(&F, 7);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    /* DHCPv6 Solicit */
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 17, ll6, dhcp6);
    udp = fb_udp(&F, 546, 547);
    fb_u8(&F, 1);
    fb_u8(&F, 0x12);
    fb_u8(&F, 0x34);
    fb_u8(&F, 0x56);
    fb_zero(&F, 4);
    fb_udp_end(&F, udp);
    fb_ip6_end(&F, ip);
    emit();
    end();
}

/* ---- fragments and VLANs --------------------------------------------------------- */

static void fixture_frag_vlan(const char *dir)
{
    uint32_t ip;
    uint32_t udp;

    begin(dir, "frag");
    /* IPv4: first fragment (MF) of a UDP datagram, then the rest */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 0x2000, 0x4242);
    udp = fb_udp(&F, 40000, 4500);
    fb_zero(&F, 64);
    fb_set16(&F, udp + 4, 2008);
    fb_ip4_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 185, 0x4242); /* offset 1480 */
    fb_zero(&F, 64);
    fb_ip4_end(&F, ip);
    emit();
    /* IPv6: fragment header, first then non-first */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 44, IP6_A, IP6_B);
    fb_u8(&F, 17);
    fb_u8(&F, 0);
    fb_u16(&F, 0x0001); /* offset 0, M */
    fb_u32(&F, 0xabcdef01u);
    udp = fb_udp(&F, 40001, 4501);
    fb_zero(&F, 32);
    fb_set16(&F, udp + 4, 3000);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 44, IP6_A, IP6_B);
    fb_u8(&F, 17);
    fb_u8(&F, 0);
    fb_u16(&F, 1448);
    fb_u32(&F, 0xabcdef01u);
    fb_zero(&F, 32);
    fb_ip6_end(&F, ip);
    emit();
    end();

    begin(dir, "vlan");
    /* 802.1Q, IPv4 UDP */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x8100);
    fb_vlan(&F, 10, 0x0800);
    ip = fb_ip4(&F, 17, IP_A, IP_B, 0, 1);
    udp = fb_udp(&F, 40000, 9);
    fb_zero(&F, 8);
    fb_udp4_end(&F, ip, udp);
    fb_pad(&F);
    emit();
    /* QinQ (802.1ad outer, 802.1Q inner), IPv6 TCP SYN-ACK */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x88a8);
    fb_vlan(&F, 100, 0x8100);
    fb_vlan(&F, 20, 0x86dd);
    ip = fb_ip6(&F, 6, IP6_A, IP6_B);
    fb_tcp(&F, 443, 50000, 1000, 2001, 0x12);
    fb_ip6_end(&F, ip);
    emit();
    /* Three tags: the third is beyond what the decoder follows. */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x88a8);
    fb_vlan(&F, 1, 0x8100);
    fb_vlan(&F, 2, 0x8100);
    fb_vlan(&F, 3, 0x0800);
    fb_pad(&F);
    emit();
    /* A tag with nothing after it */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x8100);
    fb_u16(&F, 5);
    emit();
    end();
}

/* ---- truncation at every boundary ---------------------------------------------- */

static void fixture_truncated(const char *dir)
{
    uint32_t cut;
    uint32_t ip;
    uint32_t udp;

    begin(dir, "truncated");
    /* IPv4 TCP SYN cut at every length from 0 to the whole 54 bytes: the
     * stored length is the cut, the wire length the whole. */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 1);
    fb_tcp(&F, 50000, 80, 1, 0, 0x02);
    fb_ip4_end(&F, ip);
    for (cut = 1; cut <= F.n; cut++) {
        emit_cut(cut);
    }
    /* IPv6 UDP DNS query cut at every length */
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 17, IP6_A, IP6_B);
    udp = fb_udp(&F, 50001, 53);
    fb_dns(&F, 0x0102, 0x0100, "www.example.net", 28, 0);
    fb_udp_end(&F, udp);
    fb_ip6_end(&F, ip);
    for (cut = 1; cut <= F.n; cut++) {
        emit_cut(cut);
    }
    end();
}

/* ---- a GRO-merged record and a plain-text HTTP exchange ------------------------- */

static void fixture_tcp(const char *dir)
{
    static const char req[] = "GET /private?password=hunter2 HTTP/1.1\r\nHost: www.example.net\r\n\r\n";
    static const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nsecret-body!";
    uint32_t ip;
    uint32_t i;

    begin(dir, "http");
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 1);
    fb_tcp(&F, 50100, 80, 100, 0, 0x02);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 6, IP_B, IP_A, 0x4000, 2);
    fb_tcp(&F, 80, 50100, 900, 101, 0x12);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 3);
    fb_tcp(&F, 50100, 80, 101, 901, 0x18);
    fb_put(&F, req, (uint32_t)strlen(req));
    fb_ip4_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 6, IP_B, IP_A, 0x4000, 4);
    fb_tcp(&F, 80, 50100, 901, 101 + (uint32_t)strlen(req), 0x18);
    fb_put(&F, resp, (uint32_t)strlen(resp));
    fb_ip4_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0x4000, 5);
    fb_tcp(&F, 50100, 80, 101 + (uint32_t)strlen(req), 901 + (uint32_t)strlen(resp), 0x14);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    end();

    begin(dir, "gro");
    /* What r8152's GRO can hand up: one record of 60,000 bytes. */
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 6, IP_B, IP_A, 0x4000, 9);
    fb_tcp(&F, 443, 50200, 5000, 77, 0x10);
    for (i = 0; F.n < 60000; i++) {
        fb_u8(&F, 'g');
    }
    fb_ip4_end(&F, ip);
    emit();
    end();
}

/* ---- ICMP, ICMPv6, ARP, and the rest -------------------------------------------- */

static void fixture_control(const char *dir)
{
    static const uint8_t allnodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    static const uint8_t mld2[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16 };
    static const uint8_t ll6[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0b };
    static const uint8_t lldp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    uint32_t ip;
    uint32_t in;

    begin(dir, "icmp");
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 1, IP_A, IP_B, 0, 1);
    fb_u8(&F, 8);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_u16(&F, 0x0042);
    fb_u16(&F, 7);
    fb_zero(&F, 32);
    fb_ip4_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 1, IP_B, IP_A, 0, 2);
    fb_u8(&F, 0);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_u16(&F, 0x0042);
    fb_u16(&F, 7);
    fb_zero(&F, 32);
    fb_ip4_end(&F, ip);
    emit();
    /* Port unreachable quoting the datagram that caused it */
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x0800);
    ip = fb_ip4(&F, 1, IP_B, IP_A, 0, 3);
    fb_u8(&F, 3);
    fb_u8(&F, 3);
    fb_u16(&F, 0);
    fb_u32(&F, 0);
    in = fb_ip4(&F, 17, IP_A, IP_B, 0, 9);
    fb_udp(&F, 40000, 9);
    fb_set16(&F, in + 2, 28);
    fb_ip4_end(&F, ip);
    emit();
    /* ICMPv6: router advertisement, neighbour solicitation and advertisement,
     * MLDv2 report, echo request */
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_B, 0x86dd);
    ip = fb_ip6(&F, 58, ll6, allnodes);
    fb_u8(&F, 134);
    fb_zero(&F, 15);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 58, IP6_A, allnodes);
    fb_u8(&F, 135);
    fb_zero(&F, 7);
    fb_put(&F, IP6_B, 16);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_A, MAC_B, 0x86dd);
    ip = fb_ip6(&F, 58, IP6_B, IP6_A);
    fb_u8(&F, 136);
    fb_zero(&F, 7);
    fb_put(&F, IP6_B, 16);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 0, IP6_A, mld2); /* hop-by-hop first, as MLD has */
    fb_u8(&F, 58);
    fb_u8(&F, 0);
    fb_zero(&F, 6);
    fb_u8(&F, 143);
    fb_zero(&F, 7);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 58, IP6_A, IP6_B);
    fb_u8(&F, 128);
    fb_u8(&F, 0);
    fb_u16(&F, 0);
    fb_u16(&F, 0x0099);
    fb_u16(&F, 1);
    fb_ip6_end(&F, ip);
    fb_pad(&F);
    emit();
    end();

    begin(dir, "arp");
    {
        static const struct {
            uint32_t op;
            const uint8_t *smac;
            const uint8_t *sip;
            const uint8_t *tmac;
            const uint8_t *tip;
            const uint8_t *dst;
        } arps[] = {
            { 1, MAC_A, IP_A, NULL, IP_B, MAC_BCAST },  /* who has */
            { 2, MAC_B, IP_B, MAC_A, IP_A, MAC_A },     /* is at */
            { 1, MAC_A, IP_A, NULL, IP_A, MAC_BCAST },  /* announcement */
            { 1, MAC_A, NULL, NULL, IP_B, MAC_BCAST },  /* probe */
        };
        unsigned k;

        for (k = 0; k < sizeof(arps) / sizeof(arps[0]); k++) {
            fb_reset(&F);
            fb_eth(&F, arps[k].dst, arps[k].smac, 0x0806);
            fb_u16(&F, 1);
            fb_u16(&F, 0x0800);
            fb_u8(&F, 6);
            fb_u8(&F, 4);
            fb_u16(&F, arps[k].op);
            fb_put(&F, arps[k].smac, 6);
            fb_put(&F, arps[k].sip ? arps[k].sip : zero4, 4);
            if (arps[k].tmac) {
                fb_put(&F, arps[k].tmac, 6);
            } else {
                fb_zero(&F, 6);
            }
            fb_put(&F, arps[k].tip, 4);
            fb_pad(&F);
            emit();
        }
    }
    end();

    begin(dir, "other");
    fb_reset(&F);
    fb_eth(&F, lldp, MAC_B, 0x88cc);
    fb_zero(&F, 46);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_BCAST, MAC_B, 0x8899); /* RRCP, a switch's own protocol */
    fb_zero(&F, 46);
    emit();
    fb_reset(&F);
    fb_eth(&F, lldp, MAC_B, 0x0026); /* 802.3 length, LLC (spanning tree) */
    fb_u8(&F, 0x42);
    fb_u8(&F, 0x42);
    fb_u8(&F, 0x03);
    fb_zero(&F, 43);
    emit();
    fb_reset(&F);
    fb_put(&F, MAC_BCAST, 6);
    fb_put(&F, MAC_A, 4); /* a runt: ten bytes */
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 47, IP_A, IP_B, 0, 1); /* GRE */
    fb_zero(&F, 8);
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x86dd);
    ip = fb_ip6(&F, 0, IP6_A, IP6_B); /* hop-by-hop, then no next header */
    fb_u8(&F, 59);
    fb_u8(&F, 0);
    fb_zero(&F, 6);
    fb_ip6_end(&F, ip);
    emit();
    fb_reset(&F);
    fb_eth(&F, MAC_B, MAC_A, 0x0800);
    ip = fb_ip4(&F, 6, IP_A, IP_B, 0, 1);
    F.b[ip] = 0x4f; /* IHL says 60 bytes of header: more than there is */
    fb_ip4_end(&F, ip);
    fb_pad(&F);
    emit();
    end();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: ppc_mkfixtures <dir>\n");
        return 2;
    }
    fixture_dns(argv[1]);
    fixture_dhcp(argv[1]);
    fixture_local(argv[1]);
    fixture_frag_vlan(argv[1]);
    fixture_truncated(argv[1]);
    fixture_tcp(argv[1]);
    fixture_control(argv[1]);
    return failed;
}
