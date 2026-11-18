/*
 * pocketpcap signals: the findings PacketShark marks on packets (milestone
 * M7; docs/apps/PACKETSHARK.md §4a, DS §59.2: status_warn, always with `!`
 * and a word), and the DNS and TCP diagnosis behind them (M11.1, §4b).
 *
 * Six findings, one bit each in the low byte of a record's `signal`
 * (ppc_record.h):
 *
 *   DNS failure       a DNS or LLMNR response whose code is not NOERROR
 *   reset             a TCP segment with RST
 *   retransmission    a TCP segment whose sequence space was seen before in
 *                     its direction of its connection, or that fills a gap
 *                     later than a round trip after the gap appeared
 *   unreachable       ICMP destination unreachable, ICMPv6 type 1
 *   unanswered SYN    a SYN the other end had sent nothing back to after
 *                     PPC_SIG_SYN_WAIT_NS
 *   unanswered DNS    a DNS query (port 53) with no response seen after
 *                     PPC_DNS_WAIT_NS of capture that saw every frame
 *
 * and one diagnosis code in its high byte (enum ppc_diag_code): what the two
 * tables made of the packet - a query and its answer, a handshake, which
 * kind of reset, which kind of repeat. A diagnosis is not a finding.
 *
 * Two tables, fixed size, allocated together once at START:
 *
 *   TCP connections   PPC_SIG_FLOWS, keyed by the five-tuple; the entry
 *                     silent longest replaced when a probe finds no room
 *   DNS queries       PPC_DNS_QUERIES, keyed by client address and port,
 *                     server address and port, and query id; an entry is
 *                     freed by its answer, kept PPC_DNS_KEEP_NS after it was
 *                     called unanswered (to recognise a late answer), and
 *                     replaced oldest first when full (then forgotten, not
 *                     called unanswered)
 *
 * The two late findings are decided by ppc_signals_expire(), which names
 * the packet so the caller can mark it in the store.
 *
 * These are heuristics on what one interface saw, not a TCP or DNS
 * analysis. A missing answer is never called a failure: it may have been
 * lost, sent on another interface, or dropped by the capture. Nothing is
 * judged while frames pass unseen (ppc_signals_blind()).
 *
 * Bounds-checked, allocation-free after init, no text, no time of its own
 * (tests/ppc_signals_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_SIGNALS_H
#define PPC_SIGNALS_H

#include "ppc_proto.h"
#include "ppc_record.h"

#include <stdint.h>

#define PPC_SIG_FLOWS 4096u
#define PPC_SIG_PROBES 8u
#define PPC_SIG_SYN_WAIT_NS 3000000000ull
#define PPC_SIG_ALL PPC_SIG_FINDINGS_ALL
/* DNS (M11.1): a resolver's usual timeout before it gives up on a server
 * (glibc and musl default to 5 s), and how long an unanswered query is
 * remembered so that a late answer is recognised as one. */
#define PPC_DNS_QUERIES 1024u
#define PPC_DNS_WAIT_NS 5000000000ull
#define PPC_DNS_KEEP_NS 30000000000ull
/* TCP (M11.1): a gap filled sooner than this after it appeared - or sooner
 * than the connection's own handshake time, when that was measured - is
 * reordering: the sender cannot have learnt of a loss yet. */
#define PPC_TCP_REORDER_NS 3000000ull

/* One direction of a connection. */
struct ppc_sig_dir {
    uint32_t next;    /* the sequence number after the highest byte seen */
    uint32_t ack;     /* the highest acknowledgement this direction sent */
    uint32_t hole_lo; /* a gap of sequence space not seen: [hole_lo, hole_hi) */
    uint32_t hole_hi;
    uint64_t hole_ns; /* when the gap appeared */
    uint8_t have;
    uint8_t have_ack;
    uint8_t hole;
};

