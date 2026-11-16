/*
 * pocketpcap decoder. See ppc_decode.h for the rules it keeps.
 *
 * Shape: one pass from the outside in. Each layer gets the offset where it
 * begins and `end`, the first byte it may not read (the captured length, cut
 * further by the IP length field where there is one, so Ethernet padding is
 * never read as protocol). Each layer returns the offset where its payload
 * begins. Nothing here formats text or allocates (tests/packetshark_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_decode.h"

#include <string.h>

#define ETH_HDR 14u
#define VLAN_TAGS_MAX 2
#define IPV6_EXT_MAX 8
/* DHCP: fixed BOOTP part, then the magic cookie, then options. */
#define DHCP_FIXED 236u
#define DHCP_OPTIONS (DHCP_FIXED + 4u)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static void malformed(struct ppc_record *r)
{
    r->flags |= PPC_F_MALFORMED;
}

/* ---- application protocols, by port ------------------------------------------ */

static int either(uint16_t s, uint16_t d, uint16_t port)
{
    return s == port || d == port;
}

static enum ppc_app port_app(uint16_t s, uint16_t d, int udp)
{
    if (either(s, d, 53)) {
        return PPC_APP_DNS;
    }
    if (either(s, d, 5353)) {
        return PPC_APP_MDNS;
    }
    if (either(s, d, 5355)) {
        return PPC_APP_LLMNR;
    }
    if (!udp) {
        return PPC_APP_NONE;
    }
    if (either(s, d, 67) || either(s, d, 68)) {
        return PPC_APP_DHCP;
    }
    if (either(s, d, 546) || either(s, d, 547)) {
        return PPC_APP_DHCP6;
    }
    if (either(s, d, 123)) {
        return PPC_APP_NTP;
    }
    if (either(s, d, 137)) {
        return PPC_APP_NBNS;
    }
    if (either(s, d, 1900)) {
        return PPC_APP_SSDP;
    }
    return PPC_APP_NONE;
}

/* DNS, mDNS, LLMNR and NetBIOS-NS share the DNS header. */
static void dns_header(const uint8_t *d, uint32_t off, uint32_t end, struct ppc_record *r)
{
    if (end < off + 12) {
        malformed(r);
        return;
    }
    r->info = PPC_INFO_DNS;
    r->a = (uint32_t)rd16(d + off) << 16 | rd16(d + off + 2);
    r->b = (uint32_t)rd16(d + off + 4) << 16 | rd16(d + off + 6);
}

/* The DHCP message type (option 53), walked inside the captured bytes. */
static void dhcp_message(const uint8_t *d, uint32_t off, uint32_t end, struct ppc_record *r)
{
    uint32_t i;
    uint8_t type = 0;

    /* Every app parser commits its sentence only once its fixed part is
     * there: until then the row stays the UDP one, whose arguments a and b
     * still hold. */
    if (end < off + DHCP_FIXED) {
        malformed(r);
        return;
    }
    r->info = PPC_INFO_DHCP;
    r->a = rd32(d + off + 4);
    if (end >= off + DHCP_OPTIONS && rd32(d + off + DHCP_FIXED) == 0x63825363u) {
        /* At most one option per byte: the loop ends inside `end` whatever
         * the lengths say. */
        i = off + DHCP_OPTIONS;
        while (i < end) {
            uint8_t code = d[i];

            if (code == 0) {
                i++;
                continue;
            }
            if (code == 255 || i + 1 >= end) {
                break;
            }
            if (code == 53 && d[i + 1] >= 1 && i + 2 < end) {
                type = d[i + 2];
                break;
            }
            i += 2u + d[i + 1];
        }
    }
    r->b = (uint32_t)d[off] << 8 | type;
}

static void dhcp6_message(const uint8_t *d, uint32_t off, uint32_t end, struct ppc_record *r)
{
    if (end < off + 4) {
        malformed(r);
        return;
    }
    r->info = PPC_INFO_DHCP6;
    r->a = d[off];
    r->b = (uint32_t)d[off + 1] << 16 | (uint32_t)d[off + 2] << 8 | d[off + 3];
}

