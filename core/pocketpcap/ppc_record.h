/*
 * pocketpcap: the packet record PacketShark's helper keeps for every stored
 * frame (docs/apps/PACKETSHARK.md, ADR-016 accepted).
 *
 * A record is 80 bytes, fixed, binary, and private to the helper: it never
 * crosses the socket to the shell. The shell receives text formatted from it
 * (ppc_format.h) for the rows it shows, and nothing else.
 *
 * Everything here is pure C11 with no allocation and no LVGL or libpcap, so
 * it is built and tested on the host (tests/ppc_decode_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_RECORD_H
#define PPC_RECORD_H

#include <stdint.h>

/* The colour class: one per packet, the most specific that applies. The
 * order is the graph's stacking order and the order of the eight class
 * counters on the wire (c0..c7), so it is part of the protocol. */
enum ppc_class {
    PPC_CLASS_TCP = 0,
    PPC_CLASS_UDP,
    PPC_CLASS_DNS,
    PPC_CLASS_DHCP,
    PPC_CLASS_ARP,
    PPC_CLASS_ICMP,
    PPC_CLASS_IPV6,
    PPC_CLASS_OTHER,
    PPC_CLASS_COUNT
};

enum ppc_l3 {
    PPC_L3_NONE = 0,
    PPC_L3_IPV4,
    PPC_L3_IPV6,
    PPC_L3_ARP,
    PPC_L3_OTHER,
};

/* The application protocol, from the UDP or TCP port. */
enum ppc_app {
    PPC_APP_NONE = 0,
    PPC_APP_DNS,
    PPC_APP_MDNS,
    PPC_APP_LLMNR,
    PPC_APP_DHCP,
    PPC_APP_DHCP6,
    PPC_APP_NTP,
    PPC_APP_NBNS,
    PPC_APP_SSDP,
};

/* Which sentence ppc_format() builds for the info column, and what the
 * record's a and b carry for it. */
enum ppc_info {
    PPC_INFO_NONE = 0,
    PPC_INFO_RUNT,        /* shorter than an Ethernet header */
    PPC_INFO_ETHERTYPE,   /* a: ethertype (or 802.3 length) */
    PPC_INFO_ARP,         /* a: opcode */
    PPC_INFO_IP_PROTO,    /* l4 is the protocol number */
    PPC_INFO_FRAGMENT,    /* a: fragment offset in bytes, b: IP id */
    PPC_INFO_TCP,         /* a: sequence number, b: acknowledgement number */
    PPC_INFO_UDP,         /* a: UDP length */
    PPC_INFO_ICMP,        /* a: type << 8 | code, b: echo id << 16 | echo sequence */
    PPC_INFO_ICMP6,       /* a: type << 8 | code */
    PPC_INFO_IGMP,        /* a: type */
    PPC_INFO_DNS,         /* a: id << 16 | flags, b: qdcount << 16 | ancount */
    PPC_INFO_DHCP,        /* a: transaction id, b: op << 8 | message type (0 unknown) */
    PPC_INFO_DHCP6,       /* a: message type, b: transaction id */
    PPC_INFO_NTP,         /* a: version << 8 | mode, b: stratum */
};

/* Record flags. */
#define PPC_F_OUT 0x0001u        /* sent by this device */
#define PPC_F_BROADCAST 0x0002u  /* to ff:ff:ff:ff:ff:ff */
#define PPC_F_MULTICAST 0x0004u  /* to a group address */
#define PPC_F_VLAN 0x0008u       /* carried 802.1Q or 802.1ad tags */
#define PPC_F_FRAGMENT 0x0010u   /* an IP fragment */
#define PPC_F_IPV6 0x0020u
#define PPC_F_TRUNCATED 0x0040u  /* captured fewer bytes than were on the wire */
#define PPC_F_GRO 0x0080u        /* longer than an Ethernet frame: merged by the receiver */
#define PPC_F_MALFORMED 0x0100u  /* a header ran past the captured bytes or lied */
#define PPC_F_NONFIRST 0x0200u   /* a non-first fragment: no transport header */

