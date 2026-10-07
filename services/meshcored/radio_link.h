/*
 * meshcored's side of the radiod relationship.
 *
 * This is a pocketipc *client* that must not stop answering its own clients
 * while it waits for one, which the library's pocketipc_call() cannot do: it
 * blocks until the reply comes and discards every event that arrives
 * meanwhile (docs/api/pocketipc.md). For a daemon whose whole input is
 * radio.rx events, discarding events while waiting for a reply would throw
 * away received packets. So this file drives the connection itself - one
 * non-blocking socket, requests written with ids and remembered, replies and
 * events sorted out as they arrive - using pocketipc's framing primitives
 * and none of its synchronous ones.
 *
 * It is also where the connection state machine lives:
 *
 *   disconnected -> connect -> radio.acquire -> radio.configure
 *                -> radio.subscribe -> radio.status -> ready
 *
 * Every step can fail, and each failure has a different answer. A refused
 * lease is somebody else's radio and is waited for; a refused profile is a
 * policy decision that retrying will not change; a closed socket is radiod
 * restarting and the whole sequence runs again from the top. Nothing here
 * ever takes the radio from another owner.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MCD_RADIO_LINK_H
#define MCD_RADIO_LINK_H

#include <stdbool.h>
#include <stdint.h>

struct mcd;

struct mcd_radio_link *mcd_link_new(struct mcd *d);
void mcd_link_free(struct mcd_radio_link *l);

/* The socket to add to the daemon's poll, or -1 while disconnected. */
int mcd_link_fd(const struct mcd_radio_link *l);

/* Read and dispatch whatever radiod has sent. Call when the fd is readable. */
void mcd_link_readable(struct mcd_radio_link *l);

/* Move the connection state machine on: connect when the backoff is due,
 * retry an acquire that was refused, and so on. Call once per turn. */
void mcd_link_step(struct mcd_radio_link *l, uint64_t now_ms);

/* Submit a frame for transmission with radio.send_async. Returns 0 with
 * *submit_id set, or -1 when it cannot be submitted at all - not ready, a
 * transmit already outstanding, or the write failed. No queue: radiod has
 * none either, and a daemon that silently held a packet back would be
 * transmitting at a moment its caller cannot predict. */
int mcd_link_submit_tx(struct mcd_radio_link *l, const uint8_t *bytes, int len,
                       uint64_t *submit_id);

/* Is the radio usable right now (lease held, profile applied, subscribed)? */
bool mcd_link_ready(const struct mcd_radio_link *l);

/* Give the radio back and close the connection cleanly, for shutdown. */
void mcd_link_shutdown(struct mcd_radio_link *l);

/* For mesh.status: how radiod named the lease we hold, when we hold one. */
bool mcd_link_lease_held(const struct mcd_radio_link *l);
uint64_t mcd_link_lease_owner_id(const struct mcd_radio_link *l);
bool mcd_link_connected(const struct mcd_radio_link *l);

#endif /* MCD_RADIO_LINK_H */
