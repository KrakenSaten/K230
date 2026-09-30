/*
 * Terminal: the screen a program on the PTY draws on.
 *
 * A grid of character cells, a bounded scrollback above it, and the VT
 * parser that turns the program's output bytes into changes of both. Pure C:
 * no LVGL, no I/O, no allocation after term_screen_init(). The app's view
 * reads the cells; the session feeds the bytes and sends the replies a
 * program asked for (the cursor position report) back down the PTY.
 *
 * The subset is what a shell, BusyBox's line editor and the ordinary
 * command-line tools use - docs/apps/TERMINAL.md lists it: printable UTF-8,
 * the C0 controls, cursor movement and positioning, erase in line and in
 * display, insert and delete of characters and lines, a scroll region, tab
 * stops, save and restore of the cursor, SGR (bold, underline, reverse, the
 * 8 + 8 colours; 256-colour and true-colour requests are folded onto the 16),
 * the DEC line-drawing set, the cursor, cursor-key and autowrap modes, and
 * the status and cursor-position reports. Everything else is parsed to its
 * end and ignored: an unknown or malformed sequence never prints its bytes
 * and never leaves the parser inside a sequence, and a string sequence
 * (OSC, DCS, APC, PM, SOS) that never ends is abandoned after
 * TERM_STRING_MAX bytes rather than swallowing the rest of the output.
 *
 * Memory is fixed at init: TERM_MAX_COLS x (TERM_SCROLLBACK + TERM_MAX_ROWS)
 * cells of four bytes, about 700 KB, however much output arrives.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef TERM_SCREEN_H
#define TERM_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The widest and tallest grid kept. A landscape body is 149 columns of the
 * 8 px cell and a portrait one about 60 rows of the 18 px one; a larger
 * body shows this many and leaves the rest blank. */
#define TERM_MAX_COLS 160
#define TERM_MAX_ROWS 100
#define TERM_MIN_COLS 2
#define TERM_MIN_ROWS 2

/* Lines kept above the screen. Old lines are dropped first. */
#define TERM_SCROLLBACK 1000

/* Pending report bytes (the answers to DSR and DA). A program that asks
 * faster than the answers are sent loses the excess, not the parser. */
#define TERM_REPLY_MAX 128

/* A string sequence longer than this is abandoned (see above). */
#define TERM_STRING_MAX 4096

/* A cell's colours. 0-7 the ANSI colours, 8-15 their bright forms. */
#define TERM_COLOR_DEFAULT 16

/* Attribute bits, kept in the fg byte above the colour. */
#define TERM_COLOR_MASK 0x1Fu
#define TERM_ATTR_BOLD 0x20u
#define TERM_ATTR_REVERSE 0x40u
#define TERM_ATTR_UNDERLINE 0x80u

struct term_cell {
    uint16_t ch; /* a BMP code point; ' ' when blank. Above the BMP: U+FFFD */
    uint8_t fg;  /* colour | TERM_ATTR_* */
    uint8_t bg;  /* colour */
};

enum term_parse_state {
    TERM_PS_GROUND = 0,
    TERM_PS_ESC,
    TERM_PS_ESC_INTER,
    TERM_PS_CSI,
    TERM_PS_STRING,    /* OSC, DCS, SOS, PM, APC: skipped to its end */
    TERM_PS_STRING_ESC, /* inside one, just after ESC: '\' ends it */
};

#define TERM_CSI_PARAMS 16

struct term_cursor_save {
    int x, y;
    uint8_t fg, bg;
    uint8_t g0_graphics, g1_graphics, shift_out;
    bool valid;
};

struct term_screen {
    int cols, rows;

    /* The ring: cap lines of TERM_MAX_COLS cells. The last `rows` stored
     * lines are the screen; the ones before them are the scrollback. */
    struct term_cell *cells;
    int cap;
    int first; /* ring index of the oldest stored line */
    int count; /* stored lines, always >= rows */

    int cx, cy;
    bool wrap_pending;
    uint8_t fg, bg; /* the pen */

    int top, bottom; /* the scroll region, inclusive, 0-based */
    bool autowrap;
    bool cursor_visible;
    bool app_cursor_keys; /* DECCKM: arrows send ESC O x */
    bool insert_mode;
    uint8_t g0_graphics, g1_graphics, shift_out;
    bool tabs[TERM_MAX_COLS];
    struct term_cursor_save saved;

    /* the parser */
    enum term_parse_state ps;
    int params[TERM_CSI_PARAMS];
    int nparams;
    bool param_started;
    char priv;  /* '?', '>', '<', '=' or 0 */
    char inter; /* one intermediate byte, or 0 */
    bool bad;   /* this sequence will be ignored at its end */
    size_t seq_len;
    uint32_t utf8_cp;
    int utf8_need;
    uint32_t utf8_min;
    uint32_t last_printed;

    uint8_t reply[TERM_REPLY_MAX];
    size_t reply_len;

    /* For the view. Row damage in screen rows; `all_dirty` for anything
     * that moved every row. `scrolled` counts lines pushed into the
     * scrollback since the view last took it, so a view looking back can
     * stay on the same lines. */
    bool dirty[TERM_MAX_ROWS];
    bool all_dirty;
    unsigned scrolled;

    /* Counters, for tests and diagnostics. */
    unsigned long ignored_sequences;
    unsigned long abandoned_strings;
    unsigned long dropped_replies;
};

/* Allocate the ring and reset. cols and rows are clamped to the limits
 * above. Returns 0, or -1 when the memory could not be had. */
int term_screen_init(struct term_screen *s, int cols, int rows);
void term_screen_free(struct term_screen *s);

/* A hard reset (RIS): blank screen, default modes and pen, the cursor home.
 * The scrollback is kept. */
void term_screen_reset(struct term_screen *s);

/* A new size, clamped. Growing takes lines back from the scrollback;
 * shrinking drops blank lines below the cursor first and then moves the top
 * rows into the scrollback. No reflow. */
void term_screen_resize(struct term_screen *s, int cols, int rows);

/* Program output, any split: a sequence or a UTF-8 character may span any
 * number of calls. */
void term_screen_feed(struct term_screen *s, const uint8_t *data, size_t n);

/* Bytes the program asked to be sent back (reports), moved into out.
 * Returns how many. */
size_t term_screen_take_reply(struct term_screen *s, uint8_t *out, size_t cap);

/* Lines of scrollback above the screen. */
int term_screen_scrollback(const struct term_screen *s);

/* Row `row` (0 = top) of the view `back` lines above the live screen
 * (0 = the live screen). NULL when out of range. TERM_MAX_COLS cells. */
const struct term_cell *term_screen_view_row(const struct term_screen *s, int back, int row);

/* The screen row as UTF-8 text, trailing blanks dropped. For tests and
 * diagnostics. Returns the length written (always NUL-terminated). */
size_t term_screen_row_text(const struct term_screen *s, int back, int row, char *out, size_t cap);

/* Forget damage after the view has drawn it. */
void term_screen_clear_damage(struct term_screen *s);
bool term_screen_damaged(const struct term_screen *s);

#endif
