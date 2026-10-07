/*
 * PocketFleet multiplayer transport seam (docs/apps/FLEET_MULTIPLAYER.md,
 * "Layers").
 *
 * The match state machine (apps/fleet/net) produces and consumes packets; a
 * link carries them to another node and brings theirs back. Two links exist:
 *
 *   fleet_link_mesh.c  the real one: meshcored's mesh.app_* over pocketipc,
 *                      and nothing else - Fleet never reaches radiod or the
 *                      radio (ADR-002, ADR-008);
 *   fleet_link_loop.c  a virtual opponent in the same process, playing the
 *                      real protocol with PocketFleet's AI over a channel that
 *                      can lose, duplicate and delay. A development aid and
 *                      the fake transport of P3; only POCKETFLEET_MP_FAKE
 *                      turns it on.
 *
 * Every call returns at once. A link never blocks the LVGL thread.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_LINK_H
#define POCKETFLEET_LINK_H

#include "../net/fleet_proto.h"

#include <stddef.h>
#include <stdint.h>

#define FLEET_LINK_NAME_MAX 32
#define FLEET_LINK_PEERS 8

/* Whether packets can move at all, as the player needs to hear it. */
enum fleet_link_state {
    FLEET_LINK_UP = 0,          /* the mesh service is there and online */
    FLEET_LINK_CONNECTING,      /* looking for the mesh service */
    FLEET_LINK_NO_SERVICE,      /* nothing answers: meshcored is not running */
    FLEET_LINK_RADIO_OFF,       /* the service is there, the radio is not usable */
    FLEET_LINK_STATE_COUNT
};

/* A node the player could invite: one the mesh service has heard. */
struct fleet_link_peer {
    uint8_t key[FLEET_KEY_BYTES];
    char name[FLEET_LINK_NAME_MAX];
    int64_t heard_ms;           /* our CLOCK_MONOTONIC; 0 when not heard this run */
    int hops;                   /* -1 when no route is known */
};

enum fleet_link_send {
    FLEET_LINK_SENT = 0,        /* handed to the service */
    FLEET_LINK_BUSY = 1,        /* refused, nothing went out: try again shortly */
    FLEET_LINK_FAILED = -1,     /* no service, or it refused the packet */
};

struct fleet_link_ops {
    /* Do whatever I/O is due. now_ms is CLOCK_MONOTONIC. */
    void (*poll)(void *ctx, int64_t now_ms);
    enum fleet_link_send (*send)(void *ctx, const uint8_t to[FLEET_KEY_BYTES],
                                 const uint8_t *buf, size_t n);
    /* Take one received packet. Returns 1 and fills from/buf/n, or 0. */
    int (*recv)(void *ctx, uint8_t from[FLEET_KEY_BYTES], uint8_t *buf, size_t *n);
    /* This node's key. Returns 0 once it is known. */
    int (*self_key)(void *ctx, uint8_t key[FLEET_KEY_BYTES]);
    enum fleet_link_state (*state)(void *ctx);
    /* Up to max nodes, most recently heard first. */
    int (*peers)(void *ctx, struct fleet_link_peer *out, int max);
    /* The name of a node, or NULL when it is not known. */
    const char *(*peer_name)(void *ctx, const uint8_t key[FLEET_KEY_BYTES]);
    /* Make this node known to those in range (a zero-hop advert). Returns 0
     * when the request went out. Only ever called from a button. */
    int (*advertise)(void *ctx);
    /* The service's timeout estimate for the last packet sent, or 0. */
    uint32_t (*retry_base)(void *ctx);
    void (*close)(void *ctx);
};

struct fleet_link {
    const struct fleet_link_ops *ops;
    void *ctx;
};

const char *fleet_link_state_text(enum fleet_link_state state);

#endif
