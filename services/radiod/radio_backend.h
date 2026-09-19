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
    /* Wall clock at reception (CLOCK_REALTIME, radio_now_ms). Comparable with
     * timestamps from other machines, and it jumps when the clock is set. */
    uint64_t timestamp_ms;
    /* Time since boot at reception (CLOCK_MONOTONIC, radio_mono_ms). This is
     * the one to subtract: it never jumps and never runs backwards. A backend
     * that leaves it 0 gets it stamped by the daemon when the packet is
     * drained, so no packet reaches a client without one. */
    uint64_t mono_ms;
};

/* What the backend can truthfully say about the channel right now.
 *
 * Every value is paired with a flag saying whether it is known, and the flag
 * is the point. "Unknown" and "quiet" are different answers, and a backend
 * that cannot tell them apart must say so rather than pick the reassuring
 * one: a protocol daemon deciding whether to transmit will read a fabricated
 * `busy: false` as permission. Zeroing the struct therefore means "I know
 * nothing", which is the correct answer for a backend that fills in none of
 * it. */
struct radio_channel {
    bool rssi_known;
    double rssi_dbm;      /* instantaneous channel RSSI */
    bool noise_known;
    double noise_dbm;     /* noise floor, if the backend actually measures one */
    bool activity_known;
    bool busy;            /* only meaningful while activity_known */
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
    /* Blocking transmit; fills airtime_ms. Every backend has this: it is
     * what radiod falls back to when the asynchronous pair below is absent
     * or declines, so a backend that cannot start a transmit without waiting
     * for it still works, at the cost of a daemon that is unresponsive for
     * the airtime. */
    int (*send)(struct radio_backend *b, const uint8_t *data, size_t len,
                double *airtime_ms, char *err, size_t errlen);
    /* Optional asynchronous transmit, in two halves.
     *
     * tx_begin() starts the transmission and returns without waiting for it:
     * 0 when the packet is on its way, -ENOTSUP when this backend cannot do
     * it right now (radiod then uses send() instead, with no error reported
     * to anyone), any other negative errno for a real refusal with a message
     * in err.
     *
     * tx_poll() is then called until it stops returning 0: 0 while the
     * packet is still on air, 1 when the backend has finished with it, a
     * negative errno with a message when the transmit failed. A backend that
     * re-enters receive mode after transmitting does it before returning 1,
     * and reports whether that worked through is_receiving() as everywhere
     * else - radiod does not announce the transmit complete until it has
     * asked.
     *
     * data stays valid and unchanged from tx_begin() until tx_poll() returns
     * non-zero, so a backend may keep the pointer rather than copying. It
     * points into storage the daemon owns for the life of the transmission,
     * never into the buffer a request was parsed from. */
    int (*tx_begin)(struct radio_backend *b, const uint8_t *data, size_t len,
                    char *err, size_t errlen);
    int (*tx_poll)(struct radio_backend *b, char *err, size_t errlen);
    /* Return 1 when a packet was filled, 0 when none is pending, <0 on error. */
    int (*receive)(struct radio_backend *b, struct radio_rx_packet *pkt);
    /* Optional (may be NULL): -ENOTSUP semantics when absent. */
    int (*cad)(struct radio_backend *b, bool *activity);
    int (*rssi)(struct radio_backend *b, double *dbm);
    /* Optional passive channel observation. Must not change the radio's
     * mode: it is answered while the transceiver stays in receive, and a
     * caller polling it must not be quietly taking the radio off the air.
     * Fill only what is really measured and leave the rest zeroed; see
     * struct radio_channel. Absent means radiod answers from rssi() alone
     * with everything else unknown. */
    int (*channel)(struct radio_backend *b, struct radio_channel *ch);
    int (*inject_rx)(struct radio_backend *b, const struct radio_rx_packet *pkt);
    void (*shutdown)(struct radio_backend *b);
    /* Optional: fd that becomes readable when receive() should be called. */
    int (*poll_fd)(struct radio_backend *b);
    /* Optional: 1 while the transceiver is actually in receive mode, 0 when
     * the last attempt to enter RX failed. Absent means always receiving. */
    int (*is_receiving)(struct radio_backend *b);
    /* Optional: try to re-enter receive mode after a failure. 0 on success,
     * negative errno with a message otherwise. */
    int (*resume_rx)(struct radio_backend *b, char *err, size_t errlen);
    /* Optional, test backends only: set a named debug knob (e.g. the mock's
     * "rx_failing"). Returns 0, -ENOENT for an unknown key. */
    int (*debug_set)(struct radio_backend *b, const char *key, int value);
};

struct radio_backend {
    const struct radio_backend_ops *ops;
    void *priv;
    struct radio_profile profile;
    /* Set by a backend when profile above no longer describes the hardware.
     * Configuring a transceiver is several operations, and one of them can
     * fail after an earlier one has already changed the chip: the frequency
     * and spreading factor are on the air while the CRC setting is whatever
     * the previous profile asked for. A backend that cannot put the previous
     * settings back either says so here rather than letting the daemon keep
     * reporting a profile the radio stopped matching. Cleared by the next
     * configure that succeeds. */
    bool profile_uncertain;
};

extern const struct radio_backend_ops radio_backend_mock_ops;
#ifdef POCKETOS_HAVE_SX1262
extern const struct radio_backend_ops radio_backend_sx1262_ops;
#endif

/* services/radiod/radio_time.c. radio_now_ms is CLOCK_REALTIME and jumps;
 * radio_mono_ms is CLOCK_MONOTONIC and does not. They are not
 * interchangeable. */
uint64_t radio_now_ms(void);
uint64_t radio_mono_ms(void);

#ifdef __cplusplus
}
#endif

#endif
