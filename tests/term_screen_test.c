/*
 * Terminal: the screen and its VT parser (apps/terminal/term_screen.c).
 *
 * What a program's output does to the grid, checked cell by cell: text,
 * CR/LF and the pending wrap, cursor movement, erase, insert and delete,
 * SGR, the scroll region, tabs, reports, the line-drawing set, resize, and
 * a bounded scrollback. Then the failure modes: every sequence split at
 * every byte gives the same screen as fed whole; unknown, malformed,
 * over-long and unterminated sequences print nothing of themselves and leave
 * the parser ready for the next text; random bytes never put the cursor off
 * the grid; and no amount of output allocates anything.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "term_screen.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void check_row(struct term_screen *s, int back, int r, const char *want, const char *what)
{
    char got[TERM_MAX_COLS * 3 + 1];

    term_screen_row_text(s, back, r, got, sizeof(got));
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: row %d%s is \"%s\", want \"%s\"\n", what, r, back ? " (scrolled)" : "", got, want);
    }
}

static void feed(struct term_screen *s, const char *text)
{
    term_screen_feed(s, (const uint8_t *)text, strlen(text));
}

static const struct term_cell *cell(struct term_screen *s, int r, int c)
{
    const struct term_cell *line = term_screen_view_row(s, 0, r);

    return line ? &line[c] : NULL;
}

static void fresh(struct term_screen *s, int cols, int rows)
{
    term_screen_free(s);
    if (term_screen_init(s, cols, rows) != 0) {
        printf("FAIL cannot allocate a screen\n");
        exit(2);
    }
}

static bool cursor_at(struct term_screen *s, int r, int c)
{
    return s->cy == r && s->cx == c;
}

/* ---- text ---------------------------------------------------------------- */

static void test_text(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 20, 5);
    feed(&s, "hello\r\nworld");
    check_row(&s, 0, 0, "hello", "text on the first row");
    check_row(&s, 0, 1, "world", "CR LF starts the next row at column 0");
    check("the cursor follows the text", cursor_at(&s, 1, 5));

    feed(&s, "\nX");
    check_row(&s, 0, 2, "     X", "LF alone keeps the column");
    feed(&s, "\rY");
    check_row(&s, 0, 2, "Y    X", "CR alone returns to column 0 and overwrites");
    feed(&s, "\bZ");
    check_row(&s, 0, 2, "Z    X", "BS moves one column left");
    feed(&s, "\r\b\bW");
    check_row(&s, 0, 2, "W    X", "BS stops at column 0");
    feed(&s, "\a\x05\x7f");
    check("BEL, ENQ and DEL draw nothing and do not move", cursor_at(&s, 2, 1));
    term_screen_free(&s);
}

static void test_wrap(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 10, 4);
    feed(&s, "0123456789");
    check("ten characters on a ten-column row leave the cursor on the last column", cursor_at(&s, 0, 9));
    check("with the wrap pending, not yet on the next row", s.wrap_pending);
    feed(&s, "\r\nnext");
    check_row(&s, 0, 1, "next", "CR LF after a full row does not leave an empty row (the VT100 glitch)");

    feed(&s, "\r\nabcdefghijKL");
    check_row(&s, 0, 2, "abcdefghij", "a long line fills its row");
    check_row(&s, 0, 3, "KL", "and continues on the next");

    fresh(&s, 10, 4);
    feed(&s, "\033[?7l0123456789ABC");
    check_row(&s, 0, 0, "012345678C", "with autowrap off the last column is overwritten");
    check("and the cursor stays on the row", cursor_at(&s, 0, 9));
    term_screen_free(&s);
}

/* ---- cursor -------------------------------------------------------------- */

