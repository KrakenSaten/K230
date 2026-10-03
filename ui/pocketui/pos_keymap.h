/*
 * Physical keyboard translation: TCA8418 key codes to logical keys.
 *
 * The keyboard base board carries a TCA8418 matrix controller whose FIFO
 * yields one byte per event: bit 7 is press or release, bits 0 to 6 are the
 * key number in a 7 x 10 matrix. This turns those into the one vocabulary
 * DS v0.1 §17.4 fixes - a Unicode code point for a printable character, an
 * LV_KEY_* constant for anything else - so a physical key and a tapped key
 * are the same thing by the time either reaches a field.
 *
 * Pure: no I/O, no LVGL objects, no hardware. It holds only the modifier
 * state that one event needs from the last, which is what makes the whole
 * mapping testable on the host without a keyboard attached.
 *
 * PROVENANCE. The matrix layout, the key names and the shifted symbols are
 * taken from the vendor launcher's own driver
 * (vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/ui_hardware.c,
 * tca8418_key_name, extension_keyboard_shift_symbol_for_code and
 * extension_keyboard_ascii_for_code) and from
 * k230_bsp/docs/HARDWARE_PINMAP.md. They are DOCUMENTED, not verified, apart
 * from six codes read off the physical keyboard on unit A on 2026-09-12 -
 * Shift 7, Z 18, Q 20, A 29, J 34 and W 39 - each with its raw press and
 * release byte and its keycap legend. Two of the vendor's shifted symbols
 * were wrong there and are corrected in pos_keymap.c: W gives '_' and Q
 * gives '\''. Every other entry remains DOCUMENTED, not verified: no other
 * key has been pressed on the hardware and no other keycap has been read.
 * See docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POS_KEYMAP_H
#define POS_KEYMAP_H

#include "pos_input.h"

#include <stdbool.h>
#include <stdint.h>

/* 7 rows x 10 columns. Code 0 is "no event". */
#define POS_KEYMAP_ROWS 7
#define POS_KEYMAP_COLS 10
#define POS_KEYMAP_MAX_CODE (POS_KEYMAP_ROWS * POS_KEYMAP_COLS)

/* The TCA8418 FIFO byte. */
#define POS_KEYMAP_EVENT_PRESSED 0x80u
#define POS_KEYMAP_EVENT_CODE 0x7Fu

/* What one event did, for callers that want more than the key. */
enum pos_keymap_effect {
    POS_KEYMAP_NONE = 0,  /* nothing to deliver: a release, a modifier, an
                           * unknown code, or a key with no meaning yet */
    POS_KEYMAP_KEY,       /* a logical key to push into the stream */
    POS_KEYMAP_MODIFIER,  /* modifier state changed; no key */
    POS_KEYMAP_RESERVED   /* a real key that types nothing: the function
                           * row, the LILYGO key and the mic. What each one
                           * does is the shell's (ui/shell/hw_actions.h),
                           * not a character's */
};

/* Modifier state carried between events. Zeroed by pos_keymap_reset(). */
struct pos_keymap {
    uint8_t shift; /* held */
    uint8_t ctrl;  /* held */
    uint8_t alt;   /* held */
    uint8_t fn;    /* held */
    uint8_t caps;  /* latched, toggles on press */
};

void pos_keymap_reset(struct pos_keymap *k);

/* Feed one raw FIFO byte. Returns the logical key to push, or 0 when the
 * event produces none. *effect, when given, says why.
 *
 * Only a press yields a key. A release updates the modifiers and nothing
 * else, which keeps the stream a sequence of discrete keys exactly as the
 * touch keyboard leaves it (pos_input.h). Auto-repeat is the source's
 * business, not this layer's. */
pos_key_t pos_keymap_event(struct pos_keymap *k, uint8_t event,
                           enum pos_keymap_effect *effect);

/* The key's name on the matrix, or NULL for an unused position. For
 * diagnostics and for tests that want to read a failure. */
const char *pos_keymap_name(uint8_t code);

/* True when the code is a modifier this layer tracks. */
bool pos_keymap_is_modifier(uint8_t code);

/* ---- long-press letters ------------------------------------------------ *
 *
 * No keycap carries the Nordic and German letters, so holding a key offers
 * them in a small picker (ui/shell/kbd_picker.h): A offers å Å ä Ä æ Æ and
 * O offers ø Ø ö Ö, both cases always, whatever Shift and Caps say. Only A
 * and O have any. This is the table; timing the hold and showing the picker
 * are the driver's, since this layer has no clock.
 *
 * Returns how many characters the key offers and points *choices at them
 * (Unicode code points, small letter first in each pair), or returns 0. */
unsigned pos_keymap_hold_choices(uint8_t code, const pos_key_t **choices);

#endif
