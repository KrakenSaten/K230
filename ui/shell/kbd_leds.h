/*
 * The keyboard base's three indicator LEDs.
 *
 * They hang off the XL9555 I/O expander on the keyboard's own bit-banged bus,
 * pins P03, P04 and P05 of port 0, active low (vendor launcher,
 * k230_phone_ui ui_hardware.c: xl9555 LED index 0..2; its "LED Test" page
 * drives each one alone, so they are independent). The vendor lights P03 for
 * Caps and P04 for its pinyin mode; Doors gives them:
 *
 *   P03  Caps Lock        (the vendor's Caps LED, kept)
 *   P04  microphone in use
 *   P05  camera in use
 *
 * Which physical LED sits where on the base is DOCUMENTED from the vendor's
 * index order only; it has not been looked at (docs/hardware/
 * HARDWARE_CONTROLS.md §4).
 *
 * Only the three LED bits are ever written. The rest of port 0 is read and
 * written back as found, and port 1 is not touched, so whatever else the
 * expander does on some base is left alone.
 *
 * Pure: it talks to struct kbd_bus only (read_reg_at/write_reg_at), so the
 * whole of it runs on the host against a register model
 * (tests/kbd_leds_test.c). Called on the LVGL thread, outside the key
 * controller's drain; every call claims and releases the bus itself.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_KBD_LEDS_H
#define POCKETOS_KBD_LEDS_H

#include "kbd_bus.h"

#include <stdbool.h>
#include <stdint.h>

#define KBD_LEDS_XL9555_ADDR 0x20u
#define KBD_LEDS_REG_INPUT0 0x00u
#define KBD_LEDS_REG_OUTPUT0 0x02u
#define KBD_LEDS_REG_CONFIG0 0x06u

/* What each LED means, as a bit of a state mask. */
#define KBD_LED_CAPS 0x1u
#define KBD_LED_MIC 0x2u
#define KBD_LED_CAMERA 0x4u
#define KBD_LED_ALL (KBD_LED_CAPS | KBD_LED_MIC | KBD_LED_CAMERA)

/* The port-0 pins behind them. */
#define KBD_LEDS_PIN_CAPS 0x08u   /* P03 */
#define KBD_LEDS_PIN_MIC 0x10u    /* P04 */
#define KBD_LEDS_PIN_CAMERA 0x20u /* P05 */
#define KBD_LEDS_PINS (KBD_LEDS_PIN_CAPS | KBD_LEDS_PIN_MIC | KBD_LEDS_PIN_CAMERA)

struct kbd_leds {
    const struct kbd_bus *bus;
    bool ready;     /* the expander answered and the pins are outputs */
    uint8_t shown;  /* the state mask last written successfully */
    uint8_t port;   /* OUTPUT0 as last written: the other bits as found */
    unsigned writes; /* successful OUTPUT0 writes, for tests and shell.info */
    unsigned failures;
};

/* Take the expander: every LED off first, then the three pins made outputs.
 * 1 when it answered, 0 when it did not (a base without one, a bus without
 * read_reg_at). Safe to call again after a keyboard came back. */
int kbd_leds_init(struct kbd_leds *l, const struct kbd_bus *bus);

/* Show exactly `state` (KBD_LED_*). The bus is touched only when it differs
 * from what is shown. 0 on success or nothing to do, -1 when the write
 * failed; the LEDs are then not ready until kbd_leds_init() again. */
int kbd_leds_set(struct kbd_leds *l, unsigned state);

/* Everything off, and forget the expander. For the shell's exit path. */
void kbd_leds_off(struct kbd_leds *l);

/* The state mask as the LEDs show it; 0 when not ready. */
unsigned kbd_leds_shown(const struct kbd_leds *l);

/* Read OUTPUT0 and CONFIG0 back from the expander, for a bench that wants
 * the hardware's word rather than this layer's belief. 0, or -1. */
int kbd_leds_read_port(struct kbd_leds *l, uint8_t *output0, uint8_t *config0);

/* The OUTPUT0 value for a state mask, given the other bits of the port. */
uint8_t kbd_leds_port_value(uint8_t port, unsigned state);

#endif
