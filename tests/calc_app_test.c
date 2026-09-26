/*
 * PocketCalculator in the running app, driven by a real LVGL pointer device
 * and the real logical key stream.
 *
 * The engine and view tests prove the arithmetic and the key map. This one
 * proves what they cannot: that a finger on a key reaches them, that a key
 * pushed into pos_input arrives at the display and not somewhere else, that
 * tapping does not steal focus from typing, that every key is a real target
 * on the glass in all three display modes, in portrait and in landscape and
 * clear of the panel's rounded corners, and that nothing is written or asked
 * of the shell while it all happens. It exists so that a board is the first
 * HARDWARE test of the calculator and not the first test of it at all.
 *
 * The shell is not here, so this file plays its part: it hosts the app the
 * way ui/shell/shell.c does (header, padded body, the display geometry set
 * before anything is built), and it defines every app.h entry point as a
 * counter. The calculator needs none of them, and the counters say whether
 * that stays true.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/calculator_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "calc_view.h"
#include "pocketui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The reference panel in its native portrait, and the corner squares the
 * shell describes it with on the T-Display K230 (DS section 21.1). */
#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
/* Where the shell's content area starts for this app in the display's
 * orientation (ui/shell/chrome.h, DS sections 30 and 36), so the frame built
 * here is the one shell.c builds: the top edge, since no chrome reserves a
 * row there any more. */
#define STATUS_H chrome_height(chrome_resolve(app_calculator.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
/* Rows measured under the retired bars - 56 px in portrait (v0.0.9/10), 32
 * in landscape (DS section 30) - moved up with the frame's top. */
#define V010_ROW(y) ((y) - 56 + STATUS_H)
#define V010_LROW(y) ((y) - 32 + STATUS_H)

#define MINUS "\xE2\x88\x92"
#define TIMES "\xC3\x97"
#define DIVIDE "\xC3\xB7"
#define PLUS_MINUS "\xC2\xB1"

/* The app's own layout under the body, as calc_app.c builds it: one frame,
 * and in it the display and the keypad. */
#define KID_FRAME 0
#define KID_DISPLAY 0
#define KID_PAD 1
#define DISPLAY_EXPRESSION 0
#define DISPLAY_NUMBER 1

extern const struct pocketos_app app_calculator;

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

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

/* ---- the shell's side of app.h, as counters ----------------------------- */

static int shell_calls;
static int keyboard_requests;

void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
    shell_calls++;
}
void pocketos_shell_go_home(void) { shell_calls++; }
int pocketos_shell_reduced_motion(void)
{
    shell_calls++;
    return 0;
}
int64_t pocketos_shell_system_day(void)
{
    shell_calls++;
    return -1;
}
const char *pocketos_shell_radio_state(void)
{
    shell_calls++;
    return NULL;
}
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    keyboard_requests++;
}
void pocketos_shell_keyboard_hide(void) { keyboard_requests++; }
int pocketos_shell_keyboard_visible(void)
{
    shell_calls++;
    return 0;
}

/* ---- display and finger ------------------------------------------------ */

/* Forty rows of the long side, at up to four bytes a pixel. */
static uint8_t draw_buf[PANEL_H * 40 * 4];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_display_t *disp;

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

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

/* Until every pushed key has been delivered: the keypad device reads on its
 * own period, so a key is not there the moment it is pushed. */
