/*
 * Where Vision's parts go, for the two shapes (DS §38): the picture in the
 * frame's own shape, as large as the body lets it be; the status; the mode
 * switcher; and the mode's own buttons. Tall, the switcher and the buttons
 * share the foot of the screen; wide, they are a column beside the picture.
 * The counters are not here: they lie on the picture's corners
 * (vision_app.c). Pure arithmetic on the body's size; it never asks which
 * way the display is turned (DS §21.2).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_LAYOUT_H
#define VISION_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define VISION_GAP 16
#define VISION_BTN_H 64
#define VISION_STATUS_H 28
/* The mode switcher: a touch target's height, and its width when the mode's
 * buttons stand beside it (alone in its row, or in the wide column, it takes
 * the row's width). */
#define VISION_MODE_H 64
#define VISION_MODE_W 160
#define VISION_PICTURE_MAX 1024
#define VISION_SIDE_MIN 240
/* The mode's own buttons (the mode switcher is not one of them). */
#define VISION_LAYOUT_BUTTONS 4
#define VISION_LAYOUT_STATUS_LINES_MAX 4
/* Tall, one or two buttons stand beside the switcher; more get rows of their
 * own under it, three across (four make two rows of two). Wide, two across
 * the column. A selector's parts (one_row) keep one row, beside the switcher
 * when tall, wherever each still gets VISION_BTN_MIN_W. */
#define VISION_COLS_TALL 3
#define VISION_COLS_WIDE 2
#define VISION_BTN_MIN_W 100

struct vision_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct vision_layout {
    bool wide;
    struct vision_rect picture;
    struct vision_rect mode;   /* the mode switcher */
    struct vision_rect status;
    struct vision_rect btn[VISION_LAYOUT_BUTTONS];
    int buttons;
};

/* w x h is the body, the insets the corner clearance; frame_w x frame_h the
 * picture's shape as the owner holds the unit (the preview turned upright);
 * `buttons` (1 .. VISION_LAYOUT_BUTTONS) the mode's own, `one_row` when they
 * are the parts of one selector (SEGMENT's view), and `status_lines` (1 ..
 * VISION_LAYOUT_STATUS_LINES_MAX) what the mode says. 0, or -1 when the
 * body cannot hold the screen. */
int vision_layout_compute(struct vision_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t frame_w, uint32_t frame_h, int buttons, bool one_row, int status_lines);

/* THE SHEET: a panel over the picture (the mode picker, a mode's setup)
 * with a title and rows of a caption and up to three choices. On a panel
 * wide enough the caption stands left of its choices; otherwise above
 * them. Rects are relative to the panel's own corner. */
#define VISION_SHEET_ROWS_MAX 5
#define VISION_SHEET_CELLS_MAX 3
#define VISION_SHEET_PAD 16
#define VISION_SHEET_TITLE_H 28
#define VISION_SHEET_CAPTION_H 24
#define VISION_SHEET_CAPTION_W 200
#define VISION_SHEET_ROW_GAP 12
#define VISION_SHEET_SIDE_BY_SIDE_MIN_W 640

struct vision_sheet_layout {
    bool side_by_side;
    struct vision_rect title;
    struct vision_rect caption[VISION_SHEET_ROWS_MAX];
    struct vision_rect cell[VISION_SHEET_ROWS_MAX][VISION_SHEET_CELLS_MAX];
    struct vision_rect body; /* what is left under the rows, for text (h 0 when nothing) */
};

/* The sheet on a panel of panel_w x panel_h, `rows` rows of cells[i]
 * choices. 0, or -1 when it does not fit (or asks for too much). */
int vision_layout_sheet(struct vision_sheet_layout *l, int32_t panel_w, int32_t panel_h, int rows,
                        const int cells[]);

#endif
