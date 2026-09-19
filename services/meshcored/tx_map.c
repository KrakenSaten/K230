/*
 * meshcored: the transmit identity map. See tx_map.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "tx_map.h"

#include <string.h>

void mcd_tx_map_init(struct mcd_tx_map *m)
{
    memset(m, 0, sizeof(*m));
    m->next_submit_id = 1;
}

uint64_t mcd_tx_map_submit(struct mcd_tx_map *m, int request_id, int bytes, uint64_t now_ms)
{
    int i;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];

        if (s->phase != MCD_TXP_FREE) {
            continue;
        }
        s->phase = MCD_TXP_AWAITING_ACCEPT;
        s->submit_id = m->next_submit_id++;
        s->request_id = request_id;
        s->tx_id = 0;
        s->bytes = bytes;
        s->submitted_ms = now_ms;
        s->airtime_ms = 0.0;
        s->deadline_ms = now_ms + MCD_TX_ACCEPT_DEADLINE_MS;
        return s->submit_id;
    }
    return 0;
}

uint64_t mcd_tx_map_accepted(struct mcd_tx_map *m, int request_id, uint64_t tx_id,
                             double airtime_ms, uint64_t now_ms)
{
    int i;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];
        uint64_t window;

        if (s->phase != MCD_TXP_AWAITING_ACCEPT || s->request_id != request_id) {
            continue;
        }
        s->phase = MCD_TXP_IN_FLIGHT;
        s->tx_id = tx_id;
        s->airtime_ms = airtime_ms > 0.0 ? airtime_ms : 0.0;
        window = (uint64_t)(s->airtime_ms * 3.0) + MCD_TX_FLIGHT_SLACK_MS;
        if (window < MCD_TX_MIN_FLIGHT_DEADLINE_MS) {
            window = MCD_TX_MIN_FLIGHT_DEADLINE_MS;
        }
        s->deadline_ms = now_ms + window;
        return s->submit_id;
    }
    return 0;
}

uint64_t mcd_tx_map_refused(struct mcd_tx_map *m, int request_id)
{
    int i;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];
        uint64_t id;

        if (s->phase != MCD_TXP_AWAITING_ACCEPT || s->request_id != request_id) {
            continue;
        }
        id = s->submit_id;
        memset(s, 0, sizeof(*s));
        return id;
    }
    return 0;
}

uint64_t mcd_tx_map_completed(struct mcd_tx_map *m, uint64_t tx_id)
{
    int i;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];
        uint64_t id;

        /* Only a slot that has actually been given this tx_id matches. A
         * submission still awaiting acceptance has tx_id 0, and a zero in an
         * event must not select it. */
        if (s->phase != MCD_TXP_IN_FLIGHT || s->tx_id != tx_id) {
            continue;
        }
        id = s->submit_id;
        memset(s, 0, sizeof(*s));
        return id;
    }
    return 0;
}

int mcd_tx_map_abandon_all(struct mcd_tx_map *m, uint64_t *out)
{
    int i;
    int n = 0;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];

        if (s->phase == MCD_TXP_FREE) {
            continue;
        }
        out[n++] = s->submit_id;
        memset(s, 0, sizeof(*s));
    }
    return n;
}

int mcd_tx_map_expire(struct mcd_tx_map *m, uint64_t now_ms, uint64_t *out)
{
    int i;
    int n = 0;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        struct mcd_tx_slot *s = &m->slots[i];

        if (s->phase == MCD_TXP_FREE || now_ms < s->deadline_ms) {
            continue;
        }
        out[n++] = s->submit_id;
        memset(s, 0, sizeof(*s));
    }
    return n;
}

int mcd_tx_map_outstanding(const struct mcd_tx_map *m)
{
    int i;
    int n = 0;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        if (m->slots[i].phase != MCD_TXP_FREE) {
            n++;
        }
    }
    return n;
}

int mcd_tx_map_bytes(const struct mcd_tx_map *m, uint64_t submit_id)
{
    int i;

    for (i = 0; i < MCD_TX_MAP_SLOTS; i++) {
        const struct mcd_tx_slot *s = &m->slots[i];

        if (s->phase != MCD_TXP_FREE && s->submit_id == submit_id) {
            return s->bytes;
        }
    }
    return -1;
}
