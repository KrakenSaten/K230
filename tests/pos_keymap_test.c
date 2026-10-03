/*
 * Physical keyboard translation: TCA8418 codes to the one logical
 * vocabulary (DS v0.1 §17.4).
 *
 * This is the whole of the physical keyboard that can be tested without the
 * hardware, and it is deliberately the whole of what has been written: the
 * transport is documented but unverified, so nothing above this layer exists
 * yet. Everything here is fed raw FIFO bytes exactly as the controller
 * produces them - bit 7 press or release, bits 0 to 6 the matrix code.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_keymap.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;
static struct pos_keymap km;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* Press a code and expect a key. */
static void press_is(const char *what, uint8_t code, pos_key_t want)
{
    enum pos_keymap_effect eff;
    pos_key_t got = pos_keymap_event(&km, (uint8_t)(code | POS_KEYMAP_EVENT_PRESSED), &eff);

    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: code %u gave 0x%02x, want 0x%02x\n", what, code,
               (unsigned)got, (unsigned)want);
    }
}

static void release(uint8_t code)
{
    pos_keymap_event(&km, code, NULL);
}

static void press(uint8_t code)
{
    pos_keymap_event(&km, (uint8_t)(code | POS_KEYMAP_EVENT_PRESSED), NULL);
}

static enum pos_keymap_effect effect_of(uint8_t event)
{
    enum pos_keymap_effect eff;

    pos_keymap_event(&km, event, &eff);
    return eff;
}

/* Codes for the keys used repeatedly below. */
#define C_A 29
#define C_Z 18
#define C_M 13
#define C_Q 20
#define C_W 39
#define C_J 34
#define C_SHIFT 7
#define C_CTRL 23
#define C_ALT 19
#define C_FN 9
#define C_CAPS 10
#define C_SPACE 5
#define C_SPACE2 14
#define C_ENTER 21
#define C_DEL 41
#define C_TAB 6
#define C_ESC 40
#define C_UP 22
#define C_DOWN 12
#define C_LEFT 2
#define C_RIGHT 1
#define C_1 49
#define C_0 51