/* Where a connection's handshake stands. */
enum ppc_hs {
    PPC_HS_NONE = 0,
    PPC_HS_SYN,     /* a SYN seen, going hs_dir */
    PPC_HS_SYNACK,  /* its SYN-ACK seen */
    PPC_HS_OPEN,    /* the handshake completed */
    PPC_HS_MID,     /* seen without its handshake (or part of it passed unseen) */
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
    uint8_t hs;      /* enum ppc_hs */
    uint8_t hs_dir;  /* the direction of the connection's SYN */
    uint8_t ended;   /* 1: a FIN was seen, 2: a reset */
    uint8_t dirs;    /* a bit for each direction seen */
    uint8_t hs_seen; /* hs_ns holds the first SYN's time */
    uint16_t pkts;   /* segments seen, saturating */
    struct ppc_sig_dir d[2]; /* 0: a to b */
    uint32_t syn_seq;  /* the waiting SYN's packet; 0: none waits */
    uint32_t syn_isn;
    uint32_t synack_isn;
    uint32_t hs_seq;   /* the first SYN's packet */
    uint32_t irtt_us;  /* the handshake's time; 0: not measured */
    uint64_t syn_ns;
    uint64_t hs_ns;    /* the first SYN, when hs_seen */
    uint64_t last_ns;
};

/* One DNS query waiting for its answer. */
struct ppc_dns_q {
    uint8_t cli[16];
    uint8_t srv[16];
    uint16_t cport;
    uint16_t sport;
    uint16_t id;
    uint8_t used;
    uint8_t v6;
    uint8_t state;     /* 1: waiting, 2: called unanswered */
    uint8_t tries;     /* copies seen */
    uint32_t seq;      /* the latest copy's packet */
    uint64_t last_ns;  /* when the latest copy went */
};

struct ppc_signals {
    struct ppc_sig_flow *flows;
    uint32_t cap;      /* a power of two */
    uint32_t waiting;  /* SYNs waiting for an answer */
    uint64_t replaced;
    struct ppc_dns_q *dns; /* inside the same allocation as flows */
    uint32_t dns_cap;
    uint32_t dns_live;     /* entries in use */
    uint32_t dns_waiting;  /* entries waiting for an answer */
    struct ppc_diag st;    /* the running totals (M11.1); the scan fills the rest */
};

/* Allocate a table of `flows` connections (a power of two) and the DNS
 * table. 0, or -1 with nothing held. */
int ppc_signals_init(struct ppc_signals *sg, uint32_t flows);
void ppc_signals_free(struct ppc_signals *sg);

/* The TCP payload length of a frame whose transport payload starts at
 * pay_off (what ppc_decode() returned): the IP datagram's own end, not the
 * frame's, so Ethernet padding is not counted. 0 when it cannot be read. */
uint32_t ppc_tcp_payload_len(const uint8_t *d, uint32_t n, uint32_t pay_off);

/* The findings and diagnosis of one decoded packet: PPC_SIG_* bits in the
 * low byte, an enum ppc_diag_code in the high byte (the record's `signal`).
 * tcp_len is ppc_tcp_payload_len() for a TCP segment; seq is the number
 * the store will give the packet (so it can be named later). */
uint16_t ppc_signals_frame(struct ppc_signals *sg, const struct ppc_record *r, uint32_t tcp_len, uint32_t seq);

/* A late finding: `bit` (PPC_SIG_SYN_UNANSWERED or PPC_SIG_DNS_NOREPLY)
 * belongs on packet `seq`. */
typedef void (*ppc_sig_late_fn)(void *ctx, uint32_t seq, uint16_t bit);

/* Whether ppc_signals_expire() has anything to look at. */
int ppc_signals_pending(const struct ppc_signals *sg);

/* Report every SYN that has waited PPC_SIG_SYN_WAIT_NS and every DNS query
 * that has waited PPC_DNS_WAIT_NS at now_ns (time since the first packet,
 * as the records carry it), and forget unanswered queries older than
 * PPC_DNS_KEEP_NS. Costs nothing while nothing waits. Returns how many
 * were reported. */
uint32_t ppc_signals_expire(struct ppc_signals *sg, uint64_t now_ns, ppc_sig_late_fn fn, void *ctx);

/* Frames are about to pass unjudged (HOLD, shedding): no SYN or query that
 * waits can be called unanswered any more, no gap can be judged, and a
 * handshake under way can no longer be timed. */
void ppc_signals_blind(struct ppc_signals *sg);

/* The diagnosis as it stands: the running totals, and what a scan of the
 * connection table finds now (connections tracked, seen one way only).
 * Costs one pass over the table; meant for once a second. */
void ppc_signals_diag(const struct ppc_signals *sg, struct ppc_diag *out);

/* The word of one finding ("DNS FAIL", "TCP RST", "RETRANS", "UNREACH",
 * "NO ANSWER", "NO DNS REPLY") by bit index, and its sentence for the
 * inspector. */
const char *ppc_signal_word(unsigned index);
const char *ppc_signal_name(unsigned index);

#endif
