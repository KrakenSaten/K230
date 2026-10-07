/*
 * The owner's choice for the radio: on or off, kept across restarts.
 *
 * One file, <state dir>/radiod/radio.conf, holding "enabled=0" or
 * "enabled=1" and "backend=<name>" after an optional comment. radiod reads it
 * once at start and writes it on every radio.set_enabled that changes the
 * choice.
 *
 * The choice belongs to the backend it was made on (Doors 0.3.5). A choice
 * made on the mock - a fresh card's backend, whose "on" transmits nothing -
 * must not switch the real transceiver on without the antenna question, so
 * radiod applies a stored choice only on the backend that stored it. A file
 * from before the tag (no backend line) is still honoured for "off" and on
 * the mock; its "on" is not trusted for a transmitter, which then starts off
 * once until somebody switches it on (rf_state_applies). Written
 * atomically (temporary file, fsync, rename), so a restart or a power cut
 * finds either the old choice or the new one, never half of one.
 *
 * Nothing else is in the file. The radio profile, the lease and the MeshCore
 * identity and channels are not the radio's on/off choice and are not stored
 * here; turning the radio off never touches them (docs/api/radio.md,
 * "Radio on and off").
 *
 * Pure C, no IPC and no backend; tested on the host through the daemon
 * (tests/radiod_power_test.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_RADIOD_RF_STATE_H
#define POCKETOS_RADIOD_RF_STATE_H

#include <stdbool.h>
#include <stddef.h>

#define RF_STATE_DIR "radiod"
#define RF_STATE_FILE "radio.conf"

enum rf_state_load {
    RF_STATE_OFF = 0,        /* stored: off */
    RF_STATE_ON = 1,         /* stored: on */
    RF_STATE_ABSENT = -1,    /* no file: nobody has chosen yet */
    RF_STATE_INVALID = -2    /* a file that says neither; treated as absent */
};

/* <state_dir>/radiod/radio.conf into out. Returns 0, or -1 when it does not
 * fit. */
int rf_state_path(const char *state_dir, char *out, size_t len);

#define RF_STATE_BACKEND_MAX 32

/* Read the stored choice and the backend it was made on (backend gets "" when
 * the file names none; may be NULL). Never creates anything. */
enum rf_state_load rf_state_load(const char *path, char *backend, size_t blen);

/* Whether a stored choice applies to the backend radiod runs now: only a
 * choice made on that backend, or an untagged one that is "off" or is for
 * the mock. Anything else leaves the backend's default in force. */
bool rf_state_applies(enum rf_state_load stored, const char *stored_backend, const char *backend);

/* Store the choice and the backend it is made on, creating the directory
 * (0755) when it is missing. Returns 0, or a negative errno with a reason in
 * err. */
int rf_state_store(const char *path, bool enabled, const char *backend, char *err, size_t errlen);

#endif
