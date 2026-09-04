/*
 * LoRa time-on-air (Semtech formula). Shared by all radiod backends.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RADIOD_AIRTIME_H
#define RADIOD_AIRTIME_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* coding_rate is 5..8 meaning 4/5..4/8. Returns milliseconds, or a negative
 * value for invalid parameters. */
double lora_airtime_ms(int spreading_factor, double bandwidth_khz, int coding_rate,
                       int preamble_length, size_t payload_len, bool crc,
                       bool implicit_header);

#ifdef __cplusplus
}
#endif

#endif
