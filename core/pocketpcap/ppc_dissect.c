/*
 * pocketpcap dissector. See ppc_dissect.h for the rules it keeps.
 *
 * Shape: the decoder's (ppc_decode.c), one pass from the outside in, each
 * layer given where it begins and `end`, the first byte it may not read:
 * the stored length, cut further by the IP length field, so padding is never
 * read as protocol. Unlike the decoder it formats text, and it runs once per
 * inspected packet, not once per frame.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_dissect.h"

#include "ppc_decode.h"
#include "ppc_format.h"
#include "ppc_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ETH_HDR 14u
#define VLAN_TAGS_MAX 2
#define IPV6_EXT_MAX 8
#define DHCP_FIXED 236u
#define DHCP_OPTIONS (DHCP_FIXED + 4u)
#define DHCP_OPTION_FIELDS 6

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

/* ---- the walk ------------------------------------------------------------------------- */

struct walk {
    const struct ppc_record *r;
    const uint8_t *b;
    uint32_t n;      /* stored bytes */
    ppc_ds_cb cb;
    void *ctx;
    int layer;       /* the current layer's index, -1 before the first */
    int items;
};

static void emit(struct walk *w, struct ppc_ds_item *it)
{
    if (w->items >= PPC_DS_ITEMS) {
        return;
    }
    /* Packet-derived words (names, host names, the row's sentence) are
     * already ASCII; the dissector's own words may carry the two marks. */
    ppc_sanitize(it->name);
    ppc_sanitize_marks(it->text);
    ppc_sanitize(it->filter);
    w->items++;
    w->cb(w->ctx, it);
}

/* A new layer. Returns 0, or -1 when there is no room for another (the
 * caller stops). */
