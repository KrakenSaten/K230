/*
 * PocketCalculator's engine: precedence, entry, the result and what can be
 * done to it, the two error states and the way out of them, the limits, and
 * the formatting that keeps binary floating point off the display.
 *
 * Every case is written the way a user types it, one key at a time, and read
 * back the way the display shows it - through calc_view.c, which is linked
 * here for exactly that reason: what matters is the text on the glass, not
 * the double behind it.
 *
 * The last part is a fuzz run: a few hundred thousand keys from a fixed seed,
 * with every display string checked after every key. It is how "no NaN, no
 * inf, no -0, never wider than the display" is shown to hold for sequences
 * nobody thought to write down.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "calc_engine.h"
#include "calc_view.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINUS "\xE2\x88\x92"
#define TIMES "\xC3\x97"
#define DIVIDE "\xC3\xB7"

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void eq_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

/* ---- typing ------------------------------------------------------------ */

/* One character per key: 0-9 . + - * / = as themselves, c clear,
 * b backspace, n negate. Spaces are for the reader. Returns whether the
 * last key changed anything. */
static bool press(struct calc_engine *c, char k)
{
    enum calc_action a = CALC_ACT_NONE;

    if (k >= '0' && k <= '9') {
        a = (enum calc_action)(CALC_ACT_DIGIT_0 + (k - '0'));
    } else {
        switch (k) {
        case '.': a = CALC_ACT_POINT; break;
        case '+': a = CALC_ACT_ADD; break;
        case '-': a = CALC_ACT_SUB; break;
        case '*': a = CALC_ACT_MUL; break;
        case '/': a = CALC_ACT_DIV; break;
        case '=': a = CALC_ACT_EQUALS; break;
        case 'c': a = CALC_ACT_CLEAR; break;
        case 'b': a = CALC_ACT_BACKSPACE; break;
        case 'n': a = CALC_ACT_NEGATE; break;
        default: return false;
        }
    }
    return calc_apply(c, a);
}

static bool type(struct calc_engine *c, const char *keys)
{
    bool changed = false;

    for (; *keys; keys++) {
        if (*keys != ' ') {
            changed = press(c, *keys);
        }
    }
    return changed;
}

static const char *display(const struct calc_engine *c)
{
    static char buf[CALC_DISPLAY_MAX];

    calc_view_display(c, buf, sizeof(buf));
    return buf;
}

static const char *expression(const struct calc_engine *c)
{
    static char buf[CALC_EXPRESSION_MAX];

    calc_view_expression(c, buf, sizeof(buf));
    return buf;
}

/* From a cleared calculator, type keys, and compare both lines. */
static void expect(const char *keys, const char *want_display, const char *want_expression)
{
    struct calc_engine c;
    char what[256];

    calc_init(&c);
    type(&c, keys);
    snprintf(what, sizeof(what), "\"%s\" shows", keys);
    eq_str(what, display(&c), want_display);
    snprintf(what, sizeof(what), "\"%s\" expression", keys);
    eq_str(what, expression(&c), want_expression);
}

static void expect_display(const char *keys, const char *want_display)
{
    struct calc_engine c;
    char what[256];

    calc_init(&c);
    type(&c, keys);
    snprintf(what, sizeof(what), "\"%s\" shows", keys);
    eq_str(what, display(&c), want_display);
}

/* The last key of `keys` does nothing: it reports no change and leaves every
 * byte of the engine as it was. */
static void expect_noop(const char *what, const char *before, char key)
{
    struct calc_engine c;
    unsigned char snapshot[sizeof(c)];
    bool changed;

    calc_init(&c);
    type(&c, before);
    memcpy(snapshot, &c, sizeof(c));
    changed = press(&c, key);
    check(what, !changed && memcmp(snapshot, &c, sizeof(c)) == 0);
}

static void repeat(char *out, size_t out_len, const char *first, const char *unit, int times)
{
    size_t pos = 0;
    int i;

    pos += (size_t)snprintf(out + pos, out_len - pos, "%s", first);
    for (i = 0; i < times && pos < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%s", unit);
    }
}

/* ---- what may reach the display ---------------------------------------- */

