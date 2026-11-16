/*
 * pocketpcap row text. See ppc_format.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_format.h"

#include "ppc_decode.h"
#include "ppc_proto.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define DNS_LABELS_MAX 127
#define DNS_HOPS_MAX 16

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

void ppc_format_addr(char *out, size_t outlen, const uint8_t *a, unsigned len)
{
    if (!out || outlen == 0) {
        return;
    }
    switch (len) {
    case 4:
        snprintf(out, outlen, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        break;
    case 6:
        snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
        break;
    case 16: {
        char buf[INET6_ADDRSTRLEN];

        if (inet_ntop(AF_INET6, a, buf, sizeof(buf))) {
            snprintf(out, outlen, "%s", buf);
        } else {
            snprintf(out, outlen, "?");
        }
        break;
    }
    default:
        snprintf(out, outlen, "-");
        break;
    }
}

const char *ppc_dns_type_name(unsigned type, char *buf, size_t buflen)
{
    switch (type) {
    case 1:
        return "A";
    case 2:
        return "NS";
    case 5:
        return "CNAME";
    case 6:
        return "SOA";
    case 12:
        return "PTR";
    case 15:
        return "MX";
    case 16:
        return "TXT";
    case 28:
        return "AAAA";
    case 32:
        return "NB";
    case 33:
        return "SRV";
    case 47:
        return "NSEC";
    case 64:
        return "SVCB";
    case 65:
        return "HTTPS";
    case 255:
        return "ANY";
    default:
        snprintf(buf, buflen, "TYPE%u", type);
        return buf;
    }
}

int ppc_dns_name(const uint8_t *msg, uint32_t len, uint32_t off, char *out, size_t outlen,
                 uint32_t *next)
{
    size_t o = 0;
    uint32_t p = off;
    int hops = 0;
    int labels = 0;
    int jumped = 0;
    int rc = -1;

    if (!out || outlen == 0) {
        return -1;
    }
    out[0] = '\0';
    while (p < len && labels <= DNS_LABELS_MAX) {
        uint8_t l = msg[p];

        if (l == 0) {
            if (!jumped && next) {
                *next = p + 1;
            }
            rc = 0;
            break;
        }
        if ((l & 0xc0u) == 0xc0u) {
            uint32_t target;

            if (p + 1 >= len || ++hops > DNS_HOPS_MAX) {
                break;
            }
            target = (uint32_t)(l & 0x3fu) << 8 | msg[p + 1];
            if (!jumped && next) {
                *next = p + 2;
            }
            jumped = 1;
            /* A pointer has to go backwards: a forward or self pointer can
             * only make a loop the hop limit would then have to stop. */
            if (target >= p) {
                break;
            }
            p = target;
            continue;
        }
        if ((l & 0xc0u) != 0 || p + 1u + l > len) {
            break;
        }
        if (o > 0 && o + 1 < outlen) {
            out[o++] = '.';
        }
        {
            uint32_t i;

            for (i = 0; i < l && o + 1 < outlen; i++) {
                out[o++] = (char)msg[p + 1 + i];
            }
        }
        out[o] = '\0';
        p += 1u + l;
        labels++;
    }
    if (o < outlen) {
        out[o] = '\0';
    }
    ppc_sanitize(out);
    if (rc == 0 && o == 0) {
        snprintf(out, outlen, "<Root>");
    }
    return rc;
}

/* ---- sentences ------------------------------------------------------------------- */

static void tcp_flags_text(uint8_t f, char *out, size_t outlen)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = { { PPC_TCP_SYN, "SYN" }, { PPC_TCP_FIN, "FIN" }, { PPC_TCP_RST, "RST" },
                  { PPC_TCP_PSH, "PSH" }, { PPC_TCP_ACK, "ACK" }, { PPC_TCP_URG, "URG" } };
    size_t i;
    size_t o = 0;

    out[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (f & names[i].bit) {
            int n = snprintf(out + o, outlen - o, "%s%s", o ? ", " : "", names[i].name);

            if (n < 0 || (size_t)n >= outlen - o) {
                break;
            }
            o += (size_t)n;
        }
    }
}

static const char *icmp_text(unsigned type, unsigned code)
{
    static const char *const unreach[] = { "network unreachable", "host unreachable",
                                           "protocol unreachable", "port unreachable",
                                           "fragmentation needed", "source route failed" };

    switch (type) {
    case 0:
        return "Echo reply";
    case 8:
        return "Echo request";
    case 3:
        return code < sizeof(unreach) / sizeof(unreach[0]) ? unreach[code] : "Destination unreachable";
    case 5:
        return "Redirect";
    case 11:
        return "Time exceeded";
    case 12:
        return "Parameter problem";
    case 13:
        return "Timestamp request";
    case 14:
        return "Timestamp reply";
    default:
        return NULL;
    }
}

