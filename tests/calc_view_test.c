/*
 * PocketCalculator's view: the keyboard map, key by key; the keys that must
 * do nothing; the two lines of the display; the keypad table; and cutting an
 * expression that is too long for the glass from the left.
 *
 * The map is tested here rather than in the app because this is where it
 * lives, free of LVGL. tests/calc_app_test.c then pushes the same keys
 * through the real key stream to show they arrive.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "calc_engine.h"
#include "calc_view.h"

#include <stdio.h>
#include <string.h>

#define MINUS "\xE2\x88\x92"
#define TIMES "\xC3\x97"
#define DIVIDE "\xC3\xB7"
#define PLUS_MINUS "\xC2\xB1"

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
    if (!got || !want || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

/* ---- the keyboard map -------------------------------------------------- */

struct mapping {
    uint32_t key;
    enum calc_action action;
    const char *name;
};

static const struct mapping mapped[] = {
    { '0', CALC_ACT_DIGIT_0, "0" },         { '1', CALC_ACT_DIGIT_1, "1" },
    { '2', CALC_ACT_DIGIT_2, "2" },         { '3', CALC_ACT_DIGIT_3, "3" },
    { '4', CALC_ACT_DIGIT_4, "4" },         { '5', CALC_ACT_DIGIT_5, "5" },
    { '6', CALC_ACT_DIGIT_6, "6" },         { '7', CALC_ACT_DIGIT_7, "7" },
    { '8', CALC_ACT_DIGIT_8, "8" },         { '9', CALC_ACT_DIGIT_9, "9" },
    { '.', CALC_ACT_POINT, "." },           { ',', CALC_ACT_POINT, "," },
    { '+', CALC_ACT_ADD, "+" },             { '-', CALC_ACT_SUB, "-" },
    { '*', CALC_ACT_MUL, "*" },             { 'x', CALC_ACT_MUL, "x" },
    { 'X', CALC_ACT_MUL, "X" },             { '/', CALC_ACT_DIV, "/" },
    { '=', CALC_ACT_EQUALS, "=" },          { CALC_KEY_ENTER, CALC_ACT_EQUALS, "Enter" },
    { CALC_KEY_BACKSPACE, CALC_ACT_BACKSPACE, "Backspace" },
    { CALC_KEY_ESC, CALC_ACT_CLEAR, "Esc" }, { 'c', CALC_ACT_CLEAR, "c" },
    { 'C', CALC_ACT_CLEAR, "C" },           { 'n', CALC_ACT_NEGATE, "n" },
    { 'N', CALC_ACT_NEGATE, "N" },
};
#define MAPPED_COUNT (sizeof(mapped) / sizeof(mapped[0]))

static int is_mapped(uint32_t key)
{
    size_t i;

    for (i = 0; i < MAPPED_COUNT; i++) {
        if (mapped[i].key == key) {
            return 1;
        }
    }
    return 0;
}

