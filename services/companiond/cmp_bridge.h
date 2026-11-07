/*
 * companiond's bridge: MeshCore companion commands from one app on one side,
 * meshcored's mesh.* (docs/api/mesh.md) on the other.
 *
 * meshcored stays the only owner of the radio, the identity, the contacts and
 * the channels. This module holds nothing of the mesh but what the companion
 * protocol needs to remember about its own conversation with the app:
 *
 *   - the received messages the app has not fetched yet (the firmware's
 *     "offline queue"), filled from mesh.message events whether or not an
 *     app is connected, and kept across the app's reconnects;
 *   - the last CMP_SEND_RECORDS messages the app sent, so a frame the app
 *     sends again (after a reconnect, or as its own retry) is answered from
 *     the record instead of putting a second copy on the air, and so an ACK
 *     meshcored reports can be told to the app under the tag it was given.
 *
 * Everything else is asked of meshcored at the moment the app asks: the
 * identity, the node list, the channel table. A copy kept here would go
 * stale the moment RIFT renamed the node or joined a channel.
 *
 * The module does no I/O of its own. The caller supplies a frame sink for the
 * app, a request writer for meshcored, and the clocks (struct cmp_io), and
 * feeds it what arrives. That is what lets tests/companiond_e2e_test.py and
 * the device run the same code: main.c is only the sockets.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CMP_BRIDGE_H
#define CMP_BRIDGE_H

#include "cmp_wire.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Received messages held for the app (upstream: OFFLINE_QUEUE_SIZE 16). When
 * full, the oldest channel message makes room, as upstream does; with no
 * channel message held, the oldest direct message does (upstream would drop
 * the new one instead). Every eviction is counted and logged. */
#define CMP_QUEUE_LEN 64
/* Sent messages remembered for duplicates and ACKs. */
#define CMP_SEND_RECORDS 16
/* Message ids already queued, so a repeated mesh.message is not queued twice. */
#define CMP_SEEN_IDS 64
/* How long a command may wait for meshcored before the app is told it failed
 * and the meshcored connection is dropped (a reply that arrives later must
 * not be read as the answer to the next request: docs/api/pocketipc.md). */
#define CMP_MESH_TIMEOUT_MS 5000
/* Longest direct message text mesh.send takes (docs/api/mesh.md). */
#define CMP_TEXT_MAX 160

struct cmp_io {
    void *user;
    /* Queue one frame payload for the app. Returns 0, or -1 when there is no
     * app or its output is over the limit (the caller then drops the app). */
    int (*to_app)(void *user, const uint8_t *payload, size_t len);
    /* Write one pocketipc request to meshcored. params is consumed. Returns 0
     * or -1 (the caller then drops the meshcored connection). */
    int (*mesh_call)(void *user, uint32_t id, const char *method, cJSON *params);
    uint64_t (*mono_ms)(void *user); /* CLOCK_MONOTONIC, as meshcored's mono_ms */
    uint32_t (*wall_s)(void *user);  /* the system clock, read and never set */
};

struct cmp_queued {
    uint8_t is_channel;
    uint8_t channel;                 /* slot, channel only */
    uint8_t prefix[6];               /* sender key prefix, direct only */
    uint32_t timestamp;              /* the sender's */
    int8_t snr_x4;
    char text[CMP_MAX_FRAME + 1];
};

struct cmp_send_rec {
    bool used;
    bool is_channel;
    uint8_t channel;
    uint8_t prefix[6];
    uint32_t app_timestamp;
    uint8_t attempt;
    uint16_t text_len;
    char text[CMP_TEXT_MAX + 1];
    /* direct only */
    uint32_t message_id;
    uint32_t tag;                    /* expected_ack the app was given */
    uint32_t timeout_ms;
    bool flood;
    uint64_t sent_mono_ms;
    bool acked;
    uint64_t ack_mono_ms;            /* meshcored's, when the ACK matched */
    bool confirm_pending;            /* acked while no app was listening */
    uint64_t age;                    /* for replacing the oldest */
};

enum cmp_wait {
    CMP_WAIT_NONE = 0,
    CMP_WAIT_SELF_INFO,
    CMP_WAIT_DQ_CHANNELS,
    CMP_WAIT_DQ_PATH_HASH,
    CMP_WAIT_CONTACTS,
    CMP_WAIT_CONTACT_BY_KEY,
    CMP_WAIT_CHANNEL,
    CMP_WAIT_SEND_DM,
    CMP_WAIT_RESEND_DM,
    CMP_WAIT_CHAN_LOOKUP,
    CMP_WAIT_SEND_CHAN,
};

struct cmp_bridge {
    struct cmp_io io;

    /* meshcored */
    bool mesh_up;
    bool mesh_ready;                 /* status and info answered */
    bool mesh_reset_wanted;          /* main drops and reconnects */
    uint32_t next_id;
    uint32_t id_status, id_info, id_subscribe;
    char run_id[40];
    bool have_profile;
    double freq_mhz, bw_khz;
    int sf, cr, tx_power_dbm;
    char version[24];

    /* the app */
    bool app_up;
    bool app_started;                /* answered APP_START on this connection */
    uint8_t app_target_ver;

    /* the one command waiting for meshcored */
    enum cmp_wait wait;
    uint32_t wait_id;
    uint64_t wait_since;
    int wait_rec;                    /* send record, for the send kinds */
    uint8_t cmd[CMP_MAX_FRAME + 1];
    size_t cmd_len;
    uint8_t dq_max_channels;

    /* contact iteration */
    cJSON *iter_nodes;
    int iter_at;
    uint32_t iter_lastmod;

    struct cmp_queued queue[CMP_QUEUE_LEN];
    int q_head, q_len;
    uint32_t seen[CMP_SEEN_IDS];
    int seen_at;

    struct cmp_send_rec sends[CMP_SEND_RECORDS];
    uint64_t send_age;
    uint32_t next_tag;

    /* counters, for the log and the tests */
    uint32_t n_queued, n_evicted, n_synced, n_sent_dm, n_sent_chan, n_dup_suppressed,
        n_resent, n_confirmed, n_refused;
};

void cmp_bridge_init(struct cmp_bridge *b, const struct cmp_io *io, uint32_t tag_seed);
void cmp_bridge_free(struct cmp_bridge *b);

/* meshcored connection came up: asks for status, info and the subscription. */
void cmp_bridge_mesh_up(struct cmp_bridge *b);
/* meshcored connection went away: a command waiting is answered with an error. */
void cmp_bridge_mesh_down(struct cmp_bridge *b);
/* One frame from meshcored (response or event). Not consumed. */
void cmp_bridge_mesh_frame(struct cmp_bridge *b, const cJSON *msg);

void cmp_bridge_app_up(struct cmp_bridge *b);
void cmp_bridge_app_down(struct cmp_bridge *b);
/* One whole command frame from the app. Call only when !cmp_bridge_busy(). */
void cmp_bridge_app_frame(struct cmp_bridge *b, const uint8_t *frame, size_t len);

/* A command is waiting for meshcored: read no further frames from the app
 * until it is answered, so replies keep the order of the commands. */
bool cmp_bridge_busy(const struct cmp_bridge *b);

/* Periodic work: the request deadline, and contact frames while the caller
 * says the app's output has room (room is how many more frames it takes). */
void cmp_bridge_tick(struct cmp_bridge *b, int room);

#endif
