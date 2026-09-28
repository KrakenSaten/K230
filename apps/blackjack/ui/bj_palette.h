/*
 * PG Blackjack card world palette: the colours of paper, ink, card backs and
 * empty slots. The values are art in apps/blackjack/art/cards_palette.txt;
 * ui/shell/CMakeLists.txt turns them into `bj_palette` in the build tree.
 * Interface colours never come from here: they are tokens.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_PALETTE_H
#define PGBJ_PALETTE_H

#include "lvgl.h"

enum bj_ink {
    BJ_INK_PAPER = 0,
    BJ_INK_EDGE,
    BJ_INK_BLACK,
    BJ_INK_RED,
    BJ_INK_BACK,
    BJ_INK_BACK_LINE,
    BJ_INK_SLOT,
    BJ_INK_FELT,
    BJ_INK_COUNT
};

extern const lv_color_t bj_palette[BJ_INK_COUNT];

#endif
