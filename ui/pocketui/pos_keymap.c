/*
 * Physical keyboard translation. See pos_keymap.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_keymap.h"

/* Matrix codes 1..70. Gaps are positions the keyboard does not populate. */
#define K_SHIFT 7
#define K_ALT 19
#define K_CTRL 23
#define K_FN 9
#define K_FN_R 3
#define K_CAPS 10

static const char *const names[POS_KEYMAP_MAX_CODE + 1] = {
    [1] = "RIGHT",  [2] = "LEFT",   [3] = "FN-R",   [5] = "SPACE",
    [6] = "TAB",    [7] = "SHIFT",  [8] = "LILYGO", [9] = "FN",
    [10] = "CAPS",  [11] = "MIC",   [12] = "DOWN",  [13] = "M",
    [14] = "SPACE", [15] = "V",     [16] = "C",     [17] = "X",
    [18] = "Z",     [19] = "ALT",   [20] = "Q",     [21] = "ENTER",
    [22] = "UP",    [23] = "CTRL",  [24] = "N",     [25] = "B",
    [26] = "F",     [27] = "D",     [28] = "S",     [29] = "A",
    [32] = "L",     [33] = "K",     [34] = "J",     [35] = "H",
    [36] = "G",     [37] = "R",     [38] = "E",     [39] = "W",
    [40] = "ESC",   [41] = "DEL",   [42] = "P",     [43] = "O",
    [44] = "I",     [45] = "U",     [46] = "Y",     [47] = "T",
    [48] = "2",     [49] = "1",     [50] = "F1",    [51] = "0",
    [52] = "9",     [53] = "8",     [54] = "7",     [55] = "6",
    [56] = "5",     [57] = "4",     [58] = "3",     [59] = "F3",
    [60] = "F2",    [61] = "F11",   [62] = "F10",   [63] = "F9",
    [64] = "F8",    [65] = "F7",    [66] = "F6",    [67] = "F5",
    [68] = "F4",
};

/* The symbol printed on the keycap's shifted legend, or 0. Taken from the
 * vendor driver, with two entries corrected against the physical keycaps and
 * the raw matrix codes read on unit A, 2026-09-12: code 39 (W) carries '_'
 * and code 20 (Q) carries '\'', where the vendor's table repeated '~' and
 * '`' and had no key for either of those two symbols. The same reading
 * confirms '~' on A (code 29), '`' on J (code 34), and no symbol at all on Z
 * (code 18). Every other entry below is still the vendor's, and its keycap
 * has not been read. See docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md. */
static uint32_t shifted_symbol(uint8_t code)
{
    switch (code) {
    case 49: return '!';
    case 48: return '@';
    case 58: return '#';
    case 57: return '$';
    case 56: return '%';
    case 55: return '^';
    case 54: return '&';
    case 53: return '*';
    case 52: return '(';
    case 51: return ')';
    case 20: return '\''; /* Q, keycap and code 20 read on unit A */
    case 39: return '_';  /* W, keycap and code 39 read on unit A */
    case 38: return '-';
    case 37: return '+';
    case 47: return '=';
    case 46: return '\\';
    case 45: return '|';
    case 44: return ';';
    case 43: return ':';
    case 42: return '"';
    case 29: return '~'; /* A, confirmed against the keycap on unit A */
    case 28: return '[';
    case 27: return ']';
    case 26: return '{';
    case 36: return '}';
    case 35: return ',';
    case 34: return '`';
    case 33: return '/';
    case 32: return '?';
    case 25: return '.';
    case 24: return '<';
    case 13: return '>';
    default: return 0;
    }
}

static uint32_t digit_for(uint8_t code)
{
    switch (code) {
    case 49: return '1';
    case 48: return '2';
    case 58: return '3';
    case 57: return '4';
    case 56: return '5';
    case 55: return '6';
    case 54: return '7';
    case 53: return '8';
    case 52: return '9';
    case 51: return '0';
    default: return 0;
    }
}

const char *pos_keymap_name(uint8_t code)
{
    if (code == 0 || code > POS_KEYMAP_MAX_CODE) {
        return NULL;
    }
    return names[code];
}

