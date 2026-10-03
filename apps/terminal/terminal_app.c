/*
 * Terminal: a real shell on the device, on a pseudo-terminal.
 *
 * One screen: a monospace grid filling the app's body, inside the corner
 * clearance, under the shell's own header and status cluster, with one row
 * under it for the session (CLOSE SESSION). The grid is the screen of
 * term_screen.c; the shell behind it is the user's login shell (the one
 * /etc/passwd names for the account Doors runs under) on a PTY (term_pty.c),
 * and term_session.c moves bytes between them.
 *
 * The session outlives the screen. The first open starts the shell; leaving
 * the app takes the screen away and nothing else - the shell, what it runs
 * and everything it has written stay, and the next open shows the same
 * screen again. One LVGL timer, owned by the session rather than the
 * screen, keeps taking the program's output while no screen is there, so a
 * long command runs on at full speed and its output lands in the scrollback.
 * The session ends only when it is asked to: CLOSE SESSION (after a
 * confirmation, DS §17.5), the shell exiting and the app being left, or the
 * Doors shell itself stopping or re-executing for a rotation (app.h
 * shutdown) - never by an exec that would leave its processes to nobody.
 *
 * The keys. The grid is the focused object and the input stream's raw key
 * target (pos_input.h), so every key the physical keyboard sends reaches the
 * shell - Tab, Esc and the Ctrl chords included, which anywhere else move
 * focus, cancel or type a letter. Without a keyboard, a tap on the grid
 * brings up the shell's touch keyboard. Shift+Up and Shift+Down, or a drag,
 * move the view through the scrollback; any other key brings it back to the
 * live screen and goes to the shell.
 *
 * The text size. The grid's cells are a monospace face at a size of the
 * Terminal's own for each of Small, Medium and Large (mono_font()); when the
 * setting changes the grid is measured again and the program is told its new
 * columns and rows (SIGWINCH), as when a desktop terminal changes its font.
 *
 * Time. The timer runs every TERMINAL_TICK_MS, takes a bounded slice of
 * output (TERM_PUMP_BUDGET), invalidates what changed and nothing else, and
 * makes only non-blocking calls. A screen that scrolls as a whole is redrawn
 * at most every FULL_REDRAW_MS however fast the output comes. The one wait
 * on the LVGL thread is ending the session, bounded by TERM_PTY_HUP_GRACE_MS
 * + TERM_PTY_KILL_REAP_MS and normally a few ms (docs/apps/TERMINAL.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "terminal_app.h"

#include "app.h"
#include "pocketui.h"
#include "pos_input.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A whole-screen redraw (output scrolling) at most this often. */
#define FULL_REDRAW_MS 60
/* Space between the grid and the frame's edge. */
#define GRID_PAD 4
/* A press that moved further than this was a drag, not a tap. */
#define TAP_SLOP 12
/* The session row's action: an inline row action (DS §7), 44 px tall in a
 * 64 px row whose height is its hit area. */
#define ACTION_H 44
/* Output taken per tick while no screen is attached: about 130 KB a second,
 * far more than a build or a log writes, while a program that floods is held
 * by the PTY's back-pressure instead of taking CPU from the app in front (on
 * unit A a flood costs doors-shell 13 % with the Terminal open, 4 % closed). */
#define DETACHED_PUMP_BUDGET 4096

/* What the timer is to do with the confirmation, outside the event that
 * asked: focus moved inside an LVGL event does not stick (the Doors LVGL
 * notes), so the dialog opens and closes on the next tick. */
enum terminal_want {
    WANT_NOTHING = 0,
    WANT_CONFIRM, /* open the confirmation, focus on Cancel */
    WANT_GRID,    /* close it, focus back on the grid */
};

struct terminal_app {
    lv_obj_t *frame;
    lv_obj_t *grid;
    lv_obj_t *bar;
    lv_obj_t *bar_caption;
    lv_obj_t *close_btn;
    lv_obj_t *dialog;
    lv_obj_t *cancel_btn;
    lv_obj_t *confirm_btn;
    bool confirming;
    enum terminal_want want;
    struct pocketui_layout_guard guard;
    bool layout_pending;
    bool metrics_pending;
    bool listening;

    const lv_font_t *font;
    int32_t cell_w, cell_h;

    int view_back; /* lines above the live screen; 0 = live */
    unsigned seen_scrolled;
    int drawn_back;
    int drawn_cx, drawn_cy;
    bool drawn_cursor;
    int64_t last_full_ms;
    int bar_phase;

    int32_t drag_accum;
    int32_t drag_travel;

    const char *hint;
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
    int test_fd;
    char test_buf[1024];
    size_t test_len;
#endif
};

/* The status cluster's mark while a running shell is kept with no screen
 * over it (app.h pocketos_shell_set_background, DS §51.4), as RIFT's. */
#define TERMINAL_APP_ID "terminal"
#define TERMINAL_BACKGROUND_LABEL ">_"
#define TERMINAL_BACKGROUND_HELP "Terminal session active in background"

/* The session: one per Doors shell run, and not the screen's. `ui` is the
 * screen showing it, or NULL while the app is not open. */
static struct {
    struct term_session session;
    bool live;
    lv_timer_t *timer;
    bool paused; /* the timer, while nothing can come (tick()) */
    struct terminal_app *ui;
    unsigned opened; /* sessions started in this run, for the tests */
} bg;

