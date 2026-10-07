/*
 * The keyboard bus on a machine that has no keyboard base board: the
 * simulator, and any host build.
 *
 * It reports absence the way the real bus reports a controller that does not
 * answer, so the shell takes exactly the same path on a PC as it does on a
 * board with the base detached (design §9).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_bus_k230.h"

#include <stddef.h>

int kbd_bus_k230_create(struct kbd_bus *bus, char *why, size_t why_len)
{
    (void)bus;
    if (why && why_len > 0) {
        const char *msg = "no keyboard bus in this build";
        size_t i = 0;

        while (msg[i] != '\0' && i + 1 < why_len) {
            why[i] = msg[i];
            i++;
        }
        why[i] = '\0';
    }
    return -1;
}

void kbd_bus_k230_destroy(struct kbd_bus *bus)
{
    (void)bus;
}

int kbd_bus_k230_light_mux(const struct kbd_bus *bus)
{
    (void)bus;
    return -1;
}