#define NUM_ENTRY 0    /* being typed: a trailing point is fine, no exponent */
#define NUM_RESULT 1   /* formatted: canonical, exponent allowed */
#define NUM_OPERAND 2  /* in the expression: a committed entry or a result */

static int is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

/* -?(0|[1-9][0-9]*)(\.[0-9]*)?(e-?[1-9][0-9]*)?, at most 16 characters, and
 * never a minus in front of nothing. */
static int well_formed_number(const char *s, int kind)
{
    const char *p = s;
    const char *frac;
    size_t len = strlen(s);
    int nonzero = 0;

    if (len == 0 || len > CALC_NUMBER_CHARS) {
        return 0;
    }
    if (*p == '-') {
        p++;
    }
    if (!is_digit(*p) || (*p == '0' && is_digit(p[1]))) {
        return 0;
    }
    for (; is_digit(*p); p++) {
        nonzero |= *p != '0';
    }
    if (*p == '.') {
        p++;
        frac = p;
        for (; is_digit(*p); p++) {
            nonzero |= *p != '0';
        }
        if (kind != NUM_ENTRY && p == frac) {
            return 0; /* "5." only while typing */
        }
        if (kind == NUM_RESULT && p[-1] == '0') {
            return 0; /* trailing zeros are stripped from a result */
        }
    }
    if (*p == 'e') {
        if (kind == NUM_ENTRY) {
            return 0;
        }
        p++;
        if (*p == '-') {
            p++;
        }
        if (!is_digit(*p) || *p == '0') {
            return 0;
        }
        for (; is_digit(*p); p++) {
        }
    }
    if (*p != '\0') {
        return 0;
    }
    if (s[0] == '-' && !nonzero) {
        return 0; /* -0, -0.0, -0e5 */
    }
    return 1;
}

static int well_formed_display(const struct calc_engine *c, const char *text)
{
    if (c->state == CALC_STATE_ERROR) {
        return strcmp(text, CALC_TEXT_DIVIDE_BY_ZERO) == 0 ||
               strcmp(text, CALC_TEXT_OVERFLOW) == 0;
    }
    return well_formed_number(text, c->state == CALC_STATE_RESULT ? NUM_RESULT : NUM_ENTRY);
}

/* ---- the cases --------------------------------------------------------- */

static void test_start(void)
{
    struct calc_engine c;

    calc_init(&c);
    eq_str("a cleared calculator shows 0", display(&c), "0");
    eq_str("and no expression", expression(&c), "");
    check("and is taking an entry", c.state == CALC_STATE_ENTRY);
    check("a key that is not an action changes nothing", !calc_apply(&c, CALC_ACT_NONE));
    check("nor does the end of the list", !calc_apply(&c, CALC_ACT_COUNT));
    check("and there is no engine to press on NULL", !calc_apply(NULL, CALC_ACT_DIGIT_1));
    eq_str("NULL reads as 0", calc_number_text(NULL), "0");
}

static void test_arithmetic(void)
{
    expect("2+3=", "5", "2 + 3 =");
    expect("9-4=", "5", "9 " MINUS " 4 =");
    expect("6*7=", "42", "6 " TIMES " 7 =");
    expect("8/2=", "4", "8 " DIVIDE " 2 =");
    expect_display("7/2=", "3.5");
    expect_display("2-5=", "-3");
    expect_display("123456789012+1=", "123456789013");
    expect_display("1000000-1=", "999999");
}

static void test_precedence(void)
{
    expect("2+3*4=", "14", "2 + 3 " TIMES " 4 =");
    expect("10-4/2=", "8", "10 " MINUS " 4 " DIVIDE " 2 =");
    expect_display("2*3+4*5=", "26");
    expect_display("8/4/2=", "1");
    expect_display("7-2-1=", "4");
    expect_display("100/10*10=", "100");
    expect_display("2*3*4-5/5+1=", "24");
    expect_display("1+2*3-4/2=", "5");
    expect_display("2-3*4+5=", "-5");
    expect_display("1-2-3-4=", "-8");
    expect_display("64/4/4/2=", "2");
    expect_display("2*3/4=", "1.5");
    expect_display("1+1+1+1+1*0=", "4");
}