static int layer(struct walk *w, const char *name, int cls, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static int layer(struct walk *w, const char *name, int cls, const char *fmt, ...)
{
    struct ppc_ds_item it;
    va_list ap;

    if (w->layer + 1 >= PPC_DS_LAYERS || w->items >= PPC_DS_ITEMS) {
        return -1;
    }
    memset(&it, 0, sizeof(it));
    it.kind = PPC_DS_LAYER;
    it.layer = ++w->layer;
    it.cls = cls;
    it.off = -1;
    snprintf(it.name, sizeof(it.name), "%s", name);
    va_start(ap, fmt);
    vsnprintf(it.text, sizeof(it.text), fmt, ap);
    va_end(ap);
    emit(w, &it);
    return 0;
}

/* A field read from b[off .. off+len): emitted only when those bytes were
 * stored (off < 0: a field derived from the record, always emitted). */
static void field(struct walk *w, const char *name, int32_t off, uint32_t len, const char *filter,
                  const char *fmt, ...) __attribute__((format(printf, 6, 7)));

static void field(struct walk *w, const char *name, int32_t off, uint32_t len, const char *filter,
                  const char *fmt, ...)
{
    struct ppc_ds_item it;
    va_list ap;

    if (w->layer < 0 || (off >= 0 && ((uint32_t)off > w->n || len > w->n - (uint32_t)off))) {
        return;
    }
    memset(&it, 0, sizeof(it));
    it.kind = PPC_DS_FIELD;
    it.layer = w->layer;
    it.cls = -1;
    it.off = off;
    it.len = off >= 0 ? len : 0;
    snprintf(it.name, sizeof(it.name), "%s", name);
    snprintf(it.filter, sizeof(it.filter), "%s", filter ? filter : "");
    va_start(ap, fmt);
    vsnprintf(it.text, sizeof(it.text), fmt, ap);
    va_end(ap);
    emit(w, &it);
}

static int have(const struct walk *w, uint32_t off, uint32_t len)
{
    return off <= w->n && len <= w->n - off;
}

/* Whether the record keeps this MAC as one of its ends (ppc_record.h): an
 * ARP record keeps the sender and target MAC from the ARP message, a frame
 * with no network layer its Ethernet source and destination, an IP packet
 * none. A host filter on any other MAC would match nothing of the packet. */
static int record_keeps_mac(const struct ppc_record *r, const uint8_t *mac)
{
    switch (r->l3) {
    case PPC_L3_ARP:
        return memcmp(r->src + 4, mac, 6) == 0 || memcmp(r->dst + 4, mac, 6) == 0;
    case PPC_L3_NONE:
    case PPC_L3_OTHER:
        return memcmp(r->src, mac, 6) == 0 || memcmp(r->dst, mac, 6) == 0;
    default:
        return 0;
    }
}

/* An address field and the host filter it stands for. A MAC filters only
 * where the record keeps it (record_keeps_mac): an IP packet's Ethernet
 * addresses, and an ARP frame's Ethernet addresses that are not its sender
 * or target MAC (the broadcast destination of a request), offer no filter
 * rather than one that would show nothing of the packet. */
static void addr_field(struct walk *w, const char *name, uint32_t off, unsigned len)
{
    char a[48];
    char f[PPC_DS_FILTER];
    int filters;

    if (!have(w, off, len)) {
        return;
    }
    filters = len != 6 || record_keeps_mac(w->r, w->b + off);
    ppc_format_addr(a, sizeof(a), w->b + off, len);
    snprintf(f, sizeof(f), "host:%s", a);
    field(w, name, (int32_t)off, len, filters ? f : NULL, "%s", a);
}

static void port_field(struct walk *w, const char *name, uint32_t off)
{
    char f[16];
    unsigned p;
    const char *svc;

    if (!have(w, off, 2)) {
        return;
    }
    p = rd16(w->b + off);
    svc = ppc_port_name(p);
    snprintf(f, sizeof(f), "port:%u", p);
    field(w, name, (int32_t)off, 2, p ? f : "", "%u%s%s", p, svc ? " " : "", svc ? svc : "");
}

/* What follows the headers: how much there was, and whether it was kept. */
static void payload_field(struct walk *w, uint32_t pay, uint32_t ip_end)
{
    uint32_t total = ip_end > pay ? ip_end - pay : 0;
    uint32_t kept = w->n > pay ? min_u32(w->n, ip_end) - min_u32(pay, min_u32(w->n, ip_end)) : 0;

    if (total == 0) {
        field(w, "PAYLOAD", -1, 0, NULL, "0 B");
    } else if (kept >= total) {
        field(w, "PAYLOAD", (int32_t)pay, total, NULL, "%u B · stored", (unsigned)total);
    } else if (kept == 0) {
        field(w, "PAYLOAD", -1, 0, NULL, "%u B · not stored", (unsigned)total);
    } else {
        field(w, "PAYLOAD", (int32_t)pay, kept, NULL, "%u B · %u stored", (unsigned)total, (unsigned)kept);
    }
}

const char *ppc_port_name(unsigned port)
{
    switch (port) {
    case 20:
    case 21:
        return "ftp";
    case 22:
        return "ssh";
    case 23:
        return "telnet";
    case 25:
        return "smtp";
    case 53:
        return "domain";
    case 67:
        return "bootps";
    case 68:
        return "bootpc";
    case 80:
        return "http";
    case 110:
        return "pop3";
    case 123:
        return "ntp";
    case 137:
        return "netbios-ns";
    case 143:
        return "imap";
    case 161:
        return "snmp";
    case 443:
        return "https";
    case 546:
        return "dhcpv6-client";
    case 547:
        return "dhcpv6-server";
    case 853:
        return "domain-s";
    case 1883:
        return "mqtt";
    case 1900:
        return "ssdp";
    case 5353:
        return "mdns";
    case 5355:
        return "llmnr";
    case 8080:
        return "http-alt";
    case 10050:
        return "zabbix-agent";
    case 10051:
        return "zabbix-trapper";
    default:
        return NULL;
    }
}

/* ---- application protocols ----------------------------------------------------------- */

static void dns_layer(struct walk *w, uint32_t off, uint32_t end, const char *word, const char *summary)
{
    uint16_t flags;
    unsigned qd;
    unsigned an;
    uint32_t next = 0;
    char name[PPC_NAME_TEXT];
    char tbuf[16];

    if (end < off + 12 || layer(w, word, PPC_CLASS_DNS, "%s", summary) != 0) {
        return;
    }
    flags = rd16(w->b + off + 2);
    qd = rd16(w->b + off + 4);
    an = rd16(w->b + off + 6);
    field(w, "TRANSACTION ID", (int32_t)off, 2, NULL, "0x%04x", rd16(w->b + off));
    {
        const char *rc = ppc_dns_rcode_text(flags & 15u);

        field(w, "FLAGS", (int32_t)off + 2, 2, NULL, "%s%s%s%s", (flags & 0x8000u) ? "response" : "query",
              (flags & 0x0400u) ? " · authoritative" : "", (flags & 0x8000u) ? " · " : "",
              (flags & 0x8000u) ? (rc ? rc : "no error") : "");
    }
    field(w, "QUESTIONS", (int32_t)off + 4, 2, NULL, "%u", qd);
    field(w, "ANSWERS", (int32_t)off + 6, 2, NULL, "%u", an);
    field(w, "AUTHORITY", (int32_t)off + 8, 2, NULL, "%u", rd16(w->b + off + 8));
    field(w, "ADDITIONAL", (int32_t)off + 10, 2, NULL, "%u", rd16(w->b + off + 10));
    if (qd == 0) {
        return;
    }
    /* The first question. */
    if (ppc_dns_name(w->b + off, end - off, 12, name, sizeof(name), &next) != 0 || off + next + 4 > end) {
        return;
    }
    field(w, "QUERY", (int32_t)off + 12, next + 4 - 12, NULL, "%s %s", name[0] ? name : "<root>",
          ppc_dns_type_name(rd16(w->b + off + next), tbuf, sizeof(tbuf)));
    next += 4;
    if (qd != 1 || an == 0) {
        return;
    }
    /* The first answer. */
    {
        uint32_t rr = next;
        uint32_t after = 0;
        unsigned type;
        unsigned rdlen;
        uint32_t rd;

        if (ppc_dns_name(w->b + off, end - off, rr, name, sizeof(name), &after) != 0 || off + after + 10 > end) {
            return;
        }
        type = rd16(w->b + off + after);
        rdlen = rd16(w->b + off + after + 8);
        rd = after + 10;
        if (off + rd + rdlen > end) {
            return;
        }
        if (type == 1 && rdlen == 4) {
            char a[48];

            ppc_format_addr(a, sizeof(a), w->b + off + rd, 4);
            field(w, "ANSWER", (int32_t)(off + rr), rd + rdlen - rr, NULL, "A %s · TTL %u", a,
                  (unsigned)rd32(w->b + off + after + 4));
        } else if (type == 28 && rdlen == 16) {
            char a[48];

            ppc_format_addr(a, sizeof(a), w->b + off + rd, 16);
            field(w, "ANSWER", (int32_t)(off + rr), rd + rdlen - rr, NULL, "AAAA %s", a);
        } else if (type == 5 || type == 12 || type == 2) {
            char target[PPC_NAME_TEXT];

            if (ppc_dns_name(w->b + off, end - off, rd, target, sizeof(target), NULL) == 0) {
                field(w, "ANSWER", (int32_t)(off + rr), rd + rdlen - rr, NULL, "%s %s",
                      ppc_dns_type_name(type, tbuf, sizeof(tbuf)), target);
            }
        } else {
            field(w, "ANSWER", (int32_t)(off + rr), rd + rdlen - rr, NULL, "%s · %u bytes",
                  ppc_dns_type_name(type, tbuf, sizeof(tbuf)), rdlen);
        }
    }
}

static void dhcp_layer(struct walk *w, uint32_t off, uint32_t end, const char *summary)
{
    uint32_t i;
    int extra = 0;

    if (end < off + DHCP_FIXED || layer(w, "DHCP", PPC_CLASS_DHCP, "%s", summary) != 0) {
        return;
    }
    field(w, "OPERATION", (int32_t)off, 1, NULL, "%u %s", w->b[off],
          w->b[off] == 1 ? "request" : w->b[off] == 2 ? "reply" : "");
    field(w, "TRANSACTION ID", (int32_t)off + 4, 4, NULL, "0x%08x", (unsigned)rd32(w->b + off + 4));
    addr_field(w, "CLIENT IP", off + 12, 4);
    addr_field(w, "YOUR IP", off + 16, 4);
    addr_field(w, "SERVER IP", off + 20, 4);
    if (w->b[off + 1] == 1 && w->b[off + 2] == 6) {
        addr_field(w, "CLIENT MAC", off + 28, 6);
    }
    if (end < off + DHCP_OPTIONS || rd32(w->b + off + DHCP_FIXED) != 0x63825363u) {
        return;
    }
    i = off + DHCP_OPTIONS;
    while (i < end && extra < DHCP_OPTION_FIELDS) {
        uint8_t code = w->b[i];
        uint32_t len;

        if (code == 0) {
            i++;
            continue;
        }
        if (code == 255 || i + 1 >= end) {
            break;
        }
        len = w->b[i + 1];
        if (i + 2 + len > end) {
            break;
        }
        switch (code) {
        case 53:
            if (len >= 1) {
                const char *t = ppc_dhcp_type_text(w->b[i + 2]);

                field(w, "MESSAGE TYPE", (int32_t)i, 2 + len, NULL, "%u %s", w->b[i + 2], t ? t : "");
                extra++;
            }
            break;
        case 12: {
            char host[64];
            uint32_t k = len < sizeof(host) - 1 ? len : (uint32_t)sizeof(host) - 1;

            memcpy(host, w->b + i + 2, k);
            host[k] = '\0';
            ppc_sanitize(host);
            field(w, "HOST NAME", (int32_t)i, 2 + len, NULL, "%s", host);
            extra++;
            break;
        }
        case 50:
            if (len == 4) {
                addr_field(w, "REQUESTED IP", i + 2, 4);
                extra++;
            }
            break;
        case 54:
            if (len == 4) {
                addr_field(w, "SERVER ID", i + 2, 4);
                extra++;
            }
            break;
        case 51:
            if (len == 4) {
                field(w, "LEASE TIME", (int32_t)i, 2 + len, NULL, "%u s", (unsigned)rd32(w->b + i + 2));
                extra++;
            }
            break;
        default:
            break;
        }
        i += 2u + len;
    }
}

static void dhcp6_layer(struct walk *w, uint32_t off, uint32_t end, const char *summary)
{
    const char *t;

    if (end < off + 4 || layer(w, "DHCPv6", PPC_CLASS_DHCP, "%s", summary) != 0) {
        return;
    }
    t = ppc_dhcp6_type_text(w->b[off]);
    field(w, "MESSAGE TYPE", (int32_t)off, 1, NULL, "%u %s", w->b[off], t ? t : "");
    field(w, "TRANSACTION ID", (int32_t)off + 1, 3, NULL, "0x%06x",
          (unsigned)((uint32_t)w->b[off + 1] << 16 | (uint32_t)w->b[off + 2] << 8 | w->b[off + 3]));
}

static void ntp_layer(struct walk *w, uint32_t off, uint32_t end, const char *summary)
{
    if (end < off + 4 || layer(w, "NTP", PPC_CLASS_UDP, "%s", summary) != 0) {
        return;
    }
    field(w, "VERSION", (int32_t)off, 1, NULL, "%u", (unsigned)((w->b[off] >> 3) & 7u));
    field(w, "MODE", (int32_t)off, 1, NULL, "%u %s", (unsigned)(w->b[off] & 7u), ppc_ntp_mode_text(w->b[off] & 7u));
    field(w, "STRATUM", (int32_t)off + 1, 1, NULL, "%u", w->b[off + 1]);
    field(w, "POLL", (int32_t)off + 2, 1, NULL, "%d", (int)(int8_t)w->b[off + 2]);
}

/* ---- transport ------------------------------------------------------------------------ */

static void tcp_options(struct walk *w, uint32_t off, uint32_t len)
{
    char text[PPC_DS_TEXT];
    size_t o = 0;
    uint32_t i = 0;

    text[0] = '\0';
    while (i < len && o + 16 < sizeof(text)) {
        uint8_t kind = w->b[off + i];
        uint32_t olen;
        int k = 0;

        if (kind == 0) {
            k = snprintf(text + o, sizeof(text) - o, "%sEOL", o ? " " : "");
            o += (size_t)(k > 0 ? k : 0);
            break;
        }
        if (kind == 1) {
            k = snprintf(text + o, sizeof(text) - o, "%sNOP", o ? " " : "");
            o += (size_t)(k > 0 ? k : 0);
            i++;
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        olen = w->b[off + i + 1];
        if (olen < 2 || i + olen > len) {
            k = snprintf(text + o, sizeof(text) - o, "%s?", o ? " " : "");
            o += (size_t)(k > 0 ? k : 0);
            break;
        }
        switch (kind) {
        case 2:
            k = olen == 4 ? snprintf(text + o, sizeof(text) - o, "%sMSS %u", o ? " " : "", rd16(w->b + off + i + 2))
                          : snprintf(text + o, sizeof(text) - o, "%sMSS", o ? " " : "");
            break;
        case 3:
            k = olen == 3 ? snprintf(text + o, sizeof(text) - o, "%sWS %u", o ? " " : "", w->b[off + i + 2])
                          : snprintf(text + o, sizeof(text) - o, "%sWS", o ? " " : "");
            break;
        case 4:
            k = snprintf(text + o, sizeof(text) - o, "%sSACKOK", o ? " " : "");
            break;
        case 5:
            k = snprintf(text + o, sizeof(text) - o, "%sSACK", o ? " " : "");
            break;
        case 8:
            k = snprintf(text + o, sizeof(text) - o, "%sTS", o ? " " : "");
            break;
        default:
            k = snprintf(text + o, sizeof(text) - o, "%s#%u", o ? " " : "", kind);
            break;
        }
        o += (size_t)(k > 0 ? k : 0);
        if (o >= sizeof(text)) {
            o = sizeof(text) - 1;
        }
        i += olen;
    }
    field(w, "OPTIONS", (int32_t)off, len, NULL, "%s", text[0] ? text : "-");
}

static void tcp_layer(struct walk *w, uint32_t off, uint32_t end, uint32_t ip_end)
{
    uint32_t doff;
    char flags[48];
    char spaced[48];
    size_t i;

    if (!have(w, off, 14)) {
        return;
    }
    ppc_tcp_flags_text(w->b[off + 13] & 0x3fu, flags, sizeof(flags));
    /* "SYN, ACK" as a field; "SYN ACK" in the summary, as the row has it. */
    for (i = 0; i < sizeof(spaced) - 1 && flags[i]; i++) {
        spaced[i] = flags[i];
    }
    spaced[i] = '\0';
    {
        char *c;

        while ((c = strstr(spaced, ", ")) != NULL) {
            memmove(c, c + 1, strlen(c + 1) + 1);
        }
    }
    if (layer(w, "TCP", PPC_CLASS_TCP, "%u \xe2\x86\x92 %u%s%s", rd16(w->b + off), rd16(w->b + off + 2),
              spaced[0] ? " · " : "", spaced) != 0) {
        return;
    }
    port_field(w, "SOURCE PORT", off);
    port_field(w, "DESTINATION PORT", off + 2);
    field(w, "SEQUENCE", (int32_t)off + 4, 4, NULL, "%u", (unsigned)rd32(w->b + off + 4));
    field(w, "ACKNOWLEDGMENT", (int32_t)off + 8, 4, NULL, "%u", (unsigned)rd32(w->b + off + 8));
    doff = (uint32_t)(w->b[off + 12] >> 4) * 4u;
    field(w, "HEADER LENGTH", (int32_t)off + 12, 1, NULL, "%u bytes", (unsigned)doff);
    field(w, "FLAGS", (int32_t)off + 13, 1, NULL, "%s", flags[0] ? flags : "none");
    if (have(w, off, 20)) {
        field(w, "WINDOW", (int32_t)off + 14, 2, NULL, "%u", rd16(w->b + off + 14));
        field(w, "CHECKSUM", (int32_t)off + 16, 2, NULL, "0x%04x", rd16(w->b + off + 16));
        if (w->b[off + 13] & PPC_TCP_URG) {
            field(w, "URGENT POINTER", (int32_t)off + 18, 2, NULL, "%u", rd16(w->b + off + 18));
        }
    }
    if (doff < 20 || off + doff > end) {
        return;
    }
    if (doff > 20 && have(w, off + 20, doff - 20)) {
        tcp_options(w, off + 20, doff - 20);
    }
    payload_field(w, off + doff, ip_end);
}

static void udp_layer(struct walk *w, uint32_t off, uint32_t end, uint32_t ip_end)
{
    struct ppc_row_text t;
    uint32_t pay = off + 8;
    unsigned sp;
    unsigned dp;

    if (!have(w, off, 8) || end < off + 8) {
        return;
    }
    sp = rd16(w->b + off);
    dp = rd16(w->b + off + 2);
    if (layer(w, "UDP", PPC_CLASS_UDP, "%u \xe2\x86\x92 %u · %u B", sp, dp, rd16(w->b + off + 4)) != 0) {
        return;
    }
    port_field(w, "SOURCE PORT", off);
    port_field(w, "DESTINATION PORT", off + 2);
    field(w, "LENGTH", (int32_t)off + 4, 2, NULL, "%u", rd16(w->b + off + 4));
    field(w, "CHECKSUM", (int32_t)off + 6, 2, NULL, "0x%04x", rd16(w->b + off + 6));
    payload_field(w, pay, ip_end);
    if (w->r->flags & PPC_F_NONFIRST) {
        return;
    }
    /* The application layer's summary is the row's sentence. */
    ppc_format_row(w->r, w->b, w->n, &t);
    switch (w->r->app) {
    case PPC_APP_DNS:
        dns_layer(w, pay, end, "DNS", t.info);
        break;
    case PPC_APP_MDNS:
        dns_layer(w, pay, end, "mDNS", t.info);
        break;
    case PPC_APP_LLMNR:
        dns_layer(w, pay, end, "LLMNR", t.info);
        break;
    case PPC_APP_NBNS:
        dns_layer(w, pay, end, "NBNS", t.info);
        break;
    case PPC_APP_DHCP:
        dhcp_layer(w, pay, end, t.info);
        break;
    case PPC_APP_DHCP6:
        dhcp6_layer(w, pay, end, t.info);
        break;
    case PPC_APP_NTP:
        ntp_layer(w, pay, end, t.info);
        break;
    default:
        break;
    }
}

static void icmp_layer(struct walk *w, uint32_t off, uint32_t end, int v6)
{
    unsigned type;
    unsigned code;
    const char *t;

    if (!have(w, off, 4) || end < off + 4) {
        return;
    }
    type = w->b[off];
    code = w->b[off + 1];
    t = v6 ? ppc_icmp6_text(type) : ppc_icmp_text(type, code);
    if (layer(w, v6 ? "ICMPv6" : "ICMP", v6 ? PPC_CLASS_IPV6 : PPC_CLASS_ICMP, "%s", t ? t : "type") != 0) {
        return;
    }
    field(w, "TYPE", (int32_t)off, 1, NULL, "%u%s%s", type, t ? " " : "", t ? t : "");
    field(w, "CODE", (int32_t)off + 1, 1, NULL, "%u", code);
    field(w, "CHECKSUM", (int32_t)off + 2, 2, NULL, "0x%04x", rd16(w->b + off + 2));
    if (end >= off + 8 && ((!v6 && (type == 0 || type == 8)) || (v6 && (type == 128 || type == 129)))) {
        field(w, "IDENTIFIER", (int32_t)off + 4, 2, NULL, "%u", rd16(w->b + off + 4));
        field(w, "SEQUENCE", (int32_t)off + 6, 2, NULL, "%u", rd16(w->b + off + 6));
    }
    if (v6 && (type == 135 || type == 136 || type == 137) && end >= off + 24) {
        addr_field(w, "TARGET", off + 8, 16);
    }
}

static void igmp_layer(struct walk *w, uint32_t off, uint32_t end)
{
    const char *t;

    if (!have(w, off, 1) || end < off + 1) {
        return;
    }
    t = ppc_igmp_text(w->b[off]);
    if (layer(w, "IGMP", PPC_CLASS_OTHER, "%s", t ? t : "type") != 0) {
        return;
    }
    field(w, "TYPE", (int32_t)off, 1, NULL, "0x%02x%s%s", w->b[off], t ? " " : "", t ? t : "");
    if (end >= off + 8) {
        field(w, "CHECKSUM", (int32_t)off + 2, 2, NULL, "0x%04x", rd16(w->b + off + 2));
        addr_field(w, "GROUP", off + 4, 4);
    }
}

static const char *ip_proto_name(unsigned p)
{
    switch (p) {
    case 1:
        return "ICMP";
    case 2:
        return "IGMP";
    case 6:
        return "TCP";
    case 17:
        return "UDP";
    case 41:
        return "IPv6";
    case 47:
        return "GRE";
    case 50:
        return "ESP";
    case 51:
        return "AH";
    case 58:
        return "ICMPv6";
    case 132:
        return "SCTP";
    default:
        return "";
    }
}

static void transport(struct walk *w, uint32_t off, uint32_t end, uint32_t ip_end, uint8_t proto, int v6)
{
    switch (proto) {
    case 6:
        tcp_layer(w, off, end, ip_end);
        break;
    case 17:
        udp_layer(w, off, end, ip_end);
        break;
    case 1:
        if (!v6) {
            icmp_layer(w, off, end, 0);
        }
        break;
    case 58:
        if (v6) {
            icmp_layer(w, off, end, 1);
        }
        break;
    case 2:
        igmp_layer(w, off, end);
        break;
    default:
        break;
    }
}

/* ---- network ------------------------------------------------------------------------- */

static void arp_layer(struct walk *w, uint32_t off)
{
    unsigned op;

    if (!have(w, off, 8)) {
        return;
    }
    op = rd16(w->b + off + 6);
    if (layer(w, "ARP", PPC_CLASS_ARP, "%s", op == 1 ? "REQUEST" : op == 2 ? "REPLY" : "OPERATION") != 0) {
        return;
    }
    field(w, "OPERATION", (int32_t)off + 6, 2, NULL, "%u %s", op, op == 1 ? "request" : op == 2 ? "reply" : "");
    if (rd16(w->b + off) == 1 && rd16(w->b + off + 2) == 0x0800 && w->b[off + 4] == 6 && w->b[off + 5] == 4) {
        addr_field(w, "SENDER MAC", off + 8, 6);
        addr_field(w, "SENDER IP", off + 14, 4);
        addr_field(w, "TARGET MAC", off + 18, 6);
        addr_field(w, "TARGET IP", off + 24, 4);
    } else {
        field(w, "HARDWARE TYPE", (int32_t)off, 2, NULL, "%u", rd16(w->b + off));
        field(w, "PROTOCOL TYPE", (int32_t)off + 2, 2, NULL, "0x%04x", rd16(w->b + off + 2));
    }
}

static void ipv4_layer(struct walk *w, uint32_t off)
{
    const uint8_t *p = w->b + off;
    uint32_t ihl;
    uint32_t total;
    uint32_t end;
    uint32_t ip_end;
    uint16_t frag;
    char fl[16];
    char cls[16];

    if (!have(w, off, 20) || (p[0] >> 4) != 4) {
        return;
    }
    ihl = (uint32_t)(p[0] & 15u) * 4u;
    frag = rd16(p + 6);
    snprintf(fl, sizeof(fl), "%s%s%s", (frag & 0x4000u) ? "DF" : "", (frag & 0x4000u) && (frag & 0x2000u) ? " " : "",
             (frag & 0x2000u) ? "MF" : "");
    if (layer(w, "IPv4", -1, "TTL %u%s%s · ID 0x%04X", p[8], fl[0] ? " · " : "", fl, rd16(p + 4)) != 0) {
        return;
    }
    field(w, "VERSION", (int32_t)off, 1, NULL, "4 · header %u bytes", (unsigned)ihl);
    field(w, "DSCP · ECN", (int32_t)off + 1, 1, NULL, "%u · %u", (unsigned)(p[1] >> 2), (unsigned)(p[1] & 3u));
    field(w, "TOTAL LENGTH", (int32_t)off + 2, 2, NULL, "%u", rd16(p + 2));
    field(w, "IDENTIFICATION", (int32_t)off + 4, 2, NULL, "0x%04x", rd16(p + 4));
    field(w, "FLAGS", (int32_t)off + 6, 1, NULL, "%s", fl[0] ? fl : "none");
    field(w, "FRAGMENT OFFSET", (int32_t)off + 6, 2, NULL, "%u", (unsigned)(frag & 0x1fffu) * 8u);
    field(w, "TTL", (int32_t)off + 8, 1, NULL, "%u", p[8]);
    snprintf(cls, sizeof(cls), "class:%u", (unsigned)w->r->cls);
    field(w, "PROTOCOL", (int32_t)off + 9, 1, p[9] == 6 || p[9] == 17 || p[9] == 1 ? cls : NULL, "%u %s", p[9],
          ip_proto_name(p[9]));
    field(w, "CHECKSUM", (int32_t)off + 10, 2, NULL, "0x%04x", rd16(p + 10));
    addr_field(w, "SOURCE", off + 12, 4);
    addr_field(w, "DESTINATION", off + 16, 4);
    if (ihl < 20) {
        return;
    }
    if (ihl > 20 && have(w, off + 20, ihl - 20)) {
        field(w, "OPTIONS", (int32_t)off + 20, ihl - 20, NULL, "%u bytes", (unsigned)(ihl - 20));
    }
    total = rd16(p + 2);
    ip_end = off + (total < ihl ? ihl : total);
    end = min_u32(ip_end, w->n);
    if (frag & 0x1fffu) {
        if (layer(w, "Fragment", -1, "OFFSET %u", (unsigned)(frag & 0x1fffu) * 8u) == 0) {
            field(w, "OFFSET", (int32_t)off + 6, 2, NULL, "%u bytes", (unsigned)(frag & 0x1fffu) * 8u);
            payload_field(w, off + ihl, ip_end);
        }
        return;
    }
    transport(w, off + ihl, end, ip_end, p[9], 0);
}

static const char *ext_name(unsigned nh)
{
    switch (nh) {
    case 0:
        return "Hop-by-hop";
    case 43:
        return "Routing";
    case 44:
        return "Fragment";
    case 60:
        return "Destination options";
    default:
        return "?";
    }
}

static void ipv6_layer(struct walk *w, uint32_t off)
{
    const uint8_t *p = w->b + off;
    uint32_t end;
    uint32_t ip_end;
    uint32_t h;
    uint8_t nh;
    char cls[16];
    int i;

    if (!have(w, off, 40) || (p[0] >> 4) != 6) {
        return;
    }
    if (layer(w, "IPv6", -1, "HOP LIMIT %u · NEXT %u", p[7], p[6]) != 0) {
        return;
    }
    field(w, "TRAFFIC CLASS", (int32_t)off, 2, NULL, "0x%02x", (unsigned)((rd16(p) >> 4) & 0xffu));
    field(w, "FLOW LABEL", (int32_t)off + 1, 3, NULL, "0x%05x", (unsigned)(rd32(p) & 0xfffffu));
    field(w, "PAYLOAD LENGTH", (int32_t)off + 4, 2, NULL, "%u", rd16(p + 4));
    snprintf(cls, sizeof(cls), "class:%u", (unsigned)w->r->cls);
    field(w, "NEXT HEADER", (int32_t)off + 6, 1, p[6] == 6 || p[6] == 17 || p[6] == 58 ? cls : NULL, "%u %s", p[6],
          ip_proto_name(p[6]));
    field(w, "HOP LIMIT", (int32_t)off + 7, 1, NULL, "%u", p[7]);
    addr_field(w, "SOURCE", off + 8, 16);
    addr_field(w, "DESTINATION", off + 24, 16);
    ip_end = off + 40u + rd16(p + 4);
    if (rd16(p + 4) == 0) {
        ip_end = w->n; /* jumbo or unknown: what was stored */
    }
    end = min_u32(ip_end, w->n);
    nh = p[6];
    h = off + 40;
    for (i = 0; i < IPV6_EXT_MAX && (nh == 0 || nh == 43 || nh == 44 || nh == 60); i++) {
        if (h + 8 > end) {
            return;
        }
        if (nh == 44) {
            uint32_t fo = rd16(w->b + h + 2) & 0xfff8u;

            field(w, "EXTENSION", (int32_t)h, 8, NULL, "Fragment · offset %u%s", (unsigned)fo,
                  (w->b[h + 3] & 1u) ? " · more" : "");
            nh = w->b[h];
            h += 8;
            if (fo) {
                return;
            }
        } else {
            uint32_t len = ((uint32_t)w->b[h + 1] + 1u) * 8u;

            if (h + len > end) {
                return;
            }
            field(w, "EXTENSION", (int32_t)h, len, NULL, "%s · %u bytes", ext_name(nh), (unsigned)len);
            nh = w->b[h];
            h += len;
        }
    }
    transport(w, h, end, ip_end, nh, 1);
}

/* ---- the frame ------------------------------------------------------------------------ */

static const char *dir_text(const struct ppc_record *r)
{
    if (r->flags & PPC_F_OUT) {
        return "OUT, sent by this device";
    }
    if (r->flags & PPC_F_BROADCAST) {
        return "BROADCAST";
    }
    if (r->flags & PPC_F_MULTICAST) {
        return "MULTICAST";
    }
    return "IN";
}

static void frame_layer(struct walk *w)
{
    const struct ppc_record *r = w->r;
    char notes[96];
    char cls[16];

    if (layer(w, "Frame", -1, "%u B · %s", (unsigned)r->wire_len,
              r->info == PPC_INFO_RUNT ? "RUNT" : (r->flags & PPC_F_VLAN) ? "802.1Q" : "ETHERNET II") != 0) {
        return;
    }
    field(w, "ON THE WIRE", -1, 0, NULL, "%u bytes", (unsigned)r->wire_len);
    if (w->n == 0 && r->cap_len > 0) {
        field(w, "STORED", -1, 0, NULL, "summary only · bytes left the buffer");
    } else {
        field(w, "STORED", -1, 0, NULL, "%u bytes", (unsigned)w->n);
    }
    field(w, "DIRECTION", -1, 0, NULL, "%s", dir_text(r));
    snprintf(cls, sizeof(cls), "class:%u", (unsigned)r->cls);
    field(w, "PROTOCOL", -1, 0, r->cls < PPC_CLASS_COUNT ? cls : NULL, "%s · %s", ppc_proto_word(r),
          ppc_class_name(r->cls));
    snprintf(notes, sizeof(notes), "%s%s%s%s", (r->flags & PPC_F_TRUNCATED) ? "TRUNCATED " : "",
             (r->flags & PPC_F_GRO) ? "GRO " : "", (r->flags & PPC_F_MALFORMED) ? "MALFORMED " : "",
             (r->flags & PPC_F_FRAGMENT) ? "FRAGMENT " : "");
    if (notes[0]) {
        notes[strlen(notes) - 1] = '\0';
        field(w, "NOTES", -1, 0, NULL, "%s", notes);
    }
}

void ppc_dissect(const struct ppc_record *r, const uint8_t *b, uint32_t n, ppc_ds_cb cb, void *ctx)
{
    struct walk w;
    uint32_t off = ETH_HDR;
    uint16_t et;
    int tags = 0;
    char d[48];
    char s[48];

    if (!r || !cb) {
        return;
    }
    memset(&w, 0, sizeof(w));
    w.r = r;
    w.b = b;
    w.n = b ? n : 0;
    w.cb = cb;
    w.ctx = ctx;
    w.layer = -1;
    frame_layer(&w);
    if (w.n < ETH_HDR) {
        return;
    }
    ppc_format_addr(d, sizeof(d), w.b, 6);
    ppc_format_addr(s, sizeof(s), w.b + 6, 6);
    if (layer(&w, "Ethernet", -1, "%s \xe2\x86\x92 %s", s, d) != 0) {
        return;
    }
    addr_field(&w, "DESTINATION", 0, 6);
    addr_field(&w, "SOURCE", 6, 6);
    et = rd16(w.b + 12);
    while ((et == 0x8100 || et == 0x88a8 || et == 0x9100) && tags < VLAN_TAGS_MAX) {
        uint16_t tci;

        if (!have(&w, off, 4)) {
            return;
        }
        tci = rd16(w.b + off);
        field(&w, "VLAN", (int32_t)off, 2, NULL, "ID %u · PRIORITY %u", (unsigned)(tci & 0x0fffu), (unsigned)(tci >> 13));
        et = rd16(w.b + off + 2);
        off += 4;
        tags++;
    }
    if (et < 0x0600) {
        field(&w, "LENGTH", (int32_t)off - 2, 2, NULL, "%u (802.3)", et);
        return;
    }
    field(&w, "TYPE", (int32_t)off - 2, 2, et == 0x0806 ? "class:4" : NULL, "0x%04x%s", et,
          et == 0x0800   ? " IPv4"
          : et == 0x86dd ? " IPv6"
          : et == 0x0806 ? " ARP"
                         : "");
    switch (et) {
    case 0x0806:
        arp_layer(&w, off);
        break;
    case 0x0800:
        ipv4_layer(&w, off);
        break;
    case 0x86dd:
        ipv6_layer(&w, off);
        break;
    default:
        break;
    }
}

/* ---- the ends and the hex view ------------------------------------------------------- */

static void end_of(const struct ppc_record *r, int dst, struct ppc_endpoint *e)
{
    const uint8_t *a = dst ? r->dst : r->src;
    unsigned port = dst ? r->dport : r->sport;
    const char *svc;

    memset(e, 0, sizeof(*e));
    switch (r->l3) {
    case PPC_L3_IPV4:
        ppc_format_addr(e->addr, sizeof(e->addr), a, 4);
        break;
    case PPC_L3_IPV6:
        ppc_format_addr(e->addr, sizeof(e->addr), a, 16);
        break;
    case PPC_L3_ARP:
        ppc_format_addr(e->addr, sizeof(e->addr), a, 4);
        ppc_format_addr(e->sub, sizeof(e->sub), a + 4, 6);
        {
            char mac[48];

            snprintf(mac, sizeof(mac), "%s", e->sub);
            snprintf(e->sub, sizeof(e->sub), "MAC %.40s", mac);
        }
        break;
    default:
        ppc_format_addr(e->addr, sizeof(e->addr), a, 6);
        break;
    }
    if (!dst && r->l3 != PPC_L3_ARP && r->l3 != PPC_L3_IPV4 && r->l3 != PPC_L3_IPV6) {
        snprintf(e->sub, sizeof(e->sub), "MAC");
    }
    if ((r->l3 == PPC_L3_IPV4 || r->l3 == PPC_L3_IPV6) && (r->l4 == 6 || r->l4 == 17) &&
        !(r->flags & PPC_F_NONFIRST) && (r->info == PPC_INFO_TCP || r->info == PPC_INFO_UDP || r->app != PPC_APP_NONE)) {
        svc = ppc_port_name(port);
        snprintf(e->sub, sizeof(e->sub), "PORT %u%s%s", port, svc ? " · " : "", svc ? svc : "");
        {
            char *c;

            for (c = e->sub; *c; c++) {
                if (*c >= 'a' && *c <= 'z') {
                    *c = (char)(*c - 'a' + 'A');
                }
            }
        }
    }
    snprintf(e->filter, sizeof(e->filter), "host:%s", e->addr);
    ppc_sanitize(e->addr);
    ppc_sanitize_marks(e->sub);
    ppc_sanitize(e->filter);
}

void ppc_endpoints(const struct ppc_record *r, struct ppc_endpoint *src, struct ppc_endpoint *dst)
{
    end_of(r, 0, src);
    end_of(r, 1, dst);
}

void ppc_hex_line(const uint8_t *b, uint32_t n, uint32_t off, char *hex, size_t hexlen, char *ascii,
                  size_t asciilen)
{
    size_t o = 0;
    uint32_t j;

    if (hexlen) {
        hex[0] = '\0';
    }
    if (asciilen) {
        ascii[0] = '\0';
    }
    for (j = 0; j < PPC_HEX_PER_LINE && off + j < n; j++) {
        uint8_t c = b[off + j];
        int k = snprintf(hex + o, hexlen > o ? hexlen - o : 0, "%s%02x", j == 0 ? "" : j == 4 ? "  " : " ", c);

        if (k < 0 || (size_t)k >= hexlen - o) {
            break;
        }
        o += (size_t)k;
        if (j + 1 < asciilen) {
            ascii[j] = c >= 0x20 && c < 0x7f ? (char)c : '.';
            ascii[j + 1] = '\0';
        }
    }
}
