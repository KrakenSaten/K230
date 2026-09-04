/*
 * radiod mock backend: no hardware, real airtime maths, injectable packets.
 * Behaviour is deterministic so tests can pin values.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "airtime.h"
#include "radio_backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_QUEUE 16

struct mock_priv {
    struct radio_rx_packet queue[MOCK_QUEUE];
    size_t head;
    size_t count;
};

static int mock_init(struct radio_backend *b, char *err, size_t errlen)
{
    struct mock_priv *m = calloc(1, sizeof(*m));

    if (!m) {
        snprintf(err, errlen, "out of memory");
        return -ENOMEM;
    }
    b->priv = m;
    return 0;
}

static void mock_get_caps(struct radio_backend *b, struct radio_caps *caps)
{
    (void)b;
    /* Mirrors SX1262 limits so mock and hardware reject the same requests. */
    caps->frequency_min_mhz = 150.0;
    caps->frequency_max_mhz = 960.0;
    caps->tx_power_min_dbm = -9;
    caps->tx_power_max_dbm = 22;
    caps->max_payload = RADIO_MAX_PAYLOAD;
    caps->cad = true;
}

static int mock_configure(struct radio_backend *b, const struct radio_profile *p,
                          char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    b->profile = *p;
    return 0;
}

static int mock_send(struct radio_backend *b, const uint8_t *data, size_t len,
                     double *airtime_ms, char *err, size_t errlen)
{
    const struct radio_profile *p = &b->profile;

    (void)data;
    *airtime_ms = lora_airtime_ms(p->spreading_factor, p->bandwidth_khz,
                                  p->coding_rate, p->preamble_length, len,
                                  p->crc, false);
    if (*airtime_ms < 0) {
        snprintf(err, errlen, "invalid profile for airtime");
        return -EINVAL;
    }
    return 0;
}

static int mock_receive(struct radio_backend *b, struct radio_rx_packet *pkt)
{
    struct mock_priv *m = b->priv;

    if (m->count == 0) {
        return 0;
    }
    *pkt = m->queue[m->head];
    m->head = (m->head + 1) % MOCK_QUEUE;
    m->count--;
    return 1;
}

static int mock_cad(struct radio_backend *b, bool *activity)
{
    struct mock_priv *m = b->priv;

    *activity = m->count > 0;
    return 0;
}

static int mock_rssi(struct radio_backend *b, double *dbm)
{
    struct mock_priv *m = b->priv;

    *dbm = m->count > 0 ? -75.0 : -115.0;
    return 0;
}

static int mock_inject_rx(struct radio_backend *b, const struct radio_rx_packet *pkt)
{
    struct mock_priv *m = b->priv;
    size_t tail;

    if (m->count >= MOCK_QUEUE) {
        return -ENOSPC;
    }
    tail = (m->head + m->count) % MOCK_QUEUE;
    m->queue[tail] = *pkt;
    m->count++;
    return 0;
}

static void mock_shutdown(struct radio_backend *b)
{
    free(b->priv);
    b->priv = NULL;
}

const struct radio_backend_ops radio_backend_mock_ops = {
    .name = "mock",
    .chip = "mock",
    .init = mock_init,
    .get_caps = mock_get_caps,
    .configure = mock_configure,
    .send = mock_send,
    .receive = mock_receive,
    .cad = mock_cad,
    .rssi = mock_rssi,
    .inject_rx = mock_inject_rx,
    .shutdown = mock_shutdown,
    .poll_fd = NULL,
};