static const char *icmp6_text(unsigned type)
{
    switch (type) {
    case 1:
        return "Destination unreachable";
    case 2:
        return "Packet too big";
    case 3:
        return "Time exceeded";
    case 4:
        return "Parameter problem";
    case 128:
        return "Echo request";
    case 129:
        return "Echo reply";
    case 130:
        return "Multicast listener query";
    case 131:
        return "Multicast listener report";
    case 132:
        return "Multicast listener done";
    case 133:
        return "Router solicitation";
    case 134:
        return "Router advertisement";
    case 135:
        return "Neighbor solicitation";
    case 136:
        return "Neighbor advertisement";
    case 137:
        return "Redirect";
    case 143:
        return "Multicast listener report v2";
    default:
        return NULL;
    }
}

static const char *igmp_text(unsigned type)
{
    switch (type) {
    case 0x11:
        return "Membership query";
    case 0x12:
        return "Membership report v1";
    case 0x16:
        return "Membership report v2";
    case 0x17:
        return "Leave group";
    case 0x22:
        return "Membership report v3";
    default:
        return NULL;
    }
}

static const char *dhcp_type_text(unsigned type)
{
    static const char *const names[] = { NULL,      "Discover", "Offer",   "Request", "Decline",
                                         "ACK",     "NAK",      "Release", "Inform" };

    return type < sizeof(names) / sizeof(names[0]) ? names[type] : NULL;
}

static const char *dhcp6_type_text(unsigned type)
{
    static const char *const names[] = { NULL,           "Solicit",  "Advertise", "Request",
                                         "Confirm",      "Renew",    "Rebind",    "Reply",
                                         "Release",      "Decline",  "Reconfigure",
                                         "Information-request", "Relay-forward", "Relay-reply" };

    return type < sizeof(names) / sizeof(names[0]) ? names[type] : NULL;
}

static const char *ntp_mode_text(unsigned mode)
{
    static const char *const names[] = { "reserved",  "symmetric active", "symmetric passive",
                                         "client",    "server",           "broadcast",
                                         "control",   "private" };

    return names[mode & 7u];
}

static const char *dns_rcode_text(unsigned rcode)
{
    switch (rcode) {
    case 1:
        return "Format error";
    case 2:
        return "Server failure";
    case 3:
        return "No such name";
    case 4:
        return "Not implemented";
    case 5:
        return "Refused";
    default:
        return NULL;
    }
}

/* Where the transport payload begins in the stored bytes, found the same way
 * the decoder found it when the frame arrived. */
static uint32_t payload_in(const uint8_t *bytes, uint32_t n)
{
    struct ppc_frame f = { bytes, n, n, 0, PPC_PKT_UNKNOWN };
    struct ppc_record tmp;

    return ppc_decode(&f, &tmp);
}

static void dns_info(const struct ppc_record *r, const uint8_t *bytes, uint32_t n, char *out,
                     size_t outlen)
{
    unsigned id = r->a >> 16;
    unsigned flags = r->a & 0xffffu;
    unsigned qd = r->b >> 16;
    int response = (flags & 0x8000u) != 0;
    unsigned opcode = (flags >> 11) & 0xfu;
    const char *rc = response ? dns_rcode_text(flags & 0xfu) : NULL;
    const char *kind = r->app == PPC_APP_NBNS ? (opcode == 5 ? "Registration" : "Name query")
                       : opcode == 0          ? "Standard query"
                       : opcode == 5          ? "Update"
                                              : "Query";
    char name[PPC_NAME_TEXT] = "";
    char tbuf[16];
    const char *type = "";

    if (n > 0 && qd > 0) {
        uint32_t pay = payload_in(bytes, n);
        uint32_t next = 0;

        /* Compression pointers count from the DNS header, so the name is
         * read inside the message, not the frame. A name that cannot be
         * read whole is left out rather than shown in part. */
        if (pay + 12 <= n &&
            ppc_dns_name(bytes + pay, n - pay, 12, name, sizeof(name), &next) == 0 &&
            pay + next + 2 <= n) {
            type = ppc_dns_type_name(rd16(bytes + pay + next), tbuf, sizeof(tbuf));
        } else {
            name[0] = '\0';
        }
    }
    snprintf(out, outlen, "%s%s 0x%04x%s%s%s%s%s%s", kind, response ? " response" : "", id,
             type[0] ? " " : "", type, name[0] ? " " : "", name, rc ? " " : "", rc ? rc : "");
}

static void ssdp_line(const uint8_t *bytes, uint32_t n, char *out, size_t outlen)
{
    uint32_t pay = payload_in(bytes, n);
    size_t o = 0;

    while (pay < n && bytes[pay] != '\r' && bytes[pay] != '\n' && o + 1 < outlen && o < 60) {
        out[o++] = (char)bytes[pay++];
    }
    out[o] = '\0';
    ppc_sanitize(out);
}