/* ---- colours ------------------------------------------------------------- *
 *
 * No colour of the terminal's own: the 16 ANSI colours are the theme's
 * tokens and the identity accents (DS §37), which are readable on every
 * theme's background in every mode, so a red error or a green directory
 * follows Normal, Outdoor and Night like everything else on the screen. */

static const unsigned ansi_identity[8] = {
    0, /* black: not used, see below */
    0, /* red: identity 0, salmon red */
    3, /* green */
    2, /* yellow */
    5, /* blue */
    7, /* magenta */
    4, /* cyan */
    0, /* white: not used */
};

static lv_color_t fg_color(uint8_t c, bool bold)
{
    lv_color_t col;

    switch (c) {
    case TERM_COLOR_DEFAULT:
    case 15:
        return pos_theme_color(POS_COLOR_TEXT_PRIMARY);
    case 0:
    case 8:
        return pos_theme_color(bold || c == 8 ? POS_COLOR_TEXT_SECONDARY : POS_COLOR_TEXT_MUTED);
    case 7:
        return pos_theme_color(bold ? POS_COLOR_TEXT_PRIMARY : POS_COLOR_TEXT_SECONDARY);
    default:
        col = pos_identity_hue(ansi_identity[c & 7]);
        /* Bright colours, and bold (this font has no heavier face): lighter. */
        return (c >= 8 || bold) ? lv_color_lighten(col, LV_OPA_30) : col;
    }
}

static lv_color_t bg_color(uint8_t c)
{
    switch (c) {
    case TERM_COLOR_DEFAULT:
    case 0:
        return pos_theme_color(POS_COLOR_BG);
    case 8:
        return pos_theme_color(POS_COLOR_SURFACE_RAISED);
    case 7:
        return pos_theme_color(POS_COLOR_TEXT_SECONDARY);
    case 15:
        return pos_theme_color(POS_COLOR_TEXT_PRIMARY);
    default:
        /* Half way to the background, so text in the default colour on it
         * still reads. */
        return lv_color_mix(pos_identity_hue(ansi_identity[c & 7]), pos_theme_color(POS_COLOR_BG),
                            LV_OPA_50);
    }
}

/* ---- the font ------------------------------------------------------------ */

/* The grid's face at each text size: the Design System's monospace face
 * (the caption face, §4), at sizes of the Terminal's own rather than any
 * role's. Small is the 8 x 18 px cell the grid always had; Medium and Large
 * grow it by about a fifth and two fifths, as DS §46 grows reading text, and
 * keep over 40 columns in portrait. The font's letter spacing is not
 * taken: a grid needs every cell the glyph's own width (DS §49). */
static const int mono_px[POS_TEXT_SIZE_COUNT] = { 14, 17, 20 };

static const lv_font_t *mono_font(void)
{
    enum pos_text_size size = pos_theme_current_text_size();
    struct pos_type_spec spec;

    if (size < 0 || size >= POS_TEXT_SIZE_COUNT) {
        size = POS_TEXT_SIZE_SMALL;
    }
    spec.face = POS_FACE_MONO;
    spec.px = mono_px[size];
    return pos_type_font(spec);
}

static void set_font(struct terminal_app *a, const lv_font_t *font)
{
    a->font = font;
    a->cell_h = lv_font_get_line_height(font);
    a->cell_w = lv_font_get_glyph_width(font, 'M', 0);
    if (a->cell_w <= 0) {
        a->cell_w = 8;
    }
    if (a->cell_h <= 0) {
        a->cell_h = 18;
    }
}

/* A character the font cannot draw would be drawn as nothing and move every
 * later character of its run by a cell; it is drawn as '?' instead. */
static uint32_t drawable(const lv_font_t *font, uint32_t c)
{
    lv_font_glyph_dsc_t g;

    if (c < 0x80) {
        return c;
    }
    return lv_font_get_glyph_dsc(font, &g, c, 0) ? c : '?';
}