static void test_cursor(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 20, 10);
    feed(&s, "\033[3;5HX");
    check("CUP is 1-based, row then column", cell(&s, 2, 4)->ch == 'X');
    feed(&s, "\033[HA");
    check("CUP with no parameters is home", cell(&s, 0, 0)->ch == 'A');
    feed(&s, "\033[5;5H\033[2AB");
    check("CUU moves up", cell(&s, 2, 4)->ch == 'B');
    feed(&s, "\033[5;5H\033[3BC");
    check("CUD moves down", cell(&s, 7, 4)->ch == 'C');
    feed(&s, "\033[5;5H\033[4CD");
    check("CUF moves right", cell(&s, 4, 8)->ch == 'D');
    feed(&s, "\033[5;9H\033[3DE");
    check("CUB moves left", cell(&s, 4, 5)->ch == 'E');
    feed(&s, "\033[5;5H\033[99A");
    check("CUU stops at the top", cursor_at(&s, 0, 4));
    feed(&s, "\033[99B");
    check("CUD stops at the bottom", cursor_at(&s, 9, 4));
    feed(&s, "\033[99C");
    check("CUF stops at the right edge", cursor_at(&s, 9, 19));
    feed(&s, "\033[99D");
    check("CUB stops at the left edge", cursor_at(&s, 9, 0));
    feed(&s, "\033[99;99H");
    check("CUP clamps into the grid", cursor_at(&s, 9, 19));
    feed(&s, "\033[0;0H");
    check("CUP 0;0 is home", cursor_at(&s, 0, 0));
    feed(&s, "\033[7G");
    check("CHA sets the column", cursor_at(&s, 0, 6));
    feed(&s, "\033[4d");
    check("VPA sets the row", cursor_at(&s, 3, 6));
    feed(&s, "\033[2E");
    check("CNL moves down to column 0", cursor_at(&s, 5, 0));
    feed(&s, "\033[5;5H\0337\033[HQ\0338R");
    check("ESC 7 / ESC 8 save and restore the cursor", cell(&s, 4, 4)->ch == 'R');
    feed(&s, "\033[6;6H\033[s\033[H\033[uS");
    check("CSI s / CSI u save and restore it too", cell(&s, 5, 5)->ch == 'S');
    feed(&s, "\033[?25l");
    check("DECTCEM hides the cursor", !s.cursor_visible);
    feed(&s, "\033[?25h");
    check("and shows it again", s.cursor_visible);
    feed(&s, "\033[?1h");
    check("DECCKM selects application cursor keys", s.app_cursor_keys);
    feed(&s, "\033[?1l");
    check("and normal ones again", !s.app_cursor_keys);
    term_screen_free(&s);
}

/* ---- erase, insert, delete ----------------------------------------------- */

static void fill(struct term_screen *s)
{
    int r;

    feed(s, "\033[H");
    for (r = 0; r < s->rows; r++) {
        char line[64];

        snprintf(line, sizeof(line), "\033[%d;1H%c123456789", r + 1, 'a' + r);
        feed(s, line);
    }
}

