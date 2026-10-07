/*
 * LoRa time-on-air per Semtech SX1276/SX126x datasheets:
 *   Tsym = 2^SF / BW
 *   Tpreamble = (Npreamble + 4.25) * Tsym
 *   Npayload = 8 + max(ceil((8PL - 4SF + 28 + 16CRC - 20IH) / (4(SF - 2DE))) * (CR + 4), 0)
 * with DE = 1 when low-data-rate optimisation applies (Tsym > 16 ms).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "airtime.h"

#include <math.h>

double lora_airtime_ms(int sf, double bw_khz, int cr, int preamble, size_t len,
                       bool crc, bool implicit_header)
{
    double tsym_ms;
    int de;
    double num;
    double den;
    double npayload;

    if (sf < 5 || sf > 12 || bw_khz <= 0 || cr < 5 || cr > 8 || preamble < 1) {
        return -1.0;
    }
    tsym_ms = ldexp(1.0, sf) / bw_khz;
    de = tsym_ms > 16.0 ? 1 : 0;
    num = 8.0 * (double)len - 4.0 * sf + 28.0 + (crc ? 16.0 : 0.0) -
          (implicit_header ? 20.0 : 0.0);
    den = 4.0 * (sf - 2 * de);
    npayload = ceil(num / den) * (double)cr;
    if (npayload < 0) {
        npayload = 0;
    }
    npayload += 8.0;
    return (preamble + 4.25) * tsym_ms + npayload * tsym_ms;
}
