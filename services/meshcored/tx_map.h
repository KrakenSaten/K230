/*
 * The map between a MeshCore transmit and a radiod one.
 *
 * Three identities are in play and they are not the same thing:
 *
 *   submit_id  meshcored's own, allocated when the protocol core hands over
 *              a frame. It is what the runtime is told the outcome for, and
 *              it exists before radiod has said anything at all.
 *   request id the pocketipc id of the radio.send_async request.
 *   tx_id      radiod's, which arrives in that request's reply and names the
 *              radio.tx_done event that will follow.
 *
 * Keeping them apart is what makes the awkward cases answerable. A tx_done
 * for a tx_id this daemon does not hold is a stale event from a transmit that
 * has already been accounted for - or from a radiod that restarted - and is
 * counted and dropped rather than reported as somebody's completion. A second
 * tx_done for the same tx_id is the same thing. And a submission whose reply
 * never comes because the connection went away is resolved as
 * MCD_TX_UNKNOWN: the outcome cannot be known, which is a different answer
 * from "it failed", and the one that does not cause a retransmission.
 *
 * radiod transmits one packet at a time and refuses a second with BUSY
 * (docs/api/radio.md, "One at a time"), and the MeshCore dispatcher likewise
 * holds one outbound packet at a time. The table is therefore small; it is a
 * table rather than a single slot so that a late reply or a duplicate
 * completion from the previous transmit cannot be mistaken for the current
 * one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MCD_TX_MAP_H
#define MCD_TX_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define MCD_TX_MAP_SLOTS 8

/* ---- the completion deadline -------------------------------------------
 *
 * radiod promises exactly one radio.tx_done per accepted request on every
 * outcome path, and it keeps that promise - except across a restart, where
 * it has no transmit state to deliver a completion from and says so
 * (docs/api/radio.md, "Restart"). Its own documentation also records a
 * deferred item: a backend that lost a hardware completion would leave it in
 * `tx` for ever, with no event to say why (docs/KNOWN_ISSUES.md, "No
 * completion deadline for an asynchronous backend").
 *
 * Either way the packet stops being this service's problem only if something
 * here decides it has. Without a deadline one lost completion is permanent:
 * the slot stays taken, every later submission is refused because one is
 * outstanding, and the node goes quiet for the rest of the session with
 * nothing in the log to explain it. That is not hypothetical - it is what
 * tests/meshcored_harness_test.sh found when it was first asked to drop a
 * completion.
 *
 * So a submission has a bound, and passing it resolves the transmit as
 * UNKNOWN: not as failed, because the bytes may well have gone out, and an
 * outcome of "failed" is the one that invites a retransmission.
 *
 * The bound is generous on purpose. Acceptance is answered before the radio
 * is touched, so five seconds for it is several orders of magnitude of
 * slack. Once accepted, the transmit is bounded by its own time on air,
 * which radiod computes and reports, so the deadline is three times that
 * plus two seconds - long enough that a slow completion is never mistaken
 * for a lost one.
 */
#define MCD_TX_ACCEPT_DEADLINE_MS 5000
#define MCD_TX_MIN_FLIGHT_DEADLINE_MS 5000
#define MCD_TX_FLIGHT_SLACK_MS 2000

enum mcd_tx_phase {
    MCD_TXP_FREE = 0,
    MCD_TXP_AWAITING_ACCEPT, /* the request is written; no tx_id yet */
    MCD_TXP_IN_FLIGHT        /* radiod gave us a tx_id; waiting for tx_done */
};

struct mcd_tx_slot {
    enum mcd_tx_phase phase;
    uint64_t submit_id;
    int request_id;
    uint64_t tx_id;
    int bytes;
    uint64_t submitted_ms;
    uint64_t deadline_ms;
    double airtime_ms;
};

struct mcd_tx_map {
    struct mcd_tx_slot slots[MCD_TX_MAP_SLOTS];
    uint64_t next_submit_id;
};

void mcd_tx_map_init(struct mcd_tx_map *m);

/* Take a slot for a submission that is about to be written. Returns the new
 * submit_id, or 0 when every slot is taken - which the caller must treat as
 * "cannot send now", not as a silent drop. */
uint64_t mcd_tx_map_submit(struct mcd_tx_map *m, int request_id, int bytes, uint64_t now_ms);

/* The reply to that request arrived and radiod accepted it. airtime_ms is
 * what radiod said the packet will take on the air, and sets the completion
 * deadline; pass 0 when it did not say. Returns the submit_id, or 0 if no
 * slot is waiting on this request id. */
uint64_t mcd_tx_map_accepted(struct mcd_tx_map *m, int request_id, uint64_t tx_id,
                             double airtime_ms, uint64_t now_ms);

/* The reply arrived and radiod refused it. Returns the submit_id (whose slot
 * is now free) or 0. */
uint64_t mcd_tx_map_refused(struct mcd_tx_map *m, int request_id);

/* A radio.tx_done for tx_id. Returns the submit_id and frees the slot, or 0
 * when no slot holds that tx_id - a stale or duplicate completion. */
uint64_t mcd_tx_map_completed(struct mcd_tx_map *m, uint64_t tx_id);

/* The connection went away. Every outstanding submission is resolved: each
 * one's submit_id is written to out[] (which must hold MCD_TX_MAP_SLOTS) and
 * the count is returned. The table is empty afterwards. */
int mcd_tx_map_abandon_all(struct mcd_tx_map *m, uint64_t *out);

/* Resolve every submission whose deadline has passed. Their submit_ids are
 * written to out[] (which must hold MCD_TX_MAP_SLOTS) and the count is
 * returned; their slots are freed. The caller reports each one as
 * MCD_TX_UNKNOWN - the outcome is not known, which is a different answer
 * from "it failed" and the one that does not cause a retransmission. */
int mcd_tx_map_expire(struct mcd_tx_map *m, uint64_t now_ms, uint64_t *out);

/* How many submissions are outstanding. */
int mcd_tx_map_outstanding(const struct mcd_tx_map *m);
/* The bytes recorded for a submission still in the table, or -1. */
int mcd_tx_map_bytes(const struct mcd_tx_map *m, uint64_t submit_id);

#endif /* MCD_TX_MAP_H */