static void test_erase(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 10, 5);
    fill(&s);
    feed(&s, "\033[2;5H\033[K");
    check_row(&s, 0, 1, "b123", "EL 0 erases from the cursor to the end");
    feed(&s, "\033[3;5H\033[1K");
    check_row(&s, 0, 2, "     56789", "EL 1 erases from the start through the cursor");
    feed(&s, "\033[4;5H\033[2K");
    check_row(&s, 0, 3, "", "EL 2 erases the whole line");
    check("erasing does not move the cursor", cursor_at(&s, 3, 4));

    fill(&s);
    feed(&s, "\033[3;4H\033[J");
    check_row(&s, 0, 1, "b123456789", "ED 0 keeps what is above");
    check_row(&s, 0, 2, "c12", "ED 0 erases from the cursor");
    check_row(&s, 0, 4, "", "and everything below");

    fill(&s);
    feed(&s, "\033[3;4H\033[1J");
    check_row(&s, 0, 1, "", "ED 1 erases what is above");
    check_row(&s, 0, 2, "    456789", "and the row through the cursor");
    check_row(&s, 0, 3, "d123456789", "and keeps what is below");

    fill(&s);
    feed(&s, "\033[2J");
    check_row(&s, 0, 0, "", "ED 2 erases the screen");
    check_row(&s, 0, 4, "", "all of it");

    fill(&s);
    feed(&s, "\033[1;3H\033[2P");
    check_row(&s, 0, 0, "a1456789", "DCH deletes and pulls the rest left");
    feed(&s, "\033[2;3H\033[2@");
    check_row(&s, 0, 1, "b1  234567", "ICH inserts blanks and pushes the rest right");
    feed(&s, "\033[3;3H\033[3X");
    check_row(&s, 0, 2, "c1   56789", "ECH blanks in place");
    feed(&s, "\033[1;1H\033[4h!\033[4l");
    check_row(&s, 0, 0, "!a1456789", "insert mode pushes the row right");

    fill(&s);
    feed(&s, "\033[2;1H\033[L");
    check_row(&s, 0, 1, "", "IL inserts a blank line at the cursor");
    check_row(&s, 0, 2, "b123456789", "and pushes the rest down");
    check_row(&s, 0, 4, "d123456789", "the bottom line falls off");
    feed(&s, "\033[2;1H\033[2M");
    check_row(&s, 0, 1, "c123456789", "DL deletes lines and pulls the rest up");
    check_row(&s, 0, 4, "", "leaving blanks at the bottom");
    check("DL at the top never feeds the scrollback", term_screen_scrollback(&s) == 0);

    fill(&s);
    feed(&s, "\033[1;1H\033[41m\033[K");
    check("an erase takes the background colour of the pen", cell(&s, 0, 0)->bg == 1);
    term_screen_free(&s);
}

/* ---- SGR ----------------------------------------------------------------- */

static uint8_t color(const struct term_cell *c)
{
    return (uint8_t)(c->fg & TERM_COLOR_MASK);
}

static void test_sgr(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 40, 3);
    feed(&s, "\033[1;31mR\033[0mN");
    check("SGR 1;31: bold red", color(cell(&s, 0, 0)) == 1 && (cell(&s, 0, 0)->fg & TERM_ATTR_BOLD));
    check("SGR 0: default, not bold", color(cell(&s, 0, 1)) == TERM_COLOR_DEFAULT &&
                                            !(cell(&s, 0, 1)->fg & TERM_ATTR_BOLD));
    feed(&s, "\033[32;44mG\033[39mD\033[49mE");
    check("foreground and background together", color(cell(&s, 0, 2)) == 2 && cell(&s, 0, 2)->bg == 4);
    check("SGR 39 resets the foreground only", color(cell(&s, 0, 3)) == TERM_COLOR_DEFAULT &&
                                                     cell(&s, 0, 3)->bg == 4);
    check("SGR 49 resets the background", cell(&s, 0, 4)->bg == TERM_COLOR_DEFAULT);
    feed(&s, "\033[93;101mB\033[m");
    check("bright colours 90-97 and 100-107", color(cell(&s, 0, 5)) == 11 && cell(&s, 0, 5)->bg == 9);
    feed(&s, "\033[7mV\033[27mv\033[4mU\033[24mu\033[1mb\033[22mn");
    check("SGR 7 reverse", cell(&s, 0, 6)->fg & TERM_ATTR_REVERSE);
    check("SGR 27 ends it", !(cell(&s, 0, 7)->fg & TERM_ATTR_REVERSE));
    check("SGR 4 underline", cell(&s, 0, 8)->fg & TERM_ATTR_UNDERLINE);
    check("SGR 24 ends it", !(cell(&s, 0, 9)->fg & TERM_ATTR_UNDERLINE));
    check("SGR 22 ends bold", (cell(&s, 0, 10)->fg & TERM_ATTR_BOLD) && !(cell(&s, 0, 11)->fg & TERM_ATTR_BOLD));
    feed(&s, "\033[1;34mx\033[31my\033[m");
    check("a colour change keeps bold", color(cell(&s, 0, 13)) == 1 && (cell(&s, 0, 13)->fg & TERM_ATTR_BOLD));
    feed(&s, "\033[38;5;196ma\033[38;5;2mb\033[48;5;21mc\033[38;5;244md\033[m");
    check("256 colours: 196 is a bright red", color(cell(&s, 0, 14)) == 9);
    check("256 colours below 16 are the 16", color(cell(&s, 0, 15)) == 2);
    check("256 colours as background: 21 is a blue", (cell(&s, 0, 16)->bg & 7) == 4);
    check("256 colours: a grey is a grey", color(cell(&s, 0, 17)) == 8 || color(cell(&s, 0, 17)) == 7);
    feed(&s, "\033[38;2;0;255;0mt\033[48;2;255;0;0mu\033[m");
    check("true colour folds onto the 16 (green)", color(cell(&s, 0, 18)) == 10);
    check("true colour background (red)", cell(&s, 0, 19)->bg == 9);
    feed(&s, "\033[38;5mk\033[m");
    check("a cut-short 256-colour SGR changes nothing", color(cell(&s, 0, 20)) == TERM_COLOR_DEFAULT);
    feed(&s, "\033[31;38;2;1mq\033[m");
    check("a cut-short true colour keeps what came before it", color(cell(&s, 0, 21)) == 1);
    feed(&s, "\033[5;8;9;53mw\033[m");
    check("attributes the screen cannot show are ignored", cell(&s, 0, 22)->ch == 'w' &&
                                                           color(cell(&s, 0, 22)) == TERM_COLOR_DEFAULT);
    term_screen_free(&s);
}

