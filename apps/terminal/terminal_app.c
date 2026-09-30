/*
 * Terminal: a real shell on the device, on a pseudo-terminal.
 *
 * One screen: a monospace grid filling the app's body, inside the corner
 * clearance, under the shell's own header and status cluster. The grid is
 * the screen of term_screen.c; the shell behind it is the user's login shell
 * (the one /etc/passwd names for the account Doors runs under) on a PTY
 * (term_pty.c), and term_session.c moves bytes between them.
 *
 * The keys. The grid is the focused object and the input stream's raw key
 * target (pos_input.h), so every key the physical keyboard sends reaches the
 * shell - Tab, Esc and the Ctrl chords included, which anywhere else move
 * focus, cancel or type a letter. Without a keyboard, a tap on the grid
 * brings up the shell's touch keyboard. Shift+Up and Shift+Down, or a drag,
 * move the view through the scrollback; any other key brings it back to the
 * live screen and goes to the shell.
 *
 * Time. One LVGL timer every TERMINAL_TICK_MS takes a bounded slice of
 * output (TERM_PUMP_BUDGET), invalidates what changed and nothing else, and
 * makes only non-blocking calls. A screen that scrolls as a whole is redrawn
 * at most every FULL_REDRAW_MS however fast the output comes. The one wait
 * on the LVGL thread is destroy() clearing the shell's session, bounded by
 * TERM_PTY_HUP_GRACE_MS + TERM_PTY_KILL_REAP_MS and normally a few ms.
 *
 * Leaving the app ends the shell and everything it started in its session:
 * there is no background terminal. A rotation restarts the Doors shell, so
 * it ends the terminal's shell too (docs/apps/TERMINAL.md).
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

struct terminal_app {
    struct term_session session;
    bool session_ok;

    lv_obj_t *frame;
    lv_obj_t *grid;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    bool layout_pending;

    const lv_font_t *font;
    int32_t cell_w, cell_h;

    int view_back; /* lines above the live screen; 0 = live */
    unsigned seen_scrolled;
    int drawn_back;
    int drawn_cx, drawn_cy;
    bool drawn_cursor;
    int64_t last_full_ms;

    int32_t drag_accum;
    int32_t drag_travel;

    const char *hint;
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
    int test_fd;
    char test_buf[1024];
    size_t test_len;
#endif
};

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

/* The caption role's font: the Design System's monospace face. Its letter
 * spacing is not taken - a grid needs every cell the glyph's own width. */
