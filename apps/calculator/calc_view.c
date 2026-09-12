/*
 * PocketCalculator's view. See calc_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "calc_view.h"

#include <stdio.h>
#include <string.h>

#define GLYPH_MINUS "\xE2\x88\x92"  /* U+2212 */
#define GLYPH_TIMES "\xC3\x97"      /* U+00D7 */
#define GLYPH_DIVIDE "\xC3\xB7"     /* U+00F7 */
#define GLYPH_PLUS_MINUS "\xC2\xB1" /* U+00B1 */

enum calc_action calc_view_action_for_key(uint32_t key)
{
    if (key >= '0' && key <= '9') {
        return (enum calc_action)(CALC_ACT_DIGIT_0 + (int)(key - '0'));
    }
    switch (key) {
    case '.':
    case ',':
        return CALC_ACT_POINT;
    case '+':
        return CALC_ACT_ADD;
    case '-':
        return CALC_ACT_SUB;
    case '*':
    case 'x':
    case 'X':
        return CALC_ACT_MUL;
    case '/':
        return CALC_ACT_DIV;
    case '=':
    case CALC_KEY_ENTER:
        return CALC_ACT_EQUALS;
    case CALC_KEY_BACKSPACE:
        return CALC_ACT_BACKSPACE;
    case CALC_KEY_ESC:
    case 'c':
    case 'C':
        return CALC_ACT_CLEAR;
    case 'n':
    case 'N':
        return CALC_ACT_NEGATE;
    default:
        return CALC_ACT_NONE;
    }
}

void calc_view_display(const struct calc_engine *c, char *out, size_t out_len)
{
    const char *text = "0";

    if (!out || out_len == 0) {
        return;
    }
    if (c && c->state == CALC_STATE_ERROR) {
        text = c->error == CALC_ERROR_DIVIDE_BY_ZERO ? CALC_TEXT_DIVIDE_BY_ZERO
                                                      : CALC_TEXT_OVERFLOW;
    } else if (c) {
        text = calc_number_text(c);
    }
    snprintf(out, out_len, "%s", text);
}

const char *calc_view_operator_glyph(enum calc_op op)
{
    switch (op) {
    case CALC_OP_ADD:
        return "+";
    case CALC_OP_SUB:
        return GLYPH_MINUS;
    case CALC_OP_MUL:
        return GLYPH_TIMES;
    case CALC_OP_DIV:
        return GLYPH_DIVIDE;
    }
    return "?";
}

/* Append s at *pos, never past the end: a line that does not fit is cut
 * rather than overrun. */
static void append(char *out, size_t out_len, size_t *pos, const char *s)
{
    size_t n = strlen(s);

    if (*pos >= out_len - 1) {
        return;
    }
    if (n > out_len - 1 - *pos) {
        n = out_len - 1 - *pos;
    }
    memcpy(out + *pos, s, n);
    *pos += n;
    out[*pos] = '\0';
}

void calc_view_expression(const struct calc_engine *c, char *out, size_t out_len)
{
    size_t pos = 0;
    int i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!c) {
        return;
    }
    for (i = 0; i < c->count; i++) {
        if (i > 0) {
            append(out, out_len, &pos, " ");
        }
        append(out, out_len, &pos, c->text[i]);
        if (calc_has_operator_after(c, i)) {
            append(out, out_len, &pos, " ");
            append(out, out_len, &pos, calc_view_operator_glyph(c->op[i]));
        }
    }
    if (c->count > 0 && c->state != CALC_STATE_ENTRY) {
        append(out, out_len, &pos, " =");
    }
}

/* ---- the keypad -------------------------------------------------------- */

