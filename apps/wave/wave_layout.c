/*
 * Wave's layout and keyboard policy. See wave_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_layout.h"

#include <string.h>

void wave_layout_choose(struct wave_layout *out, int w, int h, int landscape, int kb_visible)
{
    memset(out, 0, sizeof(*out));
    /* The orientation decides the keyboard; the room decides the shape. A
     * portrait run whose body is somehow wider than tall still keeps the
     * portrait keyboard rule. */
    out->field_tap_shows_keyboard = !landscape;
    out->show_keys = landscape;
    out->keys_label = kb_visible ? "HIDE" : "KEYS";

    if (landscape && h > 0 && h < WAVE_STRIP_MAX_H) {
        out->shape = WAVE_SHAPE_STRIP;
    } else if (w > h && w >= WAVE_HISTORY_MIN_W + WAVE_RAIL_W) {
        out->shape = WAVE_SHAPE_WIDE;
    } else {
        out->shape = WAVE_SHAPE_TALL;
    }
    out->show_history = out->shape != WAVE_SHAPE_STRIP;
    out->show_rail = out->shape != WAVE_SHAPE_STRIP;
}

int wave_layout_keys_shows(const struct wave_layout *l, int kb_visible)
{
    (void)l;
    return !kb_visible;
}
