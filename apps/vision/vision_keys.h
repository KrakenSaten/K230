/*
 * Vision's keyboard: which of the screen's buttons a key moves to.
 *
 * With a keyboard and no touch (an external display, where the board's
 * touch controller has no driver: docs/hardware/HDMI_OUTPUT.md), every one
 * of Vision's controls is a button - the mode's row and the sheet's choices
 * - so moving a focus ring over the buttons on show and pressing Enter is
 * the whole of it. The arrows move to the nearest button that way, Tab and
 * Shift-Tab to the next and the previous in their own order, both wrapping
 * the way a list does.
 *
 * Pure arithmetic on the buttons' rectangles, as laid out (vision_layout.h),
 * so the moves are tested on the host (tests/vision_keys_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_KEYS_H
#define VISION_KEYS_H

#include "vision_layout.h"

enum vision_key_move {
    VISION_KEY_NONE = 0,
    VISION_KEY_LEFT,
    VISION_KEY_RIGHT,
    VISION_KEY_UP,
    VISION_KEY_DOWN,
    VISION_KEY_NEXT, /* Tab: the next in order, after the last the first */
    VISION_KEY_PREV, /* Shift-Tab */
};

/* The button a move goes to from `from` among the n rectangles r (all
 * in one coordinate space). An arrow goes to the button whose centre lies
 * that way and is nearest, the distance across the arrow counting double so
 * that a button in line wins over a nearer one beside it; with none that way
 * it stays. `from` out of range (no focus yet) lands on the first button
 * for any move. Returns -1 when n is 0. */
int vision_keys_move(const struct vision_rect *r, int n, int from, enum vision_key_move m);

#endif
