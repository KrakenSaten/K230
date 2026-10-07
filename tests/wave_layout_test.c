/*
 * Wave's layout and keyboard policy on the reference panel's bodies:
 * portrait with and without the touch keyboard, landscape with and without
 * it, and the edges - a landscape body too narrow for two columns, a very
 * short portrait body, an unsized body.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_layout.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    struct wave_layout l;

    /* Portrait: 528 x 1106 content, 528 x 820 above the keyboard (unit A, DS §36 frame). */
    wave_layout_choose(&l, 528, 1106, 0, 0);
    check("portrait is TALL", l.shape == WAVE_SHAPE_TALL);
    check("portrait shows the history and the controls", l.show_history && l.show_rail);
    check("portrait: a tap on the field brings the touch keyboard", l.field_tap_shows_keyboard);
    check("portrait has no KEYS button (the field is the way in)", !l.show_keys);
    wave_layout_choose(&l, 528, 820, 0, 1);
    check("portrait with the keyboard up stays TALL, history still there",
          l.shape == WAVE_SHAPE_TALL && l.show_history && l.show_rail);
    wave_layout_choose(&l, 528, 180, 0, 1);
    check("even a very short portrait body is never a strip (no KEYS to get out)",
          l.shape == WAVE_SHAPE_TALL && !l.show_keys);

    /* Landscape: 1192 x 442, 1192 x 156 above the keyboard. */
    wave_layout_choose(&l, 1192, 442, 1, 0);
    check("landscape is WIDE", l.shape == WAVE_SHAPE_WIDE);
    check("landscape: a tap on the field does NOT bring the touch keyboard",
          !l.field_tap_shows_keyboard);
    check("landscape offers KEYS so it works without a physical keyboard",
          l.show_keys && strcmp(l.keys_label, "KEYS") == 0);
    check("KEYS shows the keyboard when it is down", wave_layout_keys_shows(&l, 0) == 1);
    check("landscape shows the history beside the controls", l.show_history && l.show_rail);
    check("the two columns fit the landscape body", 1192 >= WAVE_HISTORY_MIN_W + WAVE_RAIL_W);

    wave_layout_choose(&l, 1192, 156, 1, 1);
    check("landscape with the keyboard up is a STRIP", l.shape == WAVE_SHAPE_STRIP);
    check("the strip keeps only the composer row", !l.show_history && !l.show_rail);
    check("with KEYS reading HIDE, which puts the keyboard away",
          l.show_keys && strcmp(l.keys_label, "HIDE") == 0 && wave_layout_keys_shows(&l, 1) == 0);
    check("a 64 px composer row fits the strip", 64 <= 156);

    wave_layout_choose(&l, 700, 442, 1, 0);
    check("a landscape body too narrow for two columns falls back to TALL",
          l.shape == WAVE_SHAPE_TALL && !l.field_tap_shows_keyboard && l.show_keys);
    wave_layout_choose(&l, 0, 0, 1, 0);
    check("an unsized body is not a strip", l.shape != WAVE_SHAPE_STRIP);
    wave_layout_choose(&l, 1106, 528, 0, 0);
    check("the keyboard rule follows the orientation, not the box",
          l.field_tap_shows_keyboard && !l.show_keys);

    printf("wave_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
