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
    int rx_failing;   /* debug knob: simulate a transceiver that cannot enter RX */
    /* debug knob: re-entering receive fails while a packet is being handed
     * over, which is what the SX1262 does - sx_receive() calls enter_rx()
     * after readData() and reports the result through is_receiving() alone.
     * Setting rx_failing from outside cannot stand in for that: it arrives as
     * a control call, and the daemon reconciles its state around those. */
    int rx_fails_after_receive;
    /* debug knob: which stage of configure fails. 0 none, 1 before the radio
     * is touched, 2 after it has moved but before the CRC setting, 3 after it
     * has moved and receive mode could not be entered. */
    int configure_fail_stage;
    /* When set, the stage above applies to the next configure only. That is
     * the difference between a transceiver that glitched once - where asking
     * for the previous profile again puts it back - and one that has stopped
     * answering, where the rollback fails the same way and nothing can say
     * what the radio is doing. */
    int configure_fail_once;
    struct radio_profile hw_profile; /* what the radio is actually doing */
    int hw_configured;
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

/* Configuring the real transceiver is three operations in order: begin() puts
 * the whole radio configuration on the chip, then the CRC mode, then receive
 * mode. Only the first changes what is on the air, and the two after it can
 * fail once it has. The mock models the same three stages so a failure can be
 * injected between them, because the case worth testing is not "configure
 * failed" but "configure changed the hardware and then failed".
 *
 * hw_configured tracks whether the chip - such as it is here - matches the
 * profile the daemon is reporting. */
static int mock_configure(struct radio_backend *b, const struct radio_profile *p,
                          char *err, size_t errlen)
{
    struct mock_priv *m = b->priv;

    if (m->configure_fail_stage == 1) {
        snprintf(err, errlen, "mock: begin failed before touching the radio");
        if (m->configure_fail_once) {
            m->configure_fail_stage = 0;
        }
        return -EIO;
    }
    /* Past begin(): the radio has moved whatever happens next. */
    m->hw_profile = *p;
    m->hw_configured = true;
    if (m->configure_fail_stage == 2) {
        snprintf(err, errlen, "mock: setCRC failed after begin");
        if (m->configure_fail_once) {
            m->configure_fail_stage = 0;
        }
        return -EIO;
    }
    if (m->configure_fail_stage == 3) {
        m->rx_failing = 1; /* startReceive failed: the chip is not listening */
        snprintf(err, errlen, "mock: startReceive failed after begin");
        if (m->configure_fail_once) {
            m->configure_fail_stage = 0;
        }
        return -EIO;
    }
    b->profile = *p;
    m->rx_failing = 0;
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
    /* The packet is good and is returned; going back into receive afterwards
     * is what failed. Nothing in this return value says so. */
    if (m->rx_fails_after_receive) {
        m->rx_failing = 1;
    }
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

static int mock_is_receiving(struct radio_backend *b)
{
    struct mock_priv *m = b->priv;

    return !m->rx_failing;
}

static int mock_resume_rx(struct radio_backend *b, char *err, size_t errlen)
{
    struct mock_priv *m = b->priv;

    if (m->rx_failing) {
        snprintf(err, errlen, "mock: rx_failing is set");
        return -EIO;
    }
    return 0;
}

static int mock_debug_set(struct radio_backend *b, const char *key, int value)
{
    struct mock_priv *m = b->priv;

    if (strcmp(key, "rx_failing") == 0) {
        m->rx_failing = value != 0;
        return 0;
    }
    if (strcmp(key, "configure_fail_stage") == 0) {
        if (value < 0 || value > 3) {
            return -EINVAL;
        }
        m->configure_fail_stage = value;
        return 0;
    }
    if (strcmp(key, "configure_fail_once") == 0) {
        m->configure_fail_once = value != 0;
        return 0;
    }
    if (strcmp(key, "rx_fails_after_receive") == 0) {
        m->rx_fails_after_receive = value != 0;
        if (!value) {
            m->rx_failing = 0;
        }
        return 0;
    }
    return -ENOENT;
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
    .is_receiving = mock_is_receiving,
    .resume_rx = mock_resume_rx,
    .debug_set = mock_debug_set,
};
