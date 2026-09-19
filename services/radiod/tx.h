/*
 * radiod's transmit state machine: one accepted transmission at a time,
 * driven from the service's own loop, completing through one callback.
 *
 * It exists as its own unit for two reasons. The synchronous radio.send and
 * the asynchronous radio.send_async must not be two implementations of
 * transmitting - the second one would be the one that drifts - so both
 * submit here and differ only in who waits. And a state machine that owns
 * the radio for the length of a packet is worth testing without a socket,
 * a daemon or a poll loop around it; tests/radiod_tx_test.c does exactly
 * that.
 *
 * Nothing here knows about JSON, clients or protocols. It is handed bytes
 * and a backend, and it says what happened.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RADIOD_TX_H
#define RADIOD_TX_H

#include "radio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

enum radio_tx_result {
    RADIO_TX_OK = 0,           /* transmitted, and receiving again */
    RADIO_TX_FAILED,           /* the backend did not transmit it */
    RADIO_TX_RX_RESUME_FAILED, /* transmitted, but the radio is not listening */
};

/* The outcome of one accepted transmission, delivered exactly once.
 *
 * `transmitted` and `rx_resumed` are separate on purpose. They answer
 * different questions and a caller usually wants only one of them: a
 * protocol daemon asking "did my packet go out, or must I send it again?"
 * reads `transmitted`, and anything asking "is the radio healthy?" reads
 * `ok`. Collapsing them into a single flag would tell a daemon whose packet
 * went out perfectly well that it had failed, and the answer to that is a
 * retransmission - twice the airtime, twice on the air, for a fault that was
 * in the receive path. */
struct radio_tx_done {
    uint64_t tx_id;
    uint64_t client_id;      /* 0 once the submitter has gone */
    bool async;              /* submitted through the asynchronous path */
    bool transmitted;        /* the bytes went out */
    bool rx_resumed;         /* the radio is in receive mode again */
    bool ok;                 /* transmitted && rx_resumed */
    enum radio_tx_result result;
    size_t bytes;
    double airtime_ms;
    uint64_t mono_ms;        /* completion, CLOCK_MONOTONIC */
    uint64_t timestamp_ms;   /* completion, CLOCK_REALTIME */
    char error[160];         /* empty when ok */
};

typedef void (*radio_tx_done_fn)(const struct radio_tx_done *d, void *user);

struct radio_tx {
    struct radio_backend *be;
    radio_tx_done_fn on_done;
    void *user;
    uint64_t next_id;

    bool active;
    bool begun;              /* tx_begin() took it; tx_poll() drives it */
    bool done_sent;
    uint64_t tx_id;
    uint64_t client_id;
    bool async;
    /* The daemon's own copy of the payload, which is what the backend is
     * given a pointer to. It has to outlive the request it arrived in: the
     * cJSON tree a request was parsed from is freed as soon as the handler
     * returns, which on the asynchronous path is long before the packet
     * leaves the antenna. */
    uint8_t payload[RADIO_MAX_PAYLOAD];
    size_t len;
    double airtime_ms;
    uint64_t started_mono_ms;

    struct radio_tx_done last;
    bool have_last;
};

void radio_tx_init(struct radio_tx *tx, struct radio_backend *be,
                   radio_tx_done_fn on_done, void *user);

bool radio_tx_active(const struct radio_tx *tx);
/* Accepted, but the backend has not been given it yet. The service loop
 * uses this to go straight back round rather than waiting out a poll
 * interval first: the acceptance has been written, and the packet should
 * not sit waiting on a timeout before it starts. */
bool radio_tx_pending_start(const struct radio_tx *tx);
uint64_t radio_tx_active_id(const struct radio_tx *tx);
uint64_t radio_tx_active_client(const struct radio_tx *tx);

/* Accept a transmission and allocate its id.
 *
 * Returns 0 with *tx_id set, -EBUSY when one is already in flight, or
 * -EINVAL for a length this backend cannot carry. Nothing is queued: a
 * refused request is refused, never held back to be sent at some later
 * moment the caller cannot predict. A protocol daemon that wants a queue
 * keeps it on its own side, where it knows its own priorities.
 *
 * The payload is copied before this returns; the caller's buffer is its own
 * again immediately. */
int radio_tx_submit(struct radio_tx *tx, const uint8_t *data, size_t len,
                    uint64_t client_id, bool async, uint64_t *tx_id);

/* Move the active transmission along. Returns 1 while one is in flight, 0
 * when there is nothing to do. Calling it when idle does nothing at all -
 * in particular it never produces a second completion for a job that has
 * already finished. */
int radio_tx_step(struct radio_tx *tx);

/* Drive the active transmission to completion, blocking for the airtime.
 * This is what the synchronous radio.send waits on, and what shutdown uses
 * so a packet already on the air is finished rather than abandoned. */
void radio_tx_run(struct radio_tx *tx);

/* The client that submitted the active transmission has disconnected. The
 * transmission continues - the radio is mid-packet and the bytes are going
 * out whatever anyone now wants - and its completion is reported with
 * client_id 0. Returns true if that was in fact the active job's submitter. */
bool radio_tx_forget_client(struct radio_tx *tx, uint64_t client_id);

/* The most recent completion, for the synchronous caller that waited for it.
 * NULL until one has happened. */
const struct radio_tx_done *radio_tx_last(const struct radio_tx *tx);

/* How long to wait before stepping again, in milliseconds. Only meaningful
 * while a transmission is active. */
#define RADIO_TX_POLL_MS 5

/* NOT IMPLEMENTED, and required before the first backend that drives a
 * transmit from a hardware completion: a deadline on tx_poll.
 *
 * A lost completion - a missed DIO1 edge, a chip that stops answering -
 * makes tx_poll return 0 for ever, and this state machine then stays in the
 * transmit for good: every later send refused BUSY, configure and cad
 * refused, radio.channel reporting nothing, and no event to say why. Only a
 * restart would clear it. The fix is a deadline derived from the expected
 * airtime, after which the transmit completes as failed and the radio is
 * taken back.
 *
 * It is absent because there is nothing to protect yet. The mock completes
 * on a deterministic deadline of its own, and the SX1262 uses the blocking
 * send() whose bound is RadioLib's; no backend reaches the polling path on
 * hardware. Adding it now would mean guessing a tolerance for a backend
 * that does not exist and carrying the guess until one does. See
 * docs/api/radio.md, "Deferred: a completion deadline for asynchronous
 * backends". */

#ifdef __cplusplus
}
#endif

#endif
