/*
 * pocketpcap network intelligence (milestone M12.1; docs/apps/PACKETSHARK.md
 * §4c): what the frames one interface saw say about the network itself.
 *
 *   devices    the stations seen sending, by MAC: frames, bytes, the IPv4
 *              address ARP or DHCP tied to the MAC, a DHCP host name, and
 *              what it was seen to be (this device, a DHCP server, the
 *              router a DHCP server named)
 *   ARP        the IPv4-to-MAC pairs ARP senders claimed; requests, replies,
 *              gratuitous ARP and probes counted; an address that changes
 *              MAC, that two MACs claim in turn, or that is defended against
 *              a probe (RFC 5227); this device's own requests left unanswered
 *   DHCP       DISCOVER, OFFER, REQUEST and ACK paired by transaction id and
 *              client MAC; the servers seen answering; the leases they gave;
 *              NAK, DECLINE, and requests no answer was seen to
 *   events     what is worth telling, in a ring: each with a severity, the
 *              addresses it is about, the packet that showed it and when
 *
 * Everything here is passive: it reads frames the capture already has and
 * sends nothing (ADR-016 rule 2). It is evidence from one interface. On a
 * switch port the answers to other devices are unicast and never arrive, so
 * a request is called unanswered only when its answer would have been seen
 * here: it is this device's own, or the client asked for a broadcast reply.
 * Nothing waiting is judged once frames pass unseen (ppc_net_blind()).
 *
 * Fixed tables in one allocation made at START; bounds-checked; no text and
 * no time of its own (tests/ppc_net_test.c, the fuzz target).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_NET_H
#define PPC_NET_H

#include "ppc_proto.h"
#include "ppc_record.h"

#include <stdint.h>

#define PPC_NET_HOSTS 256u    /* devices, a power of two */
#define PPC_NET_BINDS 256u    /* IPv4 addresses ARP spoke of, a power of two */
#define PPC_NET_DHCP 64u      /* DHCP transactions, a power of two */
#define PPC_NET_SERVERS 8u
#define PPC_NET_LEASES 8u
#define PPC_NET_EVENTS 32u
#define PPC_NET_WAITS 16u     /* this device's ARP requests waiting for a reply */
#define PPC_NET_PROBES 8u

/* A DHCP request whose answer would be seen here is called unanswered after
 * this long (a client's first retransmission comes after about 4 s, its
 * second after 8 more: RFC 2131 §4.1). */
#define PPC_NET_DHCP_WAIT_NS 10000000000ull
/* Two MACs that claim one address within this long of each other are both
 * alive: a hand-over is slower than that. */
#define PPC_NET_FLIP_NS 60000000000ull
/* A claim to an address within this long of an ARP probe for it, from
 * another MAC, is the conflict RFC 5227 probes for. */
#define PPC_NET_PROBE_NS 10000000000ull
/* This device's ARP request is unanswered after this many copies and this
 * long since the last one (Linux sends three, a second apart). */
#define PPC_NET_ARP_TRIES 3u
#define PPC_NET_ARP_WAIT_NS 2000000000ull
#define PPC_NET_ARP_FORGET_NS 10000000000ull
/* A station that asks for this many of 64 address buckets within the window
 * is sweeping the subnet. */
#define PPC_NET_SWEEP_BUCKETS 32u
#define PPC_NET_SWEEP_NS 10000000000ull
/* A lease shorter than this is unusual enough to mention. */
#define PPC_NET_SHORT_LEASE_S 120u

struct ppc_net_host {
    uint8_t mac[6];
    uint8_t used;
    uint8_t flags;       /* PPC_NH_* */
    uint8_t ip[4];       /* valid with PPC_NH_IP */
    uint32_t pkts;       /* frames it sent */
    uint64_t bytes;
    uint64_t first_ns;
    uint64_t last_ns;
    uint64_t sweep_bits; /* the address buckets it asked for in the window */
    uint64_t sweep_ns;
    char name[PPC_NET_NAME]; /* a DHCP host name, as the client sent it; "" */
};

struct ppc_net_bind {
    uint8_t ip[4];
    uint8_t mac[6];      /* who claims it now (have) */
    uint8_t prev[6];     /* who claimed it before (have_prev) */
    uint8_t probe_mac[6];
    uint8_t used;
    uint8_t have;
    uint8_t have_prev;
    uint8_t probed;
    uint8_t router;      /* a DHCP server named this address as the router */
    uint32_t seq;        /* the latest claim's packet */
    uint64_t last_ns;    /* the latest claim */
    uint64_t probe_ns;
};

/* What a DHCP transaction has shown. */
#define PPC_DH_DISCOVER 0x01u
#define PPC_DH_OFFER 0x02u
#define PPC_DH_REQUEST 0x04u
#define PPC_DH_ACK 0x08u
#define PPC_DH_NAK 0x10u
#define PPC_DH_DECLINE 0x20u