/* ---- regions, tabs, graphics -------------------------------------------- */

static void test_region(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 10, 6);
    fill(&s);
    feed(&s, "\033[2;4r");
    check("DECSTBM homes the cursor", cursor_at(&s, 0, 0));
    feed(&s, "\033[4;1H\n");
    check_row(&s, 0, 0, "a123456789", "a line feed at the region's foot leaves rows above it");
    check_row(&s, 0, 1, "c123456789", "scrolls the region up");
    check_row(&s, 0, 3, "", "and blanks its last row");
    check_row(&s, 0, 4, "e123456789", "leaves rows below it");
    check("a region scroll never feeds the scrollback", term_screen_scrollback(&s) == 0);
    feed(&s, "\033[2;1H\033M");
    check_row(&s, 0, 1, "", "RI at the region's top scrolls it down");
    check_row(&s, 0, 2, "c123456789", "pushing the rest down");
    feed(&s, "\033[r");
    check("CSI r resets the region to the screen", s.top == 0 && s.bottom == 5);
    feed(&s, "\033[5;2r");
    check("an inverted region is refused", s.top == 0 && s.bottom == 5);
    term_screen_free(&s);
}

static void test_tabs(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 30, 3);
    feed(&s, "a\tb\tc");
    check("tab stops every eight columns", cell(&s, 0, 8)->ch == 'b' && cell(&s, 0, 16)->ch == 'c');
    feed(&s, "\r\n\033[3g\033[5G\033H\ra\tb");
    check("TBC clears them and HTS sets one", cell(&s, 1, 4)->ch == 'b');
    feed(&s, "\r\n\t\t\t\t\t\tz");
    check("a tab never passes the last column", cell(&s, 2, 29)->ch == 'z');
    term_screen_free(&s);
}

static void test_graphics(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 20, 3);
    feed(&s, "\033(0lqqk\033(Bq");
    check_row(&s, 0, 0, "+--+q", "the DEC line-drawing set, then ASCII again");
    feed(&s, "\r\n\033)0x\016x\017x");
    check_row(&s, 0, 1, "x|x", "G1 through SO and SI");
    term_screen_free(&s);
}

/* ---- reports ------------------------------------------------------------- */