static void test_decimals(void)
{
    expect_display(".", "0.");
    expect_display(".5", "0.5");
    expect_display(".5*4=", "2");
    expect_display("1.5+2.25=", "3.75");
    expect_display("0.000001*1000000=", "1");
    expect_display("3.", "3.");
    expect("3.+1=", "4", "3 + 1 =");
    expect("0.50+1=", "1.5", "0.50 + 1 =");
    expect_display("1.2.3", "1.23");
    expect_display("0..5", "0.5");
    expect_display("5.+.", "0.");
    expect_noop("a second point in one number is rejected", "1.2", '.');
    expect_noop("even straight after the first", "7.", '.');
}

static void test_leading_zeros(void)
{
    expect_display("0", "0");
    expect_display("00", "0");
    expect_display("05", "5");
    expect_display("007", "7");
    expect_display("0.0", "0.0");
    expect_display("100", "100");
    expect_display("0.05", "0.05");
    expect_noop("0 then 0 is still 0", "0", '0');
}

static void test_negative(void)
{
    expect_display("5n", "-5");
    expect_display("5nn", "5");
    expect("5n+3=", "-2", "-5 + 3 =");
    expect_display("1.5n", "-1.5");
    expect_display("0.5n", "-0.5");
    expect_display("5n6", "-56");
    expect_noop("+/- on the empty entry does nothing", "", 'n');
    expect_noop("+/- on 0 does nothing: there is no -0", "0", 'n');
    expect_noop("nor on 0.00", "0.00", 'n');
    expect_noop("nor on the empty entry after an operator", "3*", 'n');
    expect("3*2n=", "-6", "3 " TIMES " -2 =");
    expect("5-3n=", "8", "5 " MINUS " -3 =");

    /* An operator first uses 0 as the first operand. */
    expect("+5=", "5", "0 + 5 =");
    expect("-5=", "-5", "0 " MINUS " 5 =");
    expect("*5=", "0", "0 " TIMES " 5 =");
    expect("/5=", "0", "0 " DIVIDE " 5 =");
}

static void test_after_result(void)
{
    struct calc_engine c;

    expect("2+3=*4=", "20", "5 " TIMES " 4 =");
    expect("2+3=7", "7", "");
    expect("2+3=.5", "0.5", "");
    expect("2+3=7+1=", "8", "7 + 1 =");

    /* Carried at full precision, shown rounded. */
    expect("1/3=*3=", "1", "0.333333333333 " TIMES " 3 =");
    expect("2/3=", "0.666666666667", "2 " DIVIDE " 3 =");

    /* +/- negates the result where it stands, and the expression that made
     * it goes, because it no longer made it. */
    expect("2+3=n", "-5", "");
    expect("2+3=n+1=", "-4", "-5 + 1 =");
    expect("2+3=nn", "5", "");
    expect_noop("+/- on a zero result does nothing", "2-2=", 'n');
    calc_init(&c);
    type(&c, "2-2=n");
    eq_str("and keeps its expression", expression(&c), "2 " MINUS " 2 =");

    /* Backspace does nothing to a result. */
    expect_noop("backspace on a result does nothing", "2+3=", 'b');
    calc_init(&c);
    type(&c, "2+3=b");
    eq_str("the result is still there", display(&c), "5");
    check("and still a result", c.state == CALC_STATE_RESULT);

    /* A result carried into the next calculation is the value, not a
     * re-typed entry: digits cannot be appended to it. */
    expect("12*2=+", "0", "24 +");
}

static void test_operator_replacement(void)
{
    expect("5+*3=", "15", "5 " TIMES " 3 =");
    expect("5+-*/2=", "2.5", "5 " DIVIDE " 2 =");
    expect("5+-", "0", "5 " MINUS);
    expect_noop("the same operator again changes nothing", "5+", '+');
    /* Backspace back to nothing, then an operator: it replaces, as it would
     * have before the digit was typed. */
    expect("5+3b*2=", "10", "5 " TIMES " 2 =");
    expect("+-*", "0", "0 " TIMES);
}

static void test_clear(void)
{
    struct calc_engine c;

    expect("12+3c", "0", "");
    expect("2+2=c", "0", "");
    expect("5/0=c", "0", "");
    expect("12+3c4=", "4", "4 =");
    expect_noop("clearing a cleared calculator changes nothing", "", 'c');
    calc_init(&c);
    type(&c, "99*9=+4");
    check("clear after anything reports a change", press(&c, 'c'));
    check("and is back at the start", c.state == CALC_STATE_ENTRY && c.count == 0 &&
                                          c.entry[0] == '\0');
}

