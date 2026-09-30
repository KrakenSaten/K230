/*
 * Terminal: the screen and its VT parser. See term_screen.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "term_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEN_DEFAULT TERM_COLOR_DEFAULT
#define CSI_PARAM_MAX 65535
/* A CSI longer than this is ignored at its end: no real one is. */
#define CSI_LEN_MAX 256

/* ---- the ring ----------------------------------------------------------- */

static struct term_cell *ring_line(const struct term_screen *s, int idx)
{
    return s->cells + (size_t)idx * TERM_MAX_COLS;
}

/* Ring index of screen row r. */
static int row_index(const struct term_screen *s, int r)
{
    return (s->first + s->count - s->rows + r) % s->cap;
}

static struct term_cell *row(const struct term_screen *s, int r)
{
    return ring_line(s, row_index(s, r));
}

static struct term_cell blank(const struct term_screen *s)
{
    struct term_cell c = { ' ', PEN_DEFAULT, s->bg };

    return c;
}

static void clear_cells(struct term_cell *line, int from, int to, struct term_cell b)
{
    int i;

    for (i = from; i < to; i++) {
        line[i] = b;
    }
}

static void mark_row(struct term_screen *s, int r)
{
    if (r >= 0 && r < s->rows) {
        s->dirty[r] = true;
    }
}

