/*
 * TCA8418 keypad controller: presence, initialisation and the FIFO drain.
 *
 * Pure state machine over a struct kbd_bus. It holds no LVGL object, opens
 * no device and knows no pin number; everything specific to the K230 is in
 * kbd_bus_k230.c. It does not translate key codes either - the raw FIFO
 * byte goes to pos_keymap (DS v0.1 §17.4), which already owns that.
 *
 * The sequences here are the vendor driver's, which is the only sequence
 * verified to work on this hardware, with two deliberate differences: every
 * return value is checked, and the drain is capped at the part's real FIFO
 * depth rather than at 32. See
 * docs/hardware/KEYBOARD_DRIVER_DESIGN_2026-09-12.md §5 and §6.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_KBD_TCA8418_H
#define POCKETOS_KBD_TCA8418_H

#include "kbd_bus.h"

#include <stdbool.h>
#include <stdint.h>

/* 7 rows x 10 columns; codes are 1-based, so code = row * 10 + col + 1. */
#define KBD_TCA8418_CODE_MIN 1
#define KBD_TCA8418_CODE_MAX 70

/* The part's FIFO holds ten events. The cap is the honest bound on how long
 * one poll may hold the UI thread; whatever is left keeps INT asserted and
 * is taken on the next poll. */
#define KBD_TCA8418_DRAIN_MAX 16

enum kbd_tca8418_state {
    KBD_TCA8418_ABSENT = 0, /* the probe found nothing; no retries */
    KBD_TCA8418_READY,      /* configured and draining */
    KBD_TCA8418_FAILED      /* was ready, a transaction failed; retrying */
};

struct kbd_tca8418 {
    const struct kbd_bus *bus;
    enum kbd_tca8418_state state;

    /* The INT line as a cheap gate on the expensive bus (design §14). It is
     * switched off at runtime when the line proves it is not telling the
     * truth, in either direction. */
    bool gate;
    unsigned gate_false_asserted; /* said "pending", drain found nothing */
    uint64_t next_sweep_us;       /* unconditional drain, even when gated */

    /* Retry throttle after a failure: 2 s, then 5 s, then 30 s. */
    uint64_t next_retry_us;
    unsigned retry_count;

    /* Diagnostics, reported rather than inferred. */
    unsigned overflow_count;
    unsigned unknown_count;
    unsigned events_total;
    bool overflow_pending;
};

/* Called once per accepted event with the raw FIFO byte: bit 7 press or
 * release, bits 0..6 the matrix code. */
typedef void (*kbd_tca8418_event_fn)(void *user, uint8_t raw);

/* Probe and configure. Returns 1 when the controller answered, 0 when it did
 * not. Zero is a normal outcome: the base board may simply not be attached,
 * and the shell runs on touch alone (design §9). */
int kbd_tca8418_init(struct kbd_tca8418 *k, const struct kbd_bus *bus,
                     uint64_t now_us);

/* One poll. Returns the number of events delivered, or -1 when the bus
 * failed. Cheap when the gate is trusted and nothing is pending. */
int kbd_tca8418_poll(struct kbd_tca8418 *k, uint64_t now_us,
                     kbd_tca8418_event_fn on_event, void *user);

/* True once per overflow. The caller must drop whatever it believes about
 * held modifiers, because the controller has just dropped events that would
 * have released them. */
bool kbd_tca8418_take_overflow(struct kbd_tca8418 *k);

/* True while the controller is configured and being drained. */
bool kbd_tca8418_ready(const struct kbd_tca8418 *k);

/* Whether the INT line is still being used as a gate. For diagnostics and
 * for the hardware smoke, which has to know which mode it measured. */
bool kbd_tca8418_gated(const struct kbd_tca8418 *k);

#endif
