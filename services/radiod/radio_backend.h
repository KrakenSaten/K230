/*
 * radiod backend interface. Backends own the transceiver; radiod owns policy,
 * statistics and IPC. See docs/api/radio.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RADIOD_RADIO_BACKEND_H
#define RADIOD_RADIO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MAX_PAYLOAD 255

struct radio_profile {
    double frequency_mhz;
    double bandwidth_khz;
    int spreading_factor;
    int coding_rate;      /* 5..8 = 4/5..4/8 */
    int sync_word;
    int preamble_length;
    int tx_power_dbm;
    bool crc;
};

struct radio_rx_packet {
    uint8_t data[RADIO_MAX_PAYLOAD];
    size_t len;
    double rssi_dbm;
    double snr_db;
    double frequency_error_hz;
    uint64_t timestamp_ms;
};

struct radio_caps {
    double frequency_min_mhz;
    double frequency_max_mhz;
    int tx_power_min_dbm;
    int tx_power_max_dbm;
    int max_payload;
    bool cad;
};

struct radio_backend;

struct radio_backend_ops {
    const char *name;
    const char *chip;
    /* All return 0 on success, negative errno on failure; err gets a message. */
    int (*init)(struct radio_backend *b, char *err, size_t errlen);
    void (*get_caps)(struct radio_backend *b, struct radio_caps *caps);
    int (*configure)(struct radio_backend *b, const struct radio_profile *p,
                     char *err, size_t errlen);
    /* Blocking transmit; fills airtime_ms. */
    int (*send)(struct radio_backend *b, const uint8_t *data, size_t len,
                double *airtime_ms, char *err, size_t errlen);
    /* Return 1 when a packet was filled, 0 when none is pending, <0 on error. */
    int (*receive)(struct radio_backend *b, struct radio_rx_packet *pkt);
    /* Optional (may be NULL): -ENOTSUP semantics when absent. */
    int (*cad)(struct radio_backend *b, bool *activity);
    int (*rssi)(struct radio_backend *b, double *dbm);
    int (*inject_rx)(struct radio_backend *b, const struct radio_rx_packet *pkt);
    void (*shutdown)(struct radio_backend *b);
    /* Optional: fd that becomes readable when receive() should be called. */
    int (*poll_fd)(struct radio_backend *b);
};

struct radio_backend {
    const struct radio_backend_ops *ops;
    void *priv;
    struct radio_profile profile;
};

extern const struct radio_backend_ops radio_backend_mock_ops;
#ifdef POCKETOS_HAVE_SX1262
extern const struct radio_backend_ops radio_backend_sx1262_ops;
#endif

uint64_t radio_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif
