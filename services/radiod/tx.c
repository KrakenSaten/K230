/*
 * radiod's transmit state machine. See tx.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "tx.h"

#include "airtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void radio_tx_init(struct radio_tx *tx, struct radio_backend *be,
                   radio_tx_done_fn on_done, void *user)
{
    memset(tx, 0, sizeof(*tx));
    tx->be = be;
    tx->on_done = on_done;
    tx->user = user;
    /* Ids start at 1 so 0 can mean "no transmission" everywhere. */
    tx->next_id = 1;
}

bool radio_tx_active(const struct radio_tx *tx)
{
    return tx->active;
}

bool radio_tx_pending_start(const struct radio_tx *tx)
{
    return tx->active && !tx->begun;
}

uint64_t radio_tx_active_id(const struct radio_tx *tx)
{
    return tx->active ? tx->tx_id : 0;
}

uint64_t radio_tx_active_client(const struct radio_tx *tx)
{
    return tx->active ? tx->client_id : 0;
}

const struct radio_tx_done *radio_tx_last(const struct radio_tx *tx)
{
    return tx->have_last ? &tx->last : NULL;
}

/* Is the transceiver listening? A backend without is_receiving() cannot be
 * asked, and the established reading of that (see update_rx_state in main.c)
 * is that it always receives when it is not transmitting. */
static bool rx_is_up(struct radio_tx *tx)
{
    if (!tx->be->ops->is_receiving) {
        return true;
    }
    return tx->be->ops->is_receiving(tx->be) != 0;
}

/* Finish the active job exactly once and hand the outcome to the owner.
 *
 * The guard is not decoration. There are three ways into here - the poll
 * said it was finished, the blocking send returned, the backend refused -
 * and the loop that drives them all runs again immediately afterwards. A
 * second completion for one accepted transmission would be counted twice in
 * the airtime statistics, and a protocol daemon matching completions to
 * requests by tx_id would see an answer to a question it had already had. */
static void tx_complete(struct radio_tx *tx, bool transmitted, const char *err)
{
    struct radio_tx_done d;

    if (!tx->active || tx->done_sent) {
        return;
    }
    tx->done_sent = true;

    memset(&d, 0, sizeof(d));
    d.tx_id = tx->tx_id;
    d.client_id = tx->client_id;
    d.async = tx->async;
    d.bytes = tx->len;
    d.airtime_ms = tx->airtime_ms;
    d.transmitted = transmitted;
    /* Asked here, after the backend has finished with the radio and had its
     * chance to go back to receiving, and before anyone is told the transmit
     * is done. A completion that arrives while the radio is still keyed - or
     * while nobody has checked whether it came back - is the report a
     * protocol daemon would act on by transmitting again. */
    d.rx_resumed = rx_is_up(tx);
    d.ok = d.transmitted && d.rx_resumed;
    if (!d.transmitted) {
        d.result = RADIO_TX_FAILED;
    } else if (!d.rx_resumed) {
        d.result = RADIO_TX_RX_RESUME_FAILED;
    } else {
        d.result = RADIO_TX_OK;
    }
    if (!d.ok) {
        if (err && err[0]) {
            snprintf(d.error, sizeof(d.error), "%s", err);
        } else if (d.result == RADIO_TX_RX_RESUME_FAILED) {
            snprintf(d.error, sizeof(d.error),
                     "transmitted, but the transceiver did not return to receive");
        } else {
            snprintf(d.error, sizeof(d.error), "transmit failed");
        }
    }
    d.mono_ms = radio_mono_ms();
    d.timestamp_ms = radio_now_ms();

    tx->last = d;
    tx->have_last = true;
    /* Freed before the callback runs, so a callback that wants to submit the
     * next transmission straight away can. */
    tx->active = false;
    tx->begun = false;
    if (tx->on_done) {
        tx->on_done(&d, tx->user);
    }
}