static size_t utf8_put(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/* ---- drawing ------------------------------------------------------------- */

static void grid_origin(const struct terminal_app *a, int32_t *x, int32_t *y)
{
    lv_area_t c;

    lv_obj_get_coords(a->grid, &c);
    *x = c.x1 + GRID_PAD;
    *y = c.y1 + GRID_PAD;
}

static bool cursor_shown(const struct terminal_app *a)
{
    return a->view_back == 0 && bg.session.phase == TERM_SESSION_RUNNING && bg.session.screen.cursor_visible;
}

static void draw_run(struct terminal_app *a, lv_layer_t *layer, const struct term_cell *line, int from,
                     int to, int32_t x0, int32_t y, bool cursor)
{
    uint8_t fgc = line[from].fg;
    bool bold = (fgc & TERM_ATTR_BOLD) != 0;
    bool reverse = (fgc & TERM_ATTR_REVERSE) != 0;
    lv_color_t fg = fg_color((uint8_t)(fgc & TERM_COLOR_MASK), bold);
    lv_color_t bg = bg_color(line[from].bg);
    bool fill = line[from].bg != TERM_COLOR_DEFAULT;
    char text[TERM_MAX_COLS * 3 + 1];
    size_t len = 0;
    bool ink = false;
    int i;

    if (reverse) {
        lv_color_t t = fg;

        fg = bg;
        bg = t;
        fill = true;
    }
    if (cursor) {
        fg = pos_theme_color(POS_COLOR_TEXT_ON_ACCENT);
        bg = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
        fill = true;
    }
    if (fill) {
        lv_draw_rect_dsc_t rect;
        lv_area_t r = { x0 + from * a->cell_w, y, x0 + to * a->cell_w - 1, y + a->cell_h - 1 };

        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = bg;
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = 0;
        lv_draw_rect(layer, &rect, &r);
    }
    for (i = from; i < to; i++) {
        uint32_t c = drawable(a->font, line[i].ch);

        ink = ink || c != ' ';
        len += utf8_put(text + len, c);
    }
    text[len] = '\0';
    if (ink || (fgc & TERM_ATTR_UNDERLINE)) {
        lv_draw_label_dsc_t label;
        lv_area_t r = { x0 + from * a->cell_w, y, x0 + (to + 1) * a->cell_w, y + a->cell_h - 1 };

        lv_draw_label_dsc_init(&label);
        label.font = a->font;
        label.color = fg;
        label.opa = LV_OPA_COVER;
        label.text = text;
        label.text_local = 1;
        label.flag = LV_TEXT_FLAG_EXPAND;
        if (fgc & TERM_ATTR_UNDERLINE) {
            label.decor = LV_TEXT_DECOR_UNDERLINE;
        }
        lv_draw_label(layer, &label, &r);
    }
}

static void grid_draw(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const struct term_screen *s;
    lv_draw_rect_dsc_t rect;
    lv_area_t coords;
    int32_t x0;
    int32_t y0;
    int r;

    if (!a || !layer || !bg.live || bg.session.phase == TERM_SESSION_OFF) {
        return;
    }
    s = &bg.session.screen;
    lv_obj_get_coords(a->grid, &coords);
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = pos_theme_color(POS_COLOR_BG);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 0;
    lv_draw_rect(layer, &rect, &coords);
    grid_origin(a, &x0, &y0);

    for (r = 0; r < s->rows; r++) {
        const struct term_cell *line = term_screen_view_row(s, a->view_back, r);
        int32_t y = y0 + r * a->cell_h;
        int from = 0;
        int c;
        int cur = (cursor_shown(a) && r == s->cy) ? s->cx : -1;

        /* Only the rows the redraw needs. */
        if (y + a->cell_h <= layer->_clip_area.y1 || y > layer->_clip_area.y2) {
            continue;
        }
        if (!line) {
            continue;
        }
        for (c = 1; c <= s->cols; c++) {
            bool split = c == s->cols || c == cur || c - 1 == cur || line[c].fg != line[from].fg ||
                         line[c].bg != line[from].bg;

            if (split) {
                draw_run(a, layer, line, from, c, x0, y, from == cur);
                from = c;
            }
        }
    }
}

/* ---- invalidation -------------------------------------------------------- */

static void invalidate_row(struct terminal_app *a, int r)
{
    lv_area_t area;
    int32_t x0;
    int32_t y0;

    if (r < 0 || r >= bg.session.screen.rows) {
        return;
    }
    grid_origin(a, &x0, &y0);
    lv_obj_get_coords(a->grid, &area);
    area.y1 = y0 + r * a->cell_h;
    area.y2 = area.y1 + a->cell_h - 1;
    lv_obj_invalidate_area(a->grid, &area);
}

static void set_hint(struct terminal_app *a)
{
    const char *hint = NULL;

    if (bg.session.phase == TERM_SESSION_ENDED) {
        hint = "ENDED";
    } else if (a->view_back > 0) {
        hint = "SCROLLBACK";
    }
    if (hint != a->hint) {
        pocketos_shell_set_status_hint(hint ? hint : "");
        a->hint = hint;
    }
}

/* The session row says what leaving does: a running shell is kept, an ended
 * one is not (destroy() below). Only a change is written. */
static void update_bar(struct terminal_app *a)
{
    int phase = (int)bg.session.phase;

    if (phase == a->bar_phase) {
        return;
    }
    a->bar_phase = phase;
    lv_label_set_text(a->bar_caption,
                      phase == TERM_SESSION_RUNNING ? "KEPT WHEN YOU LEAVE" : "SHELL ENDED");
}

static void refresh(struct terminal_app *a, int64_t now)
{
    struct term_screen *s = &bg.session.screen;
    bool cursor = cursor_shown(a);
    int r;

    /* A view looking back stays on the lines it shows while output pushes
     * more into the scrollback, until those lines are dropped. */
    if (a->view_back > 0) {
        a->view_back += (int)(s->scrolled - a->seen_scrolled);
    }
    a->seen_scrolled = s->scrolled;
    if (a->view_back > term_screen_scrollback(s)) {
        a->view_back = term_screen_scrollback(s);
    }

    if (a->view_back != a->drawn_back || (a->view_back > 0 && s->all_dirty)) {
        lv_obj_invalidate(a->grid);
        a->last_full_ms = now;
        term_screen_clear_damage(s);
    } else if (s->all_dirty) {
        if (now - a->last_full_ms < FULL_REDRAW_MS) {
            return; /* the damage waits for the next tick */
        }
        lv_obj_invalidate(a->grid);
        a->last_full_ms = now;
        term_screen_clear_damage(s);
    } else if (a->view_back == 0) {
        for (r = 0; r < s->rows; r++) {
            if (s->dirty[r]) {
                invalidate_row(a, r);
            }
        }
        term_screen_clear_damage(s);
        if (cursor != a->drawn_cursor || s->cx != a->drawn_cx || s->cy != a->drawn_cy) {
            invalidate_row(a, a->drawn_cy);
            invalidate_row(a, s->cy);
        }
    } else {
        term_screen_clear_damage(s);
    }
    a->drawn_back = a->view_back;
    a->drawn_cx = s->cx;
    a->drawn_cy = s->cy;
    a->drawn_cursor = cursor;
    set_hint(a);
}

/* ---- layout -------------------------------------------------------------- */

/* The grid's columns and rows in the current cells, or 0 when it has no
 * size yet. */
static void measure(struct terminal_app *a, int *cols, int *rows)
{
    lv_area_t c;

    lv_obj_get_coords(a->grid, &c);
    *cols = (int)((lv_area_get_width(&c) - 2 * GRID_PAD) / a->cell_w);
    *rows = (int)((lv_area_get_height(&c) - 2 * GRID_PAD) / a->cell_h);
    if (*cols < TERM_MIN_COLS || *rows < TERM_MIN_ROWS) {
        *cols = 0;
        *rows = 0;
    }
}

static void apply_insets(struct terminal_app *a)
{
    struct pos_insets in;

    if (pocketui_layout_begin(&a->guard, a->frame, &in)) {
        lv_obj_set_style_pad_left(a->frame, in.left, 0);
        lv_obj_set_style_pad_top(a->frame, in.top, 0);
        lv_obj_set_style_pad_right(a->frame, in.right, 0);
        lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
        lv_obj_update_layout(a->frame);
    }
}

static void layout(struct terminal_app *a)
{
    int cols;
    int rows;

    apply_insets(a);
    measure(a, &cols, &rows);
    if (!cols) {
        /* Not laid out yet (or hidden behind the confirmation): stay
         * pending, so the next tick measures again whether or not a size
         * change is ever reported. */
        return;
    }
    a->layout_pending = false;
    if (bg.live && (cols != bg.session.screen.cols || rows != bg.session.screen.rows)) {
        term_session_resize(&bg.session, cols, rows);
        a->view_back = 0;
        lv_obj_invalidate(a->grid);
    }
}

static void on_frame_size(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);

    /* Not measured here: a size change arrives inside LVGL's layout pass,
     * where the grid's size is not final. The timer does it. */
    a->layout_pending = true;
}

/* The text size (or the theme, or the mode) changed. The cells are not
 * measured here either: the timer takes the new face and lays the grid out
 * again in it. */
static void on_theme(void *user)
{
    struct terminal_app *a = user;

    a->metrics_pending = true;
}

static void apply_metrics(struct terminal_app *a)
{
    const lv_font_t *f = mono_font();

    a->metrics_pending = false;
    /* The caption's one line is measured in its font as styled now. */
    pocketui_label_fit(a->bar_caption, 1);
    if (f == a->font) {
        return;
    }
    set_font(a, f);
    a->layout_pending = true;
    lv_obj_invalidate(a->grid);
}

/* ---- keys from a test --------------------------------------------------- *
 *
 * Simulator and gate builds only (POCKETOS_SHELL_TEST_HOOKS, never the image's
 * shell): the keyboard base cannot be pressed over SSH, so a gate or a
 * shell test writes keys to the FIFO named by POCKETOS_TEST_TERMINAL_KEYS and
 * they are pushed into the input stream exactly as the physical keyboard's
 * driver pushes them - key and modifiers - to reach the grid through the raw
 * key path. A byte is a character, a newline is Enter, and {NAME} is a key:
 * TAB ESC BS ENTER UP DOWN LEFT RIGHT LBRACE F1..F12, or a character, each
 * with any of the prefixes C- (Ctrl), M- (Alt) and S- (Shift): {C-c},
 * {S-UP}. A function key is pushed as the keyboard pushes it with Fn held. */
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
#include <fcntl.h>

static void test_keys_open(struct terminal_app *a)
{
    const char *path = getenv("POCKETOS_TEST_TERMINAL_KEYS");

    a->test_fd = -1;
    if (path && *path) {
        /* Read and write, so the FIFO never reports an end between writers. */
        a->test_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
}

static bool test_key_token(const char *name, pos_key_t *key, unsigned *mods)
{
    static const struct {
        const char *name;
        pos_key_t key;
    } named[] = { { "TAB", LV_KEY_NEXT }, { "ESC", LV_KEY_ESC }, { "BS", LV_KEY_BACKSPACE },
                  { "ENTER", LV_KEY_ENTER }, { "UP", LV_KEY_UP }, { "DOWN", LV_KEY_DOWN },
                  { "LEFT", LV_KEY_LEFT }, { "RIGHT", LV_KEY_RIGHT }, { "LBRACE", '{' } };
    size_t i;

    *mods = 0;
    for (;;) {
        if (strncmp(name, "C-", 2) == 0) {
            *mods |= POS_INPUT_MOD_CTRL;
        } else if (strncmp(name, "M-", 2) == 0) {
            *mods |= POS_INPUT_MOD_ALT;
        } else if (strncmp(name, "S-", 2) == 0) {
            *mods |= POS_INPUT_MOD_SHIFT;
        } else {
            break;
        }
        name += 2;
    }
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strcmp(name, named[i].name) == 0) {
            *key = named[i].key;
            return true;
        }
    }
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);

        if (n >= 1 && n <= 12) {
            *key = POS_KEY_F(n);
            return true;
        }
    }
    if (name[0] && !name[1]) {
        *key = (unsigned char)name[0];
        return true;
    }
    return false;
}

static void test_keys_poll(struct terminal_app *a)
{
    size_t used = 0;
    int pushed = 0;
    ssize_t n;

    if (a->test_fd < 0) {
        return;
    }
    n = read(a->test_fd, a->test_buf + a->test_len, sizeof(a->test_buf) - 1 - a->test_len);
    if (n > 0) {
        a->test_len += (size_t)n;
    }
    /* A few keys a tick: the stream holds 32, and a test should not be able
     * to overrun what a person could type. */
    while (used < a->test_len && pushed < 16) {
        char c = a->test_buf[used];
        pos_key_t key;
        unsigned mods = 0;

        if (c == '{') {
            char name[24];
            char *end = memchr(a->test_buf + used, '}', a->test_len - used);
            size_t len;

            if (!end) {
                break; /* the rest of the token has not arrived */
            }
            len = (size_t)(end - (a->test_buf + used)) - 1;
            if (len < sizeof(name)) {
                memcpy(name, a->test_buf + used + 1, len);
                name[len] = '\0';
                if (test_key_token(name, &key, &mods)) {
                    pos_input_push_key_mods(key, mods);
                    pushed++;
                }
            }
            used = (size_t)(end - a->test_buf) + 1;
            continue;
        }
        key = c == '\n' ? LV_KEY_ENTER : (pos_key_t)(unsigned char)c;
        pos_input_push_key_mods(key, 0);
        pushed++;
        used++;
    }
    memmove(a->test_buf, a->test_buf + used, a->test_len - used);
    a->test_len -= used;
}

static void test_keys_close(struct terminal_app *a)
{
    if (a->test_fd >= 0) {
        close(a->test_fd);
        a->test_fd = -1;
    }
}
#else
static void test_keys_open(struct terminal_app *a)
{
    (void)a;
}

static void test_keys_poll(struct terminal_app *a)
{
    (void)a;
}

static void test_keys_close(struct terminal_app *a)
{
    (void)a;
}
#endif

/* ---- the session ----------------------------------------------------------- */

/* The account's login shell, as SSH would start it, in its home. An
 * explicit POCKETOS_TERMINAL_SHELL runs that program instead, not as a login
 * shell (the tests, and anyone who wants another shell). */
static void pick_shell(char *path, size_t n, bool *login, char *home, size_t hn)
{
    const char *over = getenv("POCKETOS_TERMINAL_SHELL");
    const char *env_home = getenv("HOME");
    struct passwd *pw = getpwuid(getuid());

    if (env_home && env_home[0] == '/') {
        snprintf(home, hn, "%s", env_home);
    } else if (pw && pw->pw_dir && pw->pw_dir[0] == '/') {
        snprintf(home, hn, "%s", pw->pw_dir);
    } else {
        snprintf(home, hn, "/");
    }
    if (over && *over) {
        snprintf(path, n, "%s", over);
        *login = false;
        return;
    }
    if (pw && pw->pw_shell && pw->pw_shell[0] == '/' && access(pw->pw_shell, X_OK) == 0) {
        snprintf(path, n, "%s", pw->pw_shell);
    } else {
        snprintf(path, n, "/bin/sh");
    }
    *login = true;
}

static void on_tick(lv_timer_t *t);

static void session_start(int cols, int rows)
{
    char path[256];
    char home[256];
    bool login = false;

    pick_shell(path, sizeof(path), &login, home, sizeof(home));
    bg.live = term_session_open(&bg.session, path, login, home, cols, rows) == 0;
    if (bg.live) {
        bg.opened++;
        bg.timer = lv_timer_create(on_tick, TERMINAL_TICK_MS, NULL);
        bg.paused = false;
    }
}

/* The shell and everything in its session end here (term_pty_close's
 * bounds), the screen and its scrollback are freed, and the timer goes: the
 * next open starts from nothing. Safe to call with no session. */
static void session_end(void)
{
    if (bg.timer) {
        lv_timer_delete(bg.timer);
        bg.timer = NULL;
    }
    if (bg.live) {
        term_session_close(&bg.session);
        bg.live = false;
    }
    pocketos_shell_set_background(TERMINAL_APP_ID, NULL, NULL);
}

/* ---- the confirmation (DS §17.5) ----------------------------------------- */

