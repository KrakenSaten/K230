/*
 * PocketCalculator's engine. See calc_engine.h, and
 * docs/apps/POCKETCALCULATOR.md for the rules as a user meets them.
 *
 * Two decisions shape this file.
 *
 * Numbers are doubles, and what reaches the screen is decimal. Every result
 * passes through calc_format_number(), which rounds to twelve significant
 * digits, and every number read back from digits (an entry, or a rounded
 * sum) is built as an exact integer scaled by an exact power of ten, which
 * IEEE 754 rounds correctly. Nothing is converted with strtod or printf's
 * %f, so neither the C library's locale nor its idea of shortest
 * round-tripping ever decides what the display says.
 *
 * Cancellation is the one place twelve digits are not enough. 0.1 + 0.2 is
 * 0.30000000000000004 in binary, which rounds to 0.3 for the screen - but
 * subtracting 0.3 from it leaves 5.5e-17, which would be shown faithfully as
 * noise. So every + and - is rounded to fifteen significant digits at the
 * scale of its larger operand (add_rounded), which is below anything the
 * display can show and above the error a double carries. What is removed is
 * precisely the digits a double never had.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "calc_engine.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Every power of ten a double holds exactly. A product or quotient of an
 * exact integer and one of these is correctly rounded. */
static const double pow10_exact[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};
#define POW10_EXACT_MAX 22

/* Below this many digits of the larger operand a sum is noise (file comment).
 * Fifteen digits of an integer are still exact in a double. */
#define SUM_DIGITS 15

/* The largest decimal exponent shown, and the smallest. A result that rounds
 * to 1e100 is an overflow; one that rounds below 1e-99 is zero. */
#define EXP_MAX 99
#define EXP_MIN (-99)

static double scale10(double v, int e)
{
    while (e > POW10_EXACT_MAX) {
        v *= pow10_exact[POW10_EXACT_MAX];
        e -= POW10_EXACT_MAX;
    }
    while (e < -POW10_EXACT_MAX) {
        v /= pow10_exact[POW10_EXACT_MAX];
        e += POW10_EXACT_MAX;
    }
    return e >= 0 ? v * pow10_exact[e] : v / pow10_exact[-e];
}

/* ax (finite, positive) rounded to `digits` significant digits: the digits
 * into d, unterminated, and the power of ten of the first into *exp10.
 *
 * snprintf's %e does the rounding, and only the digits and the exponent are
 * read back: whatever character the C library puts between the first digit
 * and the rest is skipped, so a locale with a decimal comma changes
 * nothing. */
static void decompose(double ax, int digits, char *d, int *exp10)
{
    char buf[64];
    const char *p;
    int n = 0;
    int e = 0;
    bool neg = false;

    snprintf(buf, sizeof(buf), "%.*e", digits - 1, ax);
    for (p = buf; *p && *p != 'e' && *p != 'E'; p++) {
        if (*p >= '0' && *p <= '9' && n < digits) {
            d[n++] = *p;
        }
    }
    while (n < digits) {
        d[n++] = '0';
    }
    if (*p) {
        p++;
        if (*p == '-') {
            neg = true;
            p++;
        } else if (*p == '+') {
            p++;
        }
        for (; *p >= '0' && *p <= '9'; p++) {
            e = e * 10 + (*p - '0');
        }
    }
    *exp10 = neg ? -e : e;
}

static int decimal_width(int v)
{
    int w = 1;

    if (v < 0) {
        v = -v;
        w++;
    }
    while (v >= 10) {
        v /= 10;
        w++;
    }
    return w;
}

/* Characters of the plain form of nd significant digits whose first digit
 * is worth 10^e, without a sign. */
static int plain_width(int nd, int e)
{
    if (e >= 0) {
        return nd > e + 1 ? nd + 1 : e + 1;
    }
    return 2 + (-e - 1) + nd;
}

static void copy_text(char *out, size_t out_len, const char *s)
{
    if (out && out_len) {
        snprintf(out, out_len, "%s", s);
    }
}