static void test_backspace(void)
{
    expect_display("123b", "12");
    expect_display("123bb", "1");
    expect_display("123bbb", "0");
    expect_noop("backspace on the empty entry does nothing", "123bbb", 'b');
    expect_display("5.b", "5");
    expect_display("0.5b", "0.");
    expect_display("0.5bb", "0");
    expect_display(".b", "0");
    expect_display("5nb", "0");
    expect_display("15nb", "-1");
    expect_display("0.05nb", "0.0");
    expect_display("1234567890123b", "12345678901");
    expect("5+b", "0", "5 +");
    expect_noop("backspace does not delete the operator", "5+", 'b');
    expect("5+3b=", "5", "5 =");
}

static void test_divide_by_zero(void)
{
    struct calc_engine c;

    expect("5/0=", CALC_TEXT_DIVIDE_BY_ZERO, "5 " DIVIDE " 0 =");
    calc_init(&c);
    type(&c, "5/0=");
    check("dividing by zero is the error state", c.state == CALC_STATE_ERROR &&
                                                    c.error == CALC_ERROR_DIVIDE_BY_ZERO);
    expect_display("0/0=", CALC_TEXT_DIVIDE_BY_ZERO);
    expect_display("5/0.0=", CALC_TEXT_DIVIDE_BY_ZERO);
    expect("1+2/0*3=", CALC_TEXT_DIVIDE_BY_ZERO, "1 + 2 " DIVIDE " 0 " TIMES " 3 =");
    expect_display("2+3=-5=/0=", CALC_TEXT_DIVIDE_BY_ZERO);

    /* Out again. Every key leaves the error; only a digit or the point goes
     * on to be typed. */
    expect("5/0=7", "7", "");
    expect("5/0=.", "0.", "");
    expect("5/0=+", "0", "");
    expect("5/0=+3=", "3", "3 =");
    expect("5/0=b", "0", "");
    expect("5/0==", "0", "");
    expect("5/0=n", "0", "");
    expect("5/0=c", "0", "");
    expect("5/0=7+1=", "8", "7 + 1 =");
    calc_init(&c);
    type(&c, "5/0=+");
    check("an operator leaves the error without being applied",
          c.state == CALC_STATE_ENTRY && c.count == 0);

    /* A zero divisor is reported even when the expression would also have
     * overflowed before reaching it. */
    {
        char keys[512];

        repeat(keys, sizeof(keys), "999999999999", "*999999999999", 30);
        strcat(keys, "/0=");
        expect_display(keys, CALC_TEXT_DIVIDE_BY_ZERO);
    }
}

static void test_large(void)
{
    struct calc_engine c;

    expect_display("1234567890123", "123456789012");
    expect_noop("a thirteenth digit is ignored", "123456789012", '4');
    expect_display("0.1234567890123", "0.123456789012");
    expect_display("123456.7890123", "123456.789012");
    expect_display("0.0000000000011", "0.000000000001");
    expect_display("123456789012.", "123456789012.");
    expect_display("123456789012n", "-123456789012");
    expect_display("0.123456789012n", "-0.123456789012");

    expect("999999999999*999999999999=", "9.99999999998e23",
           "999999999999 " TIMES " 999999999999 =");
    expect_display("999999999999+1=", "1e12");
    expect_display("999999999999n-1=", "-1e12");
    expect_display("123456789012*1000=", "1.23456789012e14");
    /* The sign takes a column, so a negative mantissa gives up a digit. */
    expect_display("123456789012n*1000=", "-1.2345678901e14");

    calc_init(&c);
    type(&c, "999999999999*999999999999=");
    check("a scientific result is still a result", c.state == CALC_STATE_RESULT);
    type(&c, "/999999999999=");
    eq_str("and carries on at full precision", display(&c), "999999999999");
}