static void show_grid(struct terminal_app *a, bool on)
{
    if (on) {
        lv_obj_clear_flag(a->grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(a->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a->dialog, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(a->dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

/* On the timer, never in the event that asked (see enum terminal_want). The
 * buttons are in the focus group only while the dialog is up and the grid
 * only while it is not, so Tab inside the dialog visits Cancel and Close and
 * nothing behind them; Cancel takes the focus first (§17.5). */
static void apply_want(struct terminal_app *a)
{
    enum terminal_want w = a->want;

    a->want = WANT_NOTHING;
    if (w == WANT_CONFIRM && !a->confirming && bg.live) {
        pocketos_shell_keyboard_hide();
        a->confirming = true;
        show_grid(a, false);
        lv_group_remove_obj(a->grid);
        pos_input_add_obj(a->cancel_btn);
        pos_input_add_obj(a->confirm_btn);
        pos_input_focus(a->cancel_btn);
    } else if (w == WANT_GRID && a->confirming) {
        a->confirming = false;
        show_grid(a, true);
        lv_group_remove_obj(a->cancel_btn);
        lv_group_remove_obj(a->confirm_btn);
        pos_input_add_obj(a->grid);
        pos_input_focus(a->grid);
        a->layout_pending = true;
        lv_obj_invalidate(a->grid);
    }
}

static void go_home(void *user)
{
    (void)user;
    pocketos_shell_go_home();
}

/* CLOSE SESSION, confirmed (or with nothing left running to confirm): the
 * session ends now, on this press and at no other moment, and the app is
 * left - the next open starts a fresh shell. Leaving happens after this
 * event, never inside it: going home destroys the screen this button is on. */
static void close_now(void)
{
    session_end();
    lv_async_call(go_home, NULL);
}

static void on_close_clicked(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);

    if (!bg.live) {
        return;
    }
    if (bg.session.phase != TERM_SESSION_RUNNING) {
        /* The shell has ended: nothing runs that a confirmation would save. */
        close_now();
        return;
    }
    a->want = WANT_CONFIRM;
}

static void on_cancel(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);

    a->want = WANT_GRID;
}

static void on_confirm(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);

    if (!a->confirming) {
        return;
    }
    close_now();
}

/* Back (app.h): out of the confirmation first, as its Cancel does. */
static int terminal_back(void *priv)
{
    struct terminal_app *a = priv;

    if (a && a->confirming) {
        a->want = WANT_GRID;
        return 1;
    }
    return 0;
}

/* ---- the timer ----------------------------------------------------------- */

static void tick(void)
{
    struct terminal_app *a = bg.ui;
    int64_t now = term_pty_now_ms();

    if (!bg.live) {
        return;
    }
    if (a) {
        test_keys_poll(a);
        if (a->want != WANT_NOTHING) {
            apply_want(a);
        }
        if (a->metrics_pending) {
            apply_metrics(a);
        }
        if (a->layout_pending && !a->confirming) {
            layout(a);
        }
    }
    term_session_pump(&bg.session, a ? TERM_PUMP_BUDGET : DETACHED_PUMP_BUDGET, now);
    if (a) {
        refresh(a, now);
        update_bar(a);
    } else if (bg.session.phase == TERM_SESSION_ENDED && bg.timer) {
        /* No screen, and the shell has ended and been read to the end:
         * nothing more will come until the app is opened again, and nothing
         * runs behind the other screens: the mark goes. */
        lv_timer_pause(bg.timer);
        bg.paused = true;
        pocketos_shell_set_background(TERMINAL_APP_ID, NULL, NULL);
    }
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    tick();
}

/* ---- keys ---------------------------------------------------------------- */

static unsigned term_mods(unsigned mods)
{
    return (mods & POS_INPUT_MOD_SHIFT ? TERM_MOD_SHIFT : 0u) | (mods & POS_INPUT_MOD_CTRL ? TERM_MOD_CTRL : 0u) |
           (mods & POS_INPUT_MOD_ALT ? TERM_MOD_ALT : 0u);
}

static enum term_key_kind kind_of(pos_key_t key)
{
    switch (key) {
    case LV_KEY_ENTER: return TERM_KEY_ENTER;
    case LV_KEY_BACKSPACE: return TERM_KEY_BACKSPACE;
    case LV_KEY_NEXT: return TERM_KEY_TAB; /* the keyboard's Tab */
    case LV_KEY_PREV: return TERM_KEY_BACKTAB;
    case LV_KEY_ESC: return TERM_KEY_ESC;
    case LV_KEY_UP: return TERM_KEY_UP;
    case LV_KEY_DOWN: return TERM_KEY_DOWN;
    case LV_KEY_RIGHT: return TERM_KEY_RIGHT;
    case LV_KEY_LEFT: return TERM_KEY_LEFT;
    case LV_KEY_HOME: return TERM_KEY_HOME;
    case LV_KEY_END: return TERM_KEY_END;
    case LV_KEY_DEL: return TERM_KEY_DELETE;
    default:
        if (pos_key_function(key)) {
            /* Fn with a function key (shell_kbd.c): the raw F-key. */
            return (enum term_key_kind)(TERM_KEY_F1 + pos_key_function(key) - 1);
        }
        return key >= 0x20 ? TERM_KEY_CHAR : TERM_KEY_NONE;
    }
}

static void scroll_view(struct terminal_app *a, int lines)
{
    int max = term_screen_scrollback(&bg.session.screen);
    int v = a->view_back + lines;

    v = v < 0 ? 0 : v > max ? max : v;
    if (v != a->view_back) {
        a->view_back = v;
        refresh(a, term_pty_now_ms());
    }
}

static void on_key(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);
    const uint32_t *param = lv_event_get_param(e);
    struct term_key k;
    pos_key_t key;
    unsigned mods = 0;

    if (!a || !param || !bg.live || a->confirming) {
        return;
    }
    if (!pos_input_raw_decode(*param, &key, &mods)) {
        /* An ordinary delivery: only while the grid is not the raw target,
         * which it always is. ASCII and the LV_KEY_* constants only. */
        if (*param >= 0x80) {
            return;
        }
        key = *param;
    }
    k.kind = kind_of(key);
    k.cp = key;
    k.mods = term_mods(mods);
    if (k.kind == TERM_KEY_NONE) {
        return;
    }
    /* The view's own keys. */
    if ((k.mods & TERM_MOD_SHIFT) && (k.kind == TERM_KEY_UP || k.kind == TERM_KEY_DOWN)) {
        int half = bg.session.screen.rows / 2 > 0 ? bg.session.screen.rows / 2 : 1;

        scroll_view(a, k.kind == TERM_KEY_UP ? half : -half);
        return;
    }
    if (a->view_back != 0) {
        scroll_view(a, -a->view_back);
    }
    term_session_key(&bg.session, &k);
}

/* ---- touch --------------------------------------------------------------- */

static void on_pressed(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);

    a->drag_accum = 0;
    a->drag_travel = 0;
}