static void arp_info(const struct ppc_record *r, char *out, size_t outlen)
{
    char s[PPC_ADDR_TEXT];
    char t[PPC_ADDR_TEXT];
    char m[PPC_ADDR_TEXT];
    static const uint8_t zero[4];

    if (memcmp(r->src, zero, 4) == 0 && memcmp(r->dst, zero, 4) == 0 && r->a != 1) {
        snprintf(out, outlen, "ARP opcode %u", (unsigned)r->a);
        return;
    }
    ppc_format_addr(s, sizeof(s), r->src, 4);
    ppc_format_addr(t, sizeof(t), r->dst, 4);
    ppc_format_addr(m, sizeof(m), r->src + 4, 6);
    switch (r->a) {
    case 1:
        if (memcmp(r->src, r->dst, 4) == 0) {
            snprintf(out, outlen, "Announcement %s", s);
        } else if (memcmp(r->src, zero, 4) == 0) {
            snprintf(out, outlen, "Who has %s? (probe)", t);
        } else {
            snprintf(out, outlen, "Who has %s? Tell %s", t, s);
        }
        break;
    case 2:
        snprintf(out, outlen, "%s is at %s", s, m);
        break;
    default:
        snprintf(out, outlen, "ARP opcode %u", (unsigned)r->a);
        break;
    }
}

static void info_text(const struct ppc_record *r, const uint8_t *bytes, uint32_t n, char *out,
                      size_t outlen)
{
    char tmp[64];

    switch (r->info) {
    case PPC_INFO_RUNT:
        if (r->flags & PPC_F_TRUNCATED) {
            snprintf(out, outlen, "Captured %u of %u bytes", (unsigned)n, (unsigned)r->wire_len);
        } else {
            snprintf(out, outlen, "Runt frame, %u bytes", (unsigned)r->wire_len);
        }
        return;
    case PPC_INFO_ETHERTYPE:
        if (r->a < 0x0600) {
            snprintf(out, outlen, "802.3 frame, length %u", (unsigned)r->a);
        } else {
            snprintf(out, outlen, "Ethertype 0x%04x", (unsigned)r->a);
        }
        return;
    case PPC_INFO_ARP:
        arp_info(r, out, outlen);
        return;
    case PPC_INFO_IP_PROTO:
        if (r->l4 == 59) {
            snprintf(out, outlen, "No next header");
        } else {
            snprintf(out, outlen, "IP protocol %u", (unsigned)r->l4);
        }
        return;
    case PPC_INFO_FRAGMENT:
        snprintf(out, outlen, "Fragment, protocol %u, offset %u, id 0x%x", (unsigned)r->l4,
                 (unsigned)r->a, (unsigned)r->b);
        return;
    case PPC_INFO_TCP:
        tcp_flags_text(r->tcp_flags, tmp, sizeof(tmp));
        if (r->tcp_flags & PPC_TCP_ACK) {
            snprintf(out, outlen, "%u > %u [%s] Seq=%u Ack=%u%s", r->sport, r->dport, tmp,
                     (unsigned)r->a, (unsigned)r->b, r->app == PPC_APP_NONE ? "" : " (DNS)");
        } else {
            snprintf(out, outlen, "%u > %u [%s] Seq=%u%s", r->sport, r->dport, tmp, (unsigned)r->a,
                     r->app == PPC_APP_NONE ? "" : " (DNS)");
        }
        return;
    case PPC_INFO_UDP:
        if (r->app == PPC_APP_SSDP && n > 0) {
            ssdp_line(bytes, n, tmp, sizeof(tmp));
            if (tmp[0]) {
                snprintf(out, outlen, "%s", tmp);
                return;
            }
        }
        snprintf(out, outlen, "%u > %u Len=%u", r->sport, r->dport,
                 r->a >= 8 ? (unsigned)r->a - 8u : 0u);
        return;
    case PPC_INFO_ICMP: {
        const char *t = icmp_text(r->a >> 8, r->a & 0xffu);

        if ((r->a >> 8) == 0 || (r->a >> 8) == 8) {
            snprintf(out, outlen, "%s id=0x%04x seq=%u", t, (unsigned)(r->b >> 16), (unsigned)(r->b & 0xffffu));
        } else if (t) {
            snprintf(out, outlen, "%s", t);
        } else {
            snprintf(out, outlen, "Type %u, code %u", (unsigned)(r->a >> 8), (unsigned)(r->a & 0xffu));
        }
        return;
    }
    case PPC_INFO_ICMP6: {
        unsigned type = r->a >> 8;
        const char *t = icmp6_text(type);

        if ((type == 135 || type == 136) && n > 0) {
            uint32_t pay = payload_in(bytes, n);

            if (pay + 24 <= n) {
                char a[PPC_ADDR_TEXT];

                ppc_format_addr(a, sizeof(a), bytes + pay + 8, 16);
                snprintf(out, outlen, "%s %s %s", t, type == 135 ? "for" : "of", a);
                return;
            }
        }
        if (type == 128 || type == 129) {
            snprintf(out, outlen, "%s id=0x%04x seq=%u", t, (unsigned)(r->b >> 16), (unsigned)(r->b & 0xffffu));
        } else if (t) {
            snprintf(out, outlen, "%s", t);
        } else {
            snprintf(out, outlen, "Type %u, code %u", type, (unsigned)(r->a & 0xffu));
        }
        return;
    }
    case PPC_INFO_IGMP: {
        const char *t = igmp_text(r->a);

        if (t) {
            snprintf(out, outlen, "%s", t);
        } else {
            snprintf(out, outlen, "IGMP type 0x%02x", (unsigned)r->a);
        }
        return;
    }
    case PPC_INFO_DNS:
        dns_info(r, bytes, n, out, outlen);
        return;
    case PPC_INFO_DHCP: {
        const char *t = dhcp_type_text(r->b & 0xffu);

        if (t) {
            snprintf(out, outlen, "DHCP %s, transaction 0x%08x", t, (unsigned)r->a);
        } else {
            snprintf(out, outlen, "BOOTP %s, transaction 0x%08x", (r->b >> 8) == 2 ? "reply" : "request",
                     (unsigned)r->a);
        }
        return;
    }
    case PPC_INFO_DHCP6: {
        const char *t = dhcp6_type_text(r->a);

        snprintf(out, outlen, "%s, transaction 0x%06x", t ? t : "DHCPv6", (unsigned)r->b);
        return;
    }
    case PPC_INFO_NTP:
        snprintf(out, outlen, "NTP version %u, %s", (unsigned)(r->a >> 8), ntp_mode_text(r->a & 7u));
        return;
    default:
        /* A header that could not be read far enough to say anything. */
        snprintf(out, outlen, "%s header cut short or invalid", ppc_proto_word(r));
        return;
    }
}