static void test_formatting(void)
{
    char out[CALC_NUMBER_MAX];

    expect_display("0.1+0.2=", "0.3");
    expect_display("1/3=", "0.333333333333");
    expect_display("2/3=", "0.666666666667");
    expect_display("1.50+1=", "2.5");
    expect_display("2.5*2=", "5");
    expect_display("1.1*1.1=", "1.21");
    expect_display("3.3*3=", "9.9");
    expect_display("0.7+0.1=", "0.8");
    expect_display("4.35*100=", "435");

    /* Cancellation: the digits a double never had are not shown. */
    expect_display("0.1+0.2-0.3=", "0");
    expect_display("0.3-0.1-0.2=", "0");
    expect_display("1.1*1.1-1.21=", "0");
    expect_display("1+0.000000000001-1=", "0.000000000001");
    expect_display("1+0.000000000001-1=/1000=", "1e-15");
    expect_display("0.1+0.2=-0.3=", "0");

    /* Never -0. */
    expect_display("5n*0=", "0");
    expect_display("0-0=", "0");
    expect_display("5n+5=", "0");
    expect_display("0*5n=", "0");
    expect_display("0.5n*0=", "0");

    /* Small numbers: plain while the unsigned form fits in 15 characters. */
    expect_display("1/1000000=", "0.000001");
    expect_display("0.000000000001/10=", "0.0000000000001");
    expect_display("0.000000000001/1000=", "1e-15");
    expect_display("1/300=", "3.33333333333e-3");
    expect_display("1/300=n", "-3.3333333333e-3");
    expect_display("1/30=", "0.0333333333333");
    expect_display("1/30=n", "-0.0333333333333");

    /* The formatter on its own. */
    check("0", calc_format_number(0.0, out, sizeof(out)) == CALC_FORMAT_OK &&
               strcmp(out, "0") == 0);
    check("-0 is written 0", calc_format_number(-0.0, out, sizeof(out)) == CALC_FORMAT_OK &&
                                 strcmp(out, "0") == 0);
    eq_str("0.5", (calc_format_number(0.5, out, sizeof(out)), out), "0.5");
    eq_str("100", (calc_format_number(100.0, out, sizeof(out)), out), "100");
    eq_str("1234.5", (calc_format_number(1234.5, out, sizeof(out)), out), "1234.5");
    eq_str("1e11", (calc_format_number(1e11, out, sizeof(out)), out), "100000000000");
    eq_str("1e12", (calc_format_number(1e12, out, sizeof(out)), out), "1e12");
    eq_str("pi", (calc_format_number(3.14159265358979, out, sizeof(out)), out),
           "3.14159265359");
    eq_str("2.5e-7", (calc_format_number(2.5e-7, out, sizeof(out)), out), "0.00000025");
    eq_str("123456789012.4", (calc_format_number(123456789012.4, out, sizeof(out)), out),
           "123456789012");
    eq_str("999999999999.5 rounds up and out of plain",
           (calc_format_number(999999999999.5, out, sizeof(out)), out), "1e12");
    eq_str("1e-99", (calc_format_number(1e-99, out, sizeof(out)), out), "1e-99");
    eq_str("-1.23456789012345e-50",
           (calc_format_number(-1.23456789012345e-50, out, sizeof(out)), out),
           "-1.23456789e-50");
    eq_str("9.99999999999e99",
           (calc_format_number(9.99999999999e99, out, sizeof(out)), out), "9.99999999999e99");
    check("1e-100 is zero", calc_format_number(1e-100, out, sizeof(out)) == CALC_FORMAT_ZERO &&
                                strcmp(out, "0") == 0);
    check("and so is -1e-100, without a sign",
          calc_format_number(-1e-100, out, sizeof(out)) == CALC_FORMAT_ZERO &&
              strcmp(out, "0") == 0);
    check("1e100 overflows", calc_format_number(1e100, out, sizeof(out)) ==
                                 CALC_FORMAT_OVERFLOW && out[0] == '\0');
    check("so does what rounds to it",
          calc_format_number(9.9999999999999e99, out, sizeof(out)) == CALC_FORMAT_OVERFLOW);
    check("NaN is never written", calc_format_number(NAN, out, sizeof(out)) ==
                                      CALC_FORMAT_OVERFLOW && out[0] == '\0');
    check("nor inf", calc_format_number(INFINITY, out, sizeof(out)) == CALC_FORMAT_OVERFLOW);
    check("nor -inf", calc_format_number(-INFINITY, out, sizeof(out)) == CALC_FORMAT_OVERFLOW);
}

