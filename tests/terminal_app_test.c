/*
 * Terminal in the running app (apps/terminal/terminal_app.c), under real
 * LVGL devices: keys pushed into the one input stream, as the physical
 * keyboard pushes them, reach a real /bin/sh on a real PTY, and what the
 * shell prints is on the grid the app draws.
 *
 * What only the app can get wrong is checked here: that Tab, Esc and a Ctrl
 * chord reach the shell instead of moving focus, cancelling or typing a
 * letter; that the modifiers never leak into an ordinary field; that the
 * grid fills the body in portrait and in landscape and follows the body when
 * it shrinks; that Shift+Up and a drag scroll back and any key comes back;
 * that a tap asks for the touch keyboard only when no keyboard is attached;
 * that the grid's cells follow Small, Medium and Large and the shell is told
 * its new size, with wrapping and cursor positioning in the new cells; that
 * the session outlives the screen - leaving keeps the shell and its jobs
 * running and their output read, reopening shows the same screen - and that
 * CLOSE SESSION (after its confirmation), an ended shell left behind, and
 * the Doors shell's own exit each leave no timer, no child, no descriptor
 * and no raw key target.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_styles.h"
#include "terminal_app.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30

extern const struct pocketos_app app_terminal;

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failed++;
    }
}

/* ---- the shell's side of app.h ------------------------------------------- */

static char g_hint[64];
static int g_keyboard_asked;
static int g_went_home;
static enum pocketos_keyboard g_keyboard = POCKETOS_KEYBOARD_ABSENT;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    g_keyboard_asked++;
}

void pocketos_shell_keyboard_hide(void)
{
}

int pocketos_shell_keyboard_visible(void)
{
    return 0;
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->keyboard = g_keyboard;
}

/* The real one closes the app; here the test does, after counting it. */
void pocketos_shell_go_home(void)
{
    g_went_home++;
}

/* ---- display, finger, time ----------------------------------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 4];
static lv_display_t *disp;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pump(int ms)
{
    int64_t end = mono_ms() + ms;
    int64_t last = mono_ms();

    do {
        struct timespec d = { 0, 5 * 1000000L };
        int64_t now;

        nanosleep(&d, NULL);
        now = mono_ms();
        lv_tick_inc((uint32_t)(now - last));
        last = now;
        lv_timer_handler();
    } while (mono_ms() < end);
}

static int count_timers(void)
{
    lv_timer_t *t = lv_timer_get_next(NULL);
    int n = 0;

    while (t) {
        n++;
        t = lv_timer_get_next(t);
    }
    return n;
}

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n - 1;
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/* ---- the screen ------------------------------------------------------------ */

static const struct term_session *sess(void)
{
    return terminal_app_session(app_priv);
}

/* A stored line holding text (or, with exact, that is text after any
 * prompts). */
static bool shows_(const char *text, bool exact)
{
    const struct term_session *s = sess();
    char line[TERM_MAX_COLS * 3 + 1];
    int back;
    int r;

    if (!s || s->phase == TERM_SESSION_OFF) {
        return false;
    }
    for (back = term_screen_scrollback(&s->screen); back >= 0; back--) {
        for (r = 0; r < (back ? 1 : s->screen.rows); r++) {
            const char *p = line;

            term_screen_row_text(&s->screen, back, r, line, sizeof(line));
            if (!exact) {
                if (strstr(line, text)) {
                    return true;
                }
                continue;
            }
            while (strncmp(p, "TPROMPT> ", 9) == 0) {
                p += 9;
            }
            if (strcmp(p, text) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool wait_for(const char *text, bool exact, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (shows_(text, exact)) {
            return true;
        }
        pump(10);
    }
    return shows_(text, exact);
}

/* ---- keys ------------------------------------------------------------------ */

static void push(pos_key_t k, unsigned mods)
{
    /* The stream holds 32; the device drains it on every handler pass. */
    while (pos_input_queued() > 24) {
        pump(5);
    }
    pos_input_push_key_mods(k, mods);
}

static void type(const char *text)
{
    for (; *text; text++) {
        push(*text == '\r' ? LV_KEY_ENTER : (pos_key_t)(unsigned char)*text, 0);
    }
    pump(20);
}

/* ---- hosting ----------------------------------------------------------------- */

static void use_display(enum pos_rotation rotation, int32_t reserve)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { PANEL_CORNER, PANEL_CORNER, PANEL_CORNER, PANEL_CORNER },
    };
    struct pos_display_geometry g;
    int32_t top;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    top = chrome_height(chrome_resolve(app_terminal.chrome, g.width > g.height, false));
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - top - reserve);
    lv_obj_set_pos(g_content, 0, top);
    pump(20);
}

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    lv_obj_update_layout(app_root);
    app_priv = app_terminal.create(app_body);
    pump(20);
}

