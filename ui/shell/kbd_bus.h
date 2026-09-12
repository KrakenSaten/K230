/*
 * The physical keyboard's transport, as a vtable.
 *
 * The chip layer (kbd_tca8418.c) talks to nothing else: no device path, no
 * GPIO, no /dev/mem. That is what lets the whole state machine - the init
 * sequence, the FIFO drain, overflow recovery and the retry throttle - be
 * tested on the host against a fake bus, with no keyboard attached and no
 * hardware of any kind.
 *
 * Two implementations ship: kbd_bus_k230.c, the bit-banged I2C on GPIO46/47
 * described in docs/hardware/KEYBOARD_DRIVER_DESIGN_2026-09-12.md, and
 * kbd_bus_absent.c, which reports no device and is what the simulator gets.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_KBD_BUS_H
#define POCKETOS_KBD_BUS_H

#include <stdint.h>

/* Calls returning int give 0 on success and -1 on failure. */
struct kbd_bus {
    /* Take the bus for one transaction group: on the K230 this muxes io46
     * and io47 away from their normal function to plain GPIO. Every claim
     * is paired with a release, including on every failure path. */
    int (*claim)(void *ctx);
    /* Give it back, restoring the mux value that was read at init. */
    void (*release)(void *ctx);

    int (*read_reg)(void *ctx, uint8_t reg, uint8_t *value);
    int (*write_reg)(void *ctx, uint8_t reg, uint8_t value);

    /* The controller's reset pulse: active low, held for the whole pulse. */
    int (*reset_pulse)(void *ctx);

    /* The INT line's level: 0 when asserted (low, events pending), 1 when
     * idle, and -1 when the line cannot be read. -1 is not an error: it
     * tells the chip layer to poll unconditionally instead (design §14). */
    int (*irq_level)(void *ctx);

    void *ctx;
};

#endif
