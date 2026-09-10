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
 * k230_bsp/docs/HARDWARE_PINMAP.md. They are DOCUMENTED, not verified: no
 * key has been pressed on the physical keyboard yet, and the keycap legends
 * have not been read off the hardware. See
 * docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md.
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
    POS_KEYMAP_RESERVED   /* a real key whose meaning is not settled: the
                           * function row, Fn, the LILYGO key and the mic */
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

#endif
