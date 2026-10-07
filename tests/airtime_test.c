/*
 * Pins LoRa time-on-air values against widely published calculator results
 * (Semtech formula, explicit header, CRC on, preamble 8).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "airtime.h"

#include <math.h>
#include <stdio.h>

struct tc {
    int sf;
    double bw;
    int cr;
    size_t len;
    double expect_ms;
};

int main(void)
{
    static const struct tc cases[] = {
        { 7, 125.0, 5, 10, 41.216 },
        { 12, 125.0, 5, 10, 991.232 },
        { 9, 125.0, 5, 32, 246.784 },
        { 7, 250.0, 8, 100, 133.248 },
    };
    size_t i;
    int failed = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double got = lora_airtime_ms(cases[i].sf, cases[i].bw, cases[i].cr, 8,
                                     cases[i].len, true, false);
        int ok = fabs(got - cases[i].expect_ms) < 0.01;

        printf("%s SF%d BW%.0f CR4/%d %zuB: %.3f ms (expect %.3f)\n",
               ok ? "ok  " : "FAIL", cases[i].sf, cases[i].bw, cases[i].cr,
               cases[i].len, got, cases[i].expect_ms);
        failed += !ok;
    }
    if (lora_airtime_ms(4, 125.0, 5, 8, 1, true, false) >= 0 ||
        lora_airtime_ms(7, 125.0, 9, 8, 1, true, false) >= 0) {
        printf("FAIL invalid parameters not rejected\n");
        failed++;
    } else {
        printf("ok   invalid parameters rejected\n");
    }
    return failed ? 1 : 0;
}
