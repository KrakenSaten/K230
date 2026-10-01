/*
 * meshcored: the MeshCore protocol service for Doors.
 *
 * It owns the MeshCore runtime and nothing below it. radiod owns the radio
 * (ADR-002): meshcored opens no SPI device, drives no GPIO, links no RadioLib
 * and never touches an SX1262. Every byte in and out goes over radiod's
 * generic radio.* IPC (docs/api/radio.md).
 *
 *     SX1262  ->  radiod  ->  [radio.* IPC]  ->  meshcored  ->  [mesh.* IPC]
 *
 * This header is the daemon's own shared vocabulary: the service state, the
 * configuration, the counters and the one struct the four C files pass
 * around. The MeshCore protocol itself lives behind mesh_runtime.h, which is
 * the only door between this JSON-speaking daemon and the C++ protocol core -
 * no MeshCore header is included on this side, and no cJSON on that one.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MCD_H
#define MCD_H

#include "mesh_runtime.h"
#include "tx_map.h"

#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"

#include <stdbool.h>
#include <stdint.h>

#define MCD_API_VERSION 0
#define MCD_SERVICE_NAME "meshcored"

/* ---- service state (docs/api/mesh.md) ---------------------------------
 *
 * One enum, one writer (mcd_set_state), and every transition announced as a
 * mesh.state event. The order is the start-up order, so a reader can tell
 * "not there yet" from "was there and lost it" by the reason rather than by
 * guessing from the value.
 */
enum mcd_state {
    MCD_STARTING = 0,       /* before the first connection attempt */
    MCD_WAITING_FOR_RADIOD, /* not connected; retrying with backoff */
    MCD_WAITING_FOR_LEASE,  /* connected; the radio lease is held elsewhere */
    MCD_CONFIGURING,        /* lease held; applying the profile, subscribing */
    MCD_ONLINE,             /* profile applied, subscribed, lease held */
    MCD_DEGRADED,           /* attached, but the radio is not usable */
    MCD_ERROR,              /* radiod refused the profile; retrying will not help */
    MCD_STATE_COUNT
};

const char *mcd_state_name(enum mcd_state s);

/* ---- the radio profile -------------------------------------------------
 *
 * The MeshCore profile proven on air by the accepted P0 gate
 * (docs/hardware/MESHCORE_INTEROP_GATE.md). The power is a service value
 * with the tested default, not a regulatory policy: duty cycle, ERP and
 * sub-band rules belong above the raw radio backend and stay the operator's
 * (docs/api/radio.md, "Region guard").
 */
#define MCD_DEFAULT_FREQUENCY_MHZ 869.618
#define MCD_DEFAULT_BANDWIDTH_KHZ 62.5
#define MCD_DEFAULT_SPREADING_FACTOR 8
#define MCD_DEFAULT_CODING_RATE 5
#define MCD_DEFAULT_SYNC_WORD 0x12
#define MCD_DEFAULT_PREAMBLE 32
#define MCD_DEFAULT_TX_POWER_DBM 2

struct mcd_profile {
    double frequency_mhz;
    double bandwidth_khz;
    int spreading_factor;
    int coding_rate;
    int sync_word;
    int preamble_length;
    int tx_power_dbm;
    bool crc;
};

void mcd_profile_defaults(struct mcd_profile *p);
/* Range check before radiod ever sees it, so a typo in /etc/default/meshcored
 * is refused at start-up rather than at the first configure. Returns 0, or -1
 * with why written to err. */
int mcd_profile_validate(const struct mcd_profile *p, char *err, size_t errlen);

/* ---- reconnect backoff -------------------------------------------------
 *
 * Bounded, doubling, and reset by a success. meshcored may come up before
 * radiod does (BusyBox init starts S60radiod before anything that would use
 * it, but nothing guarantees the socket is listening), and radiod may go away
 * at any moment. Neither is an error; both are waited out.
 */
#define MCD_BACKOFF_MIN_MS 500
#define MCD_BACKOFF_MAX_MS 30000

struct mcd_backoff {
    uint32_t delay_ms;
    uint64_t next_attempt_ms;
};

void mcd_backoff_reset(struct mcd_backoff *b);
/* Record a failed attempt at now_ms and schedule the next one. */
void mcd_backoff_failed(struct mcd_backoff *b, uint64_t now_ms);
bool mcd_backoff_due(const struct mcd_backoff *b, uint64_t now_ms);