static void drain(void)
{
    int t;

    for (t = 0; t < 1000 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

/* The display the shell would open: the reference panel with corner squares
 * of `corner` px, turned to `rotation`, the geometry handed to PocketUI and
 * the content area below the status bar sized to it (shell.c). Called with
 * the app open, it is the body changing size under a running app. */
static void use_panel(enum pos_rotation rotation, struct pos_corners corners)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = corners,
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    /* The lifted finger's last point could be off the turned display, which
     * LVGL warns about on every read. */
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    struct pos_corners c = { corner, corner, corner, corner };

    use_panel(rotation, c);
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
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_calculator.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    app_calculator.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

/* ---- finding things in the app's tree ---------------------------------- */

static lv_obj_t *kid(lv_obj_t *parent, int i)
{
    return parent ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

static lv_obj_t *frame(void) { return kid(app_body, KID_FRAME); }
static lv_obj_t *display_panel(void) { return kid(frame(), KID_DISPLAY); }
static lv_obj_t *keypad(void) { return kid(frame(), KID_PAD); }
static lv_obj_t *expression_label(void) { return kid(display_panel(), DISPLAY_EXPRESSION); }
static lv_obj_t *number_label(void) { return kid(display_panel(), DISPLAY_NUMBER); }

static const char *text_of(lv_obj_t *label)
{
    return label ? lv_label_get_text(label) : "(missing)";
}

static const char *number(void) { return text_of(number_label()); }
static const char *expression(void) { return text_of(expression_label()); }

/* Depth-first search for a visible label with this text; returns its
 * clickable ancestor, which is what a finger would press. A hidden subtree
 * cannot be pressed, so it is not searched. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            lv_obj_t *p = obj;

            while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
                p = lv_obj_get_parent(p);
            }
            return p ? p : obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_labelled(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* The key a finger would press for one character of a script: digits and
 * the point as themselves, + - * / = as the operator keys, c clear,
 * b backspace, n +/-. */
static lv_obj_t *key_for(char k)
{
    char digit[2] = { k, 0 };
    const char *label = digit;

    switch (k) {
    case '-': label = MINUS; break;
    case '*': label = TIMES; break;
    case '/': label = DIVIDE; break;
    case 'c': label = "C"; break;
    case 'b': label = LV_SYMBOL_BACKSPACE; break;
    case 'n': label = PLUS_MINUS; break;
    default: break;
    }
    return find_labelled(keypad(), label);
}

static void tap_keys(const char *keys)
{
    for (; *keys; keys++) {
        tap_obj(key_for(*keys));
    }
}

/* Push bytes into the logical key stream, the way any source does, a few at
 * a time so the bounded queue never refuses one. \n is LV_KEY_ENTER, \b is
 * LV_KEY_BACKSPACE and \x1b is LV_KEY_ESC, by value. */
static void type_keys(const char *keys)
{
    int n = 0;

    for (; *keys; keys++) {
        if (!pos_input_push_key((pos_key_t)(unsigned char)*keys)) {
            printf("FAIL the key stream refused a key\n");
            failed++;
            checks++;
        }
        if (++n == 16) {
            drain();
            n = 0;
        }
    }
    drain();
}

static void push_key(pos_key_t key)
{
    pos_input_push_key(key);
    drain();
}

static int keypad_keys(lv_obj_t **out, int max)
{
    lv_obj_t *pad = keypad();
    int n = 0;
    uint32_t i;

    for (i = 0; pad && i < lv_obj_get_child_count(pad); i++) {
        lv_obj_t *k = lv_obj_get_child(pad, i);

        if (lv_obj_has_flag(k, LV_OBJ_FLAG_CLICKABLE) && n < max) {
            out[n++] = k;
        }
    }
    return n;
}

static int text_width(lv_obj_t *label)
{
    lv_point_t size;

    lv_text_get_size(&size, lv_label_get_text(label),
                     lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Whether a label's text is drawn whole: no wider than the label, and the
 * label no wider than the panel that holds it. */
static int label_fits(lv_obj_t *label)
{
    lv_area_t l;
    lv_area_t p;

    if (!label) {
        return 0;
    }
    lv_obj_update_layout(label);
    lv_obj_get_coords(label, &l);
    lv_obj_get_coords(display_panel(), &p);
    return text_width(label) <= lv_obj_get_content_width(label) && l.x1 >= p.x1 &&
           l.x2 <= p.x2 && l.y1 >= p.y1 && l.y2 <= p.y2;
}

static int inside(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static int safe(const lv_area_t *a)
{
    return pos_display_rect_is_safe(pocketui_display_geometry(), a->x1, a->y1, a->x2, a->y2);
}

static int same_area(const lv_area_t *a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    return a->x1 == x1 && a->y1 == y1 && a->x2 == x2 && a->y2 == y2;
}

static void get_area(lv_obj_t *obj, lv_area_t *a)
{
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static int has_textarea(lv_obj_t *obj)
{
    uint32_t i;

    if (!obj) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return 1;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        if (has_textarea(lv_obj_get_child(obj, i))) {
            return 1;
        }
    }
    return 0;
}

/* The smallest key width and height, for the report and the checks. */
static void smallest_key(int32_t *w, int32_t *h)
{
    lv_obj_t *keys[32];
    int n = keypad_keys(keys, 32);
    int i;

    *w = LV_COORD_MAX;
    *h = LV_COORD_MAX;
    for (i = 0; i < n; i++) {
        lv_area_t a;

        lv_obj_get_coords(keys[i], &a);
        *w = LV_MIN(*w, lv_area_get_width(&a));
        *h = LV_MIN(*h, lv_area_get_height(&a));
    }
}

/* The whole keypad and display, checked for the geometry and mode applied.
 * The usable area is the body's content box inside the safe area: on screen,
 * in the body, outside every rounded-corner square (pos_display.h). */
static void check_layout(const char *mode)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    const bool wide = g->width > g->height;
    lv_obj_t *keys[32];
    lv_area_t screen = { 0, STATUS_H, g->width - 1, g->height - 1 };
    lv_area_t body;
    lv_area_t content;
    lv_area_t panel;
    lv_area_t pad;
    char what[160];
    int n;
    int i;
    int j;
    int small = 0;
    int off = 0;
    int unsafe = 0;
    int overlapping = 0;
    int faces_out = 0;
    int32_t kw;
    int32_t kh;
    int32_t foot;

    lv_obj_update_layout(app_body);
    n = keypad_keys(keys, 32);
    snprintf(what, sizeof(what), "%s: the keypad has 19 keys", mode);
    check(what, n == CALC_PAD_KEYS);
    lv_obj_get_coords(app_body, &body);
    content = body;
    content.x1 += POCKETUI_PAD;
    content.x2 -= POCKETUI_PAD;
    content.y1 += POCKETUI_BODY_PAD_TOP;
    content.y2 -= POCKETUI_PAD;
    for (i = 0; i < n; i++) {
        lv_area_t a;
        lv_area_t f;

        lv_obj_get_coords(keys[i], &a);
        if (lv_area_get_width(&a) < POCKETUI_TOUCH_MIN || lv_area_get_height(&a) < POCKETUI_TOUCH_MIN) {
            small++;
        }
        if (!inside(&a, &screen) || !inside(&a, &content)) {
            off++;
        }
        if (!safe(&a)) {
            unsafe++;
        }
        for (j = i + 1; j < n; j++) {
            lv_area_t b;

            lv_obj_get_coords(keys[j], &b);
            if (overlap(&a, &b)) {
                overlapping++;
            }
        }
        lv_obj_get_coords(lv_obj_get_child(keys[i], 0), &f);
        if (!inside(&f, &a)) {
            faces_out++;
        }
    }
    snprintf(what, sizeof(what), "%s: every key is at least 64 x 64", mode);
    check(what, small == 0);
    snprintf(what, sizeof(what), "%s: every key is wholly on screen, in the body's content box", mode);
    check(what, off == 0);
    snprintf(what, sizeof(what), "%s: every key is in the safe area, clear of the rounded corners", mode);
    check(what, unsafe == 0);
    snprintf(what, sizeof(what), "%s: no two keys overlap", mode);
    check(what, overlapping == 0);
    snprintf(what, sizeof(what), "%s: every key's face is inside its key", mode);
    check(what, faces_out == 0);

    lv_obj_get_coords(display_panel(), &panel);
    lv_obj_get_coords(keypad(), &pad);
    snprintf(what, sizeof(what), "%s: the display is on screen, in the body and in the safe area", mode);
    check(what, inside(&panel, &screen) && inside(&panel, &content) && safe(&panel));
    snprintf(what, sizeof(what), "%s: the display and the keypad do not overlap", mode);
    check(what, !overlap(&panel, &pad));

    /* As low as the safe area lets it go: the body's foot, or the top of the
     * corner squares where they reach higher. */
    foot = LV_MIN(content.y2, g->height - 1 - LV_MAX(g->corners.bottom_left, g->corners.bottom_right));
    if (!wide) {
        snprintf(what, sizeof(what), "%s: tall: the display is above the keypad", mode);
        check(what, panel.y2 < pad.y1);
        snprintf(what, sizeof(what), "%s: tall: both are the body's full width", mode);
        check(what, panel.x1 == content.x1 && panel.x2 == content.x2 && pad.x1 == content.x1 &&
                        pad.x2 == content.x2);
        snprintf(what, sizeof(what), "%s: tall: the keypad ends at the foot of the safe body (y %d)",
                 mode, (int)foot);
        check(what, pad.y2 == foot);
    } else {
        snprintf(what, sizeof(what), "%s: wide: the display is left of the keypad, a 20 px gutter apart",
                 mode);
        check(what, pad.x1 - panel.x2 - 1 == POCKETUI_PAD);
        snprintf(what, sizeof(what), "%s: wide: together they span the body's width", mode);
        check(what, panel.x1 == content.x1 && pad.x2 == content.x2);
        snprintf(what, sizeof(what), "%s: wide: and share it equally (%d and %d px)", mode,
                 (int)lv_area_get_width(&panel), (int)lv_area_get_width(&pad));
        check(what, LV_ABS(lv_area_get_width(&panel) - lv_area_get_width(&pad)) <= 1);
        snprintf(what, sizeof(what), "%s: wide: both run from the top of the body to its safe foot (y %d)",
                 mode, (int)foot);
        check(what, panel.y1 == content.y1 && pad.y1 == content.y1 && panel.y2 == foot &&
                        pad.y2 == foot);
    }
    smallest_key(&kw, &kh);
    snprintf(what, sizeof(what), "%s: the smallest key (%d x %d) is a comfortable target: %s", mode,
             (int)kw, (int)kh, wide ? "at least 120 wide and 64 tall" : "at least 120 x 120");
    check(what, kw >= 120 && kh >= (wide ? POCKETUI_TOUCH_MIN : 120));
    snprintf(what, sizeof(what), "%s: nothing scrolls", mode);
    check(what, lv_obj_get_scroll_bottom(app_body) <= 0 && lv_obj_get_scroll_top(app_body) == 0 &&
                    lv_obj_get_scroll_bottom(frame()) <= 0 && lv_obj_get_scroll_top(frame()) == 0 &&
                    !lv_obj_has_flag(frame(), LV_OBJ_FLAG_SCROLLABLE));
    snprintf(what, sizeof(what), "%s: the number is drawn whole", mode);
    check(what, label_fits(number_label()));
    snprintf(what, sizeof(what), "%s: and so is the expression", mode);
    check(what, label_fits(expression_label()));
}

/* Taps through everything the calculator does, in whatever layout is up. */
static void check_behaviour(const char *where)
{
    char what[128];

    tap_keys("c2+3*4=");
    snprintf(what, sizeof(what), "%s: tapped 2 + 3 x 4 = is 14", where);
    check_str(what, number(), "14");
    snprintf(what, sizeof(what), "%s: with its expression", where);
    check_str(what, expression(), "2 + 3 " TIMES " 4 =");
    tap_keys("c");
    snprintf(what, sizeof(what), "%s: C clears", where);
    check(what, strcmp(number(), "0") == 0 && strcmp(expression(), "") == 0);
    tap_keys("1.25*4=");
    snprintf(what, sizeof(what), "%s: a decimal", where);
    check_str(what, number(), "5");
    tap_keys("c7n-3=");
    snprintf(what, sizeof(what), "%s: a negative", where);
    check_str(what, number(), "-10");
    tap_keys("c2+3==");
    snprintf(what, sizeof(what), "%s: = twice is = once", where);
    check_str(what, number(), "5");
    tap_keys("c9/0=");
    snprintf(what, sizeof(what), "%s: division by zero says so", where);
    check_str(what, number(), CALC_TEXT_DIVIDE_BY_ZERO);
    tap_keys("c123b");
    snprintf(what, sizeof(what), "%s: backspace", where);
    check_str(what, number(), "12");
    type_keys("\x1b" "6*7\n");
    snprintf(what, sizeof(what), "%s: typed keys reach the same display", where);
    check_str(what, number(), "42");
    snprintf(what, sizeof(what), "%s: and the display keeps the focus", where);
    check(what, pos_input_focused() == display_panel());
    type_keys("c");
}

/* The whole mode loop: layout, the longest expression and the widest numbers
 * in each of the three modes. */
static void check_modes(const char *orientation)
{
    static const char *const modes[] = { "normal", "outdoor", "night" };
    char why[128];
    char what[160];
    char label[64];
    int i;
    int m;

    for (m = 0; m < 3; m++) {
        snprintf(label, sizeof(label), "%s %s", orientation, modes[m]);
        snprintf(what, sizeof(what), "%s mode applies", label);
        check(what, pos_theme_apply(NULL, modes[m], why, sizeof(why)) == 0);
        pump(60);
        type_keys("c");
        for (i = 0; i < 20; i++) {
            type_keys("123456789012+");
        }
        check_layout(label);
        snprintf(what, sizeof(what), "%s: the long expression is refitted and still fits", label);
        check(what, strncmp(expression(), CALC_ELLIPSIS, 3) == 0 && label_fits(expression_label()));

        /* The widest things the main line can be asked to show. */
        {
            static const char *const widest[][2] = {
                { "c0.123456789012n", "-0.123456789012" },
                { "c999999999999*999999999999=", "9.99999999998e23" },
                { "c123456789012n*1000=", "-1.2345678901e14" },
                { "c1/300=n", "-3.3333333333e-3" },
                { "c888888888888n", "-888888888888" },
                { "c5/0=", CALC_TEXT_DIVIDE_BY_ZERO },
                { "c999999999999*999999999999*999999999999*999999999999*999999999999"
                  "*999999999999*999999999999*999999999999*999999999999=", CALC_TEXT_OVERFLOW },
            };
            size_t w;

            for (w = 0; w < sizeof(widest) / sizeof(widest[0]); w++) {
                type_keys(widest[w][0]);
                snprintf(what, sizeof(what), "%s: shows %s", label, widest[w][1]);
                check_str(what, number(), widest[w][1]);
                snprintf(what, sizeof(what), "%s: %s is drawn whole", label, widest[w][1]);
                check(what, label_fits(number_label()));
            }
        }
        /* And the keypad still takes taps. */
        tap_keys("c6*7=");
        snprintf(what, sizeof(what), "%s: the keypad works", label);
        check_str(what, number(), "42");
    }
    pos_theme_apply(NULL, "normal", why, sizeof(why));
    pump(60);
    type_keys("c");
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int empty = 1;

    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            empty = 0;
        }
    }
    closedir(d);
    return empty;
}

int main(void)
{
    static char state_dir[] = "/tmp/calc_app_state.XXXXXX";
    static char config_dir[] = "/tmp/calc_app_config.XXXXXX";
    static char runtime_dir[] = "/tmp/calc_app_runtime.XXXXXX";
    lv_indev_t *finger;
    lv_obj_t *keys[32];
    lv_area_t a;
    lv_area_t b;
    int i;

    /* Wherever PocketOS would put anything, point it somewhere empty and
     * look again at the end. */
    if (!mkdtemp(state_dir) || !mkdtemp(config_dir) || !mkdtemp(runtime_dir)) {
        printf("FAIL cannot make temporary directories\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
    setenv("POCKETOS_CONFIG_DIR", config_dir, 1);
    setenv("POCKETOS_RUNTIME_DIR", runtime_dir, 1);

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);

    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    /* The unit's panel, as the shell opens it in portrait. */
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 1. it opens ---------------------------------------------------- */

    app_start();
    check("the app returns its state", app_priv != NULL);
    check("the app adds one box to the body", lv_obj_get_child_count(app_body) == 1u);
    check_str("it opens on 0", number(), "0");
    check_str("with no expression", expression(), "");
    check("the display is the focused object", pos_input_focused() == display_panel());
    check("the keypad has 19 keys", keypad_keys(keys, 32) == CALC_PAD_KEYS);
    check("it has no tick: there is no clock to follow", app_calculator.tick == NULL);
    check_str("its id", app_calculator.id, "calculator");
    check_str("its name", app_calculator.name, "Calculator");
    check("and an icon", app_calculator.icon && app_calculator.icon[0]);
    check("there is no text field for a keyboard to type into", !has_textarea(app_body));

    /* ---- 2. a finger on the keypad -------------------------------------- */

    tap_keys("2+3*4=");
    check_str("tapped 2 + 3 x 4 = is 14, with precedence", number(), "14");
    check_str("and the expression is drawn with the operator glyphs", expression(),
              "2 + 3 " TIMES " 4 =");
    tap_keys("c12+3*4=");
    check_str("tapped 12 + 3 x 4 = is 24", number(), "24");
    check_str("with its expression", expression(), "12 + 3 " TIMES " 4 =");
    check("tapping left the focus on the display", pos_input_focused() == display_panel());
    tap_keys("c");
    check_str("C clears", number(), "0");
    check_str("and the expression", expression(), "");
    tap_keys("7-2*3=");
    check_str("7 - 2 x 3 = 1", number(), "1");
    check_str("with its expression", expression(), "7 " MINUS " 2 " TIMES " 3 =");
    tap_keys("c8/4/2=");
    check_str("8 / 4 / 2 = 1", number(), "1");
    check_str("with its expression", expression(), "8 " DIVIDE " 4 " DIVIDE " 2 =");
    tap_keys("c1.5n");
    check_str("the point and +/-", number(), "-1.5");
    tap_keys("b");
    check_str("the backspace key", number(), "-1.");
    tap_keys("c123b");
    check_str("backspace removes a digit", number(), "12");
    tap_keys("c2+3==");
    check_str("= twice is = once", number(), "5");
    check_str("and the expression is unchanged", expression(), "2 + 3 =");
    tap_keys("5/0=");
    check_str("a digit after a result starts again, and dividing by zero says so",
              number(), CALC_TEXT_DIVIDE_BY_ZERO);
    check_str("keeping the expression", expression(), "5 " DIVIDE " 0 =");
    tap_keys("7");
    check_str("a digit leaves the error", number(), "7");
    check_str("and starts afresh", expression(), "");
    check("focus is still on the display after all of that",
          pos_input_focused() == display_panel());

    /* The pressed role is there while the finger is. */
    {
        lv_obj_t *seven = key_for('7');
        lv_obj_t *equals = key_for('=');

        lv_obj_get_coords(seven, &a);
        finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
        finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(60);
        check("a key under a finger is pressed", lv_obj_has_state(seven, LV_STATE_PRESSED));
        check("and nothing is animating", lv_anim_count_running() == 0);
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(60);
        check("and is not once it lifts", !lv_obj_has_state(seven, LV_STATE_PRESSED));
        lv_obj_get_coords(equals, &a);
        finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
        finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(60);
        check("= shows its pressed state too", lv_obj_has_state(equals, LV_STATE_PRESSED));
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(60);
        check("still nothing animating", lv_anim_count_running() == 0);
    }

    /* A tap on the display itself changes nothing and keeps the focus. */
    tap_keys("c42");
    tap_obj(display_panel());
    check_str("tapping the display changes nothing", number(), "42");
    check("and keeps the focus there", pos_input_focused() == display_panel());

    /* ---- 3. keys through the stream ------------------------------------- */

    type_keys("\x1b");
    check_str("Esc clears", number(), "0");
    check_str("everything", expression(), "");
    type_keys("7*8\n");
    check_str("typed 7 * 8 Enter", number(), "56");
    check_str("with its expression", expression(), "7 " TIMES " 8 =");
    type_keys("c0123456789");
    check_str("c clears, and every digit types", number(), "123456789");
    type_keys("c.5");
    check_str(". is the point", number(), "0.5");
    type_keys("c,25");
    check_str(", is the point too", number(), "0.25");
    type_keys("c9+1=");
    check_str("+ adds", number(), "10");
    type_keys("c9-1=");
    check_str("- subtracts", number(), "8");
    check_str("and is drawn as a minus sign", expression(), "9 " MINUS " 1 =");
    type_keys("c3*4=");
    check_str("* multiplies", number(), "12");
    type_keys("c3x5=");
    check_str("x multiplies", number(), "15");
    type_keys("c3X6=");
    check_str("X multiplies", number(), "18");
    type_keys("c8/2=");
    check_str("/ divides", number(), "4");
    type_keys("c9/3\n");
    check_str("Enter is =", number(), "3");
    type_keys("c123\b");
    check_str("Backspace removes a digit", number(), "12");
    type_keys("c5n");
    check_str("n is +/-", number(), "-5");
    type_keys("N");
    check_str("and so is N", number(), "5");
    type_keys("C");
    check_str("C clears as well", number(), "0");
    type_keys("2+2=\x1b");
    check_str("Esc clears a result", number(), "0");
    type_keys("2+3==");
    check_str("typed = twice is = once", number(), "5");
    type_keys("5/0=");
    check_str("typed division by zero", number(), CALC_TEXT_DIVIDE_BY_ZERO);
    type_keys("+");
    check_str("an operator leaves the error without applying", number(), "0");
    check_str("with nothing in the expression", expression(), "");

    /* Keys the calculator does not know do nothing, and do not move focus. */
    type_keys("c12");
    push_key('a');
    push_key('%');
    push_key(' ');
    push_key('q');
    push_key(LV_KEY_UP);
    push_key(LV_KEY_DOWN);
    push_key(LV_KEY_LEFT);
    push_key(LV_KEY_RIGHT);
    push_key(LV_KEY_HOME);
    push_key(LV_KEY_END);
    push_key(LV_KEY_DEL);
    push_key(0x00E6); /* a Norwegian letter from the symbol layer */
    push_key(0x00D7); /* the times sign itself is not a key */
    check_str("unmapped keys change nothing", number(), "12");
    check_str("nor the expression", expression(), "");
    push_key(LV_KEY_NEXT);
    push_key(LV_KEY_PREV);
    check("Next and Prev leave the focus on the display",
          pos_input_focused() == display_panel());
    type_keys("3");
    check_str("and typing still arrives", number(), "123");

    /* Tapping and typing, interleaved: one calculation, one sink. */
    type_keys("c");
    tap_keys("1");
    type_keys("2");
    tap_keys("+");
    type_keys("3");
    tap_keys("=");
    check_str("tapped 1, typed 2, tapped +, typed 3, tapped =", number(), "15");
    check_str("is 12 + 3", expression(), "12 + 3 =");
    type_keys("*2\n");
    check_str("and typing carries the tapped result on", number(), "30");

    /* ---- 4. overflow and a long expression ------------------------------ */

    type_keys("c999999999999");
    for (i = 0; i < 9; i++) {
        type_keys("*999999999999");
    }
    type_keys("=");
    check_str("a product past 1e100 is an overflow", number(), CALC_TEXT_OVERFLOW);
    check("its expression is cut to the line", strncmp(expression(), CALC_ELLIPSIS, 3) == 0);
    check("and drawn whole", label_fits(expression_label()));

    type_keys("c");
    for (i = 0; i < 20; i++) {
        type_keys("123456789012+");
    }
    type_keys("1");
    check_str("a long expression leaves the entry alone", number(), "1");
    check("the expression starts with an ellipsis", strncmp(expression(), CALC_ELLIPSIS, 3) == 0);
    {
        const char *e = expression();
        size_t len = strlen(e);

        check("and keeps its newest end", len > 3 && strcmp(e + len - 3, " + ") != 0 &&
                                              strcmp(e + len - 2, " +") == 0);
        check("which begins at a space, not inside a number", e[3] == ' ');
    }
    check("it is no wider than the line", label_fits(expression_label()));
    type_keys("c");

    /* ---- 5. portrait: the glass, in every mode -------------------------- */

    check_modes("portrait");

    /* The portrait geometry to the pixel. On a panel with square corners it
     * is the v0.0.9 layout: the display 368 px tall at the top of the body,
     * the keypad 672 px at its foot. With the unit's 30 px corners the only
     * difference is the keypad and the display's foot lifted 10 px, clear of
     * the corner squares; the keys themselves are unchanged. */
    get_area(display_panel(), &a);
    get_area(keypad(), &b);
    /* The keypad keeps its v0.0.9 place against the foot; the display above
     * it takes the 56 px the status bar gave back (DS section 36), from row
     * 96 rather than 152. */
    check("portrait, 30 px corners: the display is 20..547 x 96..509",
          same_area(&a, 20, V010_ROW(152), 547, 509));
    check("portrait, 30 px corners: the keypad is 20..547 x 530..1201, 10 px above the body's foot",
          same_area(&b, 20, 530, 547, 1201));
    /* A panel's corners are fixed for a run, so another panel is another
     * start. */
    app_stop();
    use_display(POS_ROTATION_0, 0);
    app_start();
    get_area(display_panel(), &a);
    get_area(keypad(), &b);
    check("portrait, square corners: the display is the v0.0.9 20..547 x 152..519, from row 96",
          same_area(&a, 20, V010_ROW(152), 547, 519));
    check("portrait, square corners: the keypad is the v0.0.9 20..547 x 540..1211",
          same_area(&b, 20, 540, 547, 1211));
    {
        int32_t kw;
        int32_t kh;

        smallest_key(&kw, &kh);
        check("and every key is at least the v0.0.9 126 x 128", kw >= 126 && kh == 128);
    }
    check_layout("portrait, square corners");
    app_stop();

    /* Each foot corner on its own, larger than the unit's: the clearance is
     * whichever corner reaches furthest, from either end. */
    {
        static const struct pos_corners one[2] = { { 0, 0, 0, 44 }, { 0, 0, 44, 0 } };
        static const char *const which[2] = { "bottom-left", "bottom-right" };
        char what[96];
        int k;

        for (k = 0; k < 2; k++) {
            use_panel(POS_ROTATION_0, one[k]);
            app_start();
            snprintf(what, sizeof(what), "portrait, only the %s corner rounded, 44 px", which[k]);
            check_layout(what);
            get_area(keypad(), &b);
            snprintf(what, sizeof(what), "portrait, only the %s corner: the keypad ends at y 1187", which[k]);
            check(what, b.y2 == PANEL_H - 1 - 44);
            app_stop();
        }
    }

    /* ---- 6. landscape ---------------------------------------------------- */

    /* The shell applies a rotation by opening the display again, so an app
     * meets landscape by being created in it. */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    check("landscape: it opens on 0", strcmp(number(), "0") == 0 && strcmp(expression(), "") == 0);
    check("landscape: the display is the focused object", pos_input_focused() == display_panel());
    check_layout("landscape");
    get_area(display_panel(), &a);
    get_area(keypad(), &b);
    /* From row 96: the same 72 px header and 24 px body padding straight
     * from the top edge (DS section 36; row 128 under the 32 px COMPACT bar
     * of section 30). */
    check("landscape: the display is 20..605 x 96..537", same_area(&a, 20, V010_LROW(128), 605, 537));
    check("landscape: the keypad is 626..1211 x 96..537", same_area(&b, 626, V010_LROW(128), 1211, 537));
    check_behaviour("landscape");
    check_modes("landscape");
    app_stop();
    use_display(POS_ROTATION_270, 0);
    app_start();
    check_layout("landscape, square corners");
    app_stop();
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();

    /* ---- 7. the box changing shape under a running app ------------------- */

    tap_keys("c2+3*4");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("turned to portrait with the app open, the layout follows", lv_obj_get_child_count(app_body) == 1u);
    check_layout("portrait, after landscape");
    check_str("and the calculation in progress is still there", number(), "4");
    check_str("with its expression", expression(), "2 + 3 " TIMES);
    tap_keys("=");
    check_str("finished in portrait, 2 + 3 x 4 = 14", number(), "14");
    /* Short operands, so that the wider landscape line has room for more of
     * them: a fitted line is only cut at a space. */
    type_keys("c");
    for (i = 0; i < 31; i++) {
        type_keys("7+");
    }
    {
        size_t tall_len = strlen(expression());

        use_display(POS_ROTATION_270, PANEL_CORNER);
        check_layout("landscape, after portrait");
        check("the long expression is refitted to the wider line: more of it shows, and it fits",
              strncmp(expression(), CALC_ELLIPSIS, 3) == 0 && strlen(expression()) > tall_len &&
                  label_fits(expression_label()));
        use_display(POS_ROTATION_0, PANEL_CORNER);
        check("and refitted to the narrower line when turned back",
              strlen(expression()) == tall_len && label_fits(expression_label()));
        use_display(POS_ROTATION_270, PANEL_CORNER);
    }
    check("focus stayed on the display through both changes", pos_input_focused() == display_panel());
    check_behaviour("landscape, after portrait");

    /* A wide body too short for five rows of 64 px keys - the landscape
     * content area with 300 px taken off its foot, as the touch keyboard's
     * sheet would take them - keeps the tall layout whole and scrolls, rather
     * than shrinking a key under the touch minimum. */
    tap_keys("c42");
    lv_obj_set_height(g_content, PANEL_W - STATUS_H - 300);
    pump(60);
    lv_obj_update_layout(app_body);
    get_area(display_panel(), &a);
    get_area(keypad(), &b);
    check("a wide body 140 px tall keeps the display above the keypad", a.y2 < b.y1);
    check("with the keypad its full 672 px", lv_area_get_height(&b) == 672);
    check("and the display at its own height, not squeezed away", lv_area_get_height(&a) > 100);
    {
        int32_t kw;
        int32_t kh;

        smallest_key(&kw, &kh);
        check("and no key under 64 px", kw >= POCKETUI_TOUCH_MIN && kh >= POCKETUI_TOUCH_MIN);
    }
    check("the frame scrolls to reach it", lv_obj_has_flag(frame(), LV_OBJ_FLAG_SCROLLABLE) &&
                                               lv_obj_get_scroll_bottom(frame()) > 0);
    check("and clips what it scrolls, so no key is drawn over the header",
          !lv_obj_has_flag(frame(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    type_keys("+8");
    lv_obj_scroll_to_y(frame(), lv_obj_get_scroll_bottom(frame()), LV_ANIM_OFF);
    pump(60);
    tap_keys("=");
    check_str("scrolled to the foot, = is there to tap", number(), "50");
    lv_obj_set_height(g_content, PANEL_W - STATUS_H);
    pump(60);
    check("given its height back, the frame is scrolled home and stops scrolling",
          lv_obj_get_scroll_y(frame()) == 0 && !lv_obj_has_flag(frame(), LV_OBJ_FLAG_SCROLLABLE));
    check_str("the result is still on the display", number(), "50");
    check_layout("landscape, the sheet's height given back");
    app_stop();
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 8. opened and closed ------------------------------------------- */

    check("closing leaves nothing behind", lv_obj_get_child_count(g_content) == 0u);
    for (i = 0; i < 5; i++) {
        use_display(i % 2 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
        app_start();
        tap_keys("9*9=");
        type_keys("+1\n");
        app_stop();
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("five rounds in both orientations leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("and no focusable object", pos_input_focused() == NULL);
    app_start();
    check_str("a fresh open remembers nothing", number(), "0");
    check_str("of any expression", expression(), "");
    check("and takes the focus again", pos_input_focused() == display_panel());
    type_keys("6*7\n");
    check_str("and takes keys", number(), "42");
    app_stop();

    /* ---- 9. it asked for nothing and wrote nothing ---------------------- */

    check("no keyboard was asked for or dismissed", keyboard_requests == 0);
    check("no shell service was called: no clock, no radio, no status hint",
          shell_calls == 0);
    check("nothing was written to the state directory", dir_is_empty(state_dir));
    check("nor the config directory", dir_is_empty(config_dir));
    check("nor the runtime directory", dir_is_empty(runtime_dir));
    rmdir(state_dir);
    rmdir(config_dir);
    rmdir(runtime_dir);

    printf("calc_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