/* Signals (findings) are set by ppc_stats in a later milestone; the record
 * carries the bits from now on so the format does not change. */
#define PPC_SIG_RETRANS 0x0001u
#define PPC_SIG_RST 0x0002u
#define PPC_SIG_DNS_FAIL 0x0004u
#define PPC_SIG_UNREACH 0x0008u
#define PPC_SIG_SYN_UNANSWERED 0x0010u
#define PPC_SIGNAL_COUNT 5

/* TCP flag bits as on the wire (byte 13). */
#define PPC_TCP_FIN 0x01u
#define PPC_TCP_SYN 0x02u
#define PPC_TCP_RST 0x04u
#define PPC_TCP_PSH 0x08u
#define PPC_TCP_ACK 0x10u
#define PPC_TCP_URG 0x20u

/* Ethernet frame without FCS, with one tag: anything longer was merged. */
#define PPC_ETH_MAX_FRAME 1518u

struct ppc_record {
    uint64_t rel_ns;     /* since the session's first packet, clock-step corrected */
    uint64_t frame_off;  /* monotonic byte offset of the stored bytes (ppc_store.h) */
    uint32_t seq;        /* 1-based, unique in a session, never reused */
    uint32_t wire_len;   /* length on the wire; 32 bits because a GRO record is large */
    uint16_t cap_len;    /* bytes stored for this frame */
    uint16_t flags;      /* PPC_F_* */
    uint8_t cls;         /* enum ppc_class */
    uint8_t l3;          /* enum ppc_l3 */
    uint8_t l4;          /* IP protocol number; 0 when none */
    uint8_t app;         /* enum ppc_app */
    uint8_t tcp_flags;
    uint8_t info;        /* enum ppc_info */
    uint16_t sport;
    uint16_t dport;
    uint16_t signal;     /* PPC_SIG_* */
    /* IPv6 addresses whole; IPv4 in the first 4 bytes; for ARP the sender
     * and target IPv4 address in the first 4 bytes and their MAC in bytes
     * 4..9; for other link-layer frames the MAC in the first 6. */
    uint8_t src[16];
    uint8_t dst[16];
    uint32_t a;          /* the sentence's arguments (enum ppc_info) */
    uint32_t b;
};

_Static_assert(sizeof(struct ppc_record) == 80, "struct ppc_record is 80 bytes");

/* One frame as a source hands it over: bytes valid for the call only. */
struct ppc_frame {
    const uint8_t *data;
    uint32_t cap_len;   /* bytes in data */
    uint32_t wire_len;  /* bytes on the wire */
    uint64_t ts_ns;     /* wall-clock stamp from the kernel, nanoseconds */
    uint8_t pkttype;    /* PPC_PKT_* */
};

/* Direction as the source knows it (the values of linux/if_packet.h's
 * PACKET_HOST .. PACKET_OUTGOING, so a raw socket passes them through). */
#define PPC_PKT_HOST 0
#define PPC_PKT_BROADCAST 1
#define PPC_PKT_MULTICAST 2
#define PPC_PKT_OTHERHOST 3
#define PPC_PKT_OUTGOING 4
#define PPC_PKT_UNKNOWN 255

/* What is stored of each frame (DESIGN_DECISIONS D7, accepted 2026-11-15). */
enum ppc_keep {
    PPC_KEEP_HEADERS = 0, /* headers; the whole message only for network plumbing */
    PPC_KEEP_FULL,        /* everything captured; chosen per session, confirmed */
};

/* HEADERS keeps at most this much of a plumbing message (ARP, ICMP, ICMPv6,
 * DNS, mDNS, LLMNR, DHCP, DHCPv6, NTP, NetBIOS-NS) after its start. */
#define PPC_PLUMBING_MAX 1024u

#endif