static const lv_font_t *mono_font(void)
{
    lv_style_value_t v;

    if (lv_style_get_prop(pos_style(POS_STYLE_CAPTION), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND &&
        v.ptr) {
        return v.ptr;
    }
    return LV_FONT_DEFAULT;
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
    return a->view_back == 0 && a->session.phase == TERM_SESSION_RUNNING && a->session.screen.cursor_visible;
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

    if (!a || !layer || !a->session_ok || a->session.phase == TERM_SESSION_OFF) {
        return;
    }
    s = &a->session.screen;
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

    if (r < 0 || r >= a->session.screen.rows) {
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

    if (a->session.phase == TERM_SESSION_ENDED) {
        hint = "ENDED";
    } else if (a->view_back > 0) {
        hint = "SCROLLBACK";
    }
    if (hint != a->hint) {
        pocketos_shell_set_status_hint(hint ? hint : "");
        a->hint = hint;
    }
}

static void refresh(struct terminal_app *a, int64_t now)
{
    struct term_screen *s = &a->session.screen;
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

static void layout(struct terminal_app *a)
{
    struct pos_insets in;
    lv_area_t c;
    int cols;
    int rows;

    a->layout_pending = false;
    if (pocketui_layout_begin(&a->guard, a->frame, &in)) {
        lv_obj_set_style_pad_left(a->frame, in.left, 0);
        lv_obj_set_style_pad_top(a->frame, in.top, 0);
        lv_obj_set_style_pad_right(a->frame, in.right, 0);
        lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
        lv_obj_update_layout(a->frame);
    }
    lv_obj_get_coords(a->grid, &c);
    cols = (int)((lv_area_get_width(&c) - 2 * GRID_PAD) / a->cell_w);
    rows = (int)((lv_area_get_height(&c) - 2 * GRID_PAD) / a->cell_h);
    if (cols < TERM_MIN_COLS || rows < TERM_MIN_ROWS) {
        return; /* not laid out yet */
    }
    if (a->session_ok && (cols != a->session.screen.cols || rows != a->session.screen.rows)) {
        term_session_resize(&a->session, cols, rows);
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

/* ---- keys from a test --------------------------------------------------- *
 *
 * Simulator and gate builds only (POCKETOS_SHELL_TEST_HOOKS, never the image's
 * shell): the keyboard base cannot be pressed over SSH, so a gate or a
 * shell test writes keys to the FIFO named by POCKETOS_TEST_TERMINAL_KEYS and
 * they are pushed into the input stream exactly as the physical keyboard's
 * driver pushes them - key and modifiers - to reach the grid through the raw
 * key path. A byte is a character, a newline is Enter, and {NAME} is a key:
 * TAB ESC BS ENTER UP DOWN LEFT RIGHT LBRACE, or a character, each with any
 * of the prefixes C- (Ctrl), M- (Alt) and S- (Shift): {C-c}, {S-UP}. */
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

/* ---- the timer ----------------------------------------------------------- */

static void tick(struct terminal_app *a)
{
    int64_t now = term_pty_now_ms();

    if (!a->session_ok) {
        return;
    }
    test_keys_poll(a);
    if (a->layout_pending) {
        layout(a);
    }
    term_session_pump(&a->session, TERM_PUMP_BUDGET, now);
    refresh(a, now);
}

static void on_tick(lv_timer_t *t)
{
    tick(lv_timer_get_user_data(t));
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
    default: return key >= 0x20 ? TERM_KEY_CHAR : TERM_KEY_NONE;
    }
}

static void scroll_view(struct terminal_app *a, int lines)
{
    int max = term_screen_scrollback(&a->session.screen);
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

    if (!a || !param || !a->session_ok) {
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
        int half = a->session.screen.rows / 2 > 0 ? a->session.screen.rows / 2 : 1;

        scroll_view(a, k.kind == TERM_KEY_UP ? half : -half);
        return;
    }
    if (a->view_back != 0) {
        scroll_view(a, -a->view_back);
    }
    term_session_key(&a->session, &k);
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

    if (!indev || !a->session_ok) {
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

/* ---- the app ------------------------------------------------------------- */

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

static void *terminal_create(lv_obj_t *root)
{
    struct terminal_app *a = calloc(1, sizeof(*a));
    char path[256];
    char home[256];
    bool login = false;
    lv_area_t c;
    int cols;
    int rows;

    if (!a) {
        return NULL;
    }
    a->font = mono_font();
    a->cell_h = lv_font_get_line_height(a->font);
    a->cell_w = lv_font_get_glyph_width(a->font, 'M', 0);
    if (a->cell_w <= 0) {
        a->cell_w = 8;
    }
    if (a->cell_h <= 0) {
        a->cell_h = 18;
    }

    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
    a->grid = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->grid);
    lv_obj_set_size(a->grid, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(a->grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                   LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(a->grid, LV_OBJ_FLAG_CLICKABLE);
    pos_theme_watch(a->grid);

    lv_obj_update_layout(a->frame);
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
    lv_obj_get_coords(a->grid, &c);
    cols = (int)((lv_area_get_width(&c) - 2 * GRID_PAD) / a->cell_w);
    rows = (int)((lv_area_get_height(&c) - 2 * GRID_PAD) / a->cell_h);
    if (cols < TERM_MIN_COLS || rows < TERM_MIN_ROWS) {
        /* Not sized yet; the first tick lays it out and resizes. */
        cols = 80;
        rows = 24;
        a->layout_pending = true;
    }

    pick_shell(path, sizeof(path), &login, home, sizeof(home));
    a->session_ok = term_session_open(&a->session, path, login, home, cols, rows) == 0;

    lv_obj_add_event_cb(a->grid, grid_draw, LV_EVENT_DRAW_MAIN, a);
    lv_obj_add_event_cb(a->grid, on_key, LV_EVENT_KEY, a);
    lv_obj_add_event_cb(a->grid, on_pressed, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->grid, on_pressing, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->grid, on_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);

    pos_input_add_obj(a->grid);
    pos_input_focus(a->grid);
    pos_input_set_raw_target(a->grid);

    a->drawn_cy = -1;
    test_keys_open(a);
    a->timer = lv_timer_create(on_tick, TERMINAL_TICK_MS, a);
    lv_obj_invalidate(a->grid);
    return a;
}

static void terminal_destroy(void *priv)
{
    struct terminal_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->grid, NULL, a);
    lv_obj_remove_event_cb_with_user_data(a->frame, NULL, a);
    if (pos_input_raw_target() == a->grid) {
        pos_input_set_raw_target(NULL);
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    test_keys_close(a);
    /* The shell and everything in its session end here: no terminal runs
     * without its screen. */
    term_session_close(&a->session);
    if (a->hint) {
        pocketos_shell_set_status_hint("");
    }
    free(a);
}

const struct term_session *terminal_app_session(void *priv)
{
    return priv ? &((struct terminal_app *)priv)->session : NULL;
}

int terminal_app_view_back(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->view_back : 0;
}

lv_obj_t *terminal_app_grid(void *priv)
{
    return priv ? ((struct terminal_app *)priv)->grid : NULL;
}

void terminal_app_tick_now(void *priv)
{
    if (priv) {
        tick(priv);
    }
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
};