static void on_pressing(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t v;
    int lines = 0;

    if (!indev || !bg.live) {
        return;
    }
    lv_indev_get_vect(indev, &v);
    a->drag_travel += LV_ABS(v.x) + LV_ABS(v.y);
    a->drag_accum += v.y;
    /* Dragging down shows older lines, as a list moves under a finger. */
    while (a->drag_accum >= a->cell_h) {
        a->drag_accum -= a->cell_h;
        lines++;
    }
    while (a->drag_accum <= -a->cell_h) {
        a->drag_accum += a->cell_h;
        lines--;
    }
    if (lines) {
        scroll_view(a, lines);
    }
}

static void on_clicked(lv_event_t *e)
{
    struct terminal_app *a = lv_event_get_user_data(e);
    struct pocketos_orientation o;

    if (a->drag_travel > TAP_SLOP) {
        return;
    }
    /* With the keyboard base attached, the keys are there already. */
    pocketos_shell_orientation(&o);
    if (o.keyboard != POCKETOS_KEYBOARD_PRESENT && !pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, a);
    }
}

/* ---- the screen ------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* A button that never takes the focus by a tap: the grid keeps the keys. */
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, bool primary)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
    }
    return b;
}

static void build_bar(struct terminal_app *a)
{
    a->bar = box(a->frame);
    lv_obj_set_size(a->bar, LV_PCT(100), POCKETUI_ROW_H);
    lv_obj_set_flex_flow(a->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(a->bar, GRID_PAD, 0);
    lv_obj_set_style_pad_column(a->bar, 12, 0);

    a->bar_caption = pocketui_label(a->bar, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->bar_caption, 1);
    lv_obj_set_flex_grow(a->bar_caption, 1);
    lv_label_set_long_mode(a->bar_caption, LV_LABEL_LONG_DOT);
    pocketui_label_fit(a->bar_caption, 1);
    a->bar_phase = -1;

    a->close_btn = button(a->bar, "CLOSE SESSION", on_close_clicked, a, false);
    lv_obj_set_size(a->close_btn, LV_SIZE_CONTENT, ACTION_H);
    lv_obj_set_style_pad_hor(a->close_btn, 16, 0);
    /* The row's height is the hit area (DS §7). */
    lv_obj_set_ext_click_area(a->close_btn, (POCKETUI_ROW_H - ACTION_H) / 2);
}

static void build_dialog(struct terminal_app *a)
{
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *buttons;

    a->dialog = pocketui_card(a->frame);
    lv_obj_add_flag(a->dialog, LV_OBJ_FLAG_HIDDEN);
    title = pocketui_label(a->dialog, "Close the session?", POS_STYLE_TITLE);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(100));
    body = pocketui_label(a->dialog,
                          "The shell and everything it is running end now. The next time Terminal "
                          "opens, it starts a new shell.",
                          POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);

    buttons = box(a->dialog);
    lv_obj_set_size(buttons, LV_PCT(100), 56);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    /* Cancel first and accented: closing ends programs, which cannot be
     * undone (§17.5, the power-off case). */
    a->cancel_btn = button(buttons, "CANCEL", on_cancel, a, true);
    lv_obj_set_height(a->cancel_btn, LV_PCT(100));
    lv_obj_set_flex_grow(a->cancel_btn, 1);
    a->confirm_btn = button(buttons, "CLOSE SESSION", on_confirm, a, false);
    lv_obj_set_height(a->confirm_btn, LV_PCT(100));
    lv_obj_set_flex_grow(a->confirm_btn, 1);
    /* Esc and any other way out are Cancel (§17.5). */
    lv_obj_add_event_cb(a->cancel_btn, on_cancel, LV_EVENT_CANCEL, a);
    lv_obj_add_event_cb(a->confirm_btn, on_cancel, LV_EVENT_CANCEL, a);
}

