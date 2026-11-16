/*
 * pocketpcap signals: the findings PacketShark marks on packets (milestone
 * M7; docs/apps/PACKETSHARK.md §4a, DS §59.2: status_warn, always with `!`
 * and a word).
 *
 * Five findings, one bit each in a record's `signal` (ppc_record.h):
 *
 *   DNS failure       a DNS or LLMNR response whose code is not NOERROR
 *   reset             a TCP segment with RST
 *   retransmission    a TCP segment whose sequence space was seen before in
 *                     its direction of its connection
 *   unreachable       ICMP destination unreachable, ICMPv6 type 1
 *   unanswered SYN    a SYN the other end had sent nothing back to after
 *                     PPC_SIG_SYN_WAIT_NS
 *
 * The first four are decided from the packet itself and from a table of the
 * TCP connections seen (fixed size, allocated once at START, the oldest
 * entry replaced when a probe finds no room). The fifth is decided later,
 * by ppc_signals_expire(), which names the SYN's packet so the caller can
 * mark it in the store.
 *
 * These are heuristics on what one interface saw, not a TCP analysis: a
 * segment that arrives out of order reads as a retransmission, a keep-alive
 * is not counted, and a connection that began before the capture is judged
 * from the first segment seen.
 *
 * Bounds-checked, allocation-free after init, no text, no time of its own
 * (tests/ppc_signals_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_SIGNALS_H
#define PPC_SIGNALS_H

#include "ppc_record.h"

#include <stdint.h>

#define PPC_SIG_FLOWS 4096u
#define PPC_SIG_PROBES 8u
#define PPC_SIG_SYN_WAIT_NS 3000000000ull
#define PPC_SIG_ALL 0x1fu

/* One direction of a connection. */
struct ppc_sig_dir {
    uint32_t next;   /* the sequence number after the highest byte seen */
    uint8_t have;
};

struct ppc_sig_flow {
    uint8_t a[16];   /* the lower end (address, then port) */
    uint8_t b[16];
    uint16_t pa;
    uint16_t pb;
    uint8_t used;
    uint8_t v6;
    uint8_t syn_dir; /* the direction the waiting SYN went */
    uint8_t syn_done;/* this SYN was already reported unanswered */
    struct ppc_sig_dir d[2]; /* 0: a to b */
    uint32_t syn_seq;  /* the waiting SYN's packet; 0: none waits */
    uint32_t syn_isn;
    uint64_t syn_ns;
    uint64_t last_ns;
};

struct ppc_signals {
    struct ppc_sig_flow *flows;
    uint32_t cap;     /* a power of two */
    uint32_t waiting; /* SYNs waiting for an answer */
    uint64_t replaced;
};

/* Allocate a table of `flows` connections (a power of two). 0, or -1 with
 * nothing held. */
int ppc_signals_init(struct ppc_signals *sg, uint32_t flows);
void ppc_signals_free(struct ppc_signals *sg);

/* The TCP payload length of a frame whose transport payload starts at
 * pay_off (what ppc_decode() returned): the IP datagram's own end, not the
 * frame's, so Ethernet padding is not counted. 0 when it cannot be read. */
uint32_t ppc_tcp_payload_len(const uint8_t *d, uint32_t n, uint32_t pay_off);

/* The findings of one decoded packet, PPC_SIG_* bits. tcp_len is
 * ppc_tcp_payload_len() for a TCP segment; seq is the number the store
 * will give the packet (so a SYN can be named later). */
uint16_t ppc_signals_frame(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq);

typedef void (*ppc_sig_late_fn)(void *ctx, uint32_t seq);

/* Report every SYN that has waited PPC_SIG_SYN_WAIT_NS at now_ns (time
 * since the first packet, as the records carry it). Costs nothing while no
 * SYN waits. Returns how many were reported. */
uint32_t ppc_signals_expire(struct ppc_signals *sg, uint64_t now_ns, ppc_sig_late_fn fn, void *ctx);

/* Frames are about to pass unjudged (HOLD, shedding): no SYN that waits
 * can be called unanswered any more. */
void ppc_signals_blind(struct ppc_signals *sg);

/* The word of one finding ("DNS FAIL", "TCP RST", "RETRANS", "UNREACH",
 * "NO ANSWER") by bit index, and its sentence for the inspector. */
const char *ppc_signal_word(unsigned index);
const char *ppc_signal_name(unsigned index);

#endif
