/*
 * Terminal: what each key sends to the program (apps/terminal/term_keys.c).
 *
 * The bytes are the contract with the program on the PTY - the kernel's line
 * discipline, the shell's line editor, less and vi - so each is checked
 * exactly: Enter is CR, Backspace the tty's erase character DEL, Ctrl+C the
 * interrupt character, the cursor keys in both modes, Alt as an ESC prefix.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "term_keys.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void expect(const char *what, enum term_key_kind kind, uint32_t cp, unsigned mods, bool app,
                   const char *want, size_t want_len)
{
    struct term_key k = { kind, cp, mods };
    uint8_t out[TERM_KEY_MAX];
    size_t n = term_key_encode(&k, app, out);

    checks++;
    if (n != want_len || memcmp(out, want, n) != 0) {
        size_t i;

        failed++;
        printf("FAIL %s: got", what);
        for (i = 0; i < n; i++) {
            printf(" %02x", out[i]);
        }
        printf(", want");
        for (i = 0; i < want_len; i++) {
            printf(" %02x", (uint8_t)want[i]);
        }
        printf("\n");
    }
}

#define E(what, kind, cp, mods, app, want) expect(what, kind, cp, mods, app, want, sizeof(want) - 1)

int main(void)
{
    E("a letter is itself", TERM_KEY_CHAR, 'a', 0, false, "a");
    E("a shifted symbol is itself", TERM_KEY_CHAR, '|', TERM_MOD_SHIFT, false, "|");
    E("a character above ASCII is UTF-8", TERM_KEY_CHAR, 0xE6, 0, false, "\xc3\xa6");
    E("a character above the BMP is UTF-8", TERM_KEY_CHAR, 0x1F600, 0, false, "\xf0\x9f\x98\x80");
    E("Enter is CR", TERM_KEY_ENTER, 0, 0, false, "\r");
    E("Backspace is DEL, the tty's erase", TERM_KEY_BACKSPACE, 0, 0, false, "\x7f");
    E("Ctrl+Backspace is BS", TERM_KEY_BACKSPACE, 0, TERM_MOD_CTRL, false, "\x08");
    E("Tab is HT", TERM_KEY_TAB, 0, 0, false, "\t");
    E("Shift+Tab is back-tab", TERM_KEY_TAB, 0, TERM_MOD_SHIFT, false, "\033[Z");
    E("back-tab", TERM_KEY_BACKTAB, 0, 0, false, "\033[Z");
    E("Escape is ESC", TERM_KEY_ESC, 0, 0, false, "\033");
    E("Ctrl+C is ETX, the interrupt", TERM_KEY_CHAR, 'c', TERM_MOD_CTRL, false, "\x03");
    E("Ctrl+Shift+C is the same", TERM_KEY_CHAR, 'C', TERM_MOD_CTRL | TERM_MOD_SHIFT, false, "\x03");
    E("Ctrl+D is EOT, end of file", TERM_KEY_CHAR, 'd', TERM_MOD_CTRL, false, "\x04");
    E("Ctrl+L is FF, clear", TERM_KEY_CHAR, 'l', TERM_MOD_CTRL, false, "\x0c");
    E("Ctrl+Z is SUB, suspend", TERM_KEY_CHAR, 'z', TERM_MOD_CTRL, false, "\x1a");
    E("Ctrl+A is SOH", TERM_KEY_CHAR, 'a', TERM_MOD_CTRL, false, "\x01");
    E("Ctrl+Space is NUL", TERM_KEY_CHAR, ' ', TERM_MOD_CTRL, false, "\0");
    E("Ctrl+[ is ESC", TERM_KEY_CHAR, '[', TERM_MOD_CTRL, false, "\033");
    E("Ctrl+\\ is FS, quit", TERM_KEY_CHAR, '\\', TERM_MOD_CTRL, false, "\x1c");
    E("Ctrl+_ is US", TERM_KEY_CHAR, '_', TERM_MOD_CTRL, false, "\x1f");
    E("Ctrl with a key that has no control code sends the key", TERM_KEY_CHAR, '.', TERM_MOD_CTRL, false, ".");
    E("Alt+x is ESC x", TERM_KEY_CHAR, 'x', TERM_MOD_ALT, false, "\033x");
    E("Ctrl+Alt+c is ESC ETX", TERM_KEY_CHAR, 'c', TERM_MOD_ALT | TERM_MOD_CTRL, false, "\033\x03");
    E("Alt+Enter is ESC CR", TERM_KEY_ENTER, 0, TERM_MOD_ALT, false, "\033\r");
    E("Up", TERM_KEY_UP, 0, 0, false, "\033[A");
    E("Down", TERM_KEY_DOWN, 0, 0, false, "\033[B");
    E("Right", TERM_KEY_RIGHT, 0, 0, false, "\033[C");
    E("Left", TERM_KEY_LEFT, 0, 0, false, "\033[D");
    E("Up in application mode", TERM_KEY_UP, 0, 0, true, "\033OA");
    E("Left in application mode", TERM_KEY_LEFT, 0, 0, true, "\033OD");
    E("Ctrl+Right carries the modifier", TERM_KEY_RIGHT, 0, TERM_MOD_CTRL, false, "\033[1;5C");
    E("Alt+Up carries the modifier", TERM_KEY_UP, 0, TERM_MOD_ALT, true, "\033[1;3A");
    E("Home", TERM_KEY_HOME, 0, 0, false, "\033[H");
    E("End in application mode", TERM_KEY_END, 0, 0, true, "\033OF");
    E("Delete", TERM_KEY_DELETE, 0, 0, false, "\033[3~");
    E("Page Up", TERM_KEY_PAGE_UP, 0, 0, false, "\033[5~");

    /* The function keys (Fn + F1..F11 on the keyboard base), as xterm
     * sends them. */
    E("F1", TERM_KEY_F1, 0, 0, false, "\033OP");
    E("F2", TERM_KEY_F1 + 1, 0, 0, false, "\033OQ");
    E("F3", TERM_KEY_F1 + 2, 0, 0, false, "\033OR");
    E("F4", TERM_KEY_F1 + 3, 0, 0, false, "\033OS");
    E("F4 is the same in application mode", TERM_KEY_F1 + 3, 0, 0, true, "\033OS");
    E("F5", TERM_KEY_F1 + 4, 0, 0, false, "\033[15~");
    E("F6 skips 16", TERM_KEY_F1 + 5, 0, 0, false, "\033[17~");
    E("F7", TERM_KEY_F1 + 6, 0, 0, false, "\033[18~");
    E("F8", TERM_KEY_F1 + 7, 0, 0, false, "\033[19~");
    E("F9", TERM_KEY_F1 + 8, 0, 0, false, "\033[20~");
    E("F10", TERM_KEY_F1 + 9, 0, 0, false, "\033[21~");
    E("F11 skips 22", TERM_KEY_F1 + 10, 0, 0, false, "\033[23~");
    E("F12", TERM_KEY_F12, 0, 0, false, "\033[24~");
    E("Page Down", TERM_KEY_PAGE_DOWN, 0, 0, false, "\033[6~");
    E("no key sends nothing", TERM_KEY_NONE, 0, 0, false, "");
    E("a control character as a character key sends nothing", TERM_KEY_CHAR, 0x03, 0, false, "");
    E("DEL as a character key sends nothing", TERM_KEY_CHAR, 0x7F, 0, false, "");
    E("a surrogate sends nothing", TERM_KEY_CHAR, 0xD800, 0, false, "");
    E("beyond Unicode sends nothing", TERM_KEY_CHAR, 0x110000, 0, false, "");
    checks++;
    if (term_key_encode(NULL, false, (uint8_t[TERM_KEY_MAX]){ 0 }) != 0) {
        failed++;
        printf("FAIL no key at all sends nothing\n");
    }
    printf("term_keys_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