void ppc_format_row(const struct ppc_record *r, const uint8_t *bytes, uint32_t n,
                    struct ppc_row_text *out)
{
    size_t len;

    memset(out, 0, sizeof(*out));
    if (!bytes) {
        n = 0;
    }
    snprintf(out->dir, sizeof(out->dir), "%s",
             (r->flags & PPC_F_OUT)         ? "out"
             : (r->flags & PPC_F_BROADCAST) ? "bcast"
             : (r->flags & PPC_F_MULTICAST) ? "mcast"
                                            : "in");
    snprintf(out->word, sizeof(out->word), "%s", ppc_proto_word(r));
    switch (r->l3) {
    case PPC_L3_IPV4:
        ppc_format_addr(out->src, sizeof(out->src), r->src, 4);
        ppc_format_addr(out->dst, sizeof(out->dst), r->dst, 4);
        break;
    case PPC_L3_IPV6:
        ppc_format_addr(out->src, sizeof(out->src), r->src, 16);
        ppc_format_addr(out->dst, sizeof(out->dst), r->dst, 16);
        break;
    case PPC_L3_ARP:
        ppc_format_addr(out->src, sizeof(out->src), r->src + 4, 6);
        if (r->flags & PPC_F_BROADCAST) {
            snprintf(out->dst, sizeof(out->dst), "broadcast");
        } else {
            ppc_format_addr(out->dst, sizeof(out->dst), r->dst + 4, 6);
        }
        break;
    case PPC_L3_OTHER:
        ppc_format_addr(out->src, sizeof(out->src), r->src, 6);
        ppc_format_addr(out->dst, sizeof(out->dst), r->dst, 6);
        break;
    default:
        snprintf(out->src, sizeof(out->src), "-");
        snprintf(out->dst, sizeof(out->dst), "-");
        break;
    }
    info_text(r, bytes, n, out->info, sizeof(out->info));
    len = strlen(out->info);
    if ((r->flags & PPC_F_GRO) && len + 6 < sizeof(out->info)) {
        snprintf(out->info + len, sizeof(out->info) - len, " [GRO]");
        len = strlen(out->info);
    }
    if ((r->flags & PPC_F_MALFORMED) && r->info != PPC_INFO_NONE && r->info != PPC_INFO_RUNT &&
        len + 12 < sizeof(out->info)) {
        snprintf(out->info + len, sizeof(out->info) - len, " [malformed]");
    }
    ppc_sanitize(out->info);
}
