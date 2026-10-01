/*
 * The K230's keyboard bus: bit-banged I2C on GPIO46 (SCL) and GPIO47 (SDA),
 * reset on GPIO43 and the controller's INT line on GPIO42.
 *
 * Two implementations satisfy this header. kbd_bus_k230.c is the real one
 * and is built only for the panel (POCKETOS_DISPLAY=drm); kbd_bus_absent.c
 * reports "no device" and is what the simulator links, so the shell follows
 * the same absent path on a PC as on a board with no base attached.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_KBD_BUS_K230_H
#define POCKETOS_KBD_BUS_K230_H

#include "kbd_bus.h"

#include <stddef.h>

/* Fill in bus and take the lines. Returns 0 on success, -1 when the bus is
 * unavailable, with a short reason in why (never NULL-terminated short of
 * why_len). Failure is not an error: the shell logs it once and runs on
 * touch alone. */
int kbd_bus_k230_create(struct kbd_bus *bus, char *why, size_t why_len);

/* Release the lines and put the pin mux back exactly as it was found. */
void kbd_bus_k230_destroy(struct kbd_bus *bus);

/* Give io52 to PWM4, the keyboard light's output (kbd_light.h). The pin is
 * not the bus's, but its mux lives in the block this file alone maps; the
 * value found is saved and put back by kbd_bus_k230_destroy(). -1 when the
 * bus was never created. */
int kbd_bus_k230_light_mux(const struct kbd_bus *bus);

#endif
