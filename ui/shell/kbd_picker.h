/*
 * The long-press letter picker of the keyboard base.
 *
 * Hold A or O in a text field and a small row of letters appears beside the
 * field: å Å ä Ä æ Æ for A, ø Ø ö Ö for O (pos_keymap_hold_choices). Left and
 * Right move the selection, Enter types it, Esc cancels; a finger can tap a
 * letter, and a tap anywhere else cancels. A key that means none of those
 * closes the picker and then does what it always does.
 *
 * It is deliberately not a popup framework. It never takes the focus: the
 * field keeps it, and its text and its caret, throughout. While the picker is
 * up the driver (shell_kbd.c) hands it the keyboard's keys before they reach
 * the stream, and the chosen letter goes into the one stream
 * (pos_input_push_key) as a code point like any typed key, which pos_input
 * packs as UTF-8 for the field. The focus group, the one-deep redirection an
 * alert owns (pos_input.h, DS §18.8) and the touch keyboard are untouched.
 *
 * It closes on its own when what it was opened for has gone: the field lost
 * the focus or was deleted, or an alert took the keys.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_KBD_PICKER_H
#define DOORS_KBD_PICKER_H

#include "pos_input.h"
#include "pos_keyboard.h"

#include <stdbool.h>

/* How long a key is held before the picker opens: the touch keyboard's one
 * hold timing (DS §17.3), not a second one. */
#define KBD_PICKER_HOLD_MS POS_KB_REPEAT_DELAY_MS

/* The most choices one key may offer. */
#define KBD_PICKER_MAX 8

/* Whether a held key may open the picker now: the object the keys reach is
 * an enabled text area, it is not the raw key target, and no alert holds
 * the keys. Anywhere else a held key is just a key. */
bool kbd_picker_can_open(void);

/* Show the choices beside the focused text area, the first one selected.
 * False, with nothing shown, when kbd_picker_can_open() is false or n is
 * 0 or more than KBD_PICKER_MAX. Opening while open replaces the choices. */
bool kbd_picker_open(const pos_key_t *choices, unsigned n);

bool kbd_picker_is_open(void);

/* A key from the keyboard base while the picker is up. Left and Right move
 * the selection, Enter types it and closes, Esc closes: each returns true,
 * the key consumed. Any other key closes the picker and returns false, so
 * the caller delivers it as usual. False, doing nothing, when closed. */
bool kbd_picker_key(pos_key_t key);

/* Close it if what it was opened for has gone. The driver's poll calls this
 * every tick; it costs a pointer comparison while the picker is closed. */
void kbd_picker_check(void);

/* Close without typing anything. Safe when closed. */
void kbd_picker_close(void);

/* For tests: the selected index, and the choice buttons (NULL past the
 * end or while closed), which a test taps as a finger would. */
unsigned kbd_picker_selected(void);
lv_obj_t *kbd_picker_choice(unsigned i);

#endif
