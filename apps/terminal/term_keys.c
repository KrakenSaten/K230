/*
 * Terminal: key encoding. See term_keys.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "term_keys.h"

#include <string.h>

static size_t utf8(uint32_t c, uint8_t *out)
{
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (uint8_t)(0xC0 | (c >> 6));
        out[1] = (uint8_t)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c >= 0xD800 && c <= 0xDFFF) {
        return 0;
    }
    if (c < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (c >> 12));
        out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (uint8_t)(0xF0 | (c >> 18));
        out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (c & 0x3F));
        return 4;
    }
    return 0;
}

/* Ctrl with a character: the control code a VT100 sends, or -1 when the
 * combination has none and the character goes as it is. */
static int control_code(uint32_t c)
{
    if (c >= 'a' && c <= 'z') {
        return (int)(c - 'a' + 1);
    }
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 1);
    }
    switch (c) {
    case ' ': case '@': case '2': return 0x00;
    case '[': case '3': return 0x1B;
    case '\\': case '4': return 0x1C;
    case ']': case '5': return 0x1D;
    case '^': case '6': return 0x1E;
    case '_': case '-': case '7': return 0x1F;
    case '?': case '8': return 0x7F;
    default: return -1;
    }
}

/* A cursor or editing key: ESC [ <final> or, with modifiers, the xterm form
 * ESC [ 1 ; <1 + mods> <final>. */
static size_t cursor(char final, bool app, unsigned mods, uint8_t *out)
{
    unsigned m = (mods & TERM_MOD_SHIFT ? 1u : 0u) + (mods & TERM_MOD_ALT ? 2u : 0u) +
                 (mods & TERM_MOD_CTRL ? 4u : 0u);

    out[0] = 0x1B;
    if (m) {
        out[1] = '[';
        out[2] = '1';
        out[3] = ';';
        out[4] = (uint8_t)('1' + m);
        out[5] = (uint8_t)final;
        return 6;
    }
    out[1] = app ? 'O' : '[';
    out[2] = (uint8_t)final;
    return 3;
}

static size_t tilde(char code, uint8_t *out)
{
    out[0] = 0x1B;
    out[1] = '[';
    out[2] = (uint8_t)code;
    out[3] = '~';
    return 4;
}

size_t term_key_encode(const struct term_key *k, bool app, uint8_t out[TERM_KEY_MAX])
{
    size_t n = 0;
    int cc;

    if (!k) {
        return 0;
    }
    /* Alt is an ESC in front of what the key would send, for the keys that
     * are one character; the cursor keys carry it in their parameter. */
    if ((k->mods & TERM_MOD_ALT) &&
        (k->kind == TERM_KEY_CHAR || k->kind == TERM_KEY_ENTER || k->kind == TERM_KEY_BACKSPACE ||
         k->kind == TERM_KEY_TAB || k->kind == TERM_KEY_ESC)) {
        out[n++] = 0x1B;
    }
    switch (k->kind) {
    case TERM_KEY_CHAR:
        if (k->mods & TERM_MOD_CTRL) {
            cc = control_code(k->cp);
            if (cc >= 0) {
                out[n++] = (uint8_t)cc;
                return n;
            }
        }
        if (k->cp < 0x20 || k->cp == 0x7F) {
            return 0; /* a control character is not a character key */
        }
        {
            size_t u = utf8(k->cp, out + n);

            return u ? n + u : 0;
        }
    case TERM_KEY_ENTER:
        out[n++] = '\r';
        return n;
    case TERM_KEY_BACKSPACE:
        out[n++] = (k->mods & TERM_MOD_CTRL) ? 0x08 : 0x7F;
        return n;
    case TERM_KEY_TAB:
        if (k->mods & TERM_MOD_SHIFT) {
            memcpy(out + n, "\033[Z", 3);
            return n + 3;
        }
        out[n++] = '\t';
        return n;
    case TERM_KEY_BACKTAB:
        memcpy(out, "\033[Z", 3);
        return 3;
    case TERM_KEY_ESC:
        out[n++] = 0x1B;
        return n;
    case TERM_KEY_UP: return cursor('A', app, k->mods, out);
    case TERM_KEY_DOWN: return cursor('B', app, k->mods, out);
    case TERM_KEY_RIGHT: return cursor('C', app, k->mods, out);
    case TERM_KEY_LEFT: return cursor('D', app, k->mods, out);
    case TERM_KEY_HOME: return cursor('H', app, k->mods, out);
    case TERM_KEY_END: return cursor('F', app, k->mods, out);
    case TERM_KEY_DELETE: return tilde('3', out);
    case TERM_KEY_PAGE_UP: return tilde('5', out);
    case TERM_KEY_PAGE_DOWN: return tilde('6', out);
    case TERM_KEY_NONE:
    default:
        return 0;
    }
}
