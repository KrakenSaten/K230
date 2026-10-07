/*
 * The developer debug overlay's one line, with no LVGL in it.
 *
 *   CPU 18% · RAM 42% · 51°C · NET ↓12 ↑2 KB/s · LORA ↓848 ↑1
 *
 * From answers the shell already asks for when the overlay is on: sysd's
 * system.status (cpu_percent, memory, temperature_c, and the interfaces'
 * byte counters, docs/api/system.md) and radiod's radio.stats (packets
 * received and sent since radiod started). The network figure is a rate:
 * the bytes counted across the real interfaces between two answers, over
 * the time between them, so the first answer shows a dash. The radio part
 * is left out whole when radiod does not answer.
 *
 * Nothing here can carry an address, a name or a secret: it reads numbers
 * only, and the line is built from fixed words and those numbers.
 *
 * tests/overlay_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_OVERLAY_MODEL_H
#define DOORS_OVERLAY_MODEL_H

#include <cjson/cJSON.h>
#include <stdint.h>

#define OVERLAY_TEXT_MAX 128

struct overlay_model {
    int have_net;       /* a previous byte count to take a rate from */
    uint64_t rx;
    uint64_t tx;
    uint32_t at_ms;
    char text[OVERLAY_TEXT_MAX];
};

void overlay_model_init(struct overlay_model *m);
/* status: system.status or NULL (sysd not answering); radio: radio.stats or
 * NULL; now_ms: a monotonic millisecond clock. */
void overlay_model_update(struct overlay_model *m, const cJSON *status, const cJSON *radio, uint32_t now_ms);

/* Bytes over a sum of real interfaces in a system.status answer: 0 and the
 * sums when every counted interface had both counters, -1 when none did.
 * An interface without a six-octet MAC (sit0, a tunnel) is not counted. */
int overlay_net_bytes(const cJSON *status, uint64_t *rx, uint64_t *tx);

#endif
