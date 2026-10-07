/*
 * PocketCalculator's view: which key means what, what the two lines of the
 * display say, and where every key sits on the keypad.
 *
 * Pure, like the engine. No LVGL: the three non-printing keys the calculator
 * listens for are written here as the numbers LVGL gives them, and
 * calc_app.c checks at compile time that the two still agree. That keeps the
 * whole keyboard map unit-testable (tests/calc_view_test.c) without a
 * display.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETCALCULATOR_VIEW_H
#define POCKETCALCULATOR_VIEW_H

#include "calc_engine.h"

#include <stdint.h>

/* LVGL's LV_KEY_BACKSPACE, LV_KEY_ENTER and LV_KEY_ESC. */
#define CALC_KEY_BACKSPACE 8u
#define CALC_KEY_ENTER 10u
#define CALC_KEY_ESC 27u

/* The keyboard map. One logical key - a code point, or one of the three
 * above - to one action, whatever produced the key (DS v0.1 section 17.4):
 *
 *   0-9            the digit
 *   .  ,           the decimal point
 *   +              add
 *   -              subtract
 *   *  x  X        multiply
 *   /              divide
 *   =  Enter       equals
 *   Backspace      backspace
 *   Esc  c  C      clear
 *   n  N           +/- (negate)
 *
 * Anything else is CALC_ACT_NONE, and the app ignores it without a sound. */
enum calc_action calc_view_action_for_key(uint32_t key);

/* The words shown instead of a number in the error state. */
#define CALC_TEXT_DIVIDE_BY_ZERO "Can't divide by 0"
#define CALC_TEXT_OVERFLOW "Overflow"

/* The main line: the entry, the result, or the error in words. */
#define CALC_DISPLAY_MAX 32
void calc_view_display(const struct calc_engine *c, char *out, size_t out_len);

/* The line above it: the expression, operators as the keypad draws them,
 * "12 + 3 x" while typing and "12 + 3 x 4 =" once evaluated. "" when there
 * is none. Thirty-two numbers of sixteen characters, 31 operators of up to
 * five bytes with their spaces, " =" and the terminator fit. */
#define CALC_EXPRESSION_MAX 704
void calc_view_expression(const struct calc_engine *c, char *out, size_t out_len);

/* The operator as it is drawn: "+", and U+2212, U+00D7 and U+00F7 for the
 * other three, which the product fonts carry (tools/design/gen_fonts.sh). */
const char *calc_view_operator_glyph(enum calc_op op);

/* ---- the keypad -------------------------------------------------------- */

#define CALC_PAD_COLS 4
#define CALC_PAD_ROWS 5
#define CALC_PAD_KEYS 19

/*   C   <x   +/-  /
 *   7   8    9    x
 *   4   5    6    -
 *   1   2    3    +
 *   0 (two)  .    =
 *
 * Every action but CALC_ACT_NONE is on it exactly once, and = sits at the
 * bottom of the operator column (tests/calc_view_test.c). */
struct calc_pad_key {
    enum calc_action action;
    uint8_t row;
    uint8_t col;
    uint8_t span; /* columns */
};
extern const struct calc_pad_key calc_view_pad[CALC_PAD_KEYS];

/* The text on a key. NULL for backspace, which the app draws as a symbol
 * glyph rather than a character. */
const char *calc_view_key_label(enum calc_action action);

/* ---- a line too long for the display ---------------------------------- */

/* U+2026, the ellipsis the expression line starts with once it is cut. */
#define CALC_ELLIPSIS "\xE2\x80\xA6"

/* The width in pixels of one code point, as whoever draws it measures it. */
typedef int32_t (*calc_glyph_width_fn)(uint32_t codepoint, void *user);

/* Fit text into max_width, cutting from the left: the newest part of an
 * expression is the part being worked on. text itself when it fits;
 * otherwise the ellipsis and the longest tail that fits beside it, starting
 * at a space where one allows, so the line never begins with half a
 * number. */
void calc_view_fit_left(const char *text, int32_t max_width, calc_glyph_width_fn width,
                        void *user, char *out, size_t out_len);

#endif
