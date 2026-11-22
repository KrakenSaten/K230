/*
 * What of a capture may be read from outside the app (`capture.status` on the
 * shell's socket, docs/api/shell.md): its state and aggregate figures, and
 * nothing that names a host or belongs to one packet.
 *
 * Two steps, so that the boundary is a type and not a habit:
 *
 *   psk_remote_take()  the model  ->  struct psk_remote_figures
 *   psk_remote_json()  the figures ->  JSON
 *
 * struct psk_remote_figures can hold counts, rates, sizes, two enumerated
 * words and an interface name, and has no room for anything else: no address,
 * no name, no port, no row, no byte of a packet, no sequence number of one,
 * no file name, no text the helper wrote. The JSON is made from that struct
 * alone. psk_remote_take() is the one place that reads the model here, and
 * tests/packetshark_lint.sh holds which of its members it may read.
 *
 * Pure C, no LVGL, no file, no clock (tests/psk_remote_test.c).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_REMOTE_H
#define PSK_REMOTE_H

#include "psk_model.h"

#include <stdbool.h>
#include <stdint.h>

struct cJSON;

/* No figure is sent larger than this: above it cJSON would print an exponent,
 * which a reader of whole numbers does not take. */
#define PSK_REMOTE_MAX 999999999999999ull

/* What the app's screen shows, decided by the app as it decides it for the
 * screen (psk_app.c): setup, a capture (running, held, saving or stopped),
 * or the panel of a capture whose helper ended badly. */
enum psk_remote_phase { PSK_REMOTE_IDLE = 0, PSK_REMOTE_CAPTURE, PSK_REMOTE_ENDED };

/* The words of `state` and `reason` (docs/api/shell.md). */
enum psk_remote_state {
    PSK_RS_IDLE = 0,
    PSK_RS_CAPTURING,
    PSK_RS_HOLD,
    PSK_RS_SAVING,
    PSK_RS_STOPPED,
    PSK_RS_ENDED,
    PSK_RS_COUNT
};

enum psk_remote_reason {
    PSK_RR_NONE = 0,
    PSK_RR_USER,
    PSK_RR_LIMIT,
    PSK_RR_LINK,
    PSK_RR_END,
    PSK_RR_ERROR,
    PSK_RR_HELPER_CRASHED,
    PSK_RR_HELPER_HUNG,
    PSK_RR_COUNT
};

#define PSK_REMOTE_DIAG 18

struct psk_remote_figures {
    uint8_t state;         /* enum psk_remote_state */
    uint8_t reason;        /* enum psk_remote_reason */
    bool keep_full;
    char iface[PPC_IFACE_MAX]; /* eth, wlan, usb or synth and one or two digits, or "" */
    uint32_t limit_s;      /* 0: none */
    uint32_t fill_permille;
    uint64_t elapsed_ms;
    uint64_t seen;
    uint64_t stored;
    uint64_t held;
    uint64_t kernel_drops;
    uint64_t iface_drops;
    uint64_t shed;
    uint64_t bytes;
    uint64_t pps;
    uint64_t bps;
    uint64_t classes[PPC_CLASS_COUNT];
    uint64_t findings[PPC_SIGNAL_COUNT];
    uint64_t diag[PSK_REMOTE_DIAG]; /* in the order of psk_remote_diag_name() */
};

/* Fill `out` from the model for what the screen shows. In PSK_REMOTE_IDLE
 * there is no capture and every figure is zero, whatever the model still
 * holds of an earlier one. keep_full and limit_s are the choices the capture
 * was started with. */
void psk_remote_take(const struct psk_model *m, enum psk_remote_phase phase, bool keep_full, uint32_t limit_s,
                     struct psk_remote_figures *out);

/* The `capture.status` answer, or NULL when memory is short. The caller owns
 * it. */
struct cJSON *psk_remote_json(const struct psk_remote_figures *f);

const char *psk_remote_state_name(unsigned state);
const char *psk_remote_reason_name(unsigned reason);
/* The key of diag[i] and of classes[i]; NULL past the end. */
const char *psk_remote_diag_name(unsigned i);
const char *psk_remote_class_name(unsigned i);

#endif
