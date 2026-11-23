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
    PSK_RR_LEFT,
    PSK_RR_REFUSED, /* a start request: refused on the unit, or it could not start */
    PSK_RR_TIMEOUT, /* a start request: nobody answered it in time */
    PSK_RR_COUNT
};

/* Where a start request stands, for whoever asked and reads capture.status
 * rather than listening: its sheet is up, or how the last one ended. */
enum psk_remote_request { PSK_RQ_NONE = 0, PSK_RQ_SHOWN, PSK_RQ_REFUSED, PSK_RQ_TIMEOUT, PSK_RQ_COUNT };

#define PSK_REMOTE_DIAG 18

struct psk_remote_figures {
    uint8_t state;         /* enum psk_remote_state */
    uint8_t reason;        /* enum psk_remote_reason */
    uint8_t request;       /* enum psk_remote_request: where a start request stands */
    bool keep_full;
    bool by_companion;     /* started from a start request's sheet, by a tap on it */
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
 * was started with; by_companion, that it was started from the sheet of a
 * start request. */
void psk_remote_take(const struct psk_model *m, enum psk_remote_phase phase, bool keep_full, uint32_t limit_s,
                     bool by_companion, struct psk_remote_figures *out);

/* The `capture.status` answer, or NULL when memory is short. The caller owns
 * it. */
struct cJSON *psk_remote_json(const struct psk_remote_figures *f);

/* The `capture.state` event: the state, why, and what the capture was
 * started as. No figure. NULL when memory is short. */
struct cJSON *psk_remote_state_json(const struct psk_remote_figures *f);

/* The app is being left with a capture in it: the capture ends there and its
 * packets go (ADR-016 rule 5). The totals stay, the rates do not. */
void psk_remote_left(struct psk_remote_figures *f);

/* ---- when to raise the events (capture.state, capture.stats) ------------------
 *
 * Asked on every poll of the app; says which one event, if any, is due now.
 *
 *   capture.state  when the state, its reason or what the capture was started
 *                  as differs from what was last raised; at most one attempt
 *                  every PSK_REMOTE_STATE_GAP_MS, so a helper that flaps
 *                  cannot make more of them. A change that could not be
 *                  raised (the unit is locked) is tried again, and what is
 *                  then raised is the state as it is by then.
 *   capture.stats  the figures: once after each raised state that has any
 *                  (so the totals of a stopped capture go out once, last),
 *                  and every PSK_REMOTE_STATS_GAP_MS while capturing or held.
 *                  Never while a change of state waits to be raised: figures
 *                  never carry a state nobody was told of.
 *
 * So in any second this asks for at most 1000 / PSK_REMOTE_STATE_GAP_MS of
 * each. A state that changes again before its figures went out does not get
 * them: the newer state is raised first. The app raises one event past this,
 * the last word when it is left with a capture (psk_remote_left), and starts
 * afresh each time it is opened. Pure, the clock passed in. */
#define PSK_REMOTE_STATE_GAP_MS 250
#define PSK_REMOTE_STATS_GAP_MS 1000
#define PSK_REMOTE_DUE_STATE 0x1u
#define PSK_REMOTE_DUE_STATS 0x2u

struct psk_remote_emit {
    bool announced;        /* a state has been raised */
    uint8_t state;         /* what was raised last */
    uint8_t reason;
    bool keep_full;
    uint32_t limit_s;
    char iface[PPC_IFACE_MAX];
    bool owed;             /* the figures of that state have not gone out */
    int64_t state_try_ms;  /* the last attempts */
    int64_t stats_try_ms;
};

void psk_remote_emit_init(struct psk_remote_emit *e);
/* PSK_REMOTE_DUE_STATE, PSK_REMOTE_DUE_STATS or 0; never both. */
unsigned psk_remote_due(const struct psk_remote_emit *e, const struct psk_remote_figures *f, int64_t now_ms);
/* The attempt was made; `raised` is what the shell said of it. */
void psk_remote_tried(struct psk_remote_emit *e, unsigned what, const struct psk_remote_figures *f, int64_t now_ms,
                      bool raised);

const char *psk_remote_state_name(unsigned state);
const char *psk_remote_reason_name(unsigned reason);
/* The key of diag[i] and of classes[i]; NULL past the end. */
const char *psk_remote_diag_name(unsigned i);
const char *psk_remote_class_name(unsigned i);

#endif