static void ntp_message(const uint8_t *d, uint32_t off, uint32_t end, struct ppc_record *r)
{
    if (end < off + 2) {
        malformed(r);
        return;
    }
    if (end < off + 48) {
        malformed(r);
    }
    r->info = PPC_INFO_NTP;
    r->a = (uint32_t)((d[off] >> 3) & 7u) << 8 | (d[off] & 7u);
    r->b = d[off + 1];
}

static void app_class(struct ppc_record *r)
{
    switch (r->app) {
    case PPC_APP_DNS:
    case PPC_APP_MDNS:
    case PPC_APP_LLMNR:
        r->cls = PPC_CLASS_DNS;
        break;
    case PPC_APP_DHCP:
    case PPC_APP_DHCP6:
        r->cls = PPC_CLASS_DHCP;
        break;
    default:
        break;
    }
}

/* ---- transport ------------------------------------------------------------------- */

static uint32_t transport(const uint8_t *d, uint32_t off, uint32_t end, uint8_t proto, int v6,
                          struct ppc_record *r)
{
    r->l4 = proto;
    switch (proto) {
    case 6: {
        uint32_t doff;

        r->cls = PPC_CLASS_TCP;
        if (end < off + 20) {
            malformed(r);
            return end;
        }
        r->info = PPC_INFO_TCP;
        r->sport = rd16(d + off);
        r->dport = rd16(d + off + 2);
        r->a = rd32(d + off + 4);
        r->b = rd32(d + off + 8);
        r->tcp_flags = d[off + 13] & 0x3fu;
        doff = (uint32_t)(d[off + 12] >> 4) * 4u;
        if (doff < 20 || off + doff > end) {
            malformed(r);
            return end;
        }
        r->app = (uint8_t)port_app(r->sport, r->dport, 0);
        app_class(r);
        if (r->app != PPC_APP_NONE) {
            /* DNS over TCP: a two-byte length, then the message. The row
             * still reads as TCP's (a segment of a stream), the class is
             * DNS. */
            r->info = PPC_INFO_TCP;
        }
        return off + doff;
    }
    case 17: {
        uint32_t pay = off + 8;

        r->cls = PPC_CLASS_UDP;
        if (end < off + 8) {
            malformed(r);
            return end;
        }
        r->info = PPC_INFO_UDP;
        r->sport = rd16(d + off);
        r->dport = rd16(d + off + 2);
        r->a = rd16(d + off + 4);
        r->app = (uint8_t)port_app(r->sport, r->dport, 1);
        app_class(r);
        switch (r->app) {
        case PPC_APP_DNS:
        case PPC_APP_MDNS:
        case PPC_APP_LLMNR:
        case PPC_APP_NBNS:
            dns_header(d, pay, end, r);
            break;
        case PPC_APP_DHCP:
            dhcp_message(d, pay, end, r);
            break;
        case PPC_APP_DHCP6:
            dhcp6_message(d, pay, end, r);
            break;
        case PPC_APP_NTP:
            ntp_message(d, pay, end, r);
            break;
        default:
            break;
        }
        return pay;
    }
    case 1:
        if (v6) {
            break;
        }
        r->cls = PPC_CLASS_ICMP;
        if (end < off + 4) {
            malformed(r);
            return end;
        }
        r->info = PPC_INFO_ICMP;
        r->a = (uint32_t)d[off] << 8 | d[off + 1];
        if ((d[off] == 0 || d[off] == 8) && end >= off + 8) {
            r->b = rd32(d + off + 4);
        }
        return off;
    case 58:
        if (!v6) {
            break;
        }
        r->cls = PPC_CLASS_IPV6;
        if (end < off + 4) {
            malformed(r);
            return end;
        }
        r->info = PPC_INFO_ICMP6;
        r->a = (uint32_t)d[off] << 8 | d[off + 1];
        if ((d[off] == 128 || d[off] == 129) && end >= off + 8) {
            r->b = rd32(d + off + 4);
        }
        return off;
    case 2:
        r->cls = v6 ? PPC_CLASS_IPV6 : PPC_CLASS_OTHER;
        if (end < off + 1) {
            malformed(r);
            return end;
        }
        r->info = PPC_INFO_IGMP;
        r->a = d[off];
        /* The IGMP message is its header: kept like one. */
        return min_u32(off + 8u, end);
    default:
        break;
    }
    r->cls = v6 ? PPC_CLASS_IPV6 : PPC_CLASS_OTHER;
    r->info = PPC_INFO_IP_PROTO;
    return off;
}

