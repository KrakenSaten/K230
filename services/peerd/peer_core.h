/*
 * peerd's engine (docs/decisions/ADR-017): nodes, kept peers, the inbox,
 * signed discovery frames and sealed datagrams.
 *
 * It owns no socket, no file and no clock. Bytes come in through
 * pcore_input() with the address they came from and the time; bytes go out,
 * and kept peers are persisted, through the callbacks. So the whole of the
 * security-relevant behaviour runs in a test with two engines wired back to
 * back (tests/peer_core_test.c) and under a fuzzer (tests/peer_fuzz.c).
 *
 * Addresses are IPv4 in host byte order; times are monotonic milliseconds.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_CORE_H
#define PEERD_CORE_H

#include "peer_crypto.h"
#include "peer_store.h"
#include "peer_wire.h"

#include <stddef.h>
#include <stdint.h>

#define PCORE_NODES 32
#define PCORE_KEPT PS_KEPT_MAX
#define PCORE_INBOX 32
#define PCORE_INBOX_PER_SENDER 8
#define PCORE_IFACES 4
#define PCORE_PROBES 4

#define PCORE_LEASE_MS 30000
#define PCORE_BEACON_MS 2000
#define PCORE_PROBE_TRIES 3
#define PCORE_PROBE_INTERVAL_MS 1000
#define PCORE_LOOKUP_INTERVAL_MS 5000
#define PCORE_STALE_MS 10000            /* a kept peer not heard for this long is looked for */
#define PCORE_TABLE_FRESH_MS 10000      /* an announced table not repeated for this long is gone */
#define PCORE_KEPT_WRITE_MS 250         /* at most one write of the kept list per this long */
#define PCORE_TICK_MS 250

/* Why a frame was dropped, or what was done: the counters of peer.status. */
enum pcore_count {
    PCORE_C_RX = 0,             /* frames handed in */
    PCORE_C_MALFORMED,          /* length, version, type or a field */
    PCORE_C_OWN,                /* our own key as the sender */
    PCORE_C_OTHER,              /* not addressed to us */
    PCORE_C_UNKNOWN,            /* DATA from a key that is not a node */
    PCORE_C_NOT_VISIBLE,        /* from a stranger while not visible */
    PCORE_C_STATED,             /* a stranger's signed frame whose stated address is not its source */
    PCORE_C_REPLAY,
    PCORE_C_BUDGET,             /* the verification budget was spent */
    PCORE_C_BAD_KEY,            /* a key that cannot be used */
    PCORE_C_AUTH,               /* the tag or the signature did not verify */
    PCORE_C_INBOX_FULL,
    PCORE_C_ACCEPTED_DATA,
    PCORE_C_ACCEPTED_SIGNED,
    PCORE_C_TX,
    PCORE_C_TX_FAILED,
    PCORE_C_REPLY_LIMITED,      /* a reply not sent: over the reply limits */
    PCORE_C_KEPT_WRITES,
    PCORE_C_KEPT_WRITE_FAILED,
    PCORE_C_EVICTED_NODES,
    PCORE_C_COUNT
};

struct pcore_iface {
    uint32_t addr;
    uint32_t bcast;
};

struct pcore_dgram {
    uint64_t id;                /* 1 upwards, per run, in the order delivered */
    uint8_t from[PC_KEY];
    uint8_t port;
    uint16_t len;
    uint8_t data[PW_PAYLOAD_MAX];
    int64_t mono_ms;
};

struct pcore_node_info {
    uint8_t key[PC_KEY];
    char name[PW_NAME_MAX + 1];
    uint32_t addr;              /* 0: no endpoint known */
    uint16_t port;
    int64_t heard_ms;           /* -1: not heard this run */
    int kept;
    int proved;                 /* shown live this run: a challenge-bound reply or a sealed datagram */
    int table;                  /* a fresh table announcement follows */
    struct pw_table t;
};

enum pcore_probe_state {
    PCORE_PROBE_NONE = 0,
    PCORE_PROBE_PENDING,
    PCORE_PROBE_OK,
    PCORE_PROBE_FAILED,
};

struct pcore_probe_info {
    uint32_t id;
    uint32_t addr;
    uint16_t port;
    enum pcore_probe_state state;
    uint8_t key[PC_KEY];        /* when OK: who answered */
};