int radio_tx_submit(struct radio_tx *tx, const uint8_t *data, size_t len,
                    uint64_t client_id, bool async, uint64_t *tx_id)
{
    struct radio_caps caps;
    const struct radio_profile *p;
    double airtime;

    if (tx->active) {
        return -EBUSY;
    }
    tx->be->ops->get_caps(tx->be, &caps);
    if (len == 0 || len > sizeof(tx->payload) ||
        (caps.max_payload > 0 && len > (size_t)caps.max_payload)) {
        return -EINVAL;
    }

    memset(tx->payload, 0, sizeof(tx->payload));
    memcpy(tx->payload, data, len);
    tx->len = len;
    tx->client_id = client_id;
    tx->async = async;
    tx->tx_id = tx->next_id++;
    tx->done_sent = false;
    tx->begun = false;
    tx->started_mono_ms = radio_mono_ms();

    /* The expected time on air, from the profile the packet is going out
     * under. Both backends compute exactly this inside their own send(), so
     * it is the same number, available early enough to tell the submitter
     * what it has committed to. A profile the formula cannot evaluate would
     * have been refused by radio.configure long before this; it is recorded
     * as zero rather than guessed at. */
    p = &tx->be->profile;
    airtime = lora_airtime_ms(p->spreading_factor, p->bandwidth_khz, p->coding_rate,
                              p->preamble_length, len, p->crc, false);
    tx->airtime_ms = airtime > 0 ? airtime : 0.0;

    tx->active = true;
    if (tx_id) {
        *tx_id = tx->tx_id;
    }
    return 0;
}

/* The blocking path: the backend has no asynchronous transmit, or declined
 * one. This is what the SX1262 does - RadioLib's transmit() does not return
 * until the packet has left - and the daemon is unresponsive for the airtime
 * exactly as it was before any of this existed. What has changed is where
 * the wait happens: from the service's loop rather than from inside the
 * request handler, so the submitter has already had its answer. */
static void tx_run_blocking(struct radio_tx *tx)
{
    char err[160] = "";
    double airtime = 0.0;
    int rc = tx->be->ops->send(tx->be, tx->payload, tx->len, &airtime, err, sizeof(err));

    if (airtime > 0) {
        tx->airtime_ms = airtime;
    }
    tx_complete(tx, rc >= 0, err);
}

int radio_tx_step(struct radio_tx *tx)
{
    char err[160] = "";
    int rc;

    if (!tx->active) {
        return 0;
    }

    if (!tx->begun) {
        if (!tx->be->ops->tx_begin || !tx->be->ops->tx_poll) {
            tx_run_blocking(tx);
            return 0;
        }
        rc = tx->be->ops->tx_begin(tx->be, tx->payload, tx->len, err, sizeof(err));
        if (rc == -ENOTSUP) {
            /* Not a failure: this backend cannot start a transmit without
             * waiting for it right now. Nobody is told, because nothing has
             * gone wrong - the packet still goes out. */
            tx_run_blocking(tx);
            return 0;
        }
        if (rc < 0) {
            tx_complete(tx, false, err[0] ? err : "the backend refused the transmit");
            return 0;
        }
        tx->begun = true;
        /* Do not poll in the same turn. The point of the asynchronous path
         * is that the loop goes back to serving clients while the packet is
         * on the air; polling immediately would make a short packet complete
         * inside the submitting turn on a fast backend, and that is the one
         * behaviour these two paths must not share. */
        return 1;
    }

    rc = tx->be->ops->tx_poll(tx->be, err, sizeof(err));
    if (rc == 0) {
        return 1;
    }
    tx_complete(tx, rc > 0, err);
    return 0;
}

void radio_tx_run(struct radio_tx *tx)
{
    /* Bounded by the airtime, which is bounded by the profile: the longest
     * legal LoRa packet in the worst corner of the parameter space is about
     * 225 s (docs/api/radio.md).
     *
     * Paced rather than spun. A backend whose send() blocks - the SX1262 -
     * never gets here twice and the sleep costs nothing. A backend with an
     * asynchronous transmit would otherwise have this loop calling tx_poll()
     * as fast as the processor allows for the whole time the packet is on
     * the air, which on a handheld is a measurable amount of battery spent
     * on asking a question whose answer cannot change for another 40 ms. */
    while (radio_tx_step(tx)) {
        struct timespec ts = { 0, RADIO_TX_POLL_MS * 1000000L };

        nanosleep(&ts, NULL);
    }
}

bool radio_tx_forget_client(struct radio_tx *tx, uint64_t client_id)
{
    if (!tx->active || client_id == 0 || tx->client_id != client_id) {
        return false;
    }
    /* The submitter is gone; the packet is not. Abandoning a transmit half
     * way would leave the transceiver keyed and the daemon's idea of the
     * radio's state wrong for the rest of the session, so it runs to the
     * end, its statistics are counted, and its completion is announced to
     * whoever is still listening. */
    tx->client_id = 0;
    return true;
}
