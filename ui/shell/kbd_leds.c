/*
 * The keyboard base's indicator LEDs. See kbd_leds.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_leds.h"

#include <stddef.h>

uint8_t kbd_leds_port_value(uint8_t port, unsigned state)
{
    uint8_t lit = 0;

    if (state & KBD_LED_CAPS) {
        lit |= KBD_LEDS_PIN_CAPS;
    }
    if (state & KBD_LED_MIC) {
        lit |= KBD_LEDS_PIN_MIC;
    }
    if (state & KBD_LED_CAMERA) {
        lit |= KBD_LEDS_PIN_CAMERA;
    }
    /* Active low: a lit LED is a 0 on its pin. */
    return (uint8_t)((port & ~KBD_LEDS_PINS) | (KBD_LEDS_PINS & ~lit));
}

static int rd(const struct kbd_bus *b, uint8_t reg, uint8_t *v)
{
    return b->read_reg_at(b->ctx, KBD_LEDS_XL9555_ADDR, reg, v);
}

static int wr(const struct kbd_bus *b, uint8_t reg, uint8_t v)
{
    return b->write_reg_at(b->ctx, KBD_LEDS_XL9555_ADDR, reg, v);
}

int kbd_leds_init(struct kbd_leds *l, const struct kbd_bus *bus)
{
    uint8_t out;
    uint8_t cfg;
    int ok = 0;

    if (!l) {
        return 0;
    }
    l->bus = bus;
    l->ready = false;
    l->shown = 0;
    if (!bus || !bus->read_reg_at || !bus->write_reg_at || !bus->claim || !bus->release) {
        return 0;
    }
    if (bus->claim(bus->ctx) != 0) {
        l->failures++;
        return 0;
    }
    /* Off before output: the pins are written high while they are still
     * inputs, so making them outputs cannot flash an LED on. The vendor's
     * order (xl9555_led_init). */
    if (rd(bus, KBD_LEDS_REG_OUTPUT0, &out) == 0 && rd(bus, KBD_LEDS_REG_CONFIG0, &cfg) == 0) {
        out = kbd_leds_port_value(out, 0);
        if (wr(bus, KBD_LEDS_REG_OUTPUT0, out) == 0 &&
            wr(bus, KBD_LEDS_REG_CONFIG0, (uint8_t)(cfg & ~KBD_LEDS_PINS)) == 0) {
            l->port = out;
            l->ready = true;
            ok = 1;
        }
    }
    bus->release(bus->ctx);
    if (!ok) {
        l->failures++;
    }
    return ok;
}

int kbd_leds_set(struct kbd_leds *l, unsigned state)
{
    const struct kbd_bus *b;
    uint8_t v;
    int rc;

    if (!l || !l->ready) {
        return l ? -1 : 0;
    }
    state &= KBD_LED_ALL;
    if (state == l->shown) {
        return 0;
    }
    b = l->bus;
    if (b->claim(b->ctx) != 0) {
        l->ready = false;
        l->failures++;
        return -1;
    }
    v = kbd_leds_port_value(l->port, state);
    rc = wr(b, KBD_LEDS_REG_OUTPUT0, v);
    b->release(b->ctx);
    if (rc != 0) {
        /* What the LEDs show is now unknown. Not ready until the next init,
         * which writes every one of them from scratch. */
        l->ready = false;
        l->failures++;
        return -1;
    }
    l->port = v;
    l->shown = (uint8_t)state;
    l->writes++;
    return 0;
}

void kbd_leds_off(struct kbd_leds *l)
{
    if (!l) {
        return;
    }
    if (l->ready) {
        (void)kbd_leds_set(l, 0);
    }
    l->ready = false;
    l->shown = 0;
}

int kbd_leds_read_port(struct kbd_leds *l, uint8_t *output0, uint8_t *config0)
{
    const struct kbd_bus *b;
    int rc;

    if (!l || !l->ready || !output0 || !config0) {
        return -1;
    }
    b = l->bus;
    if (b->claim(b->ctx) != 0) {
        return -1;
    }
    rc = rd(b, KBD_LEDS_REG_OUTPUT0, output0) == 0 && rd(b, KBD_LEDS_REG_CONFIG0, config0) == 0 ? 0 : -1;
    b->release(b->ctx);
    return rc;
}

unsigned kbd_leds_shown(const struct kbd_leds *l)
{
    return l && l->ready ? l->shown : 0;
}