enum calc_format calc_format_number(double x, char *out, size_t out_len)
{
    char d[CALC_RESULT_DIGITS];
    char buf[CALC_NUMBER_MAX + 8];
    char *p = buf;
    double ax;
    bool neg;
    int nd = CALC_RESULT_DIGITS;
    int e;
    int i;

    copy_text(out, out_len, "");
    if (!isfinite(x)) {
        return CALC_FORMAT_OVERFLOW;
    }
    if (x == 0.0) {
        copy_text(out, out_len, "0");
        return CALC_FORMAT_OK;
    }
    neg = x < 0.0;
    ax = neg ? -x : x;
    decompose(ax, nd, d, &e);
    if (e > EXP_MAX) {
        return CALC_FORMAT_OVERFLOW;
    }
    if (e < EXP_MIN) {
        /* Rounded below anything the display shows. Written as a plain 0,
         * never as -0: the sign of nothing is not information. */
        copy_text(out, out_len, "0");
        return CALC_FORMAT_ZERO;
    }
    while (nd > 1 && d[nd - 1] == '0') {
        nd--;
    }

    if (neg) {
        *p++ = '-';
    }
    /* The sign is left out of this comparison on purpose, so a number and
     * its negation are always written in the same notation. */
    if (e < 12 && plain_width(nd, e) <= CALC_NUMBER_CHARS - 1) {
        if (e >= 0) {
            for (i = 0; i <= e; i++) {
                *p++ = i < nd ? d[i] : '0';
            }
            if (nd > e + 1) {
                *p++ = '.';
                for (i = e + 1; i < nd; i++) {
                    *p++ = d[i];
                }
            }
        } else {
            *p++ = '0';
            *p++ = '.';
            for (i = 0; i < -e - 1; i++) {
                *p++ = '0';
            }
            for (i = 0; i < nd; i++) {
                *p++ = d[i];
            }
        }
        *p = '\0';
        copy_text(out, out_len, buf);
        return CALC_FORMAT_OK;
    }

    /* Scientific. Here the sign does count: the width is fixed, so a minus
     * costs a mantissa digit, and the value is rounded again to the digits
     * that fit rather than cut. One digit always fits ("-5e-99" is six). */
    while ((neg ? 1 : 0) + nd + (nd > 1 ? 1 : 0) + 1 + decimal_width(e) > CALC_NUMBER_CHARS) {
        nd--;
        decompose(ax, nd, d, &e);
        if (e > EXP_MAX) {
            /* 9.99999999999e99 has room for twelve digits; its negation has
             * room for eleven, and to eleven it rounds to 1e100. */
            copy_text(out, out_len, "");
            return CALC_FORMAT_OVERFLOW;
        }
        while (nd > 1 && d[nd - 1] == '0') {
            nd--;
        }
    }
    *p++ = d[0];
    if (nd > 1) {
        *p++ = '.';
        for (i = 1; i < nd; i++) {
            *p++ = d[i];
        }
    }
    snprintf(p, sizeof(buf) - (size_t)(p - buf), "e%d", e);
    copy_text(out, out_len, buf);
    return CALC_FORMAT_OK;
}

/* ---- arithmetic -------------------------------------------------------- */

/* a + b, with the digits below SUM_DIGITS of the larger operand removed
 * (file comment). A difference that is all noise becomes exactly 0. */
static double add_rounded(double a, double b)
{
    char d[SUM_DIGITS];
    double r = a + b;
    double aa = a < 0.0 ? -a : a;
    double ab = b < 0.0 ? -b : b;
    double big;
    double ar;
    double v;
    int64_t mant = 0;
    int eb;
    int er;
    int keep;
    int i;

    if (!isfinite(r) || r == 0.0) {
        return r + 0.0; /* and a -0 becomes 0 */
    }
    big = aa > ab ? aa : ab;
    ar = r < 0.0 ? -r : r;
    decompose(big, SUM_DIGITS, d, &eb);
    decompose(ar, SUM_DIGITS, d, &er);
    keep = er - eb + SUM_DIGITS;
    if (keep <= 0) {
        return 0.0;
    }
    if (keep > SUM_DIGITS) {
        keep = SUM_DIGITS;
    }
    decompose(ar, keep, d, &er);
    for (i = 0; i < keep; i++) {
        mant = mant * 10 + (d[i] - '0');
    }
    v = scale10((double)mant, er - (keep - 1));
    return r < 0.0 ? -v : v;
}