static void test_reports(void)
{
    struct term_screen s = { 0 };
    uint8_t out[TERM_REPLY_MAX + 1];
    size_t n;
    int i;

    fresh(&s, 20, 10);
    feed(&s, "\033[4;7H\033[6n");
    n = term_screen_take_reply(&s, out, sizeof(out) - 1);
    out[n] = 0;
    check("DSR 6 reports the cursor, 1-based", strcmp((char *)out, "\033[4;7R") == 0);
    feed(&s, "\033[5n\033[c");
    n = term_screen_take_reply(&s, out, sizeof(out) - 1);
    out[n] = 0;
    check("DSR 5 and DA answer", strcmp((char *)out, "\033[0n\033[?6c") == 0);
    n = term_screen_take_reply(&s, out, sizeof(out));
    check("a reply is taken once", n == 0);
    for (i = 0; i < 100; i++) {
        feed(&s, "\033[6n");
    }
    check("a flood of requests keeps the replies bounded", s.reply_len <= TERM_REPLY_MAX);
    check("and counts what it dropped", s.dropped_replies > 0);
    term_screen_free(&s);
}

/* ---- UTF-8 ----------------------------------------------------------------- */

static void test_utf8(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 20, 3);
    feed(&s, "\xc3");
    feed(&s, "\xa6");
    feed(&s, "\xe2\x82");
    feed(&s, "\xac");
    check("a character split across feeds arrives whole", cell(&s, 0, 0)->ch == 0xE6 && cell(&s, 0, 1)->ch == 0x20AC);
    feed(&s, "\xc3" "A");
    check("a sequence cut short is one replacement, then the byte", cell(&s, 0, 2)->ch == 0xFFFD &&
                                                                      cell(&s, 0, 3)->ch == 'A');
    feed(&s, "\xc0\xaf\xed\xa0\x80\xff");
    check("overlong, surrogate and impossible bytes are replacements",
          cell(&s, 0, 4)->ch == 0xFFFD && cell(&s, 0, 5)->ch == 0xFFFD && cell(&s, 0, 6)->ch == 0xFFFD);
    feed(&s, "\xf0\x9f\x98\x80");
    check("above the BMP is stored as a replacement, one cell", cell(&s, 0, 7)->ch == 0xFFFD &&
                                                                   s.cx == 8);
    feed(&s, "e\xcc\x81");
    check("a combining mark takes no cell", s.cx == 9);
    term_screen_free(&s);
}

/* ---- malformed and unknown ------------------------------------------------ */

static void test_malformed(void)
{
    struct term_screen s = { 0 };
    char big[6000];
    unsigned long before;

    fresh(&s, 40, 5);
    feed(&s, "\033[?999;1zA");
    check_row(&s, 0, 0, "A", "an unknown CSI prints nothing of itself");
    feed(&s, "\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20mB");
    check_row(&s, 0, 0, "AB", "too many parameters: ignored, not half applied");
    check("and the pen is untouched", color(cell(&s, 0, 1)) == TERM_COLOR_DEFAULT);
    feed(&s, "\033[38:5:1mC");
    check("sub-parameters are ignored, not guessed at", color(cell(&s, 0, 2)) == TERM_COLOR_DEFAULT);
    feed(&s, "\033\xc3\xa6");
    check("ESC followed by a UTF-8 character: the character prints", cell(&s, 0, 3)->ch == 0xE6);
    feed(&s, "\033[31\033[32mD");
    check("ESC inside a CSI starts over", color(cell(&s, 0, 4)) == 2);
    feed(&s, "\033[0m\033[31\030E");
    check("CAN abandons a CSI", cell(&s, 0, 5)->ch == 'E' && color(cell(&s, 0, 5)) == TERM_COLOR_DEFAULT);
    feed(&s, "\033]0;a title\007F\033]2;another\033\\G");
    check_row(&s, 0, 0, "ABC\xc3\xa6" "DEFG", "OSC, ended by BEL or by ST, prints nothing");
    feed(&s, "\033P1$r0m\033\\H\033_app\033\\I");
    check("DCS and APC are skipped to their end", cell(&s, 0, 8)->ch == 'H' && cell(&s, 0, 9)->ch == 'I');

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    before = s.abandoned_strings;
    feed(&s, "\r\n\033]0;");
    feed(&s, big);
    feed(&s, "J");
    check("an OSC that never ends is abandoned after its bound", s.abandoned_strings == before + 1);
    check("and the output after it prints again", s.last_printed == 'J' && s.ps == TERM_PS_GROUND);

    fresh(&s, 40, 5);
    feed(&s, "\033[");
    memset(big, '1', 300);
    big[300] = 0;
    feed(&s, big);
    feed(&s, "mK");
    check("an over-long CSI is ignored at its end", cell(&s, 0, 0)->ch == 'K' &&
                                                       color(cell(&s, 0, 0)) == TERM_COLOR_DEFAULT);
    feed(&s, "\033[99999999999999999999;99999999999999999999HL");
    check("huge parameters are clamped, not overflowed", cell(&s, 4, 39)->ch == 'L');
    feed(&s, "\033#8\033%GM");
    check("ESC with intermediates it does not know draws nothing", s.ps == TERM_PS_GROUND);
    feed(&s, "\033[2 qN\033[>4;1mO\033[?1049hP\033[?2004hQ");
    check("cursor shape, key modifiers, the alternate screen and bracketed paste are ignored",
          s.ps == TERM_PS_GROUND && s.fg == TERM_COLOR_DEFAULT);
    term_screen_free(&s);
}

