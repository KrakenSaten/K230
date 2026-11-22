/*
 * Card games: the Lamplight table (docs/design/card-games, direction A) -
 * the cloth under one lamp, chips, print on the cloth, and the look of the
 * controls around it. The palette is fixed in every theme (owner decision 2,
 * 2026-11-22), as the Games Hub's is; Night dims the scene as before.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_TABLE_H
#define CG_TABLE_H

#include "lvgl.h"

#include <stddef.h>
#include <stdint.h>

enum cg_cloth_kind {
    CG_CLOTH_CLARET = 0, /* Blackjack */
    CG_CLOTH_INDIGO,     /* Hold'em */
    CG_CLOTH_GREEN,      /* Klondike */
    CG_CLOTH_KINDS
};

enum cg_table_ink {
    CG_INK_IVORY = 0, /* text and outlines on the cloth */
    CG_INK_GOLD,      /* print, amounts, the winning side */
    CG_INK_DIM,       /* secondary text */
    CG_INK_DARK,      /* text on ivory */
    CG_INK_ROOM       /* the dark room round the table */
};

lv_color_t cg_table_ink(enum cg_table_ink ink);

/* Fill `coords` with the cloth, rendered once per size and kept, dimmed at
 * night. Returns 1, or 0 when there was no memory for the lamp and the
 * cloth's deep colour was laid flat instead. */
int cg_table_cloth(lv_layer_t *layer, const lv_area_t *coords, enum cg_cloth_kind kind);
/* Night dims the scene - cloth, cards and chips - by half; the interface
 * drawn over it (labels, buttons) is not dimmed. Set before drawing. */
void cg_table_set_night(int night);
/* Free the kept cloths (a game closing), and what they hold now. */
void cg_table_purge(void);
size_t cg_table_bytes(void);

/* `amount` as stacks of chips (500, 100, 50 and 10), centred on cx with their
 * feet on `base_y`; `d` is a chip's width. Returns the width drawn. */
int cg_table_chips(lv_layer_t *layer, int cx, int base_y, uint32_t amount, int d);
/* The most chips of width `d` can cover round (cx, base_y): half its width
 * across, and its height above the base (four stacks of eight). */
void cg_table_chips_reach(int d, int *half_w, int *height);

/* The room, a HUD panel and a table button in the Lamplight look. These set
 * local styles; call them after the object's own styles. */
void cg_table_style_room(lv_obj_t *obj);
void cg_table_style_hud(lv_obj_t *panel, lv_obj_t *label, lv_obj_t *value);
void cg_table_style_button(lv_obj_t *button, int primary, int enabled);
void cg_table_style_text(lv_obj_t *label, enum cg_table_ink ink);

#endif