/* The first n operands with the n - 1 operators between them, x and / before
 * + and -, left to right within each. A zero divisor is looked for before
 * anything is computed, so "Can't divide by 0" wins over an overflow earlier
 * in the same expression: it is the mistake the user can see and fix. */
static enum calc_error evaluate(const struct calc_engine *c, int n, double *out)
{
    double sum = 0.0;
    double term;
    bool have_sum = false;
    int i;

    for (i = 0; i + 1 < n; i++) {
        if (c->op[i] == CALC_OP_DIV && c->value[i + 1] == 0.0) {
            return CALC_ERROR_DIVIDE_BY_ZERO;
        }
    }
    term = c->value[0];
    for (i = 0; i + 1 < n; i++) {
        double next = c->value[i + 1];

        switch (c->op[i]) {
        case CALC_OP_MUL:
            term *= next;
            break;
        case CALC_OP_DIV:
            term /= next;
            break;
        case CALC_OP_ADD:
        case CALC_OP_SUB:
            sum = have_sum ? add_rounded(sum, term) : term;
            have_sum = true;
            term = c->op[i] == CALC_OP_SUB ? -next : next;
            break;
        }
        if (!isfinite(term) || !isfinite(sum)) {
            return CALC_ERROR_OVERFLOW;
        }
    }
    sum = have_sum ? add_rounded(sum, term) : term;
    if (!isfinite(sum)) {
        return CALC_ERROR_OVERFLOW;
    }
    *out = sum + 0.0;
    return CALC_ERROR_NONE;
}

/* ---- the entry --------------------------------------------------------- */

static bool is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

/* Digits that count against CALC_ENTRY_DIGITS: all of them, except a zero
 * standing alone in front of the point. */
static int entry_digits(const char *s)
{
    int n = 0;

    if (*s == '-') {
        s++;
    }
    if (s[0] == '0' && s[1] == '.') {
        s++;
    }
    for (; *s; s++) {
        if (is_digit(*s)) {
            n++;
        }
    }
    return n;
}

/* True for an entry worth nothing: "", "0", "0.", "0.000". Such an entry
 * never carries a minus sign. */
static bool entry_is_zero(const char *s)
{
    for (; *s; s++) {
        if (is_digit(*s) && *s != '0') {
            return false;
        }
    }
    return true;
}

/* The entry as a number: its digits as one integer (thirteen at most, so
 * exact), scaled by an exact power of ten. */
static double entry_value(const char *s)
{
    int64_t mant = 0;
    int frac = 0;
    bool point = false;
    bool neg = false;
    double v;

    for (; *s; s++) {
        if (*s == '-') {
            neg = true;
        } else if (*s == '.') {
            point = true;
        } else if (is_digit(*s)) {
            mant = mant * 10 + (*s - '0');
            if (point) {
                frac++;
            }
        }
    }
    v = scale10((double)mant, -frac);
    return neg ? -v : v;
}

static void reset(struct calc_engine *c)
{
    memset(c, 0, sizeof(*c));
    c->state = CALC_STATE_ENTRY;
    c->error = CALC_ERROR_NONE;
}

static bool pristine(const struct calc_engine *c)
{
    return c->state == CALC_STATE_ENTRY && c->count == 0 && c->entry[0] == '\0';
}

void calc_init(struct calc_engine *c)
{
    if (c) {
        reset(c);
    }
}

/* Move the entry into the expression as operand `count`. A trailing point is
 * dropped from how it is shown: "5." was always going to mean 5. */
static void commit_entry(struct calc_engine *c)
{
    size_t len = strlen(c->entry);

    c->value[c->count] = entry_value(c->entry);
    if (len > 0 && c->entry[len - 1] == '.') {
        len--;
    }
    memcpy(c->text[c->count], c->entry, len);
    c->text[c->count][len] = '\0';
    c->count++;
    c->entry[0] = '\0';
}