static void test_key_map(void)
{
    char what[96];
    size_t i;
    uint32_t k;
    int unmapped_hits = 0;
    int actions_reached[CALC_ACT_COUNT];

    memset(actions_reached, 0, sizeof(actions_reached));
    check("26 keys are mapped", MAPPED_COUNT == 26);
    for (i = 0; i < MAPPED_COUNT; i++) {
        enum calc_action got = calc_view_action_for_key(mapped[i].key);

        snprintf(what, sizeof(what), "key %s maps to action %d", mapped[i].name,
                 (int)mapped[i].action);
        check(what, got == mapped[i].action);
        if (got > CALC_ACT_NONE && got < CALC_ACT_COUNT) {
            actions_reached[got]++;
        }
    }
    for (i = CALC_ACT_NONE + 1; i < CALC_ACT_COUNT; i++) {
        snprintf(what, sizeof(what), "action %d has a key", (int)i);
        check(what, actions_reached[i] > 0);
    }

    /* Every other 7-bit key does nothing, which includes the letters, the
     * space, %, the arrows, Tab (LV_KEY_NEXT) and Delete. */
    for (k = 0; k < 0x80; k++) {
        if (!is_mapped(k) && calc_view_action_for_key(k) != CALC_ACT_NONE) {
            unmapped_hits++;
            printf("FAIL key 0x%02x maps to something\n", (unsigned)k);
        }
    }
    check("no unmapped 7-bit key does anything", unmapped_hits == 0);
    check("% is not a key: there is no percent", calc_view_action_for_key('%') == CALC_ACT_NONE);
    check("nor is a space", calc_view_action_for_key(' ') == CALC_ACT_NONE);
    check("nor carriage return", calc_view_action_for_key('\r') == CALC_ACT_NONE);
    check("LV_KEY_UP does nothing", calc_view_action_for_key(17) == CALC_ACT_NONE);
    check("LV_KEY_NEXT does nothing", calc_view_action_for_key(9) == CALC_ACT_NONE);
    check("LV_KEY_DEL does nothing", calc_view_action_for_key(127) == CALC_ACT_NONE);
    check("the times sign is not a key", calc_view_action_for_key(0x00D7) == CALC_ACT_NONE);
    check("nor the division sign", calc_view_action_for_key(0x00F7) == CALC_ACT_NONE);
    check("nor the minus sign", calc_view_action_for_key(0x2212) == CALC_ACT_NONE);
    check("nor a Norwegian letter", calc_view_action_for_key(0x00E6) == CALC_ACT_NONE);
    check("nor a digit's code point plus 256",
          calc_view_action_for_key('5' + 0x100) == CALC_ACT_NONE);
    check("nor the largest key there is", calc_view_action_for_key(0xFFFFFFFFu) == CALC_ACT_NONE);
}

/* ---- the display ------------------------------------------------------- */

