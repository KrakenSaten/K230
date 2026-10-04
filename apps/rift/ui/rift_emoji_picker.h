/*
 * RIFT COMMS: the emoji button beside a composer's field, and the picker it
 * opens.
 *
 * The picker is a small panel beside the button: a caption naming the
 * group, the groups' tabs down its left, and a grid of the group's emoji
 * (rift_emoji_pick.h), drawn by RIFT's colour emoji font. A tap on an emoji,
 * or Enter on the selected one, puts its UTF-8 into the field at the caret
 * and closes the picker. Nothing is sent: sending is still SEND or Enter in
 * the field, with the picker closed. The emoji picked go first in the first
 * group next time, and are kept in RIFT's preferences file.
 *
 * Like the keyboard base's letter picker (ui/shell/kbd_picker.h) it never
 * takes the focus: the field keeps it, with its text and its caret, so typing
 * and the composer's automatic focus work as they did. None of it is click-
 * focusable. While it is up, the field's keys come here first (a
 * preprocessing handler, before the text area acts on them): the arrows move
 * the selection - down past the last row is the next group, up past the
 * first the one before - Enter picks, Esc closes; any other key closes it and
 * then types as usual. A tap outside it closes it.
 *
 * It closes on its own when what it was opened for has gone: the field was
 * hidden, disabled or deleted, it lost the keys (Tab, a dialog), the screen
 * was laid out again (a turn, the touch keyboard), or RIFT's screen left.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_EMOJI_PICKER_H
#define RIFT_EMOJI_PICKER_H

#include "rift_app.h"

#include <stdbool.h>

/* The emoji button for field, made in parent (a flex row, after the field):
 * one touch target wide, as tall as RIFT's actions. Tapping it opens the
 * picker for field. Enabled exactly while field is (rift_emoji_picker_check).
 * NULL when field is NULL. */
lv_obj_t *rift_emoji_button(struct rift_app *a, lv_obj_t *parent, lv_obj_t *field);

/* Open the picker for field, placed beside anchor (the button). False, with
 * nothing shown, when field is missing, hidden or disabled. If field does not
 * have the keys it is given them (from the app's timer). */
bool rift_emoji_picker_open(lv_obj_t *field, lv_obj_t *anchor);

bool rift_emoji_picker_is_open(void);

/* Close without inserting anything. Safe when closed. */
void rift_emoji_picker_close(void);

/* From the app's timer: the buttons follow their fields' enabled state, and
 * an open picker closes if its field has gone. */
void rift_emoji_picker_check(struct rift_app *a);

/* For tests: the group shown, the selected cell, a cell's button and the
 * emoji it inserts (NULL past the end or while closed), and a group's tab. */
unsigned rift_emoji_picker_group(void);
unsigned rift_emoji_picker_selected(void);
lv_obj_t *rift_emoji_picker_cell(unsigned i);
const char *rift_emoji_picker_item(unsigned i);
lv_obj_t *rift_emoji_picker_tab(unsigned g);
lv_obj_t *rift_emoji_picker_box(void);

#endif