bool pos_keymap_is_modifier(uint8_t code)
{
    return code == K_SHIFT || code == K_ALT || code == K_CTRL ||
           code == K_FN || code == K_FN_R;
}

void pos_keymap_reset(struct pos_keymap *k)
{
    if (k) {
        k->shift = 0;
        k->ctrl = 0;
        k->alt = 0;
        k->fn = 0;
        k->caps = 0;
    }
}

/* The function row, Fn, the LILYGO key and the mic are real keys with no
 * settled meaning in PocketOS. They are reported as reserved rather than
 * guessed at: the vendor binds them to its own launcher's hotkeys, which is
 * not a contract PocketOS has. */
static bool is_reserved(uint8_t code)
{
    switch (code) {
    case 8:  /* LILYGO */
    case 11: /* MIC */
    case 50: case 59: case 60: case 61: case 62: case 63:
    case 64: case 65: case 66: case 67: case 68: /* F1..F11 */
        return true;
    default:
        return false;
    }
}

pos_key_t pos_keymap_event(struct pos_keymap *k, uint8_t event,
                           enum pos_keymap_effect *effect)
{
    uint8_t code = (uint8_t)(event & POS_KEYMAP_EVENT_CODE);
    bool pressed = (event & POS_KEYMAP_EVENT_PRESSED) != 0;
    uint32_t sym;
    bool upper;

    if (effect) {
        *effect = POS_KEYMAP_NONE;
    }
    if (!k || code == 0 || code > POS_KEYMAP_MAX_CODE || !names[code]) {
        return 0; /* an unused position or a code the matrix cannot produce */
    }

    /* Modifiers first: they are the only keys a release still matters for. */
    switch (code) {
    case K_SHIFT: k->shift = pressed; goto modifier;
    case K_CTRL:  k->ctrl = pressed;  goto modifier;
    case K_ALT:   k->alt = pressed;   goto modifier;
    case K_FN:
    case K_FN_R:  k->fn = pressed;    goto modifier;
    case K_CAPS:
        if (pressed) {
            k->caps = !k->caps;
        }
        goto modifier;
    default:
        break;
    }

    if (!pressed) {
        return 0; /* a release of an ordinary key delivers nothing */
    }
    if (is_reserved(code)) {
        if (effect) {
            *effect = POS_KEYMAP_RESERVED;
        }
        return 0;
    }

    /* Navigation and editing, in the logical vocabulary of DS §17.4. */
    switch (code) {
    case 1:  sym = LV_KEY_RIGHT; goto key;
    case 2:  sym = LV_KEY_LEFT; goto key;
    case 12: sym = LV_KEY_DOWN; goto key;
    case 22: sym = LV_KEY_UP; goto key;
    case 21: sym = LV_KEY_ENTER; goto key;
    case 40: sym = LV_KEY_ESC; goto key;
    case 41: sym = LV_KEY_BACKSPACE; goto key;
    case 6:  sym = LV_KEY_NEXT; goto key; /* TAB advances the focus (§17.2) */
    case 5:
    case 14: sym = ' '; goto key;         /* two space bars, one meaning */
    default: break;
    }

    /* Shifted punctuation takes precedence over the unshifted legend. */
    if (k->shift) {
        sym = shifted_symbol(code);
        if (sym) {
            goto key;
        }
    }

    sym = digit_for(code);
    if (sym) {
        goto key;
    }

    /* Letters. Caps and Shift each invert the case, so both together give
     * lower case, which is what a keyboard does. */
    if (names[code][0] >= 'A' && names[code][0] <= 'Z' && names[code][1] == '\0') {
        upper = (k->caps != 0) != (k->shift != 0);
        sym = (uint32_t)(upper ? names[code][0]
                               : (names[code][0] - 'A' + 'a'));
        goto key;
    }

    return 0;

modifier:
    if (effect) {
        *effect = POS_KEYMAP_MODIFIER;
    }
    return 0;

key:
    if (effect) {
        *effect = POS_KEYMAP_KEY;
    }
    return (pos_key_t)sym;
}