static void enter_error(struct calc_engine *c, enum calc_error error)
{
    c->state = CALC_STATE_ERROR;
    c->error = error;
    c->entry[0] = '\0';
    c->result = 0.0;
    c->result_text[0] = '\0';
}

static bool press_digit(struct calc_engine *c, char digit)
{
    bool changed = false;
    size_t len;

    if (c->state != CALC_STATE_ENTRY) {
        reset(c); /* a digit after a result or an error starts again */
        changed = true;
    }
    len = strlen(c->entry);
    if (strcmp(c->entry, "0") == 0) {
        /* Leading zeros collapse: 0 then 0 is still 0, 0 then 5 is 5. */
        if (digit == '0') {
            return changed;
        }
        c->entry[0] = digit;
        return true;
    }
    if (entry_digits(c->entry) >= CALC_ENTRY_DIGITS) {
        return changed;
    }
    c->entry[len] = digit;
    c->entry[len + 1] = '\0';
    return true;
}

static bool press_point(struct calc_engine *c)
{
    bool changed = false;
    size_t len;

    if (c->state != CALC_STATE_ENTRY) {
        reset(c);
        changed = true;
    }
    if (c->entry[0] == '\0') {
        memcpy(c->entry, "0.", 3);
        return true;
    }
    if (strchr(c->entry, '.')) {
        return changed; /* one point per number */
    }
    len = strlen(c->entry);
    c->entry[len] = '.';
    c->entry[len + 1] = '\0';
    return true;
}

static bool press_backspace(struct calc_engine *c)
{
    size_t len;
    const char *digits;

    if (c->state == CALC_STATE_ERROR) {
        reset(c);
        return true;
    }
    if (c->state == CALC_STATE_RESULT) {
        return false; /* a result is not typing, and is not edited */
    }
    len = strlen(c->entry);
    if (len == 0) {
        return false; /* the operator before an empty entry stays */
    }
    c->entry[--len] = '\0';
    digits = c->entry[0] == '-' ? c->entry + 1 : c->entry;
    if (digits[0] == '\0' || strcmp(digits, "0") == 0) {
        /* Nothing typed is left: back to the empty entry, which is where
         * the user was before the first digit - so an operator pressed next
         * replaces the previous one, as it would have then. */
        c->entry[0] = '\0';
        return true;
    }
    if (c->entry[0] == '-' && entry_is_zero(c->entry)) {
        memmove(c->entry, c->entry + 1, len); /* "-0." is "0." */
    }
    return true;
}

static bool press_negate(struct calc_engine *c)
{
    char text[CALC_NUMBER_MAX];
    size_t len;

    if (c->state == CALC_STATE_ERROR) {
        reset(c);
        return true;
    }
    if (c->state == CALC_STATE_RESULT) {
        if (c->result == 0.0) {
            return false; /* there is no -0 */
        }
        if (calc_format_number(-c->result, text, sizeof(text)) == CALC_FORMAT_OVERFLOW) {
            enter_error(c, CALC_ERROR_OVERFLOW);
            c->count = 0;
            return true;
        }
        c->result = -c->result;
        memcpy(c->result_text, text, sizeof(text));
        /* The expression above no longer produced the number below it. */
        c->count = 0;
        return true;
    }
    if (entry_is_zero(c->entry)) {
        return false; /* the empty entry, 0, 0.000: nothing to negate */
    }
    len = strlen(c->entry);
    if (c->entry[0] == '-') {
        memmove(c->entry, c->entry + 1, len);
    } else {
        memmove(c->entry + 1, c->entry, len + 1);
        c->entry[0] = '-';
    }
    return true;
}