static uint8_t frag_class(uint8_t proto, int v6)
{
    switch (proto) {
    case 6:
        return PPC_CLASS_TCP;
    case 17:
        return PPC_CLASS_UDP;
    case 1:
        return v6 ? PPC_CLASS_IPV6 : PPC_CLASS_ICMP;
    default:
        return v6 ? PPC_CLASS_IPV6 : PPC_CLASS_OTHER;
    }
}

/* ---- network --------------------------------------------------------------------- */

static uint32_t arp(const uint8_t *d, uint32_t n, uint32_t off, struct ppc_record *r)
{
    r->l3 = PPC_L3_ARP;
    r->cls = PPC_CLASS_ARP;
    r->info = PPC_INFO_ARP;
    memset(r->src, 0, sizeof(r->src));
    memset(r->dst, 0, sizeof(r->dst));
    if (n < off + 8) {
        malformed(r);
        return n;
    }
    r->a = rd16(d + off + 6);
    if (rd16(d + off) == 1 && rd16(d + off + 2) == 0x0800 && d[off + 4] == 6 && d[off + 5] == 4) {
        if (n < off + 28) {
            malformed(r);
            return n;
        }
        memcpy(r->src, d + off + 14, 4);
        memcpy(r->src + 4, d + off + 8, 6);
        memcpy(r->dst, d + off + 24, 4);
        memcpy(r->dst + 4, d + off + 18, 6);
    }
    return off;
}

static uint32_t ipv4(const uint8_t *d, uint32_t n, uint32_t off, struct ppc_record *r)
{
    const uint8_t *p = d + off;
    uint32_t ihl;
    uint32_t total;
    uint32_t end;
    uint16_t frag;
    uint32_t fo;

    r->l3 = PPC_L3_IPV4;
    memset(r->src, 0, sizeof(r->src));
    memset(r->dst, 0, sizeof(r->dst));
    if (n < off + 20) {
        malformed(r);
        return n;
    }
    ihl = (uint32_t)(p[0] & 15u) * 4u;
    if ((p[0] >> 4) != 4 || ihl < 20) {
        malformed(r);
        return off + 20;
    }
    memcpy(r->src, p + 12, 4);
    memcpy(r->dst, p + 16, 4);
    if (p[16] >= 224 && p[16] <= 239) {
        r->flags |= PPC_F_MULTICAST;
    }
    if (rd32(p + 16) == 0xffffffffu) {
        r->flags |= PPC_F_BROADCAST;
    }
    if (n < off + ihl) {
        malformed(r);
        return n;
    }
    total = rd16(p + 2);
    end = off + total;
    if (total < ihl) {
        malformed(r);
        end = off + ihl;
    }
    if (end > n) {
        /* More than was captured: a snap length, or a length that lies. */
        if (!(r->flags & PPC_F_TRUNCATED)) {
            malformed(r);
        }
        end = n;
    }
    frag = rd16(p + 6);
    fo = (uint32_t)(frag & 0x1fffu) * 8u;
    if ((frag & 0x2000u) || fo) {
        r->flags |= PPC_F_FRAGMENT;
    }
    if (fo) {
        r->flags |= PPC_F_NONFIRST;
        r->l4 = p[9];
        r->cls = frag_class(p[9], 0);
        r->info = PPC_INFO_FRAGMENT;
        r->a = fo;
        r->b = rd16(p + 4);
        return off + ihl;
    }
    return transport(d, off + ihl, end, p[9], 0, r);
}