static void mark_all(struct term_screen *s)
{
    s->all_dirty = true;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- scrolling ---------------------------------------------------------- */

static bool full_region(const struct term_screen *s)
{
    return s->top == 0 && s->bottom == s->rows - 1;
}

/* Region up by n. With the whole screen as the region, the top line goes
 * into the scrollback, which is what makes a scrollback at all. */
static void scroll_up(struct term_screen *s, int n)
{
    int height = s->bottom - s->top + 1;
    int r;

    n = clamp(n, 0, height);
    if (n == 0) {
        return;
    }
    if (full_region(s)) {
        for (r = 0; r < n; r++) {
            if (s->count < s->rows + TERM_SCROLLBACK) {
                s->count++;
            } else {
                s->first = (s->first + 1) % s->cap;
            }
            clear_cells(row(s, s->rows - 1), 0, TERM_MAX_COLS, blank(s));
            s->scrolled++;
        }
    } else {
        for (r = s->top; r + n <= s->bottom; r++) {
            memcpy(row(s, r), row(s, r + n), sizeof(struct term_cell) * TERM_MAX_COLS);
        }
        for (r = s->bottom - n + 1; r <= s->bottom; r++) {
            clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
        }
    }
    mark_all(s);
}

static void scroll_down(struct term_screen *s, int n)
{
    int height = s->bottom - s->top + 1;
    int r;

    n = clamp(n, 0, height);
    if (n == 0) {
        return;
    }
    for (r = s->bottom; r - n >= s->top; r--) {
        memcpy(row(s, r), row(s, r - n), sizeof(struct term_cell) * TERM_MAX_COLS);
    }
    for (r = s->top; r < s->top + n; r++) {
        clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
    }
    mark_all(s);
}

static void linefeed(struct term_screen *s)
{
    s->wrap_pending = false;
    if (s->cy == s->bottom) {
        scroll_up(s, 1);
    } else if (s->cy < s->rows - 1) {
        s->cy++;
    }
}

static void reverse_index(struct term_screen *s)
{
    s->wrap_pending = false;
    if (s->cy == s->top) {
        scroll_down(s, 1);
    } else if (s->cy > 0) {
        s->cy--;
    }
}

/* ---- state -------------------------------------------------------------- */

static void default_tabs(struct term_screen *s)
{
    int i;

    for (i = 0; i < TERM_MAX_COLS; i++) {
        s->tabs[i] = i > 0 && i % 8 == 0;
    }
}

void term_screen_reset(struct term_screen *s)
{
    int r;

    s->fg = PEN_DEFAULT;
    s->bg = PEN_DEFAULT;
    s->cx = 0;
    s->cy = 0;
    s->wrap_pending = false;
    s->top = 0;
    s->bottom = s->rows - 1;
    s->autowrap = true;
    s->cursor_visible = true;
    s->app_cursor_keys = false;
    s->insert_mode = false;
    s->g0_graphics = 0;
    s->g1_graphics = 0;
    s->shift_out = 0;
    s->saved.valid = false;
    s->ps = TERM_PS_GROUND;
    s->utf8_need = 0;
    s->last_printed = 0;
    default_tabs(s);
    for (r = 0; r < s->rows; r++) {
        clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
    }
    mark_all(s);
}

int term_screen_init(struct term_screen *s, int cols, int rows)
{
    memset(s, 0, sizeof(*s));
    s->cap = TERM_SCROLLBACK + TERM_MAX_ROWS;
    s->cells = malloc(sizeof(struct term_cell) * TERM_MAX_COLS * (size_t)s->cap);
    if (!s->cells) {
        return -1;
    }
    s->cols = clamp(cols, TERM_MIN_COLS, TERM_MAX_COLS);
    s->rows = clamp(rows, TERM_MIN_ROWS, TERM_MAX_ROWS);
    s->first = 0;
    s->count = s->rows;
    term_screen_reset(s);
    return 0;
}

void term_screen_free(struct term_screen *s)
{
    free(s->cells);
    s->cells = NULL;
}

/* A row with nothing on it: spaces on the default background. */
static bool row_blank(const struct term_screen *s, int r)
{
    const struct term_cell *line = row(s, r);
    int i;

    for (i = 0; i < s->cols; i++) {
        if (line[i].ch != ' ' || line[i].bg != PEN_DEFAULT || (line[i].fg & TERM_ATTR_REVERSE)) {
            return false;
        }
    }
    return true;
}

void term_screen_resize(struct term_screen *s, int cols, int rows)
{
    int i;

    cols = clamp(cols, TERM_MIN_COLS, TERM_MAX_COLS);
    rows = clamp(rows, TERM_MIN_ROWS, TERM_MAX_ROWS);
    if (!s->cells || (cols == s->cols && rows == s->rows)) {
        return;
    }
    if (cols > s->cols) {
        /* Cells past the old width may hold what an earlier, wider screen
         * left there; nothing written since could have reached them. */
        struct term_cell b = { ' ', PEN_DEFAULT, PEN_DEFAULT };

        for (i = 0; i < s->count; i++) {
            clear_cells(ring_line(s, (s->first + i) % s->cap), s->cols, cols, b);
        }
    }
    s->cols = cols;

    if (rows > s->rows) {
        int extra = rows - s->rows;
        int pull = s->count - s->rows < extra ? s->count - s->rows : extra;
        int add = extra - pull;

        s->rows += pull;
        s->cy += pull;
        for (i = 0; i < add; i++) {
            /* count + add never passes cap: cap holds TERM_MAX_ROWS beyond
             * the scrollback, and the scrollback is empty by now. */
            s->count++;
            s->rows++;
            clear_cells(row(s, s->rows - 1), 0, TERM_MAX_COLS, blank(s));
        }
    } else if (rows < s->rows) {
        int shrink = s->rows - rows;
        int up;

        /* First the blank rows at the foot, below the cursor: nothing is
         * lost by dropping them. */
        while (shrink > 0 && s->rows - 1 > s->cy && row_blank(s, s->rows - 1)) {
            s->count--;
            s->rows--;
            shrink--;
        }
        /* Then the top rows move into the scrollback, as many as the cursor
         * allows: text below the cursor stays on the screen. */
        up = shrink < s->cy ? shrink : s->cy;
        s->rows -= up;
        s->cy -= up;
        shrink -= up;
        /* Only when the cursor's row and what is below it do not fit any
         * more does the foot go: the cursor has to stay on the grid. */
        s->count -= shrink;
        s->rows -= shrink;
        /* The scrollback may now be past its bound: the oldest lines go. */
        if (s->count - s->rows > TERM_SCROLLBACK) {
            int excess = s->count - s->rows - TERM_SCROLLBACK;

            s->first = (s->first + excess) % s->cap;
            s->count -= excess;
        }
    }
    s->cx = clamp(s->cx, 0, s->cols - 1);
    s->cy = clamp(s->cy, 0, s->rows - 1);
    s->wrap_pending = false;
    s->top = 0;
    s->bottom = s->rows - 1;
    memset(s->dirty, 0, sizeof(s->dirty));
    mark_all(s);
}

/* ---- replies ------------------------------------------------------------ */

static void reply(struct term_screen *s, const char *text)
{
    size_t n = strlen(text);

    if (s->reply_len + n > TERM_REPLY_MAX) {
        s->dropped_replies++;
        return;
    }
    memcpy(s->reply + s->reply_len, text, n);
    s->reply_len += n;
}

size_t term_screen_take_reply(struct term_screen *s, uint8_t *out, size_t cap)
{
    size_t n = s->reply_len < cap ? s->reply_len : cap;

    memcpy(out, s->reply, n);
    memmove(s->reply, s->reply + n, s->reply_len - n);
    s->reply_len -= n;
    return n;
}

/* ---- printing ----------------------------------------------------------- */

/* The DEC special graphics set, as characters the terminal font has. */
static uint32_t dec_graphics(uint32_t c)
{
    switch (c) {
    case '_': return ' ';
    case '`': return '*';
    case 'a': return ':';
    case 'f': return 0xB0; /* degree */
    case 'g': return 0xB1; /* plus-minus */
    case 'j': case 'k': case 'l': case 'm': case 'n':
    case 't': case 'u': case 'v': case 'w': return '+';
    case 'o': case 'p': case 'q': case 'r': case 's': return '-';
    case 'x': return '|';
    case 'y': return '<';
    case 'z': return '>';
    case '{': return 'p';
    case '|': return '!';
    case '}': return 0xA3; /* pound */
    case '~': return 0xB7; /* middle dot */
    default: return c;
    }
}

static bool zero_width(uint32_t c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF;
}

static void put_char(struct term_screen *s, uint32_t c)
{
    struct term_cell *line;
    uint8_t graphics = s->shift_out ? s->g1_graphics : s->g0_graphics;

    if (c < 0x20 || (c >= 0x7F && c < 0xA0) || zero_width(c)) {
        return;
    }
    if (graphics && c >= 0x5F && c <= 0x7E) {
        c = dec_graphics(c);
    }
    if (s->wrap_pending) {
        s->wrap_pending = false;
        if (s->autowrap) {
            s->cx = 0;
            linefeed(s);
        }
    }
    line = row(s, s->cy);
    if (s->insert_mode && s->cx < s->cols - 1) {
        memmove(&line[s->cx + 1], &line[s->cx], sizeof(*line) * (size_t)(s->cols - 1 - s->cx));
    }
    line[s->cx].ch = (uint16_t)(c > 0xFFFF ? 0xFFFD : c);
    line[s->cx].fg = s->fg;
    line[s->cx].bg = s->bg;
    mark_row(s, s->cy);
    s->last_printed = c;
    if (s->cx >= s->cols - 1) {
        s->cx = s->cols - 1;
        s->wrap_pending = s->autowrap;
    } else {
        s->cx++;
    }
}

/* ---- C0 controls -------------------------------------------------------- */

static void begin_escape(struct term_screen *s)
{
    s->ps = TERM_PS_ESC;
    s->inter = 0;
    s->priv = 0;
    s->bad = false;
    s->seq_len = 0;
}

static void tab_forward(struct term_screen *s, int n)
{
    s->wrap_pending = false;
    while (n-- > 0 && s->cx < s->cols - 1) {
        do {
            s->cx++;
        } while (s->cx < s->cols - 1 && !s->tabs[s->cx]);
    }
}

static void tab_back(struct term_screen *s, int n)
{
    s->wrap_pending = false;
    while (n-- > 0 && s->cx > 0) {
        do {
            s->cx--;
        } while (s->cx > 0 && !s->tabs[s->cx]);
    }
}

static void control(struct term_screen *s, uint8_t b)
{
    switch (b) {
    case 0x08: /* BS */
        s->wrap_pending = false;
        if (s->cx > 0) {
            s->cx--;
        }
        break;
    case 0x09: /* HT */
        tab_forward(s, 1);
        break;
    case 0x0A: /* LF, VT, FF */
    case 0x0B:
    case 0x0C:
        linefeed(s);
        break;
    case 0x0D: /* CR */
        s->wrap_pending = false;
        s->cx = 0;
        break;
    case 0x0E: /* SO */
        s->shift_out = 1;
        break;
    case 0x0F: /* SI */
        s->shift_out = 0;
        break;
    case 0x18: /* CAN, SUB: abandon any sequence */
    case 0x1A:
        if (s->ps != TERM_PS_GROUND) {
            s->ignored_sequences++;
        }
        s->ps = TERM_PS_GROUND;
        break;
    case 0x1B:
        begin_escape(s);
        break;
    default: /* NUL, BEL, ENQ and the rest: nothing to draw */
        break;
    }
}

/* ---- ESC sequences ------------------------------------------------------ */

static void save_cursor(struct term_screen *s)
{
    s->saved.x = s->cx;
    s->saved.y = s->cy;
    s->saved.fg = s->fg;
    s->saved.bg = s->bg;
    s->saved.g0_graphics = s->g0_graphics;
    s->saved.g1_graphics = s->g1_graphics;
    s->saved.shift_out = s->shift_out;
    s->saved.valid = true;
}

static void restore_cursor(struct term_screen *s)
{
    if (!s->saved.valid) {
        s->cx = 0;
        s->cy = 0;
        s->fg = PEN_DEFAULT;
        s->bg = PEN_DEFAULT;
    } else {
        s->cx = clamp(s->saved.x, 0, s->cols - 1);
        s->cy = clamp(s->saved.y, 0, s->rows - 1);
        s->fg = s->saved.fg;
        s->bg = s->saved.bg;
        s->g0_graphics = s->saved.g0_graphics;
        s->g1_graphics = s->saved.g1_graphics;
        s->shift_out = s->saved.shift_out;
    }
    s->wrap_pending = false;
}

static void esc_dispatch(struct term_screen *s, uint8_t b)
{
    switch (b) {
    case '7': save_cursor(s); break;
    case '8': restore_cursor(s); break;
    case 'D': linefeed(s); break;
    case 'E': s->cx = 0; linefeed(s); break;
    case 'M': reverse_index(s); break;
    case 'H': s->tabs[s->cx] = true; break;
    case 'c': term_screen_reset(s); break;
    case '=': case '>': /* keypad modes: the keypad is not a separate thing here */
    case '\\': /* a stray string terminator */
        break;
    default:
        s->ignored_sequences++;
        break;
    }
}

static void esc_inter_dispatch(struct term_screen *s, uint8_t inter, uint8_t b)
{
    switch (inter) {
    case '(':
        s->g0_graphics = b == '0';
        break;
    case ')':
        s->g1_graphics = b == '0';
        break;
    default: /* ESC # 8, ESC % G, ESC SP F and the like */
        s->ignored_sequences++;
        break;
    }
}

/* ---- CSI ---------------------------------------------------------------- */

static int nparams(const struct term_screen *s)
{
    return (s->param_started || s->nparams > 0) ? s->nparams + 1 : 0;
}

/* Parameter i; 0 or missing reads as def (the count and position rule). */
static int param(const struct term_screen *s, int i, int def)
{
    int v;

    if (i >= nparams(s)) {
        return def;
    }
    v = s->params[i];
    return v == 0 ? def : v;
}

/* Parameter i as given, missing as 0 (the selector rule). */
static int param0(const struct term_screen *s, int i)
{
    return i < nparams(s) ? s->params[i] : 0;
}

static void erase_display(struct term_screen *s, int mode)
{
    int r;

    switch (mode) {
    case 0:
        clear_cells(row(s, s->cy), s->cx, TERM_MAX_COLS, blank(s));
        for (r = s->cy + 1; r < s->rows; r++) {
            clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
        }
        break;
    case 1:
        for (r = 0; r < s->cy; r++) {
            clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
        }
        clear_cells(row(s, s->cy), 0, s->cx + 1, blank(s));
        break;
    case 2:
        for (r = 0; r < s->rows; r++) {
            clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
        }
        break;
    case 3:
        /* The scrollback, and only it. */
        s->first = (s->first + s->count - s->rows) % s->cap;
        s->count = s->rows;
        break;
    default:
        s->ignored_sequences++;
        return;
    }
    mark_all(s);
}

static void erase_line(struct term_screen *s, int mode)
{
    struct term_cell *line = row(s, s->cy);

    switch (mode) {
    case 0: clear_cells(line, s->cx, TERM_MAX_COLS, blank(s)); break;
    case 1: clear_cells(line, 0, s->cx + 1, blank(s)); break;
    case 2: clear_cells(line, 0, TERM_MAX_COLS, blank(s)); break;
    default: s->ignored_sequences++; return;
    }
    mark_row(s, s->cy);
}

static void insert_chars(struct term_screen *s, int n)
{
    struct term_cell *line = row(s, s->cy);

    n = clamp(n, 1, s->cols - s->cx);
    memmove(&line[s->cx + n], &line[s->cx], sizeof(*line) * (size_t)(s->cols - s->cx - n));
    clear_cells(line, s->cx, s->cx + n, blank(s));
    s->wrap_pending = false;
    mark_row(s, s->cy);
}

static void delete_chars(struct term_screen *s, int n)
{
    struct term_cell *line = row(s, s->cy);

    n = clamp(n, 1, s->cols - s->cx);
    memmove(&line[s->cx], &line[s->cx + n], sizeof(*line) * (size_t)(s->cols - s->cx - n));
    clear_cells(line, s->cols - n, s->cols, blank(s));
    s->wrap_pending = false;
    mark_row(s, s->cy);
}

static void erase_chars(struct term_screen *s, int n)
{
    n = clamp(n, 1, s->cols - s->cx);
    clear_cells(row(s, s->cy), s->cx, s->cx + n, blank(s));
    s->wrap_pending = false;
    mark_row(s, s->cy);
}

/* IL and DL act only inside the scroll region, as a region of their own
 * that starts at the cursor's line. */
static void insert_lines(struct term_screen *s, int n)
{
    int top = s->top;

    if (s->cy < s->top || s->cy > s->bottom) {
        return;
    }
    s->top = s->cy;
    scroll_down(s, n);
    s->top = top;
    s->cx = 0;
    s->wrap_pending = false;
}

static void delete_lines(struct term_screen *s, int n)
{
    int top = s->top;
    int bottom = s->bottom;

    if (s->cy < s->top || s->cy > s->bottom) {
        return;
    }
    s->top = s->cy;
    /* Never into the scrollback: this is not the whole screen scrolling. */
    if (s->top == 0 && s->bottom == s->rows - 1) {
        int r;

        n = clamp(n, 1, s->rows);
        for (r = 0; r + n < s->rows; r++) {
            memcpy(row(s, r), row(s, r + n), sizeof(struct term_cell) * TERM_MAX_COLS);
        }
        for (r = s->rows - n; r < s->rows; r++) {
            clear_cells(row(s, r), 0, TERM_MAX_COLS, blank(s));
        }
        mark_all(s);
    } else {
        scroll_up(s, n);
    }
    s->top = top;
    s->bottom = bottom;
    s->cx = 0;
    s->wrap_pending = false;
}

/* 256-colour and true-colour requests folded onto the 16 colours. */
static uint8_t from_rgb_levels(int r, int g, int b, int max)
{
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int half = (max + 1) / 2;
    uint8_t c = (uint8_t)((r >= half ? 1 : 0) | (g >= half ? 2 : 0) | (b >= half ? 4 : 0));

    if (c == 0) {
        return hi * 3 >= max ? 8 : 0; /* dark grey or black */
    }
    return hi * 5 >= max * 4 ? (uint8_t)(c + 8) : c;
}

static uint8_t from_256(int n)
{
    if (n < 16) {
        return (uint8_t)n;
    }
    if (n < 232) {
        n -= 16;
        return from_rgb_levels(n / 36, (n / 6) % 6, n % 6, 5);
    }
    n -= 232; /* 24 greys */
    return n < 6 ? 0 : n < 12 ? 8 : n < 18 ? 7 : 15;
}

/* Returns how many further parameters the extended colour used, or -1 when
 * it was cut short (the rest of the SGR is then ignored). */
static int extended_color(const struct term_screen *s, int i, uint8_t *out)
{
    int n = nparams(s);

    if (i + 1 >= n) {
        return -1;
    }
    if (s->params[i + 1] == 5) {
        if (i + 2 >= n) {
            return -1;
        }
        *out = from_256(clamp(s->params[i + 2], 0, 255));
        return 2;
    }
    if (s->params[i + 1] == 2) {
        if (i + 4 >= n) {
            return -1;
        }
        *out = from_rgb_levels(clamp(s->params[i + 2], 0, 255), clamp(s->params[i + 3], 0, 255),
                               clamp(s->params[i + 4], 0, 255), 255);
        return 4;
    }
    return -1;
}

static void sgr(struct term_screen *s)
{
    int n = nparams(s);
    int i;

    if (n == 0) {
        s->fg = PEN_DEFAULT;
        s->bg = PEN_DEFAULT;
        return;
    }
    for (i = 0; i < n; i++) {
        int p = s->params[i];
        uint8_t attrs = (uint8_t)(s->fg & ~TERM_COLOR_MASK);
        uint8_t color;
        int used;

        if (p == 0) {
            s->fg = PEN_DEFAULT;
            s->bg = PEN_DEFAULT;
        } else if (p == 1) {
            s->fg |= TERM_ATTR_BOLD;
        } else if (p == 4) {
            s->fg |= TERM_ATTR_UNDERLINE;
        } else if (p == 7) {
            s->fg |= TERM_ATTR_REVERSE;
        } else if (p == 21 || p == 22) {
            s->fg &= (uint8_t)~TERM_ATTR_BOLD;
        } else if (p == 24) {
            s->fg &= (uint8_t)~TERM_ATTR_UNDERLINE;
        } else if (p == 27) {
            s->fg &= (uint8_t)~TERM_ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            s->fg = (uint8_t)(attrs | (p - 30));
        } else if (p == 39) {
            s->fg = (uint8_t)(attrs | PEN_DEFAULT);
        } else if (p >= 40 && p <= 47) {
            s->bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            s->bg = PEN_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            s->fg = (uint8_t)(attrs | (p - 90 + 8));
        } else if (p >= 100 && p <= 107) {
            s->bg = (uint8_t)(p - 100 + 8);
        } else if (p == 38 || p == 48) {
            used = extended_color(s, i, &color);
            if (used < 0) {
                s->ignored_sequences++;
                return;
            }
            if (p == 38) {
                s->fg = (uint8_t)(attrs | color);
            } else {
                s->bg = color;
            }
            i += used;
        }
        /* 2, 3, 5, 8, 9 and the rest: attributes this screen cannot show */
    }
}

static void set_modes(struct term_screen *s, bool on)
{
    int n = nparams(s);
    int i;

    for (i = 0; i < n; i++) {
        int p = s->params[i];

        if (s->priv == '?') {
            if (p == 1) {
                s->app_cursor_keys = on;
            } else if (p == 7) {
                s->autowrap = on;
                if (!on) {
                    s->wrap_pending = false;
                }
            } else if (p == 25) {
                s->cursor_visible = on;
                mark_row(s, s->cy);
            }
            /* 1049, 47, 1047 (the alternate screen), 2004 (bracketed paste),
             * mouse modes: not supported, and harmless to ignore */
        } else if (s->priv == 0 && p == 4) {
            s->insert_mode = on;
        }
    }
}

static void report(struct term_screen *s)
{
    char buf[32];

    switch (param0(s, 0)) {
    case 5:
        reply(s, "\033[0n");
        break;
    case 6:
        snprintf(buf, sizeof(buf), "\033[%d;%dR", s->cy + 1, s->cx + 1);
        reply(s, buf);
        break;
    default:
        s->ignored_sequences++;
        break;
    }
}

static void csi_dispatch(struct term_screen *s, uint8_t f)
{
    int n;

    if (s->inter) {
        /* CSI SP q (cursor shape), CSI ! p (soft reset) and the like */
        s->ignored_sequences++;
        return;
    }
    if (s->priv && f != 'h' && f != 'l' && f != 'c' && f != 'n') {
        s->ignored_sequences++;
        return;
    }
    switch (f) {
    case '@': insert_chars(s, param(s, 0, 1)); return;
    case 'A': {
        int lim = s->cy >= s->top ? s->top : 0;

        s->cy = clamp(s->cy - param(s, 0, 1), lim, s->rows - 1);
        break;
    }
    case 'B':
    case 'e': {
        int lim = s->cy <= s->bottom ? s->bottom : s->rows - 1;

        s->cy = clamp(s->cy + param(s, 0, 1), 0, lim);
        break;
    }
    case 'C':
    case 'a':
        s->cx = clamp(s->cx + param(s, 0, 1), 0, s->cols - 1);
        break;
    case 'D':
        s->cx = clamp(s->cx - param(s, 0, 1), 0, s->cols - 1);
        break;
    case 'E':
        s->cy = clamp(s->cy + param(s, 0, 1), 0, s->rows - 1);
        s->cx = 0;
        break;
    case 'F':
        s->cy = clamp(s->cy - param(s, 0, 1), 0, s->rows - 1);
        s->cx = 0;
        break;
    case 'G':
    case '`':
        s->cx = clamp(param(s, 0, 1) - 1, 0, s->cols - 1);
        break;
    case 'H':
    case 'f':
        s->cy = clamp(param(s, 0, 1) - 1, 0, s->rows - 1);
        s->cx = clamp(param(s, 1, 1) - 1, 0, s->cols - 1);
        break;
    case 'I': tab_forward(s, param(s, 0, 1)); return;
    case 'Z': tab_back(s, param(s, 0, 1)); return;
    case 'J': erase_display(s, param0(s, 0)); return;
    case 'K': erase_line(s, param0(s, 0)); return;
    case 'L': insert_lines(s, param(s, 0, 1)); return;
    case 'M': delete_lines(s, param(s, 0, 1)); return;
    case 'P': delete_chars(s, param(s, 0, 1)); return;
    case 'S': scroll_up(s, param(s, 0, 1)); return;
    case 'T': scroll_down(s, param(s, 0, 1)); return;
    case 'X': erase_chars(s, param(s, 0, 1)); return;
    case 'b':
        n = clamp(param(s, 0, 1), 1, s->cols * s->rows);
        while (s->last_printed && n-- > 0) {
            put_char(s, s->last_printed);
        }
        return;
    case 'c':
        if (param0(s, 0) != 0) {
            s->ignored_sequences++;
        } else if (s->priv == 0) {
            reply(s, "\033[?6c"); /* a VT102 */
        } else if (s->priv == '>') {
            reply(s, "\033[>0;0;0c");
        } else {
            s->ignored_sequences++;
        }
        return;
    case 'd':
        s->cy = clamp(param(s, 0, 1) - 1, 0, s->rows - 1);
        break;
    case 'g':
        if (param0(s, 0) == 0) {
            s->tabs[s->cx] = false;
        } else if (param0(s, 0) == 3) {
            memset(s->tabs, 0, sizeof(s->tabs));
        }
        return;
    case 'h': set_modes(s, true); return;
    case 'l': set_modes(s, false); return;
    case 'm': sgr(s); return;
    case 'n':
        if (s->priv == 0) {
            report(s);
        } else {
            s->ignored_sequences++;
        }
        return;
    case 'r': {
        int t = param(s, 0, 1);
        int b = param(s, 1, s->rows);

        if (t < b && b <= s->rows) {
            s->top = t - 1;
            s->bottom = b - 1;
            s->cx = 0;
            s->cy = 0;
            s->wrap_pending = false;
        }
        return;
    }
    case 's':
        if (nparams(s) == 0) {
            save_cursor(s);
        }
        return;
    case 'u':
        restore_cursor(s);
        return;
    default:
        s->ignored_sequences++;
        return;
    }
    /* Every cursor movement ends a pending wrap. */
    s->wrap_pending = false;
}

/* ---- the byte loop ------------------------------------------------------ */

static void print_bad_utf8(struct term_screen *s)
{
    s->utf8_need = 0;
    put_char(s, 0xFFFD);
}

static void ground(struct term_screen *s, uint8_t b)
{
    if (s->utf8_need > 0) {
        if ((b & 0xC0) == 0x80) {
            s->utf8_cp = (s->utf8_cp << 6) | (b & 0x3F);
            if (--s->utf8_need == 0) {
                uint32_t c = s->utf8_cp;

                if (c < s->utf8_min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
                    c = 0xFFFD;
                }
                put_char(s, c);
            }
            return;
        }
        print_bad_utf8(s); /* cut short: then the byte on its own */
    }
    if (b < 0x20) {
        control(s, b);
    } else if (b == 0x7F) {
        /* DEL: nothing */
    } else if (b < 0x80) {
        put_char(s, b);
    } else if ((b & 0xE0) == 0xC0) {
        s->utf8_need = 1;
        s->utf8_cp = b & 0x1F;
        s->utf8_min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        s->utf8_need = 2;
        s->utf8_cp = b & 0x0F;
        s->utf8_min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        s->utf8_need = 3;
        s->utf8_cp = b & 0x07;
        s->utf8_min = 0x10000;
    } else {
        put_char(s, 0xFFFD);
    }
}

/* A byte no sequence may contain: the sequence is abandoned, and the byte
 * is read again as ordinary output. */
static bool abandon_on_high(struct term_screen *s, uint8_t b)
{
    if (b < 0x80) {
        return false;
    }
    s->ignored_sequences++;
    s->ps = TERM_PS_GROUND;
    ground(s, b);
    return true;
}

static void csi_byte(struct term_screen *s, uint8_t b)
{
    if (++s->seq_len > CSI_LEN_MAX) {
        s->bad = true;
    }
    if (b >= '0' && b <= '9') {
        if (s->inter) {
            s->bad = true;
        } else if (s->params[s->nparams] <= (CSI_PARAM_MAX - 9) / 10) {
            s->params[s->nparams] = s->params[s->nparams] * 10 + (b - '0');
        } else {
            s->params[s->nparams] = CSI_PARAM_MAX;
        }
        s->param_started = true;
    } else if (b == ';') {
        if (s->inter || s->nparams + 1 >= TERM_CSI_PARAMS) {
            s->bad = true;
        } else {
            s->nparams++;
            s->params[s->nparams] = 0;
        }
    } else if (b == ':') {
        s->bad = true; /* sub-parameters: not supported, so not guessed at */
    } else if (b >= '<' && b <= '?') {
        if (s->seq_len == 1) {
            s->priv = (char)b;
        } else {
            s->bad = true;
        }
    } else if (b >= 0x20 && b <= 0x2F) {
        if (s->inter) {
            s->bad = true;
        }
        s->inter = (char)b;
    } else if (b >= 0x40 && b <= 0x7E) {
        s->ps = TERM_PS_GROUND;
        if (s->bad) {
            s->ignored_sequences++;
        } else {
            csi_dispatch(s, b);
        }
    }
    /* 0x7F: ignored inside a sequence */
}

static void step(struct term_screen *s, uint8_t b)
{
    switch (s->ps) {
    case TERM_PS_GROUND:
        ground(s, b);
        return;

    case TERM_PS_ESC:
        if (b < 0x20) {
            control(s, b);
        } else if (abandon_on_high(s, b)) {
            /* read again as output */
        } else if (b >= 0x20 && b <= 0x2F) {
            s->inter = (char)b;
            s->ps = TERM_PS_ESC_INTER;
        } else if (b == '[') {
            s->ps = TERM_PS_CSI;
            s->nparams = 0;
            s->params[0] = 0;
            s->param_started = false;
            s->priv = 0;
            s->inter = 0;
            s->bad = false;
            s->seq_len = 0;
        } else if (b == ']' || b == 'P' || b == 'X' || b == '^' || b == '_') {
            s->ps = TERM_PS_STRING;
            s->seq_len = 0;
        } else if (b != 0x7F) {
            s->ps = TERM_PS_GROUND;
            esc_dispatch(s, b);
        }
        return;

    case TERM_PS_ESC_INTER:
        if (b < 0x20) {
            control(s, b);
        } else if (abandon_on_high(s, b)) {
            /* read again as output */
        } else if (b <= 0x2F) {
            s->bad = true;
        } else if (b != 0x7F) {
            s->ps = TERM_PS_GROUND;
            if (s->bad) {
                s->ignored_sequences++;
            } else {
                esc_inter_dispatch(s, (uint8_t)s->inter, b);
            }
        }
        return;

    case TERM_PS_CSI:
        if (b < 0x20) {
            control(s, b);
        } else if (!abandon_on_high(s, b)) {
            csi_byte(s, b);
        }
        return;

    case TERM_PS_STRING:
        if (b == 0x07) {
            s->ps = TERM_PS_GROUND;
        } else if (b == 0x1B) {
            s->ps = TERM_PS_STRING_ESC;
        } else if (b == 0x18 || b == 0x1A) {
            s->ps = TERM_PS_GROUND;
            s->ignored_sequences++;
        } else if (++s->seq_len > TERM_STRING_MAX) {
            s->ps = TERM_PS_GROUND;
            s->abandoned_strings++;
        }
        return;

    case TERM_PS_STRING_ESC:
        if (b == '\\') {
            s->ps = TERM_PS_GROUND;
        } else {
            /* Not a terminator: the string is over, and this ESC starts
             * whatever comes next. */
            begin_escape(s);
            step(s, b);
        }
        return;
    }
}

void term_screen_feed(struct term_screen *s, const uint8_t *data, size_t n)
{
    size_t i;

    if (!s->cells) {
        return;
    }
    for (i = 0; i < n; i++) {
        step(s, data[i]);
    }
}

/* ---- the view ----------------------------------------------------------- */

int term_screen_scrollback(const struct term_screen *s)
{
    return s->count - s->rows;
}

const struct term_cell *term_screen_view_row(const struct term_screen *s, int back, int r)
{
    if (!s->cells || back < 0 || back > term_screen_scrollback(s) || r < 0 || r >= s->rows) {
        return NULL;
    }
    return ring_line(s, (s->first + s->count - s->rows - back + r) % s->cap);
}

static size_t put_utf8(char *out, size_t cap, size_t at, uint32_t c)
{
    char b[4];
    size_t n;

    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else {
        b[0] = (char)(0xE0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    }
    if (at + n >= cap) {
        return at;
    }
    memcpy(out + at, b, n);
    return at + n;
}

size_t term_screen_row_text(const struct term_screen *s, int back, int r, char *out, size_t cap)
{
    const struct term_cell *line = term_screen_view_row(s, back, r);
    size_t at = 0;
    int last = -1;
    int i;

    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!line) {
        return 0;
    }
    for (i = 0; i < s->cols; i++) {
        if (line[i].ch != ' ') {
            last = i;
        }
    }
    for (i = 0; i <= last; i++) {
        at = put_utf8(out, cap, at, line[i].ch);
    }
    out[at] = '\0';
    return at;
}

void term_screen_clear_damage(struct term_screen *s)
{
    memset(s->dirty, 0, sizeof(s->dirty));
    s->all_dirty = false;
}

bool term_screen_damaged(const struct term_screen *s)
{
    int r;

    if (s->all_dirty) {
        return true;
    }
    for (r = 0; r < s->rows; r++) {
        if (s->dirty[r]) {
            return true;
        }
    }
    return false;
}
