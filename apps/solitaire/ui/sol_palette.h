/*
 * PG Solitaire card world palette: the colours of paper, ink, card backs and
 * empty slots. The values are art in apps/solitaire/art/cards_palette.txt;
 * ui/shell/CMakeLists.txt turns them into `sol_palette` in the build tree.
 * Interface colours never come from here: they are tokens.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_PALETTE_H
#define PGSOL_PALETTE_H

#include "lvgl.h"

enum sol_ink {
    SOL_INK_PAPER = 0,
    SOL_INK_EDGE,
    SOL_INK_BLACK,
    SOL_INK_RED,
    SOL_INK_BACK,
    SOL_INK_BACK_LINE,
    SOL_INK_SLOT,
    SOL_INK_FELT,
    SOL_INK_COUNT
};

extern const lv_color_t sol_palette[SOL_INK_COUNT];

#endif
