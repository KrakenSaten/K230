/*
 * PocketOS touch keyboard (DS v0.1 §17.3).
 *
 * A sheet of keys docked to the bottom of the screen. It is a *source* of the
 * logical key stream and nothing more: every key it produces goes through
 * pos_input_push_key(), so a field cannot tell a tapped character from one
 * typed on the host keyboard or, later, on a physical one (§17.4). It never
 * touches a text area, never looks at what is focused, and holds no text.
 *
 * Geometry is DS §17.3 to the pixel, including the approved deviation DEV-1:
 * keys are 52 wide against the 64 px minimum, and 64 tall, so the minimum is
 * met in height and missed in width only.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POS_KEYBOARD_H
#define POS_KEYBOARD_H

#include "lvgl.h"

#include <stdbool.h>

/* DS §17.3 geometry. The sheet is the panel's full width. */
#define POS_KB_W 568
#define POS_KB_H 296
/* 6 each side leaves a 556-wide row; 8 above and below the four rows makes
 * the 296: 8 + 4x64 + 3x8 + 8. */
#define POS_KB_PAD_H 6
#define POS_KB_PAD_V 8
#define POS_KB_KEY_W 52
#define POS_KB_KEY_H 64
#define POS_KB_WIDE_W 80
#define POS_KB_GAP 4
#define POS_KB_ROW_GAP 8

/* DS §17.3: Backspace repeats after a 400 ms hold, then every 60 ms. */
#define POS_KB_REPEAT_DELAY_MS 400
#define POS_KB_REPEAT_MS 60

/* Not in the DS: the window in which a second Shift tap means "lock" rather
 * than "off". Matched to the repeat delay so the keyboard has one hold
 * timing rather than two. */
#define POS_KB_SHIFT_LOCK_MS 400

/* What the right key of row 4 does, which DS §17.3 makes depend on the field
 * the caller is editing rather than on anything the keyboard can see. */
enum pos_kb_return {
    POS_KB_RETURN_DONE,    /* single-line: commits, and asks the owner to hide */
    POS_KB_RETURN_NEWLINE  /* multi-line: inserts a line break */
};

enum pos_kb_layer {
    POS_KB_LAYER_ALPHA,
    POS_KB_LAYER_SYMBOL
};

enum pos_kb_shift {
    POS_KB_SHIFT_OFF,
    POS_KB_SHIFT_ONCE, /* next character only */
    POS_KB_SHIFT_LOCK
};

/* Create the sheet, hidden, aligned to the bottom of parent. */
lv_obj_t *pos_keyboard_create(lv_obj_t *parent);

/* Which key sits on the right of row 4. Default is POS_KB_RETURN_DONE. */
void pos_keyboard_set_return(lv_obj_t *kb, enum pos_kb_return ret);

/* Called when Done is pressed, so the owner can hide the sheet and commit.
 * Done also pushes LV_KEY_ENTER, which is what makes a single-line field
 * report itself ready; the keyboard does not decide what "commit" means. */
void pos_keyboard_set_done_cb(lv_obj_t *kb, void (*cb)(void *user), void *user);

/* Show and hide. Neither changes focus: DS §17.2 says opening the keyboard
 * must not move it, and the sheet's keys are deliberately outside the focus
 * group so a tap on a key cannot move it either. */
void pos_keyboard_show(lv_obj_t *kb);
void pos_keyboard_hide(lv_obj_t *kb);
bool pos_keyboard_is_shown(lv_obj_t *kb);

/* State, for callers that mirror it and for tests. */
enum pos_kb_layer pos_keyboard_layer(lv_obj_t *kb);
enum pos_kb_shift pos_keyboard_shift(lv_obj_t *kb);

/* The key currently drawn with this label, or NULL. Labels change with the
 * layer and with Shift, so "q" and "Q" are different lookups. Lets a test
 * press what a finger presses rather than calling into the implementation. */
lv_obj_t *pos_keyboard_key(lv_obj_t *kb, const char *label);

#endif