const struct calc_pad_key calc_view_pad[CALC_PAD_KEYS] = {
    { CALC_ACT_CLEAR, 0, 0, 1 },   { CALC_ACT_BACKSPACE, 0, 1, 1 },
    { CALC_ACT_NEGATE, 0, 2, 1 },  { CALC_ACT_DIV, 0, 3, 1 },
    { CALC_ACT_DIGIT_7, 1, 0, 1 }, { CALC_ACT_DIGIT_8, 1, 1, 1 },
    { CALC_ACT_DIGIT_9, 1, 2, 1 }, { CALC_ACT_MUL, 1, 3, 1 },
    { CALC_ACT_DIGIT_4, 2, 0, 1 }, { CALC_ACT_DIGIT_5, 2, 1, 1 },
    { CALC_ACT_DIGIT_6, 2, 2, 1 }, { CALC_ACT_SUB, 2, 3, 1 },
    { CALC_ACT_DIGIT_1, 3, 0, 1 }, { CALC_ACT_DIGIT_2, 3, 1, 1 },
    { CALC_ACT_DIGIT_3, 3, 2, 1 }, { CALC_ACT_ADD, 3, 3, 1 },
    { CALC_ACT_DIGIT_0, 4, 0, 2 }, { CALC_ACT_POINT, 4, 2, 1 },
    { CALC_ACT_EQUALS, 4, 3, 1 },
};

const char *calc_view_key_label(enum calc_action action)
{
    static const char *const digits[10] = { "0", "1", "2", "3", "4",
                                            "5", "6", "7", "8", "9" };

    if (action >= CALC_ACT_DIGIT_0 && action <= CALC_ACT_DIGIT_9) {
        return digits[action - CALC_ACT_DIGIT_0];
    }
    switch (action) {
    case CALC_ACT_POINT:
        return ".";
    case CALC_ACT_ADD:
        return "+";
    case CALC_ACT_SUB:
        return GLYPH_MINUS;
    case CALC_ACT_MUL:
        return GLYPH_TIMES;
    case CALC_ACT_DIV:
        return GLYPH_DIVIDE;
    case CALC_ACT_EQUALS:
        return "=";
    case CALC_ACT_CLEAR:
        return "C";
    case CALC_ACT_NEGATE:
        return GLYPH_PLUS_MINUS;
    default:
        return NULL; /* backspace, and anything that is not a key */
    }
}

/* ---- a line too long for the display ---------------------------------- */

/* The code point that starts at s. Malformed UTF-8 is taken a byte at a
 * time, which only matters for measuring and cannot run past the string. */
static uint32_t decode(const unsigned char *s)
{
    if (s[0] < 0x80) {
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
               (s[2] & 0x3F);
    }
    return s[0];
}

void calc_view_fit_left(const char *text, int32_t max_width, calc_glyph_width_fn width,
                        void *user, char *out, size_t out_len)
{
    const unsigned char *u = (const unsigned char *)text;
    size_t n;
    size_t i;
    size_t cut_any;
    size_t cut_space;
    int32_t total = 0;
    int32_t tail = 0;
    int32_t ellipsis;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || !width) {
        return;
    }
    n = strlen(text);
    if (n == 0) {
        return; /* nothing is never too wide, whatever the room */
    }
    for (i = 0; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            total += width(decode(u + i), user);
        }
    }
    if (total <= max_width) {
        snprintf(out, out_len, "%s", text);
        return;
    }

    /* Walk in from the right, one character at a time, until the next one
     * would not fit beside the ellipsis. Remember the leftmost place reached
     * and the leftmost space reached. */
    ellipsis = width(0x2026, user);
    cut_any = n;
    cut_space = n;
    i = n;
    while (i > 0) {
        size_t start = i - 1;

        while (start > 0 && (u[start] & 0xC0) == 0x80) {
            start--;
        }
        tail += width(decode(u + start), user);
        if (ellipsis + tail > max_width) {
            break;
        }
        cut_any = start;
        if (u[start] == ' ') {
            cut_space = start;
        }
        i = start;
    }
    snprintf(out, out_len, "%s%s", CALC_ELLIPSIS, text + (cut_space < n ? cut_space : cut_any));
}