static void *terminal_create(lv_obj_t *root)
{
    struct terminal_app *a = calloc(1, sizeof(*a));
    int cols;
    int rows;

    if (!a) {
        return NULL;
    }
    set_font(a, mono_font());

    a->frame = box(root);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    a->grid = box(a->frame);
    lv_obj_set_width(a->grid, LV_PCT(100));
    lv_obj_set_flex_grow(a->grid, 1);
    lv_obj_clear_flag(a->grid, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER |
                                   LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(a->grid, LV_OBJ_FLAG_CLICKABLE);
    pos_theme_watch(a->grid);
    build_bar(a);
    build_dialog(a);

    lv_obj_update_layout(a->frame);
    apply_insets(a);
    measure(a, &cols, &rows);

    if (!bg.live) {
        if (!cols) {
            /* Not sized yet; the first tick lays it out and resizes. */
            cols = 80;
            rows = 24;
            a->layout_pending = true;
        }
        session_start(cols, rows);
    } else {
        /* The session that was left running: the same screen, in this
         * screen's cells (the text size may have changed meanwhile), live. */
        if (!cols) {
            a->layout_pending = true;
        } else if (cols != bg.session.screen.cols || rows != bg.session.screen.rows) {
            term_session_resize(&bg.session, cols, rows);
        }
        if (bg.timer) {
            lv_timer_resume(bg.timer);
            bg.paused = false;
        }
    }
    bg.ui = a;
    /* On screen: not in the background any more. */
    pocketos_shell_set_background(TERMINAL_APP_ID, NULL, NULL);
    if (bg.live) {
        a->seen_scrolled = bg.session.screen.scrolled;
        term_screen_clear_damage(&bg.session.screen);
    }

    lv_obj_add_event_cb(a->grid, grid_draw, LV_EVENT_DRAW_MAIN, a);
    lv_obj_add_event_cb(a->grid, on_key, LV_EVENT_KEY, a);
    lv_obj_add_event_cb(a->grid, on_pressed, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->grid, on_pressing, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->grid, on_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    a->listening = pos_theme_add_listener(on_theme, a) == 0;

    pos_input_add_obj(a->grid);
    pos_input_focus(a->grid);
    pos_input_set_raw_target(a->grid);

    test_keys_open(a);
    /* The whole grid is drawn next, cursor included, where the cursor is
     * now: that is what refresh() must repaint when it moves. Not "nowhere"
     * (-1): a new session's first frame draws the cursor at the top-left
     * before the shell writes anything, and output that moves it without
     * touching row 0 (a login banner, a prompt starting with a new line)
     * would leave that block behind. */
    a->drawn_cy = bg.live ? bg.session.screen.cy : -1;
    a->drawn_cx = bg.live ? bg.session.screen.cx : 0;
    a->drawn_cursor = bg.live && cursor_shown(a);
    lv_obj_invalidate(a->grid);
    if (bg.live) {
        update_bar(a);
        set_hint(a);
    }
    return a;
}

/* Leaving the app takes the screen away and leaves the session running. An
 * ended shell is not kept: there is nothing in it to come back to but its
 * last words, and the next open should be a working shell. A kept one is
 * marked; the next open, session_end() and tick() clear the mark. */
static void terminal_destroy(void *priv)
{
    struct terminal_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->grid, NULL, a);
    lv_obj_remove_event_cb_with_user_data(a->frame, NULL, a);
    if (a->listening) {
        pos_theme_remove_listener(on_theme, a);
    }
    if (pos_input_raw_target() == a->grid) {
        pos_input_set_raw_target(NULL);
    }
    test_keys_close(a);
    if (bg.ui == a) {
        bg.ui = NULL;
    }
    if (bg.live && bg.session.phase != TERM_SESSION_RUNNING) {
        session_end();
    } else if (bg.live) {
        pocketos_shell_set_background(TERMINAL_APP_ID, TERMINAL_BACKGROUND_LABEL, TERMINAL_BACKGROUND_HELP);
    }
    if (a->hint) {
        pocketos_shell_set_status_hint("");
    }
    free(a);
}

/* The Doors shell is stopping or re-executing itself (app.h): the session
 * ends now, cleanly, rather than by an exec closing its PTY. */
static void terminal_shutdown(void)
{
    session_end();
}

/* ---- for the tests (terminal_app.h) --------------------------------------- */

const struct term_session *terminal_app_session(void *priv)
{
    (void)priv;
    return bg.live ? &bg.session : NULL;
}

bool terminal_app_layout_pending(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->layout_pending : false;
}

int terminal_app_view_back(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->view_back : 0;
}

lv_obj_t *terminal_app_grid(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->grid : NULL;
}

lv_obj_t *terminal_app_close_button(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->close_btn : NULL;
}

lv_obj_t *terminal_app_confirm_button(void *priv, bool close)
{
    struct terminal_app *a = priv;

    return a ? (close ? a->confirm_btn : a->cancel_btn) : NULL;
}

bool terminal_app_confirming(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->confirming : false;
}

const char *terminal_app_bar_text(void *priv)
{
    return priv ? lv_label_get_text(((struct terminal_app *)priv)->bar_caption) : NULL;
}

void terminal_app_cell(void *priv, int32_t *w, int32_t *h)
{
    struct terminal_app *a = priv;

    *w = a ? a->cell_w : 0;
    *h = a ? a->cell_h : 0;
}

unsigned terminal_app_sessions_started(void)
{
    return bg.opened;
}

bool terminal_app_timer_running(void)
{
    return bg.timer && !bg.paused;
}

void terminal_app_tick_now(void *priv)
{
    (void)priv;
    tick();
}

LV_IMAGE_DECLARE(pos_app_icon_terminal);

const struct pocketos_app app_terminal = {
    .id = "terminal",
    .name = "Terminal",
    /* The launcher draws the Doors icon (DS section 20); the glyph is the
     * text fallback. */
    .icon = LV_SYMBOL_KEYBOARD,
    .icon_mask = &pos_app_icon_terminal,
    .create = terminal_create,
    .tick = NULL,
    .destroy = terminal_destroy,
    /* No chrome or header declared: the shell's header and status cluster,
     * as every tool has, and the grid takes the whole body under them. */
    .back = terminal_back,
    .shutdown = terminal_shutdown,
};
