/*
 * gatewayd: a pocketipc client that never blocks (ADR-019 rules 4 and 7;
 * docs/design/companion/CONTRACTS.md "Upstream").
 *
 * One connection to one service, kept for as long as the gateway runs: it
 * reconnects with backoff, reads everything the service sends as it comes -
 * whether or not a client is connected, so the service's own backpressure
 * rule never has cause to fire against the gateway - and matches answers to
 * requests by id, each request with a deadline of its own.
 *
 * Nothing here waits: the connect either succeeds at once or is tried again
 * later, a write that does not fit is finished by a later tick, and a
 * request whose answer does not come is answered GW_ERR_UPSTREAM when its
 * deadline passes (a late answer is then ignored, by its id).
 *
 * It decides nothing about what may be called: the caller names the method
 * (gw_policy.c's table and three calls of the gateway's own).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_UPLINK_H
#define GATEWAYD_GW_UPLINK_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define GW_UPLINK_PENDING 24
#define GW_UPLINK_OUT_MAX (64u * 1024u)
#define GW_UPLINK_FLUSH_MS 2000       /* unsent output older than this: the link is dropped */
#define GW_UPLINK_BACKOFF_MIN_MS 250
#define GW_UPLINK_BACKOFF_MAX_MS 4000

struct gw_uplink;

struct gw_uplink_cb {
    /* The socket is connected (up), or was lost. On a loss every request
     * still waiting has been answered GW_ERR_UPSTREAM first. */
    void (*state)(void *user, bool up);
    /* One event of the service. */
    void (*event)(void *user, const char *name, const cJSON *data);
    /* The answer to the request with this tag: its result, or NULL with the
     * service's error code (1 to 6) or GW_ERR_UPSTREAM. */
    void (*reply)(void *user, uint64_t tag, const cJSON *result, int code);
};

struct gw_uplink *gw_uplink_new(const char *service, const struct gw_uplink_cb *cb, void *user);
void gw_uplink_free(struct gw_uplink *u);

/* The socket to watch for reading, or -1 while not connected. It changes
 * with every reconnect: read it again after a state callback. */
int gw_uplink_fd(const struct gw_uplink *u);
bool gw_uplink_connected(const struct gw_uplink *u);

/* The socket is readable. Callbacks run from here. */
void gw_uplink_io(struct gw_uplink *u, int64_t now_ms);
/* Connect attempts, unsent output, deadlines. Callbacks run from here. */
void gw_uplink_tick(struct gw_uplink *u, int64_t now_ms);

/* Sends one request; params is taken (may be NULL). 0 when it is on its
 * way: reply() will be called exactly once for the tag. -1 when it is not
 * (not connected, too many waiting, no room): reply() is not called. No
 * callback runs from inside this call. */
int gw_uplink_call(struct gw_uplink *u, const char *method, cJSON *params, uint64_t tag, int64_t now_ms,
                   int deadline_ms);

/* Gives the connection up now (the service answered something that cannot
 * be worked with); it is made again after the backoff. Safe from a
 * callback. */
void gw_uplink_drop(struct gw_uplink *u, int64_t now_ms);

#endif
