/*
 * gatewayd: what bounds the event stream to a client (ADR-019 rule 7 and
 * "Limits"; docs/api/gateway.md §7; THREAT_MODEL T15).
 *
 * meshcored raises a mesh.node on every advert heard and a mesh.activity on
 * every frame. Neither is passed on as it comes:
 *
 *   mesh.node      at most one per node every GW_NODE_EVENT_MS. The first is
 *                  sent at once; what follows inside that time is held, the
 *                  newest replacing the older, and sent when the time is up.
 *                  "removed" is never held and drops what was.
 *   mesh.activity  counted into one-second bins; a bin with something in it
 *                  becomes one mesh.activity_bin. Counts only: no signal
 *                  figure and no frame leaves through here.
 *
 * Pure: cJSON only, the clock is passed in.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_FEED_H
#define GATEWAYD_GW_FEED_H

#include "gw.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define GW_FEED_NODES 128
#define GW_FEED_TYPES 16
#define GW_FEED_TYPE_MAX 23

struct gw_feed_node {
    bool used;
    char key[65];        /* the node's public key, hex */
    int64_t last_ms;     /* when an event for it was last let through */
    cJSON *held;
};

struct gw_feed_bin {
    bool any;
    int64_t start_ms;
    double rx_frames, rx_bytes, tx_ok, tx_failed, tx_unknown;
    int types;
    struct {
        char name[GW_FEED_TYPE_MAX + 1];
        double n;
    } type[GW_FEED_TYPES];
};

struct gw_feed {
    struct gw_feed_node node[GW_FEED_NODES];
    struct gw_feed_bin bin;
};

void gw_feed_init(struct gw_feed *f);
/* Drops everything held and the open bin. */
void gw_feed_reset(struct gw_feed *f);

/* One mesh.node event's data, {reason, node}, already filtered; taken.
 * Returns it when it goes out now, NULL when it was held or is not usable
 * (no reason, no 64-hex public_key). */
cJSON *gw_feed_node(struct gw_feed *f, cJSON *data, int64_t now_ms);
/* A held event whose time has come, or NULL. Call until NULL. */
cJSON *gw_feed_node_due(struct gw_feed *f, int64_t now_ms);

/* One mesh.activity event's data as meshcored sent it; not taken. */
void gw_feed_activity(struct gw_feed *f, const cJSON *data, int64_t now_ms);
/* The data of mesh.activity_bin when the open bin is a second old, or NULL. */
cJSON *gw_feed_activity_due(struct gw_feed *f, int64_t now_ms);

#endif
