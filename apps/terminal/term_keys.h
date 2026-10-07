/*
 * Terminal: what a key sends down the PTY.
 *
 * The bytes a VT100-class terminal sends for a key, as the program on the
 * other end (the shell's line editor, less, vi) expects them: a character
 * as UTF-8, Enter as CR, Backspace as DEL (the tty's erase character),
 * Ctrl with a letter as its control code, Alt as an ESC prefix, and the
 * cursor keys as ESC [ A..D - or ESC O A..D while the program has asked for
 * application cursor keys (DECCKM, term_screen.app_cursor_keys), and the
 * function keys as xterm sends them: ESC O P..S for F1-F4, ESC [ nn ~ above.
 *
 * Pure: no LVGL. The app maps the logical keys it receives onto these kinds.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TERM_KEYS_H
#define TERM_KEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum term_key_kind {
    TERM_KEY_NONE = 0,
    TERM_KEY_CHAR, /* cp holds the character */
    TERM_KEY_ENTER,
    TERM_KEY_BACKSPACE,
    TERM_KEY_TAB,
    TERM_KEY_BACKTAB,
    TERM_KEY_ESC,
    TERM_KEY_UP,
    TERM_KEY_DOWN,
    TERM_KEY_RIGHT,
    TERM_KEY_LEFT,
    TERM_KEY_HOME,
    TERM_KEY_END,
    TERM_KEY_DELETE,
    TERM_KEY_PAGE_UP,
    TERM_KEY_PAGE_DOWN,
    /* The function keys, F1 first; TERM_KEY_F1 + n - 1 is Fn. They reach
     * the Terminal only with the keyboard's Fn held (hw_actions.h: a bare
     * function key is a Doors shortcut everywhere). */
    TERM_KEY_F1,
    TERM_KEY_F12 = TERM_KEY_F1 + 11,
};

#define TERM_MOD_SHIFT 0x1u
#define TERM_MOD_CTRL 0x2u
#define TERM_MOD_ALT 0x4u

struct term_key {
    enum term_key_kind kind;
    uint32_t cp;
    unsigned mods;
};

/* The longest encoding: ESC, then ESC [ 1 ; 8 A, or a 4-byte character. */
#define TERM_KEY_MAX 16

/* Encode k into out. Returns the byte count, 0 for a key that sends
 * nothing (NONE, or a character that is not one). */
size_t term_key_encode(const struct term_key *k, bool app_cursor_keys, uint8_t out[TERM_KEY_MAX]);

#endif