/* Every split of a stream gives the screen the whole stream gives. */
static void test_splits(void)
{
    static const char corpus[] =
        "\033[2J\033[H\033[1;31mred\033[0m plain \033[4munder\033[24m\r\n"
        "\xc3\xa6\xc3\xb8\xc3\xa5 \xe2\x82\xac \033]0;title\007after\r\n"
        "\033[5;10H\033[42mX\033[m\033[3;3H\033[K\033[2;2H\033[3@ins\033[P\r\n"
        "\033(0lqk\033(B\tTab\033[s\033[10;1H\033[u!\033[6n\033[38;5;33mblue\033[m";
    struct term_screen whole = { 0 };
    struct term_screen part = { 0 };
    size_t len = strlen(corpus);
    size_t cut;
    int bad = 0;

    fresh(&whole, 30, 12);
    feed(&whole, corpus);
    for (cut = 1; cut < len; cut++) {
        size_t i;

        fresh(&part, 30, 12);
        term_screen_feed(&part, (const uint8_t *)corpus, cut);
        term_screen_feed(&part, (const uint8_t *)corpus + cut, len - cut);
        for (i = 0; i < 12; i++) {
            if (memcmp(term_screen_view_row(&whole, 0, (int)i), term_screen_view_row(&part, 0, (int)i),
                       sizeof(struct term_cell) * 30) != 0) {
                bad++;
                break;
            }
        }
        if (part.cx != whole.cx || part.cy != whole.cy || part.fg != whole.fg) {
            bad++;
        }
    }
    check("the stream split at every byte gives the same screen", bad == 0);
    fresh(&part, 30, 12);
    for (cut = 0; cut < len; cut++) {
        term_screen_feed(&part, (const uint8_t *)corpus + cut, 1);
    }
    check("and fed one byte at a time", memcmp(term_screen_view_row(&whole, 0, 4), term_screen_view_row(&part, 0, 4),
                                               sizeof(struct term_cell) * 30) == 0 &&
                                            part.cx == whole.cx && part.cy == whole.cy);
    term_screen_free(&whole);
    term_screen_free(&part);
}

/* Random bytes: the grid holds, and a reset brings the screen back. */
static void test_fuzz(void)
{
    struct term_screen s = { 0 };
    uint8_t buf[4096];
    unsigned seed = 12345;
    int round;
    int bad = 0;

    fresh(&s, 37, 11);
    for (round = 0; round < 400; round++) {
        size_t i;

        for (i = 0; i < sizeof(buf); i++) {
            seed = seed * 1103515245u + 12345u;
            /* Weighted towards ESC, [ and digits, where the parser works. */
            switch ((seed >> 16) % 8) {
            case 0: buf[i] = 0x1B; break;
            case 1: buf[i] = '['; break;
            case 2: buf[i] = (uint8_t)('0' + (seed >> 8) % 10); break;
            case 3: buf[i] = ';'; break;
            default: buf[i] = (uint8_t)(seed >> 8); break;
            }
        }
        term_screen_feed(&s, buf, sizeof(buf));
        if (s.cx < 0 || s.cx >= s.cols || s.cy < 0 || s.cy >= s.rows || s.top < 0 || s.bottom >= s.rows ||
            s.top >= s.bottom || term_screen_scrollback(&s) > TERM_SCROLLBACK || s.reply_len > TERM_REPLY_MAX) {
            bad++;
        }
        if (round % 50 == 0) {
            term_screen_resize(&s, 20 + (int)(seed % 60), 5 + (int)(seed % 30));
        }
    }
    check("1.6 MB of random bytes: the cursor, region and bounds hold", bad == 0);
    feed(&s, "\030\033c\033[2Jok");
    check_row(&s, 0, 0, "ok", "and a reset gives a working screen");
    term_screen_free(&s);
}