/* Leaving the app: the screen goes, the session stays. */
static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_terminal.destroy(app_priv);
    t0 = mono_ms() - t0;
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
    return t0;
}

/* The Doors shell exiting: what is left of the session ends. */
static void shell_exit(void)
{
    app_terminal.shutdown();
    pump(20);
}

static void click(lv_obj_t *o)
{
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
}

/* ---- the journey --------------------------------------------------------------- */

static void journey(const char *name, bool landscape)
{
    char what[160];
    int timers0 = count_timers();
    int fds0 = count_fds();
    const struct term_session *s;
    lv_obj_t *grid;
    lv_area_t body;
    lv_area_t ga;
    pid_t sid;
    unsigned long seen;
    int64_t took;

#define CHECK(text, ok)                                          \
    do {                                                         \
        snprintf(what, sizeof(what), "%s: %s", name, text);      \
        check(what, ok);                                         \
    } while (0)

    app_start();
    grid = terminal_app_grid(app_priv);
    s = sess();
    CHECK("the app starts a shell", s && s->phase == TERM_SESSION_RUNNING);
    if (!s || s->phase != TERM_SESSION_RUNNING) {
        app_stop();
        shell_exit();
        return;
    }
    sid = s->pty.pid;
    CHECK("the grid has the focus", pos_input_focused() == grid);
    CHECK("and takes the keys raw", pos_input_raw_target() == grid);
    CHECK("a timer drives it", count_timers() == timers0 + 1);
    CHECK("the session row says leaving keeps it",
          strcmp(terminal_app_bar_text(app_priv), "KEPT WHEN YOU LEAVE") == 0);

    lv_obj_get_coords(app_body, &body);
    lv_obj_get_coords(grid, &ga);
    printf("note %s: body %dx%d, grid %d cols x %d rows\n", name, (int)lv_area_get_width(&body),
           (int)lv_area_get_height(&body), s->screen.cols, s->screen.rows);
    CHECK("the grid stays inside the body", ga.x1 >= body.x1 && ga.x2 <= body.x2 && ga.y1 >= body.y1 &&
                                                ga.y2 <= body.y2);
    CHECK("inside the corner clearance",
          pos_display_rect_is_safe(pocketui_display_geometry(), ga.x1, ga.y1, ga.x2, ga.y2));
    {
        lv_area_t ba;

        lv_obj_get_coords(terminal_app_close_button(app_priv), &ba);
        CHECK("CLOSE SESSION sits under the grid, inside the body",
              ba.y1 > ga.y2 && ba.y2 <= body.y2 && ba.x2 <= body.x2 &&
                  pos_display_rect_is_safe(pocketui_display_geometry(), ba.x1, ba.y1, ba.x2, ba.y2));
    }
    if (landscape) {
        CHECK("landscape: over 120 columns", s->screen.cols > 120);
        CHECK("landscape: over 12 rows", s->screen.rows > 12);
    } else {
        CHECK("portrait: over 55 columns", s->screen.cols > 55);
        CHECK("portrait: over 45 rows", s->screen.rows > 45);
    }
    {
        /* At Small the cells are the 8 x 18 px the grid always had. */
        int32_t cw;
        int32_t ch;
        int32_t gw = lv_area_get_width(&ga);

        terminal_app_cell(app_priv, &cw, &ch);
        CHECK("Small: the grid's cells are 8 x 18 px", cw == 8 && ch == 18);
        CHECK("and the columns fill the grid's width",
              s->screen.cols * cw <= gw && gw < (s->screen.cols + 1) * cw + 2 * POCKETUI_PAD);
    }
    CHECK("a prompt appears", wait_for("TPROMPT>", false, 5000));

    type("echo out-$((6*7))\r");
    CHECK("a typed command runs and its output is on the grid", wait_for("out-42", true, 3000));

    type("echo abX");
    push(LV_KEY_BACKSPACE, 0);
    type("c\r");
    CHECK("Backspace from the keyboard erases", wait_for("abc", true, 3000));

    type("od -c\r");
    pump(200);
    push(LV_KEY_UP, 0);
    push(LV_KEY_ESC, 0);
    push(LV_KEY_NEXT, 0);
    push(LV_KEY_ENTER, 0);
    push('d', POS_INPUT_MOD_CTRL);
    CHECK("an arrow, Esc and Tab reach the program", wait_for("033   [   A 033  \\t  \\n", false, 3000));
    CHECK("and Tab did not move the focus", pos_input_focused() == grid);

    type("od -c\r");
    pump(200);
    push(POS_KEY_F(5), 0);
    push(LV_KEY_ENTER, 0);
    push('d', POS_INPUT_MOD_CTRL);
    CHECK("Fn+F5 reaches the program as the VT key (ESC [ 1 5 ~)",
          wait_for("033   [   1   5   ~  \\n", false, 3000));

    type("sleep 20\r");
    pump(300);
    push('c', POS_INPUT_MOD_CTRL);
    type("echo rc=$?\r");
    CHECK("Ctrl+C from the keyboard interrupts", wait_for("rc=130", true, 3000));

    type("echo plain-");
    push('c', 0);
    type("\r");
    CHECK("the same key without Ctrl types the letter", wait_for("plain-c", true, 3000));

    /* The scrollback, by key and by finger. */
    type("seq 1 400\r");
    CHECK("output longer than the screen", wait_for("400", true, 3000));
    pump(100);
    push(LV_KEY_UP, POS_INPUT_MOD_SHIFT);
    pump(50);
    CHECK("Shift+Up looks back", terminal_app_view_back(app_priv) > 0);
    CHECK("and says so in the header", strcmp(g_hint, "SCROLLBACK") == 0);
    push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
    push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
    pump(50);
    CHECK("Shift+Down comes back", terminal_app_view_back(app_priv) == 0);
    CHECK("and the hint goes", g_hint[0] == '\0');
    {
        int asked = g_keyboard_asked;
        int32_t x = (ga.x1 + ga.x2) / 2;
        int32_t y = (ga.y1 + ga.y2) / 2;
        int i;

        finger_point.x = x;
        finger_point.y = y - 60;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(40);
        for (i = 1; i <= 10; i++) {
            finger_point.y = y - 60 + i * 20;
            pump(20);
        }
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(40);
        CHECK("a drag down looks back", terminal_app_view_back(app_priv) > 0);
        CHECK("a drag does not ask for the touch keyboard", g_keyboard_asked == asked);
        push(LV_KEY_BACKSPACE, 0);
        pump(50);
        CHECK("any key returns to the live screen", terminal_app_view_back(app_priv) == 0);

        finger_point.x = x;
        finger_point.y = y;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(40);
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(40);
        CHECK("a tap asks for the touch keyboard when none is attached", g_keyboard_asked == asked + 1);
        g_keyboard = POCKETOS_KEYBOARD_PRESENT;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(40);
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(40);
        g_keyboard = POCKETOS_KEYBOARD_ABSENT;
        CHECK("and not when the keyboard base is", g_keyboard_asked == asked + 1);
    }

    /* The body shrinks (the touch keyboard came up): the shell is told. */
    {
        int rows = s->screen.rows;

        lv_obj_set_height(g_content, lv_obj_get_height(g_content) - 300);
        pump(150);
        CHECK("a smaller body gives fewer rows", s->screen.rows < rows && s->screen.rows >= TERM_MIN_ROWS);
        {
            char want[32];

            snprintf(want, sizeof(want), "%d %d", s->screen.rows, s->screen.cols);
            type("stty size\r");
            CHECK("and the shell knows it", wait_for(want, true, 3000));
        }
        lv_obj_set_height(g_content, lv_obj_get_height(g_content) + 300);
        pump(150);
        CHECK("and the rows come back with the room", s->screen.rows == rows);
    }

    /* A draw of the whole grid with text on it. */
    lv_obj_invalidate(grid);
    lv_refr_now(NULL);
    CHECK("the grid draws", true);

    /* Leave with output flooding the screen: the screen goes at once, the
     * flood does not, and its output is still read while nobody looks. */
    type("yes flood\r");
    pump(200);
    took = app_stop();
    printf("note %s: leaving during a flood took %lld ms\n", name, (long long)took);
    CHECK("leaving returns at once", took <= 50);
    CHECK("the session is still there", sess() == s && s->phase == TERM_SESSION_RUNNING && s->pty.pid == sid);
    CHECK("its timer still runs", count_timers() == timers0 + 1 && terminal_app_timer_running());
    CHECK("no raw key target is left", pos_input_raw_target() == NULL);
    CHECK("the hint was cleared", g_hint[0] == '\0');
    seen = s->bytes_in;
    {
        int64_t t0 = mono_ms();
        unsigned long cap;

        pump(300);
        /* At most one detached budget per tick of the time that passed. */
        cap = (unsigned long)((mono_ms() - t0) / TERMINAL_TICK_MS + 2) * 4096;
        printf("note %s: %lu bytes taken in 300 ms with no screen (cap %lu)\n", name, s->bytes_in - seen, cap);
        CHECK("the flood's output is still taken with no screen", s->bytes_in > seen + 10000);
        CHECK("but at the detached budget, not the screen's", s->bytes_in - seen <= cap);
    }

    /* Back again: the same shell, still flooding; Ctrl+C reaches it. */
    app_start();
    grid = terminal_app_grid(app_priv);
    CHECK("reopening shows the same session", sess() == s && s->pty.pid == sid);
    CHECK("the grid has the focus and the keys again",
          pos_input_focused() == grid && pos_input_raw_target() == grid);
    push('c', POS_INPUT_MOD_CTRL);
    pump(300);
    type("echo after-$((7*6))\r");
    CHECK("Ctrl+C stops the flood it left running", wait_for("after-42", true, 3000));

    /* CLOSE SESSION: asked first, Esc is Cancel, then it ends everything. */
    click(terminal_app_close_button(app_priv));
    pump(80);
    CHECK("CLOSE SESSION asks first", terminal_app_confirming(app_priv));
    CHECK("the confirmation has the focus, on Cancel",
          pos_input_focused() == terminal_app_confirm_button(app_priv, false));
    CHECK("and the grid does not take the keys", !pos_input_raw_focused());
    CHECK("nothing ended on opening it", sess() == s && s->phase == TERM_SESSION_RUNNING);
    push(LV_KEY_ESC, 0);
    pump(80);
    CHECK("Esc is Cancel", !terminal_app_confirming(app_priv) && sess() == s);
    CHECK("the grid has the focus and the keys back", pos_input_focused() == grid && pos_input_raw_focused());
    type("echo still-$((5*5))\r");
    CHECK("and the shell still answers", wait_for("still-25", true, 3000));
    type("sleep 1000 &\r");
    pump(100);
    click(terminal_app_close_button(app_priv));
    pump(80);
    {
        int home = g_went_home;
        int64_t t0 = mono_ms();

        click(terminal_app_confirm_button(app_priv, true));
        took = mono_ms() - t0;
        printf("note %s: CLOSE SESSION took %lld ms\n", name, (long long)took);
        CHECK("confirming ends it within the close bound",
              took <= TERM_PTY_HUP_GRACE_MS + TERM_PTY_KILL_REAP_MS + 100);
        CHECK("the session is gone", sess() == NULL);
        CHECK("the shell and its job are gone", term_pty_session_count(sid) == 0);
        CHECK("reaped", no_child());
        pump(50);
        CHECK("and the app is left", g_went_home == home + 1);
    }
    app_stop();
    CHECK("no timer is left", count_timers() == timers0);
    CHECK("no descriptor is left", count_fds() == fds0);
    CHECK("no raw key target is left", pos_input_raw_target() == NULL);
#undef CHECK
}