static void type_keys(struct calc_engine *c, const char *keys)
{
    for (; *keys; keys++) {
        calc_apply(c, calc_view_action_for_key((unsigned char)*keys));
    }
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

static void keys_show(const char *keys, const char *want_display, const char *want_expression)
{
    struct calc_engine c;
    char what[128];

    calc_init(&c);
    type_keys(&c, keys);
    snprintf(what, sizeof(what), "keys \"%s\" show", keys);
    eq_str(what, display(&c), want_display);
    snprintf(what, sizeof(what), "keys \"%s\" expression", keys);
    eq_str(what, expression(&c), want_expression);
}

static void test_display(void)
{
    struct calc_engine c;
    char buf[CALC_DISPLAY_MAX];
    char small[4];

    calc_init(&c);
    eq_str("a cleared calculator shows 0", display(&c), "0");
    eq_str("with no expression", expression(&c), "");
    calc_view_display(NULL, buf, sizeof(buf));
    eq_str("no engine shows 0", buf, "0");
    calc_view_expression(NULL, buf, sizeof(buf));
    eq_str("and no expression", buf, "");
    calc_view_display(&c, NULL, 10); /* must not crash */
    calc_view_expression(&c, buf, 0);

    /* Typed on a keyboard, through the map. */
    keys_show("12+3x", "0", "12 + 3 " TIMES);
    keys_show("12+3x4", "4", "12 + 3 " TIMES);
    keys_show("12+3x4=", "24", "12 + 3 " TIMES " 4 =");
    keys_show("12+3*4\n", "24", "12 + 3 " TIMES " 4 =");
    keys_show("9-4/2=", "7", "9 " MINUS " 4 " DIVIDE " 2 =");
    keys_show("7,5", "7.5", "");
    keys_show("7.5X2=", "15", "7.5 " TIMES " 2 =");
    keys_show("5n", "-5", "");
    keys_show("5N", "-5", "");
    keys_show("123\b", "12", "");
    keys_show("12+3c", "0", "");
    keys_show("12+3C", "0", "");
    keys_show("12+3\x1b", "0", "");
    keys_show("5/0=", CALC_TEXT_DIVIDE_BY_ZERO, "5 " DIVIDE " 0 =");
    keys_show("5/0=\x1b", "0", "");
    keys_show("2+2=n", "-4", "");
    keys_show("2 + 2 %=", "4", "2 + 2 =");
    keys_show("hello", "0", "");
    keys_show("1a2b3", "123", "");

    /* Overflow, in words. */
    {
        char keys[256];
        size_t pos = 0;
        int i;

        pos += (size_t)snprintf(keys + pos, sizeof(keys) - pos, "999999999999");
        for (i = 0; i < 9; i++) {
            pos += (size_t)snprintf(keys + pos, sizeof(keys) - pos, "*999999999999");
        }
        snprintf(keys + pos, sizeof(keys) - pos, "=");
        calc_init(&c);
        type_keys(&c, keys);
        eq_str("an overflow says so", display(&c), CALC_TEXT_OVERFLOW);
    }

    eq_str("the divide-by-zero message", CALC_TEXT_DIVIDE_BY_ZERO, "Can't divide by 0");
    eq_str("the overflow message", CALC_TEXT_OVERFLOW, "Overflow");
    check("both fit the main line's buffer",
          sizeof(CALC_TEXT_DIVIDE_BY_ZERO) <= CALC_DISPLAY_MAX &&
              sizeof(CALC_TEXT_OVERFLOW) <= CALC_DISPLAY_MAX);

    /* A buffer too small is cut, not overrun. */
    calc_init(&c);
    type_keys(&c, "12345");
    calc_view_display(&c, small, sizeof(small));
    eq_str("a short buffer takes what fits", small, "123");
    type_keys(&c, "+6");
    calc_view_expression(&c, small, sizeof(small));
    check("and so does the expression", strlen(small) == 3);

    /* The operators, as drawn. */
    eq_str("add glyph", calc_view_operator_glyph(CALC_OP_ADD), "+");
    eq_str("subtract glyph is U+2212", calc_view_operator_glyph(CALC_OP_SUB), MINUS);
    eq_str("multiply glyph is U+00D7", calc_view_operator_glyph(CALC_OP_MUL), TIMES);
    eq_str("divide glyph is U+00F7", calc_view_operator_glyph(CALC_OP_DIV), DIVIDE);
}

/* ---- the keypad -------------------------------------------------------- */

static void test_keypad(void)
{
    int seen[CALC_ACT_COUNT];
    int cells[CALC_PAD_ROWS][CALC_PAD_COLS];
    int overlaps = 0;
    int outside = 0;
    int r, col;
    size_t i;
    char what[96];

    memset(seen, 0, sizeof(seen));
    memset(cells, 0, sizeof(cells));
    for (i = 0; i < CALC_PAD_KEYS; i++) {
        const struct calc_pad_key *k = &calc_view_pad[i];

        if (k->action > CALC_ACT_NONE && k->action < CALC_ACT_COUNT) {
            seen[k->action]++;
        }
        if (k->row >= CALC_PAD_ROWS || k->span < 1 || k->col + k->span > CALC_PAD_COLS) {
            outside++;
            continue;
        }
        for (col = k->col; col < k->col + k->span; col++) {
            if (cells[k->row][col]++) {
                overlaps++;
            }
        }
    }
    check("every key is inside the 4 x 5 grid", outside == 0);
    check("no two keys share a cell", overlaps == 0);
    for (r = 0; r < CALC_PAD_ROWS; r++) {
        for (col = 0; col < CALC_PAD_COLS; col++) {
            snprintf(what, sizeof(what), "cell %d,%d has a key", r, col);
            check(what, cells[r][col] == 1);
        }
    }
    for (i = CALC_ACT_NONE + 1; i < CALC_ACT_COUNT; i++) {
        snprintf(what, sizeof(what), "action %d is on the keypad exactly once", (int)i);
        check(what, seen[i] == 1);
    }
    check("the keypad has 19 keys for 19 actions", CALC_PAD_KEYS == CALC_ACT_COUNT - 1);

    /* Where the important ones are. */
    check("= is bottom right", calc_view_pad[18].action == CALC_ACT_EQUALS &&
                                   calc_view_pad[18].row == 4 && calc_view_pad[18].col == 3);
    check("0 is two columns wide, bottom left",
          calc_view_pad[16].action == CALC_ACT_DIGIT_0 && calc_view_pad[16].row == 4 &&
              calc_view_pad[16].col == 0 && calc_view_pad[16].span == 2);
    check("the operators run down the right-hand column",
          calc_view_pad[3].action == CALC_ACT_DIV && calc_view_pad[3].col == 3 &&
              calc_view_pad[7].action == CALC_ACT_MUL && calc_view_pad[7].col == 3 &&
              calc_view_pad[11].action == CALC_ACT_SUB && calc_view_pad[11].col == 3 &&
              calc_view_pad[15].action == CALC_ACT_ADD && calc_view_pad[15].col == 3);
    check("clear is top left", calc_view_pad[0].action == CALC_ACT_CLEAR &&
                                   calc_view_pad[0].row == 0 && calc_view_pad[0].col == 0);
    check("7 8 9 is the top row of digits, as on a calculator",
          calc_view_pad[4].action == CALC_ACT_DIGIT_7 && calc_view_pad[4].row == 1 &&
              calc_view_pad[12].action == CALC_ACT_DIGIT_1 && calc_view_pad[12].row == 3);

    /* The labels. */
    for (i = CALC_ACT_DIGIT_0; i <= CALC_ACT_DIGIT_9; i++) {
        char want[2] = { (char)('0' + (i - CALC_ACT_DIGIT_0)), 0 };

        snprintf(what, sizeof(what), "digit key %s is labelled", want);
        eq_str(what, calc_view_key_label((enum calc_action)i), want);
    }
    eq_str("point label", calc_view_key_label(CALC_ACT_POINT), ".");
    eq_str("equals label", calc_view_key_label(CALC_ACT_EQUALS), "=");
    eq_str("clear label", calc_view_key_label(CALC_ACT_CLEAR), "C");
    eq_str("negate label is U+00B1", calc_view_key_label(CALC_ACT_NEGATE), PLUS_MINUS);
    check("backspace has no text label: it is a symbol",
          calc_view_key_label(CALC_ACT_BACKSPACE) == NULL);
    check("nothing is not a key", calc_view_key_label(CALC_ACT_NONE) == NULL);
    eq_str("the + key shows the + of the expression", calc_view_key_label(CALC_ACT_ADD),
           calc_view_operator_glyph(CALC_OP_ADD));
    eq_str("the - key shows the - of the expression", calc_view_key_label(CALC_ACT_SUB),
           calc_view_operator_glyph(CALC_OP_SUB));
    eq_str("the x key shows the x of the expression", calc_view_key_label(CALC_ACT_MUL),
           calc_view_operator_glyph(CALC_OP_MUL));
    eq_str("the / key shows the / of the expression", calc_view_key_label(CALC_ACT_DIV),
           calc_view_operator_glyph(CALC_OP_DIV));
}

/* ---- cutting from the left --------------------------------------------- */

/* Every character 10 px, the ellipsis included. */
static int32_t ten(uint32_t cp, void *user)
{
    int *calls = user;

    (void)cp;
    if (calls) {
        (*calls)++;
    }
    return 10;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;

    for (; *s; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

static int utf8_valid(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;

    while (*u) {
        int need = *u < 0x80 ? 0 : (*u & 0xE0) == 0xC0 ? 1 : (*u & 0xF0) == 0xE0 ? 2 : -1;
        int i;

        if (need < 0) {
            return 0;
        }
        for (i = 1; i <= need; i++) {
            if ((u[i] & 0xC0) != 0x80) {
                return 0;
            }
        }
        u += need + 1;
    }
    return 1;
}

static void test_fit_left(void)
{
    char out[CALC_EXPRESSION_MAX];
    const char *line = "12 + 345 " TIMES " 6 " MINUS " 7890 =";
    int calls = 0;

    calc_view_fit_left("12 + 3", 100, ten, NULL, out, sizeof(out));
    eq_str("a line that fits is left alone", out, "12 + 3");
    calc_view_fit_left("12 + 3", 60, ten, NULL, out, sizeof(out));
    eq_str("exactly fitting is fitting", out, "12 + 3");

    /* 22 characters at 10 px. With 160 px there is room for the ellipsis
     * and 15 characters; the tail starts at a space. */
    calc_view_fit_left(line, 160, ten, &calls, out, sizeof(out));
    check("a long line starts with the ellipsis", strncmp(out, CALC_ELLIPSIS, 3) == 0);
    check("and fits", (int32_t)utf8_chars(out) * 10 <= 160);
    check("and its tail starts at a space, not inside a number", out[3] == ' ');
    eq_str("the newest part is what is kept", out, CALC_ELLIPSIS " " TIMES " 6 " MINUS " 7890 =");
    check("measured once per character, plus one pass", calls <= 2 * 22 + 1);
    check("the result is valid UTF-8", utf8_valid(out));

    /* One token longer than the room: cut inside it, from the left. */
    calc_view_fit_left("1234567890123456", 60, ten, NULL, out, sizeof(out));
    eq_str("a single long token is cut where it has to be", out, CALC_ELLIPSIS "23456");

    /* Multi-byte characters are never split. */
    calc_view_fit_left(TIMES TIMES TIMES TIMES TIMES, 30, ten, NULL, out, sizeof(out));
    eq_str("a glyph is one character", out, CALC_ELLIPSIS TIMES TIMES);
    check("and stays whole", utf8_valid(out));

    /* No room at all. */
    calc_view_fit_left("12 + 3", 5, ten, NULL, out, sizeof(out));
    eq_str("with no room, just the ellipsis", out, CALC_ELLIPSIS);

    /* Degenerate calls. */
    calc_view_fit_left("", 0, ten, NULL, out, sizeof(out));
    eq_str("an empty line fits even where there is no room", out, "");
    calc_view_fit_left("", -4, ten, NULL, out, sizeof(out));
    eq_str("or less than none, before anything is laid out", out, "");
    calc_view_fit_left(NULL, 100, ten, NULL, out, sizeof(out));
    eq_str("no text is an empty line", out, "");
    calc_view_fit_left("12", 100, NULL, NULL, out, sizeof(out));
    eq_str("no measure is an empty line", out, "");
    calc_view_fit_left("12", 100, ten, NULL, NULL, 10); /* must not crash */

    /* Through the engine: the longest expression there is, cut to 30
     * characters, still ends in " =". */
    {
        struct calc_engine c;
        char expr[CALC_EXPRESSION_MAX];
        size_t len;
        int i;

        calc_init(&c);
        for (i = 0; i < CALC_MAX_OPERANDS - 1; i++) {
            type_keys(&c, "123456789012n-");
        }
        type_keys(&c, "5=");
        calc_view_expression(&c, expr, sizeof(expr));
        calc_view_fit_left(expr, 300, ten, NULL, out, sizeof(out));
        len = strlen(out);
        check("the longest expression is cut to fit", utf8_chars(out) * 10 <= 300);
        check("keeping its end", len > 2 && strcmp(out + len - 2, " =") == 0);
        check("and its start is marked", strncmp(out, CALC_ELLIPSIS, 3) == 0);
    }
}

int main(void)
{
    test_key_map();
    test_display();
    test_keypad();
    test_fit_left();

    printf("calc_view_test: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