/* ---- scrollback and resize ------------------------------------------------ */

static void test_scrollback(void)
{
    struct term_screen s = { 0 };
    char line[32];
    int i;
    unsigned scrolled;

    fresh(&s, 20, 10);
    for (i = 0; i < 1500; i++) {
        snprintf(line, sizeof(line), "line %d\r\n", i);
        feed(&s, line);
    }
    check("the scrollback stops at its bound", term_screen_scrollback(&s) == TERM_SCROLLBACK);
    check_row(&s, 0, 8, "line 1499", "the newest line is on the screen");
    check_row(&s, 0, 0, "line 1491", "the screen holds the last rows");
    check_row(&s, 1, 0, "line 1490", "one line back is the line above the screen");
    check_row(&s, TERM_SCROLLBACK, 0, "line 491", "the oldest kept line is the bound's worth back");
    check("no further back than that", term_screen_view_row(&s, TERM_SCROLLBACK + 1, 0) == NULL);
    scrolled = s.scrolled;
    feed(&s, "more\r\n");
    check("a scroll is counted for a view that looks back", s.scrolled == scrolled + 1);
    feed(&s, "\033[3J");
    check("ED 3 clears the scrollback", term_screen_scrollback(&s) == 0);
    check_row(&s, 0, 8, "more", "and leaves the screen");
    term_screen_free(&s);
}