/* Results of the calls a client drives. */
enum pcore_rc {
    PCORE_OK = 0,
    PCORE_E_PARAM = -1,         /* a value out of range, our own key, a key that cannot be used */
    PCORE_E_UNKNOWN = -2,       /* not a node / not kept */
    PCORE_E_NO_ROUTE = -3,      /* a node with no address yet (a kept one is being looked for) */
    PCORE_E_FULL = -4,          /* no room: a fifth kept peer, a fifth probe */
    PCORE_E_CLOSED = -5,        /* not visible and nothing kept: nothing may be sent */
    PCORE_E_STORE = -6,         /* the kept list could not be written */
    PCORE_E_SEND = -7,          /* the transport refused the frame, or the counters are spent */
};

struct pcore_ops {
    /* Send n bytes to dst:port. broadcast is 1 for an interface's broadcast
     * address (TTL 1). Returns 0 when handed to the network. */
    int (*send)(void *user, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int broadcast);
    /* The local address a unicast to dst leaves from, or 0. */
    uint32_t (*source_for)(void *user, uint32_t dst);
    /* Write the kept list durably. 0 when it is on disk. */
    int (*kept_save)(void *user, const struct ps_kept *recs, int n);
    /* A datagram has entered the inbox (it has its id). May be NULL. */
    void (*delivered)(void *user, const struct pcore_dgram *d);
};

struct pcore;

/* id is borrowed and must outlive the engine. name may be "" (no name).
 * kept/kept_n are the records read at start. NULL when out of memory or the
 * arguments cannot be used. */
struct pcore *pcore_new(const struct pc_identity *id, uint64_t epoch, const char *name,
                        uint16_t udp_port, const struct pcore_ops *ops, void *user,
                        const struct ps_kept *kept, int kept_n);
void pcore_free(struct pcore *c);

void pcore_set_ifaces(struct pcore *c, const struct pcore_iface *ifs, int n);
int pcore_set_name(struct pcore *c, const char *name);
const char *pcore_name(const struct pcore *c);

/* A datagram from the network. */
void pcore_input(struct pcore *c, uint32_t src, uint16_t src_port, const uint8_t *buf, size_t n,
                 int64_t now);
/* Everything that is due: beacons, probe retries, held datagrams, the lease. */
void pcore_tick(struct pcore *c, int64_t now);
/* Whether a socket is needed at all: visible, a kept peer, or a probe out. */
int pcore_wants_socket(const struct pcore *c, int64_t now);

/* The visibility lease: on renews it for PCORE_LEASE_MS, off ends it. */
void pcore_set_visible(struct pcore *c, int on, int64_t now);
int pcore_visible(const struct pcore *c, int64_t now);

/* Announce (in this device's beacons) that a table is open. *tag is chosen
 * here. PCORE_OK or PCORE_E_PARAM. */
int pcore_announce(struct pcore *c, uint8_t port, uint8_t proto_min, uint8_t proto_max,
                   uint8_t seats, uint32_t *tag);
void pcore_withdraw(struct pcore *c);

/* Ask the device at addr:port who it is (ADD BY ADDRESS). Needs visibility.
 * Returns a probe id (>0) or a pcore_rc. */
int pcore_probe(struct pcore *c, uint32_t addr, uint16_t port, int64_t now);
int pcore_probes(const struct pcore *c, struct pcore_probe_info *out, int max);

int pcore_keep(struct pcore *c, const uint8_t key[PC_KEY]);
int pcore_release(struct pcore *c, const uint8_t key[PC_KEY]);
int pcore_kept(const struct pcore *c, struct ps_kept *out, int max);

/* Seal payload for a node and send it. PCORE_OK when it was handed to the
 * network. */
int pcore_send_app(struct pcore *c, const uint8_t to[PC_KEY], uint8_t port, const uint8_t *payload,
                   size_t n, int64_t now);

/* Datagrams for port with an id above after_id, oldest first. */
int pcore_inbox(const struct pcore *c, uint8_t port, uint64_t after_id, struct pcore_dgram *out,
                int max);
int pcore_nodes(const struct pcore *c, struct pcore_node_info *out, int max, int64_t now);

uint64_t pcore_counter(const struct pcore *c, enum pcore_count which);
const char *pcore_counter_name(enum pcore_count which);
uint64_t pcore_epoch(const struct pcore *c);
uint64_t pcore_sequence(const struct pcore *c);     /* the last one used */
int pcore_kept_write_pending(const struct pcore *c);

#ifdef PEER_CORE_TEST
/* Tests only (never compiled into peerd): put the sequence counter where a
 * run of 2^64 frames would have left it. */
void pcore_test_set_sequence(struct pcore *c, uint64_t seq);
#endif

#endif