/* ---- text size ------------------------------------------------------------ */

/* Text on screen row r, column c, of the live screen. */
static char cell_at(int r, int c)
{
    const struct term_session *s = sess();
    char line[TERM_MAX_COLS * 3 + 1];

    if (!s) {
        return 0;
    }
    term_screen_row_text(&s->screen, 0, r, line, sizeof(line));
    return (int)strlen(line) > c ? line[c] : ' ';
}

static void text_sizes(const char *name)
{
    static const int px[POS_TEXT_SIZE_COUNT] = { 14, 17, 20 };
    static const char *size_name[POS_TEXT_SIZE_COUNT] = { "Small", "Medium", "Large" };
    char what[160];
    int last_cols = 1 << 20;
    int last_rows = 1 << 20;
    const struct term_session *s;
    pid_t sid;
    int z;

    app_start();
    s = sess();
    if (!s) {
        check("text size: a session", false);
        app_stop();
        return;
    }
    sid = s->pty.pid;
    wait_for("TPROMPT>", false, 5000);
    type("seq 1 200\r");
    wait_for("200", true, 3000);
    for (z = 0; z < POS_TEXT_SIZE_COUNT; z++) {
        struct pos_type_spec spec = { POS_FACE_MONO, px[z] };
        const lv_font_t *f = pos_type_font(spec);
        int32_t cw;
        int32_t ch;
        lv_area_t ga;
        char want[64];

#define CHECK(text, ok)                                                          \
    do {                                                                         \
        snprintf(what, sizeof(what), "%s %s: %s", name, size_name[z], text);     \
        check(what, ok);                                                         \
    } while (0)

        pos_theme_select_text_size((enum pos_text_size)z);
        pump(150);
        terminal_app_cell(app_priv, &cw, &ch);
        lv_obj_get_coords(terminal_app_grid(app_priv), &ga);
        printf("note %s %s: mono %d px, cell %dx%d, grid %dx%d px, %d cols x %d rows\n", name, size_name[z],
               px[z], (int)cw, (int)ch, (int)lv_area_get_width(&ga), (int)lv_area_get_height(&ga),
               s->screen.cols, s->screen.rows);
        CHECK("the cells are the Terminal's face at this size",
              f && cw == lv_font_get_glyph_width(f, 'M', 0) && ch == lv_font_get_line_height(f));
        CHECK("the columns and rows fill the grid in these cells",
              s->screen.cols == (int)((lv_area_get_width(&ga) - 8) / cw) &&
                  s->screen.rows == (int)((lv_area_get_height(&ga) - 8) / ch));
        CHECK("a larger size gives fewer columns and rows", z == 0 || (s->screen.cols < last_cols &&
                                                                       s->screen.rows < last_rows));
        CHECK("the same shell, not a new one", sess() == s && s->pty.pid == sid);
        last_cols = s->screen.cols;
        last_rows = s->screen.rows;

        snprintf(want, sizeof(want), "%d %d", s->screen.rows, s->screen.cols);
        type("stty size\r");
        CHECK("the shell was told its new size", wait_for(want, true, 3000));

        /* A line one longer than the screen wraps at the new width. */
        type("c=$(stty size | cut -d' ' -f2); printf \"%0${c}dW\\n\" 0\r");
        {
            char zeros[TERM_MAX_COLS + 1];

            memset(zeros, '0', (size_t)s->screen.cols);
            zeros[s->screen.cols] = '\0';
            CHECK("a long line wraps at the new width", wait_for("W", true, 3000) && shows_(zeros, true));
        }

        /* Absolute positioning lands in the new cells. */
        type("clear; printf '\\033[5;10HZ\\033[2;3HQ'; sleep 1\r");
        pump(600);
        CHECK("cursor positioning puts text on its row and column", cell_at(4, 9) == 'Z' && cell_at(1, 2) == 'Q');
        pump(600);

        type("seq 1 300\r");
        wait_for("300", true, 3000);
        pump(100);
        push(LV_KEY_UP, POS_INPUT_MOD_SHIFT);
        pump(50);
        CHECK("the scrollback works in these cells", terminal_app_view_back(app_priv) > 0);
        push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
        push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
        push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
        pump(50);
        CHECK("and comes back live", terminal_app_view_back(app_priv) == 0);

        lv_obj_invalidate(terminal_app_grid(app_priv));
        lv_refr_now(NULL);
#undef CHECK
    }

    /* A size chosen while the app is not open is the one it opens in. */
    app_stop();
    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    pos_theme_select_text_size(POS_TEXT_SIZE_MEDIUM);
    pump(50);
    app_start();
    {
        int32_t cw;
        int32_t ch;
        struct pos_type_spec spec = { POS_FACE_MONO, px[POS_TEXT_SIZE_MEDIUM] };

        terminal_app_cell(app_priv, &cw, &ch);
        snprintf(what, sizeof(what), "%s: reopened after a size change while away, it is in that size", name);
        check(what, cw == lv_font_get_glyph_width(pos_type_font(spec), 'M', 0) && sess() == s &&
                        s->pty.pid == sid);
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    pump(100);
    app_stop();
    shell_exit();
}

/* ---- the session across leaving --------------------------------------------- */

static void persistence(void)
{
    int timers0 = count_timers();
    int fds0 = count_fds();
    unsigned started;
    const struct term_session *s;
    pid_t sid;
    int fds_attached;
    int i;
    bool same = true;

    app_start();
    s = sess();
    started = terminal_app_sessions_started();
    if (!s) {
        check("persistence: a session", false);
        app_stop();
        return;
    }
    sid = s->pty.pid;
    wait_for("TPROMPT>", false, 5000);
    type("echo first-$((3*3))\r");
    wait_for("first-9", true, 3000);
    fds_attached = count_fds();

    /* A long command, then the app is left while it runs. */
    type("for i in $(seq 1 80); do echo tick-$i; sleep 0.03; done; echo done-$((8*8))\r");
    pump(200);
    app_stop();
    check("leave: the shell keeps running", sess() == s && s->phase == TERM_SESSION_RUNNING &&
                                                term_pty_session_count(sid) > 0);
    pump(4000); /* another app in use */
    check("leave: the command ran to its end with no screen", shows_("done-64", true) && shows_("tick-80", true));
    check("leave: every line of it is kept", shows_("tick-1", true) && shows_("tick-20", true));

    app_start();
    check("reopen: the same session, not a new one",
          sess() == s && s->pty.pid == sid && terminal_app_sessions_started() == started);
    check("reopen: what it wrote meanwhile is on the screen", shows_("done-64", true));
    check("reopen: and what came before is in the history", shows_("first-9", true));
    type("echo back-$((2+3))\r");
    check("reopen: it takes input", wait_for("back-5", true, 3000));
    push(LV_KEY_UP, POS_INPUT_MOD_SHIFT);
    pump(50);
    check("reopen: the scrollback scrolls", terminal_app_view_back(app_priv) > 0);
    push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
    push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
    push(LV_KEY_DOWN, POS_INPUT_MOD_SHIFT);
    pump(50);

    /* Leaving and coming back many times costs nothing and changes nothing. */
    type("sleep 1000 &\r");
    pump(100);
    for (i = 0; i < 10; i++) {
        app_stop();
        pump(40);
        app_start();
        same = same && sess() == s && s->pty.pid == sid;
    }
    check("10 leaves and returns: the same shell every time", same && terminal_app_sessions_started() == started);
    check("and no descriptor more than the first open had", count_fds() == fds_attached);
    check("and one timer", count_timers() == timers0 + 1);
    type("jobs\r");
    check("and its background job is still there", wait_for("sleep 1000", false, 3000));

    /* The shell exits by itself while nobody looks. */
    type("sleep 1; exit 3\r");
    pump(100);
    app_stop();
    pump(2000);
    check("shell exits away: the end is read and said", s->phase == TERM_SESSION_ENDED &&
                                                            shows_("status 3", false));
    check("shell exits away: its session is cleared", term_pty_session_count(sid) == 0);
    check("shell exits away: the timer rests", !terminal_app_timer_running());
    app_start();
    check("shell exits away: reopening shows how it ended", sess() == s && strcmp(g_hint, "ENDED") == 0 &&
                                                                strcmp(terminal_app_bar_text(app_priv),
                                                                       "SHELL ENDED") == 0);
    check("shell exits away: the timer runs again with a screen", terminal_app_timer_running());
    push(LV_KEY_ENTER, 0);
    pump(100);
    check("Enter starts a new shell in the same screen", s->phase == TERM_SESSION_RUNNING && s->pty.pid != sid);
    sid = s->pty.pid;
    type("echo new-$((4*4))\r");
    check("and it answers", wait_for("new-16", true, 3000));

    /* Ended while open, then left: nothing is kept for the next open. */
    type("exit\r");
    pump(800);
    check("exit: the screen says it ended", s->phase == TERM_SESSION_ENDED);
    app_stop();
    check("leaving an ended shell lets it go", sess() == NULL);
    check("no timer left", count_timers() == timers0);
    check("no descriptor left", count_fds() == fds0);
    check("no child left", no_child());
    app_start();
    check("the next open is a fresh shell", sess() && terminal_app_sessions_started() == started + 1 &&
                                                !shows_("new-16", true));
    wait_for("TPROMPT>", false, 5000);

    /* Back out of the confirmation, as its Cancel. */
    click(terminal_app_close_button(app_priv));
    pump(80);
    check("Back closes the confirmation", terminal_app_confirming(app_priv) && app_terminal.back(app_priv) == 1);
    pump(80);
    check("and the session is untouched", !terminal_app_confirming(app_priv) && sess() &&
                                              sess()->phase == TERM_SESSION_RUNNING);
    check("Back with no confirmation up is the back slab's", app_terminal.back(app_priv) == 0);

    /* The Doors shell exits (a stop, or a rotation's exec) with the
     * session running in the background. */
    type("sleep 1000 &\r");
    pump(100);
    sid = sess()->pty.pid;
    app_stop();
    shell_exit();
    check("the shell's exit ends the session", sess() == NULL && term_pty_session_count(sid) == 0);
    check("shell exit: reaped", no_child());
    check("shell exit: no timer", count_timers() == timers0);
    check("shell exit: no descriptor", count_fds() == fds0);
}

static void churn(void)
{
    int timers0 = count_timers();
    int fds0 = count_fds();
    int i;

    for (i = 0; i < 12; i++) {
        app_start();
        if (i % 4 == 1) {
            wait_for("TPROMPT>", false, 3000);
            type("sleep 100 &\r");
        } else if (i % 4 == 2) {
            wait_for("TPROMPT>", false, 3000);
            type("yes\r");
            pump(60);
        } else if (i % 4 == 3) {
            /* CLOSE SESSION through its confirmation. */
            click(terminal_app_close_button(app_priv));
            pump(60);
            click(terminal_app_confirm_button(app_priv, true));
            pump(20);
        }
        app_stop();
    }
    shell_exit();
    check("12 opens and leaves, with a job, a flood, a close: no timer left", count_timers() == timers0);
    check("no descriptor left", count_fds() == fds0);
    check("no child left", no_child());
}

/* Created in a body that has no size yet: the layout stays owed until the
 * grid can be measured, and is done by a tick even when no size change is
 * ever reported to the app. */
static void unsized_start(void)
{
    const struct term_session *s;
    lv_obj_t *frame;
    int32_t w;
    int32_t h;

    lv_obj_update_layout(g_content); /* the size use_display() set, not the last one drawn */
    w = lv_obj_get_width(g_content);
    h = lv_obj_get_height(g_content);
    lv_obj_set_height(g_content, POCKETUI_HEADER_H);
    pump(20);
    app_start();
    s = sess();
    check("unsized: the app starts a shell at a stand-in size", s && s->phase == TERM_SESSION_RUNNING &&
                                                                 s->screen.cols == 80 && s->screen.rows == 24);
    check("unsized: the layout is owed", terminal_app_layout_pending(app_priv));
    terminal_app_tick_now(app_priv);
    check("unsized: a tick that cannot measure keeps it owed", terminal_app_layout_pending(app_priv));

    /* The frame's size-change callback is the only other way a layout is
     * asked for; without it, only the owed layout can size the grid. */
    frame = lv_obj_get_parent(terminal_app_grid(app_priv));
    lv_obj_remove_event_cb_with_user_data(frame, NULL, app_priv);
    lv_obj_set_size(g_content, w, h);
    lv_obj_update_layout(g_content);
    terminal_app_tick_now(app_priv);
    {
        lv_area_t b;
        lv_area_t g;

        lv_obj_get_coords(app_body, &b);
        lv_obj_get_coords(terminal_app_grid(app_priv), &g);
        printf("note unsized, then sized: body %dx%d, grid %dx%d: %d cols x %d rows\n",
               (int)lv_area_get_width(&b), (int)lv_area_get_height(&b), (int)lv_area_get_width(&g),
               (int)lv_area_get_height(&g), s->screen.cols, s->screen.rows);
    }
    check("unsized: once there is room, the next tick lays it out and sizes the shell",
          !terminal_app_layout_pending(app_priv) && s->screen.cols > 55 && s->screen.rows > 45);
    app_stop();
    shell_exit();
    check("unsized: no child left", no_child());
}

/* The modifiers belong to the raw target only: an ordinary field still
 * gets the letter, and Tab still moves focus. */
static void ordinary_field(void)
{
    lv_obj_t *ta = lv_textarea_create(lv_screen_active());
    lv_obj_t *other = lv_button_create(lv_screen_active());

    pos_input_add_obj(ta);
    pos_input_add_obj(other);
    pos_input_focus(ta);
    pump(20);
    pos_input_push_key_mods('c', POS_INPUT_MOD_CTRL);
    pos_input_push_key_mods('d', POS_INPUT_MOD_CTRL | POS_INPUT_MOD_ALT);
    pump(50);
    check("a field gets Ctrl+C as the letter, as before", strcmp(lv_textarea_get_text(ta), "cd") == 0);
    pos_input_push_key_mods(LV_KEY_NEXT, 0);
    pump(50);
    check("and Tab still moves the focus", pos_input_focused() == other);
    lv_obj_delete(ta);
    lv_obj_delete(other);
}

int main(void)
{
    char home[64];
    lv_indev_t *finger;

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(home, sizeof(home), "/tmp/terminal-app-%ld", (long)getpid());
    mkdir(home, 0700);
    setenv("HOME", home, 1);
    setenv("POCKETOS_TERMINAL_SHELL", "/bin/sh", 1);
    setenv("PS1", "TPROMPT> ", 1);
    unsetenv("ENV");

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);

    use_display(POS_ROTATION_0, 0);
    journey("portrait", false);
    use_display(POS_ROTATION_90, 0);
    journey("landscape", true);
    use_display(POS_ROTATION_0, 0);
    text_sizes("portrait");
    use_display(POS_ROTATION_90, 0);
    text_sizes("landscape");
    use_display(POS_ROTATION_0, 0);
    persistence();
    churn();
    unsized_start();
    ordinary_field();
    check("no child at the end", no_child());
    rmdir(home);
    printf("terminal_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
