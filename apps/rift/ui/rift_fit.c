/*
 * Words that fit at every text size: a column widened to its widest word,
 * and a caption that drops what it can spare before it is cut. See
 * rift_widgets.h.
 *
 * The column widths in RIFT are Small's (DS §3), measured in Mono 14. A
 * reader who chose Medium or Large (DS §46) gets the same words in Mono 17
 * or 19, and a fixed column would clip them at its edge - at the left one,
 * for a right-aligned number. Split from rift_widgets.c, which is near the
 * size tests/rift_lint.sh allows a file.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_widgets.h"

#include "rift_format.h"

#include <string.h>

void rift_cell_widen(lv_obj_t *cell, const char *text)
{
    int32_t now;
    int32_t want;

    if (!cell) {
        return;
    }
    now = lv_obj_get_style_width(cell, LV_PART_MAIN);
    if (!LV_COORD_IS_PX(now)) {
        return; /* a growing or content-sized cell sizes itself */
    }
    want = rift_cell_text_width(cell, text);
    if (want > now) {
        lv_obj_set_width(cell, want);
    }
}

void rift_cell_set_text_first_fit(lv_obj_t *cell, const char *const *candidates, int count)
{
    int32_t room;
    int i;

    if (!cell || !candidates || count <= 0) {
        return;
    }
    /* The room the text has: inside the cell's padding (the strip's caption
     * has some on its left, rift_strip.c). The same as its width for every
     * cell with none. */
    room = lv_obj_get_content_width(cell);
    for (i = 0; i < count; i++) {
        if (room <= 0 || rift_cell_text_width(cell, candidates[i]) <= room) {
            /* Set whole, so the fingerprint of an earlier fit no longer
             * describes what the cell says. */
            lv_obj_set_user_data(cell, NULL);
            rift_label_set(cell, candidates[i]);
            return;
        }
    }
    rift_cell_set_text_fit_room(cell, candidates[count - 1], room);
}
