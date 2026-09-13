/*
 * PocketCalculator's engine: what a key does to the calculation, and what
 * the calculation comes to.
 *
 * Pure. No LVGL, no I/O, no clock and nothing stored (tests/calculator_lint.sh
 * fails the build otherwise), so every rule below is tested on the host
 * without a display - including a fuzz run that throws hundreds of thousands
 * of keys at it and checks that nothing malformed ever reaches the screen.
 *
 * THE MODEL. Normal operator precedence ("model B"): x and / bind tighter
 * than + and -, and operators of equal precedence evaluate left to right, so
 * 2 + 3 x 4 is 14 and 8 / 4 / 2 is 1. Nothing is evaluated until =: the
 * expression is kept whole, shown on the line above the entry, and evaluated
 * in one pass when = is pressed. docs/apps/POCKETCALCULATOR.md is the rule
 * book; this header says where each rule lives.
 *
 * WHAT IS NOT HERE. No percent, no memory keys, no parentheses, no functions
 * and no history. A simple calculator, not a scientific one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETCALCULATOR_ENGINE_H
#define POCKETCALCULATOR_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

/* Digits one entry may hold. A zero standing alone in front of the point
 * ("0.5") is not counted; every other digit is, zeros after the point
 * included, because each of them takes a column of the display. */
#define CALC_ENTRY_DIGITS 12

/* The longest number the display is ever given, sign and exponent included.
 * Entries stop at 15 ("-0." and twelve digits); results are formatted to fit
 * (calc_format_number). */
#define CALC_NUMBER_CHARS 16
#define CALC_NUMBER_MAX (CALC_NUMBER_CHARS + 1)

/* Operands in one expression. An operator that would start a 33rd is
 * ignored; the 32nd can still be typed and evaluated. */
#define CALC_MAX_OPERANDS 32

/* Significant digits a result is shown with, at most. Twelve is where
 * binary floating-point noise is still far away: 0.1 + 0.2 is shown as 0.3,
 * not 0.30000000000000004. */
#define CALC_RESULT_DIGITS 12

enum calc_op {
    CALC_OP_ADD = 0,
    CALC_OP_SUB,
    CALC_OP_MUL,
    CALC_OP_DIV,
};

/* Everything a key can ask for. The touch keypad and the key stream both end
 * up here, through the same calc_apply(), which is how the app cannot treat a
 * tapped 7 and a typed 7 differently (DS v0.1 section 17.4). */
enum calc_action {
    CALC_ACT_NONE = 0,
    CALC_ACT_DIGIT_0,
    CALC_ACT_DIGIT_1,
    CALC_ACT_DIGIT_2,
    CALC_ACT_DIGIT_3,
    CALC_ACT_DIGIT_4,
    CALC_ACT_DIGIT_5,
    CALC_ACT_DIGIT_6,
    CALC_ACT_DIGIT_7,
    CALC_ACT_DIGIT_8,
    CALC_ACT_DIGIT_9,
    CALC_ACT_POINT,
    CALC_ACT_ADD,
    CALC_ACT_SUB,
    CALC_ACT_MUL,
    CALC_ACT_DIV,
    CALC_ACT_EQUALS,
    CALC_ACT_CLEAR,
    CALC_ACT_BACKSPACE,
    CALC_ACT_NEGATE,
    CALC_ACT_COUNT
};

enum calc_state {
    CALC_STATE_ENTRY = 0, /* typing, or about to: the main line is the entry */
    CALC_STATE_RESULT,    /* = was pressed: the main line is the result */
    CALC_STATE_ERROR      /* = was pressed and there is no number to show */
};

enum calc_error {
    CALC_ERROR_NONE = 0,
    CALC_ERROR_DIVIDE_BY_ZERO,
    CALC_ERROR_OVERFLOW
};

struct calc_engine {
    enum calc_state state;
    enum calc_error error;

    /* The expression. In ENTRY every operand is followed by its operator,
     * so the expression always ends in one ("12 + 3 x"). In RESULT and ERROR
     * it is the whole expression that was evaluated, one operator fewer than
     * operands, shown with " =" after it - or nothing at all once +/- has
     * changed the result it produced. */
    int count;
    double value[CALC_MAX_OPERANDS];
    char text[CALC_MAX_OPERANDS][CALC_NUMBER_MAX];
    enum calc_op op[CALC_MAX_OPERANDS];

    /* ENTRY: what has been typed since the last operator. "" is the empty
     * entry, shown as 0 and different from a typed 0 in two ways only: an
     * operator replaces the previous operator instead of committing it, and
     * = drops that trailing operator instead of using it. */
    char entry[CALC_NUMBER_MAX];

    /* RESULT: the value, at full double precision, and how it is shown. */
    double result;
    char result_text[CALC_NUMBER_MAX];
};

/* The cleared calculator: an empty entry, shown as 0, and no expression. */
void calc_init(struct calc_engine *c);

/* Apply one key. Returns true when anything changed, so a caller can skip a
 * repaint - and so a test can prove that a key which must do nothing (a
 * second =, backspace on a result) really did nothing. */
bool calc_apply(struct calc_engine *c, enum calc_action action);

/* The number on the main line: the entry ("0" while it is empty) or the
 * result. "" in the error state, whose words are the view's to choose. */
const char *calc_number_text(const struct calc_engine *c);

/* Whether the operator after operand i is shown. In ENTRY every operand has
 * one; in RESULT and ERROR the last does not, and " =" follows instead. */
bool calc_has_operator_after(const struct calc_engine *c, int i);

/* How x is shown, into out (at least CALC_NUMBER_MAX bytes).
 *
 * Rounded to CALC_RESULT_DIGITS significant digits, trailing zeros removed,
 * never "-0". Plain notation ("123.45", "0.003") when the rounded value is
 * below 10^12 and its plain form, without a sign, fits in 15 characters;
 * otherwise scientific ("1.23456789012e15", "-5e-20"): no "+", no leading
 * zeros in the exponent, and as many mantissa digits as fit in
 * CALC_NUMBER_CHARS with the sign counted.
 *
 * Returns CALC_FORMAT_OK, CALC_FORMAT_ZERO when x rounds below 1e-99 (out
 * is "0"), or CALC_FORMAT_OVERFLOW when x is not finite or rounds to 1e100 or
 * more (out is ""). Never writes nan or inf. */
enum calc_format {
    CALC_FORMAT_OK = 0,
    CALC_FORMAT_ZERO,
    CALC_FORMAT_OVERFLOW
};
enum calc_format calc_format_number(double x, char *out, size_t out_len);

#endif
