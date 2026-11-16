/*
 * pocketpcap decoder: one frame in, one 80-byte record out.
 *
 * This is the hot path of the capture helper. It runs once per frame, on
 * bytes that came off the wire and must be treated as hostile:
 *
 *   - every read is bounds-checked against the captured length; a header
 *     that runs past the end, or a length field that lies, sets
 *     PPC_F_MALFORMED and keeps what was decoded so far. The decoder never
 *     fails and never reads outside f->data[0 .. f->cap_len);
 *   - no allocation, no recursion, no text, no loop without a fixed bound
 *     (two VLAN tags, eight IPv6 extension headers, DHCP options inside the
 *     captured bytes);
 *   - no state between calls: the same frame always gives the same record.
 *
 * Protocols: Ethernet II, 802.1Q/802.1ad (two tags), 802.3 length frames,
 * ARP, IPv4 (options, fragments), IPv6 (hop-by-hop, routing, fragment and
 * destination-options headers), TCP, UDP, ICMP, ICMPv6, IGMP, and by port
 * DNS, mDNS, LLMNR, DHCP, DHCPv6, NTP, NetBIOS-NS and SSDP.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_DECODE_H
#define PPC_DECODE_H

#include "ppc_record.h"

/* Fill *out from the frame (seq, rel_ns and frame_off are left 0 for the
 * caller) and return the offset where the transport payload begins: the
 * first byte after the last header the decoder understood. For ARP, ICMP and
 * the plumbing protocols it is where their message begins, so the HEADERS
 * rule can keep the message. Never more than f->cap_len. */
uint32_t ppc_decode(const struct ppc_frame *f, struct ppc_record *out);

/* How many bytes of the frame to store (DESIGN_DECISIONS D7):
 *   FULL:    everything captured, at most 65535;
 *   HEADERS: up to payload_off, plus up to PPC_PLUMBING_MAX bytes of the
 *            message for ARP, ICMP, ICMPv6, DNS, mDNS, LLMNR, DHCP, DHCPv6,
 *            NTP and NetBIOS-NS. No other payload. */
uint32_t ppc_store_len(const struct ppc_record *r, uint32_t payload_off, uint32_t cap_len,
                       enum ppc_keep keep);

/* True for the protocols whose whole message HEADERS keeps. */
int ppc_is_plumbing(const struct ppc_record *r);

/* The word for a record's protocol column: "TCP", "mDNS", "ICMPv6", "NDP",
 * "IGMP", "LLDP", ... Never NULL; at most 7 characters. */
const char *ppc_proto_word(const struct ppc_record *r);

/* "TCP" .. "OTHER" for a class index; "?" out of range. */
const char *ppc_class_name(unsigned cls);

#endif