static uint32_t ipv6(const uint8_t *d, uint32_t n, uint32_t off, struct ppc_record *r)
{
    const uint8_t *p = d + off;
    uint32_t end;
    uint32_t h;
    uint8_t nh;
    int i;

    r->l3 = PPC_L3_IPV6;
    r->cls = PPC_CLASS_IPV6;
    r->flags |= PPC_F_IPV6;
    memset(r->src, 0, sizeof(r->src));
    memset(r->dst, 0, sizeof(r->dst));
    if (n < off + 40) {
        malformed(r);
        return n;
    }
    if ((p[0] >> 4) != 6) {
        malformed(r);
        return off + 40;
    }
    memcpy(r->src, p + 8, 16);
    memcpy(r->dst, p + 24, 16);
    if (p[24] == 0xff) {
        r->flags |= PPC_F_MULTICAST;
    }
    end = off + 40u + rd16(p + 4);
    if (rd16(p + 4) == 0 || end > n) {
        if (rd16(p + 4) != 0 && !(r->flags & PPC_F_TRUNCATED)) {
            malformed(r);
        }
        end = n;
    }
    nh = p[6];
    h = off + 40;
    for (i = 0; i < IPV6_EXT_MAX && (nh == 0 || nh == 43 || nh == 44 || nh == 60); i++) {
        if (h + 8 > end) {
            malformed(r);
            r->l4 = nh;
            return end;
        }
        if (nh == 44) {
            uint32_t fo = rd16(d + h + 2) & 0xfff8u;

            r->flags |= PPC_F_FRAGMENT;
            nh = d[h];
            h += 8;
            if (fo) {
                r->flags |= PPC_F_NONFIRST;
                r->l4 = nh;
                r->cls = frag_class(nh, 1);
                r->info = PPC_INFO_FRAGMENT;
                r->a = fo;
                r->b = rd32(d + h - 4);
                return h;
            }
        } else {
            uint32_t len = ((uint32_t)d[h + 1] + 1u) * 8u;

            nh = d[h];
            if (h + len > end) {
                malformed(r);
                r->l4 = nh;
                return end;
            }
            h += len;
        }
    }
    return transport(d, h, end, nh, 1, r);
}

/* ---- the frame ------------------------------------------------------------------- */

uint32_t ppc_decode(const struct ppc_frame *f, struct ppc_record *r)
{
    const uint8_t *d = f->data;
    uint32_t n = d ? f->cap_len : 0;
    uint32_t off = ETH_HDR;
    uint16_t et;
    int tags = 0;

    memset(r, 0, sizeof(*r));
    r->wire_len = f->wire_len;
    r->cls = PPC_CLASS_OTHER;
    if (n < f->wire_len) {
        r->flags |= PPC_F_TRUNCATED;
    }
    if (f->wire_len > PPC_ETH_MAX_FRAME) {
        r->flags |= PPC_F_GRO;
    }
    switch (f->pkttype) {
    case PPC_PKT_OUTGOING:
        r->flags |= PPC_F_OUT;
        break;
    case PPC_PKT_BROADCAST:
        r->flags |= PPC_F_BROADCAST;
        break;
    case PPC_PKT_MULTICAST:
        r->flags |= PPC_F_MULTICAST;
        break;
    default:
        break;
    }
    if (n < ETH_HDR) {
        r->info = PPC_INFO_RUNT;
        malformed(r);
        return n;
    }
    memcpy(r->dst, d, 6);
    memcpy(r->src, d + 6, 6);
    if (rd32(d) == 0xffffffffu && rd16(d + 4) == 0xffffu) {
        r->flags |= PPC_F_BROADCAST;
    } else if (d[0] & 1u) {
        r->flags |= PPC_F_MULTICAST;
    }
    et = rd16(d + 12);
    while ((et == 0x8100 || et == 0x88a8 || et == 0x9100) && tags < VLAN_TAGS_MAX) {
        r->flags |= PPC_F_VLAN;
        if (n < off + 4) {
            malformed(r);
            r->l3 = PPC_L3_OTHER;
            r->info = PPC_INFO_ETHERTYPE;
            r->a = et;
            return n;
        }
        et = rd16(d + off + 2);
        off += 4;
        tags++;
    }
    switch (et) {
    case 0x0806:
        return min_u32(arp(d, n, off, r), n);
    case 0x0800:
        return min_u32(ipv4(d, n, off, r), n);
    case 0x86dd:
        return min_u32(ipv6(d, n, off, r), n);
    default:
        r->l3 = PPC_L3_OTHER;
        r->info = PPC_INFO_ETHERTYPE;
        r->a = et;
        return off;
    }
}