static void test_overflow(void)
{
    struct calc_engine c;
    char keys[512];

    /* (10^12)^9 = 10^108: finite, and past 10^100. */
    repeat(keys, sizeof(keys), "999999999999", "*999999999999", 8);
    strcat(keys, "=");
    calc_init(&c);
    type(&c, keys);
    eq_str("a product past 1e100 overflows", display(&c), CALC_TEXT_OVERFLOW);
    check("which is the error state", c.state == CALC_STATE_ERROR &&
                                         c.error == CALC_ERROR_OVERFLOW);
    check("and the expression stays", strncmp(expression(&c), "999999999999 " TIMES, 15) == 0);
    type(&c, "4");
    eq_str("a digit starts again", display(&c), "4");

    /* Past what a double holds at all: infinity on the way, never shown. */
    repeat(keys, sizeof(keys), "999999999999", "*999999999999", 27);
    strcat(keys, "=");
    expect_display(keys, CALC_TEXT_OVERFLOW);

    /* From a carried result. */
    repeat(keys, sizeof(keys), "999999999999*999999999999=", "*999999999999", 7);
    strcat(keys, "=");
    expect_display(keys, CALC_TEXT_OVERFLOW);

    /* Just under: shown. And negated, where the sign costs the digit that
     * kept it under 1e100 once rounded, it overflows. */
    repeat(keys, sizeof(keys), "100000000000", "*100000000000", 7);
    strcat(keys, "*999999999999=");
    calc_init(&c);
    type(&c, keys);
    eq_str("9.99999999999e99 is still a number", display(&c), "9.99999999999e99");
    type(&c, "n");
    eq_str("and its negation, rounded to fit, is not", display(&c), CALC_TEXT_OVERFLOW);
    eq_str("with no expression claiming to have made it", expression(&c), "");

    /* Underflow: below 1e-99 it is 0, and carried on as 0. */
    repeat(keys, sizeof(keys), "0.000000000001", "/999999999999", 8);
    strcat(keys, "=");
    calc_init(&c);
    type(&c, keys);
    eq_str("a result below 1e-99 is 0", display(&c), "0");
    check("exactly 0, not an invisible remainder", c.result == 0.0);
    type(&c, "*999999999999=");
    eq_str("so multiplying it stays 0", display(&c), "0");
    check("and 0 has no sign", strcmp(display(&c), "0") == 0);
}

static void test_bounds(void)
{
    struct calc_engine c;
    char keys[1024];
    const char *e;
    size_t len;
    int i;

    calc_init(&c);
    for (i = 0; i < CALC_MAX_OPERANDS - 1; i++) {
        type(&c, "1+");
    }
    check("31 operands and operators are taken", c.count == CALC_MAX_OPERANDS - 1);
    check("the operator can still be replaced", press(&c, '-') && press(&c, '+'));
    type(&c, "1");
    check("the 32nd operand can be typed", strcmp(display(&c), "1") == 0);
    check("but an operator after it is ignored", !press(&c, '*'));
    eq_str("the entry is left alone", display(&c), "1");
    type(&c, "=");
    eq_str("and all 32 are evaluated", display(&c), "32");
    check("32 operands in the expression", c.count == CALC_MAX_OPERANDS);
    e = expression(&c);
    len = strlen(e);
    check("which ends in =", len > 2 && strcmp(e + len - 2, " =") == 0);

    /* The longest expression there can be fits the view's buffer whole. */
    calc_init(&c);
    for (i = 0; i < CALC_MAX_OPERANDS - 1; i++) {
        type(&c, "0.123456789012n-");
    }
    type(&c, "0.123456789012n");
    type(&c, "=");
    e = expression(&c);
    len = strlen(e);
    check("the longest expression is not cut", len < CALC_EXPRESSION_MAX - 1 &&
                                                   strcmp(e + len - 2, " =") == 0);
    check("32 of them", c.count == CALC_MAX_OPERANDS);
    /* -a, then 31 times "minus -a": 30a. */
    eq_str("and evaluates", display(&c), "3.70370367036");

    /* Keys beyond the limit do nothing wherever they are pressed. */
    repeat(keys, sizeof(keys), "", "9*", 40);
    calc_init(&c);
    type(&c, keys);
    check("forty operators stop at 31", c.count == CALC_MAX_OPERANDS - 1);
}