static void test_resize(void)
{
    struct term_screen s = { 0 };
    char line[32];
    int i;

    fresh(&s, 20, 10);
    for (i = 0; i < 30; i++) {
        snprintf(line, sizeof(line), "\r\nl%d", i);
        feed(&s, line);
    }
    check("the cursor is on the last row", s.cy == 9);
    term_screen_resize(&s, 20, 6);
    check("shrinking keeps the cursor on the grid", s.cy == 5 && s.rows == 6);
    check_row(&s, 0, 5, "l29", "and its line with it");
    /* 31 lines were written (the first, blank, and l0..l29); 21 were above
     * a 10-row screen, and shrinking by four pushes four more up. */
    check("the rows above went into the scrollback", term_screen_scrollback(&s) == 21 + 4);
    term_screen_resize(&s, 20, 12);
    check("growing takes lines back from the scrollback", s.rows == 12 && s.cy == 11);
    check_row(&s, 0, 0, "l18", "so the screen fills from above");

    /* Text below the cursor (a full-screen program's status line, or text
     * a program wrote further down and moved back up from): kept on the
     * screen, the room taken from the blank foot and the top instead. */
    fresh(&s, 20, 10);
    feed(&s, "r0\r\nr1\r\nr2\r\nr3\r\nr4\r\nr5\033[8;1Hbelow\033[6;3H");
    check("before: the cursor on row 5, text on row 7, rows 8 and 9 blank", s.cy == 5 && s.cx == 2);
    term_screen_resize(&s, 20, 6);
    check_row(&s, 0, 5, "below", "shrinking keeps the text below the cursor on the screen");
    check_row(&s, 0, 3, "r5", "the cursor's line moves up with it");
    check("and so does the cursor", s.cy == 3 && s.cx == 2);
    check_row(&s, 2, 0, "r0", "the top rows went into the scrollback");
    check("only as many as were needed after the blank foot", term_screen_scrollback(&s) == 2);

    fresh(&s, 20, 10);
    feed(&s, "\033[10;1H\033[44m\033[K\033[m\033[5;1Hcur");
    term_screen_resize(&s, 20, 9);
    check("a row with a coloured background is not blank: it is kept",
          s.rows == 9 && term_screen_view_row(&s, 0, 8)[0].bg == 4 && term_screen_scrollback(&s) == 1);

    /* When the cursor's row and what is below it do not fit, the cursor
     * stays on the grid and keeps its line; the foot goes. */
    fresh(&s, 20, 10);
    feed(&s, "\033[1;1Hcur\033[2;1Hb1\033[9;1Hb8\033[1;4H");
    term_screen_resize(&s, 20, 4);
    check("the cursor stays on the grid", s.cy == 0 && s.rows == 4);
    check_row(&s, 0, 0, "cur", "on its own line");
    check_row(&s, 0, 1, "b1", "with the text right below it");

    fresh(&s, 20, 10);
    feed(&s, "top\r\nsecond");
    term_screen_resize(&s, 20, 4);
    check_row(&s, 0, 0, "top", "shrinking drops blank rows below the cursor first");
    check("and nothing went into the scrollback", term_screen_scrollback(&s) == 0);
    term_screen_resize(&s, 20, 8);
    check("growing with no scrollback adds blank rows below", s.rows == 8 && s.cy == 1);
    check_row(&s, 0, 1, "second", "without moving the text");

    fresh(&s, 20, 4);
    feed(&s, "0123456789ABCDEFGHIJ");
    term_screen_resize(&s, 8, 4);
    check("narrowing clamps the cursor", s.cx == 7);
    term_screen_resize(&s, 30, 4);
    check_row(&s, 0, 0, "01234567", "widening again shows no stale cells");
    term_screen_resize(&s, 1000, 1000);
    check("a size beyond the limits is clamped", s.cols == TERM_MAX_COLS && s.rows == TERM_MAX_ROWS);
    term_screen_resize(&s, 0, -5);
    check("and so is one below them", s.cols == TERM_MIN_COLS && s.rows == TERM_MIN_ROWS);
    term_screen_free(&s);
}

/* No output, however much, allocates. */
static void test_no_allocation(void)
{
    struct term_screen s = { 0 };
    struct mallinfo2 m0;
    struct mallinfo2 m1;
    char chunk[8192];
    int i;

    fresh(&s, 149, 18);
    for (i = 0; i < (int)sizeof(chunk); i++) {
        chunk[i] = (i % 97 == 96) ? '\n' : (char)('!' + i % 90);
    }
    m0 = mallinfo2();
    for (i = 0; i < 1280; i++) { /* 10 MB */
        term_screen_feed(&s, (const uint8_t *)chunk, sizeof(chunk));
        feed(&s, "\033[31m\033[2K\033[10;10H\xc3\xa6\033]0;t\007\033[6n");
        s.reply_len = 0;
    }
    m1 = mallinfo2();
    check("10 MB of output allocates nothing", m1.uordblks == m0.uordblks);
    term_screen_free(&s);
}

static void test_damage(void)
{
    struct term_screen s = { 0 };

    fresh(&s, 20, 5);
    term_screen_clear_damage(&s);
    check("a clean screen reports no damage", !term_screen_damaged(&s));
    feed(&s, "\033[3;1Hx");
    check("text marks its row", s.dirty[2] && !s.dirty[1] && !s.all_dirty);
    term_screen_clear_damage(&s);
    feed(&s, "\033[5;1H\n");
    check("a scroll marks everything", s.all_dirty);
    term_screen_free(&s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_text();
    test_wrap();
    test_cursor();
    test_erase();
    test_sgr();
    test_region();
    test_tabs();
    test_graphics();
    test_reports();
    test_utf8();
    test_malformed();
    test_splits();
    test_fuzz();
    test_scrollback();
    test_resize();
    test_no_allocation();
    test_damage();
    printf("term_screen_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