/* ---- what is kept -------------------------------------------------------------------- */

int ppc_is_plumbing(const struct ppc_record *r)
{
    if (r->flags & PPC_F_NONFIRST) {
        return 0;
    }
    if (r->l3 == PPC_L3_ARP || r->info == PPC_INFO_ICMP || r->info == PPC_INFO_ICMP6) {
        return 1;
    }
    switch (r->app) {
    case PPC_APP_DNS:
    case PPC_APP_MDNS:
    case PPC_APP_LLMNR:
    case PPC_APP_DHCP:
    case PPC_APP_DHCP6:
    case PPC_APP_NTP:
    case PPC_APP_NBNS:
        return 1;
    default:
        return 0;
    }
}

uint32_t ppc_store_len(const struct ppc_record *r, uint32_t payload_off, uint32_t cap_len,
                       enum ppc_keep keep)
{
    uint32_t n;

    if (cap_len > 65535u) {
        cap_len = 65535u;
    }
    if (keep == PPC_KEEP_FULL) {
        return cap_len;
    }
    n = payload_off > cap_len ? cap_len : payload_off;
    if (ppc_is_plumbing(r)) {
        n += PPC_PLUMBING_MAX;
    }
    return n < cap_len ? n : cap_len;
}

/* ---- words --------------------------------------------------------------------------- */

static const char *ethertype_word(uint32_t et)
{
    if (et < 0x0600) {
        return "802.3";
    }
    switch (et) {
    case 0x88cc:
        return "LLDP";
    case 0x8899:
        return "RRCP";
    case 0x888e:
        return "EAPOL";
    case 0x8863:
    case 0x8864:
        return "PPPoE";
    case 0x8035:
        return "RARP";
    case 0x0842:
        return "WOL";
    case 0x88f7:
        return "PTP";
    case 0x8100:
    case 0x88a8:
    case 0x9100:
        return "VLAN";
    default:
        return "ETH";
    }
}

static const char *icmp6_word(uint32_t type)
{
    if (type >= 133 && type <= 137) {
        return "NDP";
    }
    if ((type >= 130 && type <= 132) || type == 143) {
        return "MLD";
    }
    return "ICMPv6";
}

const char *ppc_proto_word(const struct ppc_record *r)
{
    static const char *const app_word[] = { "", "DNS", "mDNS", "LLMNR", "DHCP", "DHCPv6", "NTP",
                                            "NBNS", "SSDP" };

    if (r->info == PPC_INFO_RUNT) {
        return "ETH";
    }
    switch (r->l3) {
    case PPC_L3_ARP:
        return "ARP";
    case PPC_L3_OTHER:
        return ethertype_word(r->a);
    case PPC_L3_IPV4:
    case PPC_L3_IPV6:
        break;
    default:
        return "ETH";
    }
    if (r->flags & PPC_F_NONFIRST) {
        return r->l3 == PPC_L3_IPV6 ? "IPv6" : "IPv4";
    }
    if (r->app != PPC_APP_NONE && r->app < sizeof(app_word) / sizeof(app_word[0]) &&
        r->info != PPC_INFO_TCP) {
        return app_word[r->app];
    }
    switch (r->info) {
    case PPC_INFO_TCP:
        return "TCP";
    case PPC_INFO_UDP:
        return "UDP";
    case PPC_INFO_ICMP:
        return "ICMP";
    case PPC_INFO_ICMP6:
        return icmp6_word(r->a >> 8);
    case PPC_INFO_IGMP:
        return "IGMP";
    default:
        break;
    }
    switch (r->l4) {
    case 6:
        return "TCP";
    case 17:
        return "UDP";
    case 47:
        return "GRE";
    case 50:
        return "ESP";
    case 51:
        return "AH";
    case 132:
        return "SCTP";
    default:
        return r->l3 == PPC_L3_IPV6 ? "IPv6" : "IPv4";
    }
}

const char *ppc_class_name(unsigned cls)
{
    static const char *const names[PPC_CLASS_COUNT] = { "TCP", "UDP", "DNS", "DHCP",
                                                        "ARP", "ICMP", "IPV6", "OTHER" };

    return cls < PPC_CLASS_COUNT ? names[cls] : "?";
}