enum ppc_dh_wait { PPC_DH_WAIT_NONE = 0, PPC_DH_WAIT_OFFER, PPC_DH_WAIT_ACK };

struct ppc_net_dhcp {
    uint8_t mac[6];      /* the client (chaddr) */
    uint8_t used;
    uint8_t seen;        /* PPC_DH_* */
    uint8_t wait;        /* enum ppc_dh_wait */
    uint8_t judged;      /* it was called unanswered */
    uint8_t ip[4];       /* offered, requested or acknowledged */
    uint8_t server[4];   /* the first server that offered */
    uint32_t xid;
    uint32_t seq;        /* the packet the wait runs from */
    uint64_t wait_ns;    /* when that packet went */
    uint64_t last_ns;
    char name[PPC_NET_NAME];
};

struct ppc_net_server {
    uint8_t ip[4];       /* the server identifier (option 54), else the source address */
    uint8_t mac[6];      /* the frame's source: the server, or the relay before it */
    uint8_t used;
    uint8_t relayed;     /* giaddr was set */
    uint8_t router[4];   /* what its latest OFFER or ACK named (zero: none) */
    uint8_t dns[4];
    uint8_t mask[4];
    uint32_t lease_s;
    uint32_t offers;
    uint32_t acks;
    uint32_t naks;
    uint32_t seq;        /* its latest packet */
    uint64_t first_ns;
};

struct ppc_net_lease {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t server[4];
    uint8_t used;
    uint32_t lease_s;
    uint32_t seq;        /* the ACK */
    uint64_t ns;
    char name[PPC_NET_NAME];
};

struct ppc_net_event {
    uint8_t kind;        /* enum ppc_net_kind (ppc_proto.h) */
    uint8_t sev;         /* enum ppc_net_sev */
    uint8_t used;
    uint8_t ip[4];       /* zero: none */
    uint8_t ip2[4];
    uint8_t mac[6];      /* zero: none */
    uint8_t mac2[6];
    uint32_t count;      /* times it was seen */
    uint32_t seq;        /* the latest packet that showed it */
    uint64_t first_ns;
    uint64_t last_ns;
};

struct ppc_net_wait {
    uint8_t ip[4];
    uint8_t used;
    uint8_t tries;
    uint32_t seq;        /* the first request */
    uint64_t last_ns;
};

struct ppc_net {
    void *mem;           /* the one allocation the tables are in */
    struct ppc_net_host *hosts;
    struct ppc_net_bind *binds;
    struct ppc_net_dhcp *dhcp;
    struct ppc_net_server servers[PPC_NET_SERVERS];
    struct ppc_net_lease leases[PPC_NET_LEASES];
    uint32_t lease_next; /* the ring's next place */
    struct ppc_net_event events[PPC_NET_EVENTS];
    struct ppc_net_wait waits[PPC_NET_WAITS];
    uint32_t waiting;    /* DHCP transactions and ARP requests that wait */
    struct ppc_net_totals st;
};

/* Allocate the tables. 0, or -1 with nothing held. */
int ppc_net_init(struct ppc_net *nt);
void ppc_net_free(struct ppc_net *nt);

/* One decoded frame: r as ppc_decode() filled it (with its rel_ns), the
 * frame's bytes, where its transport payload starts (what ppc_decode()
 * returned) and the number the store gives the packet. */
void ppc_net_frame(struct ppc_net *nt, const struct ppc_record *r, const uint8_t *d, uint32_t n, uint32_t pay_off,
                   uint32_t seq);

/* Whether ppc_net_expire() has anything to look at. */
int ppc_net_pending(const struct ppc_net *nt);
/* Decide what has waited long enough at now_ns (as the records' rel_ns).
 * Returns how many events it made. */
uint32_t ppc_net_expire(struct ppc_net *nt, uint64_t now_ns);
/* Frames are about to pass unjudged (HOLD, shedding): nothing that waits
 * can be called unanswered any more. */
void ppc_net_blind(struct ppc_net *nt);

/* The totals as they stand (the events counted by severity). */
void ppc_net_totals(const struct ppc_net *nt, struct ppc_net_totals *out);
/* The devices that sent the most bytes, most first; how many. */
unsigned ppc_net_top(const struct ppc_net *nt, const struct ppc_net_host **out, unsigned max);
/* The servers in the order they were first seen; how many. */
unsigned ppc_net_servers(const struct ppc_net *nt, const struct ppc_net_server **out, unsigned max);
/* The leases, the newest first; how many. */
unsigned ppc_net_leases(const struct ppc_net *nt, const struct ppc_net_lease **out, unsigned max);
/* The events, warnings before notes and the newest first within each; how
 * many. */
unsigned ppc_net_events(const struct ppc_net *nt, const struct ppc_net_event **out, unsigned max);

#endif