int main(void)
{
    unsigned code;
    int letters = 0;
    int digits = 0;

    pos_keymap_reset(&km);

    /* ---- 1. letters, unshifted ---------------------------------------- */

    press_is("A types lower case", C_A, 'a');
    release(C_A);
    press_is("Z types lower case", C_Z, 'z');
    release(C_Z);
    press_is("M types lower case", C_M, 'm');
    release(C_M);
    press_is("Q types lower case", C_Q, 'q');
    release(C_Q);

    /* Every letter on the matrix maps to its own lower-case character. */
    for (code = 1; code <= POS_KEYMAP_MAX_CODE; code++) {
        const char *n = pos_keymap_name((uint8_t)code);

        if (n && n[0] >= 'A' && n[0] <= 'Z' && n[1] == '\0') {
            enum pos_keymap_effect eff;
            pos_key_t got = pos_keymap_event(&km,
                (uint8_t)(code | POS_KEYMAP_EVENT_PRESSED), &eff);

            letters++;
            if (got != (pos_key_t)(n[0] - 'A' + 'a') || eff != POS_KEYMAP_KEY) {
                failed++;
                printf("FAIL letter %s (code %u) gave 0x%02x\n", n, code,
                       (unsigned)got);
            }
            release((uint8_t)code);
        }
    }
    checks++;
    check("the matrix carries 26 letters", letters == 26);

    /* ---- 2. digits ----------------------------------------------------- */

    press_is("1 types '1'", C_1, '1');
    release(C_1);
    press_is("0 types '0'", C_0, '0');
    release(C_0);
    for (code = 1; code <= POS_KEYMAP_MAX_CODE; code++) {
        const char *n = pos_keymap_name((uint8_t)code);

        if (n && n[0] >= '0' && n[0] <= '9' && n[1] == '\0') {
            digits++;
        }
    }
    check("the matrix carries 10 digits", digits == 10);

    /* ---- 3. Shift ------------------------------------------------------ */

    check("Shift press is a modifier, not a key",
          effect_of(C_SHIFT | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_MODIFIER);

    /* This keyboard is legended the BlackBerry way: Shift reaches the symbol
     * printed on the keycap, and capitals come from Caps. Only Z, X, C and V
     * carry no symbol, so only those four give a capital under Shift. The
     * keycaps and matrix codes of A, W, Q, J and Z were read on unit A on
     * 2026-09-12 and the expectations for them are hardware evidence; the
     * rest of the map is still the vendor's, unread. */
    press_is("Shift on a letter that carries a symbol gives the symbol", C_A, '~');
    release(C_A);
    press_is("Shift on a letter with no symbol gives the capital", C_Z, 'Z');
    release(C_Z);
    press_is("as it does for X", 17, 'X');
    release(17);
    press_is("a digit key with Shift gives its symbol", C_1, '!');
    release(C_1);
    check("Shift release is a modifier",
          effect_of(C_SHIFT) == POS_KEYMAP_MODIFIER);
    press_is("and the plain letter returns after Shift", C_A, 'a');
    release(C_A);
    press_is("as does the digit", C_1, '1');
    release(C_1);

    /* ---- 4. Caps lock -------------------------------------------------- */

    press(C_CAPS);
    release(C_CAPS);
    press_is("Caps latches upper case, including on symbol keys", C_A, 'A');
    release(C_A);
    press_is("and holds across keys", C_M, 'M');
    release(C_M);

    /* Caps and Shift together give lower case, as a keyboard does - but
     * only for a letter with no symbol, because a symbol still wins. */
    press(C_SHIFT);
    press_is("Caps plus Shift is lower case", C_Z, 'z');
    release(C_Z);
    press_is("while a symbol key still gives its symbol", C_A, '~');
    release(C_A);
    press_is("and Shift still reaches the digit symbols", C_1, '!');
    release(C_1);
    release(C_SHIFT);

    press(C_CAPS);
    release(C_CAPS);
    press_is("a second Caps press unlatches it", C_A, 'a');
    release(C_A);

    /* ---- 5. punctuation on the keycaps --------------------------------- */

    press(C_SHIFT);
    press_is("Shift+W is an underscore (keycap, unit A)", C_W, '_');
    release(C_W);
    press_is("Shift+Q is an apostrophe (keycap, unit A)", C_Q, '\'');
    release(C_Q);
    press_is("Shift+J is a backtick (keycap, unit A)", C_J, '`');
    release(C_J);
    press_is("Shift+M is a greater-than", C_M, '>');
    release(C_M);
    press_is("Shift+2 is an at sign", 48, '@');
    release(48);
    press_is("Shift+P is a double quote", 42, '"');
    release(42);
    press_is("Shift+K is a slash", 33, '/');
    release(33);
    press_is("Shift+L is a question mark", 32, '?');
    release(32);
    press_is("Shift+B is a full stop", 25, '.');
    release(25);
    press_is("Shift+H is a comma", 35, ',');
    release(35);
    release(C_SHIFT);

    /* ---- 6. space, enter, backspace ------------------------------------ */

    press_is("Space types a space", C_SPACE, ' ');
    release(C_SPACE);
    press_is("the second space bar is the same key", C_SPACE2, ' ');
    release(C_SPACE2);
    press_is("Enter is the logical enter", C_ENTER, LV_KEY_ENTER);
    release(C_ENTER);
    press_is("Del is the logical backspace", C_DEL, LV_KEY_BACKSPACE);
    release(C_DEL);
    press_is("Esc is the logical escape", C_ESC, LV_KEY_ESC);
    release(C_ESC);
    press_is("Tab advances the focus (DS 17.2)", C_TAB, LV_KEY_NEXT);
    release(C_TAB);

    /* Space and Enter must not be affected by Shift. */
    press(C_SHIFT);
    press_is("Shift+Space is still a space", C_SPACE, ' ');
    release(C_SPACE);
    press_is("Shift+Enter is still enter", C_ENTER, LV_KEY_ENTER);
    release(C_ENTER);
    release(C_SHIFT);

    /* ---- 7. arrows ------------------------------------------------------ */

    press_is("Up", C_UP, LV_KEY_UP);
    release(C_UP);
    press_is("Down", C_DOWN, LV_KEY_DOWN);
    release(C_DOWN);
    press_is("Left", C_LEFT, LV_KEY_LEFT);
    release(C_LEFT);
    press_is("Right", C_RIGHT, LV_KEY_RIGHT);
    release(C_RIGHT);

    /* ---- 8. Ctrl, Alt, Fn are tracked but produce nothing yet ---------- */

    check("Ctrl is a modifier",
          effect_of(C_CTRL | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_MODIFIER);
    check("Ctrl is recognised as one", pos_keymap_is_modifier(C_CTRL));
    press_is("a letter with Ctrl held is still the letter", C_A, 'a');
    release(C_A);
    release(C_CTRL);

    check("Alt is a modifier",
          effect_of(C_ALT | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_MODIFIER);
    release(C_ALT);
    check("Fn is a modifier",
          effect_of(C_FN | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_MODIFIER);
    release(C_FN);
    check("the right-hand Fn is the same modifier", pos_keymap_is_modifier(3));

    /* ---- 9. releases deliver nothing ----------------------------------- */

    press(C_A);
    check("a release of an ordinary key delivers no key",
          pos_keymap_event(&km, C_A, NULL) == 0);
    check("nor does a release of Enter",
          (press(C_ENTER), pos_keymap_event(&km, C_ENTER, NULL)) == 0);

    /* One press is one key: feeding the same press twice yields it twice,
     * and the layer never invents an extra event of its own. */
    {
        int n = 0;

        pos_keymap_reset(&km);
        if (pos_keymap_event(&km, C_A | POS_KEYMAP_EVENT_PRESSED, NULL)) n++;
        if (pos_keymap_event(&km, C_A, NULL)) n++;
        if (pos_keymap_event(&km, C_A | POS_KEYMAP_EVENT_PRESSED, NULL)) n++;
        if (pos_keymap_event(&km, C_A, NULL)) n++;
        check("two presses give two keys and two releases give none", n == 2);
    }

    /* ---- 10. codes the matrix cannot produce are ignored safely -------- */

    pos_keymap_reset(&km);
    check("code 0 is ignored", pos_keymap_event(&km, 0x00, NULL) == 0);
    check("code 0 pressed is ignored",
          pos_keymap_event(&km, POS_KEYMAP_EVENT_PRESSED, NULL) == 0);
    check("a gap in the matrix is ignored (code 4)",
          pos_keymap_event(&km, 4 | POS_KEYMAP_EVENT_PRESSED, NULL) == 0);
    check("another gap (code 30)",
          pos_keymap_event(&km, 30 | POS_KEYMAP_EVENT_PRESSED, NULL) == 0);
    check("a code past the matrix is ignored (69)",
          pos_keymap_event(&km, 69 | POS_KEYMAP_EVENT_PRESSED, NULL) == 0);
    check("0x7F is ignored", pos_keymap_event(&km, 0x7F, NULL) == 0);
    check("0xFF is ignored", pos_keymap_event(&km, 0xFF, NULL) == 0);
    check("a NULL state is refused", pos_keymap_event(NULL, 0x80 | C_A, NULL) == 0);
    check("no unknown code changed the modifiers",
          km.shift == 0 && km.ctrl == 0 && km.alt == 0 && km.caps == 0);

    /* ---- 11. the function row is reserved, not guessed ----------------- */

    check("F1 is reserved rather than mapped",
          effect_of(50 | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_RESERVED);
    check("F11 too", effect_of(61 | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_RESERVED);
    check("so is the LILYGO key",
          effect_of(8 | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_RESERVED);
    check("so is the mic key",
          effect_of(11 | POS_KEYMAP_EVENT_PRESSED) == POS_KEYMAP_RESERVED);
    check("and none of them produced a key",
          pos_keymap_event(&km, 50 | POS_KEYMAP_EVENT_PRESSED, NULL) == 0);

    /* ---- 12. the vocabulary is DS 17.4's, not a private one ------------ */

    check("printable keys are code points below 0x80 here",
          (press(C_A), pos_keymap_event(&km, C_A | POS_KEYMAP_EVENT_PRESSED, NULL)) < 0x80);
    release(C_A);
    check("control keys are LV_KEY_* constants",
          LV_KEY_ENTER < 0x80 && LV_KEY_BACKSPACE < 0x80);
    /* Nothing this keyboard produces collides with a printable character:
     * every LV_KEY_* used here is a C0 control code. */
    check("enter is not a letter", LV_KEY_ENTER != 'a' && LV_KEY_ENTER != 'A');
    check("backspace is not a letter",
          LV_KEY_BACKSPACE != 'h' && LV_KEY_BACKSPACE != 'H');

    /* ---- 13. reset clears everything ----------------------------------- */

    press(C_SHIFT);
    press(C_CTRL);
    press(C_CAPS);
    release(C_CAPS);
    check("modifiers are set before the reset", km.shift && km.ctrl && km.caps);
    pos_keymap_reset(&km);
    check("reset clears the modifiers",
          !km.shift && !km.ctrl && !km.alt && !km.fn && !km.caps);
    press_is("and typing is back to lower case", C_A, 'a');

    /* ---- 14. names, for diagnostics ------------------------------------ */

    check("a name is returned for a real key",
          pos_keymap_name(C_A) && strcmp(pos_keymap_name(C_A), "A") == 0);
    check("and NULL for a gap", pos_keymap_name(4) == NULL);
    check("and NULL past the matrix", pos_keymap_name(200) == NULL);
    check("and NULL for zero", pos_keymap_name(0) == NULL);

    printf("pos_keymap_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