static bool press_operator(struct calc_engine *c, enum calc_op op)
{
    if (c->state == CALC_STATE_ERROR) {
        /* Out of the error, but the operator is not applied: it would have
         * nothing to apply to but a 0 the user never typed. */
        reset(c);
        return true;
    }
    if (c->state == CALC_STATE_RESULT) {
        /* Carry on from the result, at its full precision. */
        double r = c->result;
        char text[CALC_NUMBER_MAX];

        memcpy(text, c->result_text, sizeof(text));
        reset(c);
        c->value[0] = r;
        memcpy(c->text[0], text, sizeof(text));
        c->op[0] = op;
        c->count = 1;
        return true;
    }
    if (c->entry[0] == '\0') {
        if (c->count > 0) {
            /* Right after an operator: this one replaces it. */
            if (c->op[c->count - 1] == op) {
                return false;
            }
            c->op[c->count - 1] = op;
            return true;
        }
        /* Nothing typed at all: the first operand is 0. */
        c->value[0] = 0.0;
        memcpy(c->text[0], "0", 2);
        c->op[0] = op;
        c->count = 1;
        return true;
    }
    if (c->count >= CALC_MAX_OPERANDS - 1) {
        return false; /* the entry is the last operand there is room for */
    }
    commit_entry(c);
    c->op[c->count - 1] = op;
    return true;
}

static bool press_equals(struct calc_engine *c)
{
    enum calc_error error;
    double r = 0.0;

    if (c->state == CALC_STATE_ERROR) {
        reset(c);
        return true;
    }
    if (c->state == CALC_STATE_RESULT) {
        return false; /* no repeated =: the result stays as it is */
    }
    if (pristine(c)) {
        return false; /* nothing to evaluate */
    }
    if (c->entry[0] != '\0') {
        commit_entry(c);
    }
    /* An operator with nothing after it is dropped, simply by not being
     * shown or used: the expression is count operands and count - 1
     * operators from here on. */
    error = evaluate(c, c->count, &r);
    if (error != CALC_ERROR_NONE) {
        enter_error(c, error);
        return true;
    }
    switch (calc_format_number(r, c->result_text, sizeof(c->result_text))) {
    case CALC_FORMAT_OVERFLOW:
        enter_error(c, CALC_ERROR_OVERFLOW);
        return true;
    case CALC_FORMAT_ZERO:
        r = 0.0; /* and carried on as 0, not as the invisible remainder */
        break;
    case CALC_FORMAT_OK:
        break;
    }
    c->result = r;
    c->state = CALC_STATE_RESULT;
    return true;
}

bool calc_apply(struct calc_engine *c, enum calc_action action)
{
    bool was_pristine;

    if (!c) {
        return false;
    }
    switch (action) {
    case CALC_ACT_DIGIT_0:
    case CALC_ACT_DIGIT_1:
    case CALC_ACT_DIGIT_2:
    case CALC_ACT_DIGIT_3:
    case CALC_ACT_DIGIT_4:
    case CALC_ACT_DIGIT_5:
    case CALC_ACT_DIGIT_6:
    case CALC_ACT_DIGIT_7:
    case CALC_ACT_DIGIT_8:
    case CALC_ACT_DIGIT_9:
        return press_digit(c, (char)('0' + (action - CALC_ACT_DIGIT_0)));
    case CALC_ACT_POINT:
        return press_point(c);
    case CALC_ACT_ADD:
        return press_operator(c, CALC_OP_ADD);
    case CALC_ACT_SUB:
        return press_operator(c, CALC_OP_SUB);
    case CALC_ACT_MUL:
        return press_operator(c, CALC_OP_MUL);
    case CALC_ACT_DIV:
        return press_operator(c, CALC_OP_DIV);
    case CALC_ACT_EQUALS:
        return press_equals(c);
    case CALC_ACT_CLEAR:
        was_pristine = pristine(c);
        reset(c);
        return !was_pristine;
    case CALC_ACT_BACKSPACE:
        return press_backspace(c);
    case CALC_ACT_NEGATE:
        return press_negate(c);
    case CALC_ACT_NONE:
    case CALC_ACT_COUNT:
        break;
    }
    return false;
}

const char *calc_number_text(const struct calc_engine *c)
{
    if (!c) {
        return "0";
    }
    switch (c->state) {
    case CALC_STATE_ENTRY:
        return c->entry[0] ? c->entry : "0";
    case CALC_STATE_RESULT:
        return c->result_text;
    case CALC_STATE_ERROR:
        break;
    }
    return "";
}

bool calc_has_operator_after(const struct calc_engine *c, int i)
{
    if (!c || i < 0 || i >= c->count) {
        return false;
    }
    return c->state == CALC_STATE_ENTRY || i < c->count - 1;
}