static void test_equals(void)
{
    struct calc_engine c;

    expect_noop("a second = after a result does nothing", "2+3=", '=');
    expect_noop("nor a third", "2+3==", '=');
    expect_noop("= with nothing typed does nothing", "", '=');
    expect_noop("= after backspacing everything does nothing", "7b", '=');
    calc_init(&c);
    type(&c, "2+3==");
    eq_str("the result after == is the result after =", display(&c), "5");
    eq_str("with the same expression", expression(&c), "2 + 3 =");
    expect("7=", "7", "7 =");
    expect("0=", "0", "0 =");
    expect("5+=", "5", "5 =");
    expect("5*3+=", "15", "5 " TIMES " 3 =");
    expect("+=", "0", "0 =");
    expect_display("1/3===*3=", "1");
}

/* ---- fuzz -------------------------------------------------------------- */

static uint32_t rng_state = 0x5EED1234u;

static uint32_t next_random(void)
{
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

struct fuzz_seen {
    long keys;
    long bad_display;
    long bad_operand;
    long bad_expression;
    long divide_by_zero;
    long overflow;
    long scientific;
    long negative_result;
    long small_result;
};

static void fuzz_step(struct calc_engine *c, enum calc_action a, struct fuzz_seen *s)
{
    char text[CALC_DISPLAY_MAX];
    char expr[CALC_EXPRESSION_MAX];
    int i;

    calc_apply(c, a);
    s->keys++;
    calc_view_display(c, text, sizeof(text));
    if (!well_formed_display(c, text)) {
        if (s->bad_display++ < 5) {
            printf("FAIL fuzz display \"%s\" (state %d)\n", text, (int)c->state);
        }
    }
    for (i = 0; i < c->count; i++) {
        if (!well_formed_number(c->text[i], NUM_OPERAND)) {
            if (s->bad_operand++ < 5) {
                printf("FAIL fuzz operand \"%s\"\n", c->text[i]);
            }
        }
    }
    calc_view_expression(c, expr, sizeof(expr));
    if (strlen(expr) >= CALC_EXPRESSION_MAX - 1 || strstr(expr, "nan") || strstr(expr, "inf")) {
        if (s->bad_expression++ < 5) {
            printf("FAIL fuzz expression \"%s\"\n", expr);
        }
    }
    if (c->state == CALC_STATE_ERROR) {
        if (c->error == CALC_ERROR_DIVIDE_BY_ZERO) {
            s->divide_by_zero++;
        } else {
            s->overflow++;
        }
    } else if (c->state == CALC_STATE_RESULT) {
        s->scientific += strchr(text, 'e') != NULL;
        s->negative_result += text[0] == '-';
        s->small_result += strncmp(text, "0.0", 3) == 0 || strstr(text, "e-") != NULL;
    }
}

static enum calc_action pick(const enum calc_action *bag, size_t n)
{
    return bag[next_random() % n];
}

static void test_fuzz(void)
{
    /* A plain mix, then one weighted towards big products, then one towards
     * small quotients: the uniform mix alone rarely reaches 1e100. */
    static const enum calc_action big[] = {
        CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9,
        CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9,
        CALC_ACT_DIGIT_8, CALC_ACT_MUL, CALC_ACT_MUL, CALC_ACT_EQUALS,
        CALC_ACT_NEGATE, CALC_ACT_MUL, CALC_ACT_SUB, CALC_ACT_BACKSPACE,
    };
    static const enum calc_action small[] = {
        CALC_ACT_POINT, CALC_ACT_DIGIT_0, CALC_ACT_DIGIT_0, CALC_ACT_DIGIT_0,
        CALC_ACT_DIGIT_1, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9, CALC_ACT_DIGIT_9,
        CALC_ACT_DIV, CALC_ACT_DIV, CALC_ACT_EQUALS, CALC_ACT_NEGATE,
        CALC_ACT_SUB, CALC_ACT_MUL, CALC_ACT_CLEAR, CALC_ACT_DIGIT_3,
    };
    struct calc_engine c;
    struct fuzz_seen s;
    long i;

    memset(&s, 0, sizeof(s));
    calc_init(&c);
    for (i = 0; i < 200000; i++) {
        fuzz_step(&c, (enum calc_action)(1 + next_random() % (CALC_ACT_COUNT - 1)), &s);
    }
    calc_init(&c);
    for (i = 0; i < 100000; i++) {
        /* A result is mostly carried on multiplying, so products keep
         * growing across = instead of starting again at the next digit. */
        if (c.state == CALC_STATE_RESULT && next_random() % 10 != 0) {
            fuzz_step(&c, CALC_ACT_MUL, &s);
        } else {
            fuzz_step(&c, pick(big, sizeof(big) / sizeof(big[0])), &s);
        }
    }
    calc_init(&c);
    for (i = 0; i < 100000; i++) {
        fuzz_step(&c, pick(small, sizeof(small) / sizeof(small[0])), &s);
    }

    printf("calc_engine_test: fuzz %ld keys: %ld divide-by-zero, %ld overflow, "
           "%ld scientific, %ld negative, %ld small\n",
           s.keys, s.divide_by_zero, s.overflow, s.scientific, s.negative_result, s.small_result);
    check("fuzz: every main line is a number or one of the two messages", s.bad_display == 0);
    check("fuzz: every operand in the expression is a well-formed number", s.bad_operand == 0);
    check("fuzz: every expression fits its buffer and has no nan or inf",
          s.bad_expression == 0);
    check("fuzz: it reached dividing by zero", s.divide_by_zero > 0);
    check("fuzz: it reached overflow", s.overflow > 0);
    check("fuzz: it reached scientific notation", s.scientific > 0);
    check("fuzz: it reached negative results", s.negative_result > 0);
    check("fuzz: it reached small results", s.small_result > 0);
}

/* The formatter against arbitrary doubles: every sign, every exponent a
 * double has, and the values either side of the edges. */
static void test_format_fuzz(void)
{
    char out[CALC_NUMBER_MAX];
    long bad = 0;
    long far = 0;
    long i;

    for (i = 0; i < 200000; i++) {
        uint64_t bits = ((uint64_t)next_random() << 32) | next_random();
        double mant = (double)(bits >> 11) / 9007199254740992.0; /* [0, 1) */
        int exp10 = (int)(next_random() % 661) - 330;
        double x = (mant + 0.1) * pow(10.0, exp10);
        enum calc_format f;

        if (next_random() & 1) {
            x = -x;
        }
        f = calc_format_number(x, out, sizeof(out));
        switch (f) {
        case CALC_FORMAT_OK:
            if (!well_formed_number(out, NUM_RESULT) || fabs(x) >= 1e100) {
                if (bad++ < 5) {
                    printf("FAIL format %.17g -> \"%s\"\n", x, out);
                }
            } else if (strcmp(out, "0") != 0) {
                double back = strtod(out, NULL);

                if (fabs(back - x) > fabs(x) * 1e-9) {
                    if (far++ < 5) {
                        printf("FAIL format %.17g -> \"%s\" reads back as %.17g\n", x, out, back);
                    }
                }
            }
            break;
        case CALC_FORMAT_ZERO:
            if (strcmp(out, "0") != 0 || fabs(x) >= 1e-99) {
                if (bad++ < 5) {
                    printf("FAIL format %.17g -> zero \"%s\"\n", x, out);
                }
            }
            break;
        case CALC_FORMAT_OVERFLOW:
            if (out[0] != '\0' || (isfinite(x) && fabs(x) < 9.99999999e99)) {
                if (bad++ < 5) {
                    printf("FAIL format %.17g -> overflow \"%s\"\n", x, out);
                }
            }
            break;
        }
    }
    check("formatter fuzz: every output well-formed and in its class", bad == 0);
    check("formatter fuzz: every output reads back as the value it rounds", far == 0);
}

int main(void)
{
    test_start();
    test_arithmetic();
    test_precedence();
    test_decimals();
    test_leading_zeros();
    test_negative();
    test_after_result();
    test_operator_replacement();
    test_clear();
    test_backspace();
    test_divide_by_zero();
    test_large();
    test_formatting();
    test_overflow();
    test_bounds();
    test_equals();
    test_fuzz();
    test_format_fuzz();

    printf("calc_engine_test: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