/* ---- counters ----------------------------------------------------------
 *
 * Everything meshcored reports about itself comes from here. They are
 * deliberately separate from the MeshCore dispatcher's own counters: this
 * daemon counts what radiod told it, not what the protocol library believes.
 */
struct mcd_counters {
    uint64_t rx_events;        /* radio.rx events seen */
    uint64_t rx_delivered;     /* frames handed to the MeshCore runtime */
    uint64_t rx_rejected;      /* malformed: bad hex, empty, oversized, no field */
    uint64_t rx_dropped;       /* well formed, but the receive queue was full */
    uint64_t tx_submitted;     /* radio.send_async requests written */
    uint64_t tx_accepted;      /* ...that radiod answered with a tx_id */
    uint64_t tx_refused;       /* ...that radiod refused (BUSY, no lease, bad payload) */
    uint64_t tx_ok;            /* tx_done: transmitted and receiving again */
    uint64_t tx_rx_resume_failed; /* tx_done: transmitted, radio not receiving */
    uint64_t tx_failed;        /* tx_done: not transmitted */
    uint64_t tx_unknown;       /* the connection went away before tx_done */
    uint64_t tx_done_unmatched;/* tx_done for a tx_id this daemon does not hold */
    uint64_t radiod_connects;
    uint64_t radiod_disconnects;
    uint64_t lease_acquired;
    uint64_t lease_refused;
    uint64_t lease_lost;
};

/* ---- configuration ----------------------------------------------------- */

#define MCD_NAME_MAX 32
#define MCD_PATH_MAX 512

struct mcd_config {
    char socket_name[64];      /* our own service socket (default "meshcored") */
    char radiod_socket[64];    /* the radiod socket to talk to (default "radiod") */
    char state_dir[MCD_PATH_MAX]; /* persistent state; default <state>/meshcored */
    char node_name[MCD_NAME_MAX]; /* advert name; empty = derive from the key */
    bool node_name_given;
    struct mcd_profile profile;
    bool verbose;
};

/* ---- the daemon --------------------------------------------------------- */

struct mcd_radio_link;  /* radio_link.h */

struct mcd {
    struct mcd_config cfg;
    enum mcd_state state;
    char state_reason[96];
    uint64_t state_since_ms;
    uint64_t start_ms;
    /* Names this run of the service, for mesh.status. App datagram ids
     * (mesh.app_inbox) start again from 1 on every run, and a client that
     * catches up by id resets its cursor when this changes. */
    char run_id[17];

    struct pocketipc_server *server;
    struct mcd_radio_link *link;
    struct mcd_runtime *rt;
    /* Holds the flock on the state directory for the life of the process
     * (mcd_runtime_lock_state_dir). -1 when not held. */
    int state_lock_fd;
    /* Holds the flock on <runtime>/<socket name>.lock, so one process at a
     * time serves that socket whatever state directory it was given. */
    int socket_lock_fd;

    struct mcd_counters counters;

    /* What radiod last told us about itself, for mesh.status. Absent rather
     * than invented when we have not been told. */
    bool radio_state_known;
    char radio_state[16];
    bool profile_applied;
    struct mcd_profile applied;
};

/* main.c */
uint64_t mcd_mono_ms(void);
void mcd_set_state(struct mcd *d, enum mcd_state s, const char *reason);
void mcd_broadcast(struct mcd *d, cJSON *event);

/* api.c */
void mcd_handle_request(struct pocketipc_server *s, struct pocketipc_client *c,
                        cJSON *req, void *user);
/* Event builders, so main.c and radio_link.c raise the same shapes. */
cJSON *mcd_event_state(const struct mcd *d);
cJSON *mcd_event_node(const struct mcd_node *n, const char *reason);
cJSON *mcd_event_message(const struct mcd_message *m);
cJSON *mcd_event_channel(const struct mcd_channel *c, const char *reason);
cJSON *mcd_event_activity_rx(const struct mcd_rx_meta *meta, int bytes, const char *outcome);
cJSON *mcd_event_activity_tx(uint64_t submit_id, int bytes, const char *result, uint64_t mono_ms);
cJSON *mcd_event_app(const struct mcd_app_datagram *dg);

#endif /* MCD_H */
