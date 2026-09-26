/*
 * PocketClock in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Every alarm here is made the way a finger makes one: taps on the steppers,
 * taps on the keyboard, a tap on Add. What is then read back off the disk is
 * what the store actually kept.
 *
 * The shell is not here either, so this file plays it: the three app.h
 * keyboard entry points are implemented below against the real
 * pos_keyboard, and the one alarm runtime and the one alert sheet are
 * created the way ui/shell/shell.c creates them. That is what lets the last
 * sections test the thing the runtime exists for - an alarm ringing with
 * PocketClock shut - by stepping the runtime with an injected reading
 * instead of waiting for 07:30.
 *
 * The app is hosted the way the shell hosts it - a header, then a padded
 * body - on the reference panel with its 30 px rounded corners, and the last
 * sections turn that panel under the open app: every screen laid out in
 * portrait and in landscape, with square and rounded corners, in Normal and
 * Outdoor type, with the keyboard up and down, and what is on show kept
 * through every turn.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/clock_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "clock_alert.h"
#include "clock_runtime.h"
#include "clock_store.h"
#include "clock_time.h"
#include "pocketui.h"
#include "pos_keyboard.h"
#include "shell_alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* Where the shell's content area starts for this app in the display's
 * orientation (ui/shell/chrome.h, DS sections 30 and 36), so the frame built
 * here is the one shell.c builds: the top edge, since no chrome reserves a
 * row there any more. */
#define STATUS_H chrome_height(chrome_resolve(app_clock.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
/* Rows measured on v0.0.10 under the 56 px status bar, moved up with the
 * frame's top, which is the top edge since the bar went (DS section 36):
 * nothing else about those places changed. */
#define V010_ROW(y) ((y) - 56 + STATUS_H)
#define PAIRED_BUTTON_H 56 /* DS 7: paired buttons */
#define COLUMN_W 528       /* the portrait body on the reference panel */
#define RAIL_W 288         /* two 140 px actions and their 8 px gap (DS 23.1) */

/* The app's own layout, as clock_app.c builds it. Reaching for a control by
 * position rather than by label is the only way to tell two stepper rows of
 * identical buttons apart. */
#define SCREEN_MAIN 0
#define SCREEN_ADD 1
#define SCREEN_CONFIRM 2
#define PANE_CLOCK 0
#define PANE_ALARM 1
#define PANE_WATCH 2
#define PANE_TIMER 3

extern const struct pocketos_app app_clock;

static int failed;
static int checks;
static char root[128];

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
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want);
    }
}

/* ---- the shell's side of app.h, as the shell implements it ------------- */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static char g_hint[64];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)on_done;
    (void)user;
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE
                                            ? POS_KB_RETURN_NEWLINE
                                            : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, pocketui_display_geometry()->height - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, pocketui_display_geometry()->height - STATUS_H);
}

int pocketos_shell_keyboard_visible(void)
{
    return pos_keyboard_is_shown(g_keyboard);
}

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_H * 40 * 4]; /* the long side, either way up */
static lv_display_t *disp;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

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

static void drain(void)
{
    int t;

    for (t = 0; t < 300 && pos_input_queued() > 0; t += 5) {
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
    drain();
}

static void tap_key(const char *label)
{
    tap_obj(pos_keyboard_key(g_keyboard, label));
}

/* The alpha layer shows lower case, so a capital needs Shift first. */
static void type_text(const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            tap_key("SHIFT");
        }
        one[0] = *s;
        tap_key(one);
    }
}

/* ---- finding things in the app's tree ---------------------------------- */

static lv_obj_t *app_body;
static void *app_priv;

/* Depth-first search for a label with this text; returns its clickable
 * ancestor, which is what a finger would press. A hidden screen is still in
 * the tree, and a finger cannot press what is not shown, so neither may
 * this - which is also what keeps the four panes from matching each other. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
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

/* The same search, hidden subtrees included and the label itself returned:
 * for asking whether something was built, not whether it can be pressed. */
static lv_obj_t *find_text_anywhere(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_text_anywhere(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int label_present(lv_obj_t *obj, const char *text)
{
    return find_labelled(obj, text) != NULL;
}

static const char *text_of(lv_obj_t *label)
{
    return label ? lv_label_get_text(label) : "(missing)";
}

/* NULL-safe, so that a layout that lost an object fails its checks and the
 * run goes on, rather than stopping in an LVGL assert that never returns. */
static lv_obj_t *kid(lv_obj_t *parent, uint32_t i)
{
    return parent && lv_obj_get_child_count(parent) > i ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

/* The app's tree, as clock_app.c builds it: one frame in the body, and in it
 * the main screen, the new-alarm form and the delete confirmation. */
static lv_obj_t *frame_of(void)
{
    return kid(app_body, 0);
}

static lv_obj_t *screen_of(int which)
{
    return kid(frame_of(), (uint32_t)which);
}

static lv_obj_t *pane_of(int which)
{
    return kid(screen_of(SCREEN_MAIN), (uint32_t)which + 1); /* 0 is the tabs */
}

/* A pane's two columns; the clock face's pane has none. */
static lv_obj_t *column_of(int pane, int which)
{
    return kid(pane_of(pane), (uint32_t)which);
}

/* Every pane and the new-alarm form open with a card - first in the pane, or
 * first in its first column or part - whose first child is the big number. */
static lv_obj_t *hero_in(lv_obj_t *parent)
{
    lv_obj_t *o = kid(parent, 0);

    while (o && !lv_obj_check_type(o, &lv_label_class)) {
        o = kid(o, 0);
    }
    return o;
}

/* The card around the big number. */
static lv_obj_t *hero_card(lv_obj_t *parent)
{
    lv_obj_t *label = hero_in(parent);

    return label ? lv_obj_get_parent(label) : NULL;
}

/* A stepper row is [-big][-small][value][+small][+big]. */
#define STEP_MINUS_BIG 0
#define STEP_MINUS 1
#define STEP_VALUE 2
#define STEP_PLUS 3
#define STEP_PLUS_BIG 4

static lv_obj_t *stepper_cell(lv_obj_t *parent, int row, int cell)
{
    return kid(kid(parent, (uint32_t)row), (uint32_t)cell);
}

/* The new-alarm form is when - the time, then Hour, Minute and Repeat - and
 * what - the label field, then Cancel and Add. */
#define FORM_HOUR 0
#define FORM_MINUTE 1
static lv_obj_t *form_when(void) { return kid(screen_of(SCREEN_ADD), 0); }
static lv_obj_t *form_controls(void) { return kid(form_when(), 1); }
static lv_obj_t *form_what(void) { return kid(screen_of(SCREEN_ADD), 1); }
static lv_obj_t *form_field(void) { return kid(kid(form_what(), 0), 0); }
static lv_obj_t *form_actions(void) { return kid(form_what(), 1); }

static lv_obj_t *form_cell(int row, int cell)
{
    return stepper_cell(form_controls(), row, cell);
}

/* The timer pane's setter: its second column's first item. */
static lv_obj_t *timer_setter(void)
{
    return kid(column_of(PANE_TIMER, 1), 0);
}

/* The trash button on the alarm row showing this time. */
static lv_obj_t *alarm_row_delete(const char *hm)
{
    lv_obj_t *hit = find_labelled(app_body, hm);

    if (!hit) {
        return NULL;
    }
    return lv_obj_get_child(lv_obj_get_parent(hit), 2);
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

static lv_obj_t *app_root;

/* ui/shell/shell.c's app_open(): a root in the content area, a header, and
 * the padded body the app is created in. */
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
    app_priv = app_clock.create(app_body);
    pump(60);
}

/* The shell's app_close(): the keyboard away, destroy, then the root. */
static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_clock.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

/* The display the shell would open: the reference panel with corner squares
 * of `corner` px, `long_side` tall in portrait, turned to `rotation`, the
 * geometry handed to PocketUI and the content area below the status bar
 * sized to it, less the keyboard sheet while it is up. Called with the app
 * open, it is the body changing shape under a running app. */
static void use_panel_sized(int32_t width, int32_t long_side, enum pos_rotation rotation, int32_t corner)
{
    struct pos_panel panel = {
        .width = width,
        .height = long_side,
        .corners = { corner, corner, corner, corner },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    /* The lifted finger's last point could be off the turned display, which
     * LVGL warns about on every read. */
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width,
                    g.height - STATUS_H - (pocketos_shell_keyboard_visible() ? POS_KB_H : 0));
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
}

static void use_panel(int32_t long_side, enum pos_rotation rotation, int32_t corner)
{
    use_panel_sized(PANEL_W, long_side, rotation, corner);
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    use_panel(PANEL_H, rotation, corner);
}

/* ---- a board that does not know the time ------------------------------- */

/* The app reads the wall clock itself, ten times a second, so the only way to
 * show it the board that has just booted is to be that clock. Every other
 * clock, and this one until a section asks, is the host's. */
static int g_wall_unset;

int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME && g_wall_unset) {
        ts->tv_sec = 100;
        ts->tv_nsec = 0;
        return 0;
    }
    return (int)syscall(SYS_clock_gettime, id, ts);
}

/* ---- where things are -------------------------------------------------- */

static void area_of(lv_obj_t *obj, lv_area_t *a)
{
    if (!obj) {
        lv_area_set(a, 0, 0, -1, -1);
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static int within(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* Where the app may put anything: the body's content box. */
static void body_box(lv_area_t *b)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, b);
}

/* How far the foot corner squares reach above the body's foot. */
static int32_t foot_inset(void)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    lv_area_t b;

    body_box(&b);
    return LV_MAX(0, b.y2 - (g->height - LV_MAX(g->corners.bottom_left, g->corners.bottom_right)) + 1);
}

/* The wide shape's rule, read off the body: wider than tall, and room for two
 * portrait-wide columns and the gutter. */
static int wide_body(void)
{
    lv_area_t b;
    int32_t w;

    body_box(&b);
    w = lv_area_get_width(&b);
    return w > lv_area_get_height(&b) - foot_inset() && w >= 2 * COLUMN_W + POCKETUI_PAD;
}

static int32_t scroll_y(lv_obj_t *obj)
{
    return obj ? lv_obj_get_scroll_y(obj) : -1;
}

/* The nearest box that scrolls: what a finger drags to bring obj into view,
 * and what clips it. The shell's body scrolls in principle and never in
 * practice - the frame is exactly its content box - so the frame stands for
 * it. */
static lv_obj_t *scroller_of(lv_obj_t *obj)
{
    lv_obj_t *p = obj ? lv_obj_get_parent(obj) : NULL;

    while (p && p != app_body && !lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
        p = lv_obj_get_parent(p);
    }
    return p == app_body ? frame_of() : p;
}

/* The box a scroller shows its content in: its own area, or for the frame,
 * which does not scroll and stands in for the body, the box inside its
 * padding - where the foot gives way to the rounded corners. */
static void scroller_box(lv_obj_t *s, lv_area_t *a)
{
    if (s && s == frame_of()) {
        lv_obj_update_layout(s);
        lv_obj_get_content_coords(s, a);
        return;
    }
    area_of(s, a);
}

/* Wholly on show: inside the box that scrolls it as it is scrolled now,
 * inside the body, and clear of the panel's unsafe area. */
static int in_view(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t s;
    lv_area_t b;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    area_of(obj, &a);
    scroller_box(scroller_of(obj), &s);
    body_box(&b);
    return lv_area_get_height(&a) > 0 && within(&a, &s) && within(&a, &b) &&
           pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2);
}

static int has_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    return obj && lv_obj_has_flag(obj, flag);
}

static int count_objects(lv_obj_t *obj)
{
    uint32_t i;
    int n = 1;

    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n += count_objects(lv_obj_get_child(obj, i));
    }
    return n;
}

/* Every visible thing a finger can press: clickable, with a handler. */
static int collect_targets(lv_obj_t *obj, lv_obj_t **out, int n, int max)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return n;
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(obj) > 0 && n < max) {
        out[n++] = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n = collect_targets(lv_obj_get_child(obj, i), out, n, max);
    }
    return n;
}

/* Every visible label inside its parent's box, and no two visible children
 * of a row on top of each other. A text area's own label scrolls inside it
 * and is its business. */
static int laid_out_whole(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t j;
    int bad = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_check_type(obj, &lv_textarea_class)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t l;
        lv_area_t p;

        area_of(obj, &l);
        lv_obj_get_content_coords(lv_obj_get_parent(obj), &p);
        if (l.x1 < p.x1 || l.x2 > p.x2 || l.y1 < p.y1 || l.y2 > p.y2) {
            printf("     label \"%.40s\" %d..%d x %d..%d outside its box %d..%d x %d..%d\n", lv_label_get_text(obj),
                   (int)l.x1, (int)l.x2, (int)l.y1, (int)l.y2, (int)p.x1, (int)p.x2, (int)p.y1, (int)p.y2);
            bad++;
        }
        return bad;
    }
    if (lv_obj_get_style_layout(obj, 0) == LV_LAYOUT_FLEX) {
        for (i = 0; i < lv_obj_get_child_count(obj); i++) {
            lv_obj_t *a = lv_obj_get_child(obj, i);
            lv_area_t aa;

            if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            area_of(a, &aa);
            for (j = i + 1; j < lv_obj_get_child_count(obj); j++) {
                lv_obj_t *b = lv_obj_get_child(obj, j);
                lv_area_t bb;

                if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) {
                    continue;
                }
                area_of(b, &bb);
                if (overlaps(&aa, &bb)) {
                    printf("     neighbours overlap: %d..%d x %d..%d and %d..%d x %d..%d\n", (int)aa.x1, (int)aa.x2,
                           (int)aa.y1, (int)aa.y2, (int)bb.x1, (int)bb.x2, (int)bb.y1, (int)bb.y2);
                    bad++;
                }
            }
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        bad += laid_out_whole(lv_obj_get_child(obj, i));
    }
    return bad;
}

/* The screen on show, checked whole: every target a finger's size (64 wide,
 * and the 56 of a paired button tall), on no other target, and wholly in view
 * in the body and the safe area once scrolled to - with the box that scrolls
 * it in the body and the safe area too, so nothing scrolled is ever drawn
 * into a rounded corner; every label and row laid out whole; and the body
 * the shell gives never scrolled (the app scrolls inside it). */
static void check_screen(const char *what, lv_obj_t *screen)
{
    static lv_obj_t *t[96];
    char msg[200];
    lv_area_t box;
    int n;
    int i;
    int j;
    int small = 0;
    int overlap = 0;
    int unreachable = 0;
    int unsafe = 0;

    body_box(&box);
    n = collect_targets(screen, t, 0, 96);
    snprintf(msg, sizeof(msg), "%s: there is something to press", what);
    check(msg, n > 0);
    for (i = 0; i < n; i++) {
        lv_obj_t *s = scroller_of(t[i]);
        lv_area_t a;
        lv_area_t sa;
        int32_t sy = scroll_y(s);

        area_of(t[i], &a);
        if (lv_area_get_height(&a) < PAIRED_BUTTON_H || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN) {
            printf("     small target %dx%d\n", (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
            small++;
        }
        for (j = i + 1; j < n; j++) {
            lv_area_t b;

            area_of(t[j], &b);
            if (overlaps(&a, &b)) {
                overlap++;
            }
        }
        scroller_box(s, &sa);
        if (!s || !within(&sa, &box) ||
            !pos_display_rect_is_safe(pocketui_display_geometry(), sa.x1, sa.y1, sa.x2, sa.y2)) {
            printf("     scroller %d..%d x %d..%d not inside the body %d..%d x %d..%d and the safe area\n",
                   (int)sa.x1, (int)sa.x2, (int)sa.y1, (int)sa.y2, (int)box.x1, (int)box.x2, (int)box.y1,
                   (int)box.y2);
            unsafe++;
        }
        lv_obj_scroll_to_view_recursive(t[i], LV_ANIM_OFF);
        if (!in_view(t[i])) {
            area_of(t[i], &a);
            printf("     target %d..%d x %d..%d cannot be brought wholly into view\n", (int)a.x1, (int)a.x2,
                   (int)a.y1, (int)a.y2);
            unreachable++;
        }
        if (s) {
            lv_obj_scroll_to_y(s, sy, LV_ANIM_OFF);
        }
    }
    pump(20);
    snprintf(msg, sizeof(msg), "%s: every target is at least 64 wide and 56 tall", what);
    check(msg, small == 0);
    snprintf(msg, sizeof(msg), "%s: no target lies on another", what);
    check(msg, overlap == 0);
    snprintf(msg, sizeof(msg), "%s: everything scrolls inside the body and the safe area", what);
    check(msg, unsafe == 0);
    snprintf(msg, sizeof(msg), "%s: every target can be brought wholly into view", what);
    check(msg, unreachable == 0);
    snprintf(msg, sizeof(msg), "%s: every label and row laid out whole", what);
    check(msg, laid_out_whole(screen) == 0);
    snprintf(msg, sizeof(msg), "%s: the body the shell gives does not scroll", what);
    check(msg, lv_obj_get_scroll_top(app_body) <= 0 && lv_obj_get_scroll_bottom(app_body) <= 0);
}

static void check_rect(const char *what, lv_obj_t *obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a;

    checks++;
    area_of(obj, &a);
    if (!obj || a.x1 != x1 || a.y1 != y1 || a.x2 != x2 || a.y2 != y2) {
        failed++;
        printf("FAIL %s: is %d..%d x %d..%d, want %d..%d x %d..%d\n", what, (int)a.x1, (int)a.x2, (int)a.y1,
               (int)a.y2, (int)x1, (int)x2, (int)y1, (int)y2);
    }
}

/* A tap on what a finger would have scrolled to first. */
static void tap_seen(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
        pump(20);
    }
    tap_obj(obj);
}

/* A finger drawn dy pixels from a point in small moves, the way a scroll
 * reaches LVGL from the panel, then time for the scroll to come to rest. */
static void drag_at(int32_t x, int32_t y, int32_t dy)
{
    int step;

    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (step = 1; step <= 20; step++) {
        finger_point.y = y + dy * step / 20;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(1500);
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* What is actually on disk, read back through the store. */
static int stored(struct clock_engine *out)
{
    clock_engine_init(out);
    return clock_store_load(out);
}

/* A clock reading the shell tick might have taken. The alert is reached by
 * injecting one rather than waiting for 07:30 to come round. */
static struct clock_now at(int64_t day, int hour, int minute, int wday,
                           int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.wall.valid = true;
    n.wall.day = day;
    n.wall.hour = hour;
    n.wall.minute = minute;
    n.wall.wday = wday;
    n.wall.epoch = CLOCK_WALL_VALID_FROM;
    n.mono_ms = mono_ms;
    return n;
}

/* A board that does not know the time - which is what a countdown has to
 * work through, because that is how this one boots. */
static struct clock_now unset_now(int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.mono_ms = mono_ms;
    return n;
}

/* Everything the shell would lose at a power cut, lost: the runtime starts
 * again and has only the store to go on. */
static void reboot_runtime(void)
{
    clock_runtime_deinit();
    clock_runtime_init(shell_alarm_sync);
}

/* The alert lives on the screen, not under the app, so it is looked for
 * there - which is the point of it. */
static lv_obj_t *on_screen(const char *text)
{
    return find_labelled(lv_screen_active(), text);
}

/* Step the new-alarm form to a given time. Both steppers wrap, so this
 * terminates whatever time the form opened at. */
static void set_form_time(const char *want)
{
    lv_obj_t *form = screen_of(SCREEN_ADD);
    lv_obj_t *hero = hero_in(form);
    int guard;

    for (guard = 0; guard < 24 && strncmp(text_of(hero), want, 2) != 0; guard++) {
        tap_obj(form_cell(FORM_HOUR, STEP_PLUS));
    }
    for (guard = 0; guard < 60 && strcmp(text_of(hero), want) != 0; guard++) {
        tap_obj(form_cell(FORM_MINUTE, STEP_PLUS));
    }
}

/* ---- every screen, laid out ------------------------------------------- */

/* Eight alarms, as long as their labels get, two of them off: the list the
 * Alarm pane has to hold at its fullest. */
static void seed_full_list(void)
{
    static const struct {
        int hour;
        int minute;
        enum clock_repeat repeat;
        const char *label;
    } list[CLOCK_MAX_ALARMS] = {
        { 5, 0, CLOCK_REPEAT_DAILY, "Take the bins out early" },
        { 5, 30, CLOCK_REPEAT_WEEKDAYS, "Feed the cat, then walk" },
        { 6, 0, CLOCK_REPEAT_ONCE, "WWWWWWWWWWWWWWWWWWWWWWW" },
        { 6, 45, CLOCK_REPEAT_DAILY, "Standup call with team" },
        { 12, 0, CLOCK_REPEAT_WEEKDAYS, "Lunch" },
        { 17, 30, CLOCK_REPEAT_ONCE, "Pick up the kids at 5pm" },
        { 21, 15, CLOCK_REPEAT_DAILY, "Medication, evening do" },
        { 23, 59, CLOCK_REPEAT_WEEKDAYS, NULL },
    };
    struct clock_engine *e = clock_runtime_engine();
    int i;

    while (clock_alarm_count(e) > 0) {
        clock_alarm_remove(e, 0);
    }
    for (i = 0; i < CLOCK_MAX_ALARMS; i++) {
        clock_alarm_add(e, list[i].hour, list[i].minute, list[i].repeat, list[i].label, NULL);
    }
    clock_alarm_set_enabled(e, 1, false, NULL);
    clock_alarm_set_enabled(e, 4, false, NULL);
}

static void check_columns(const char *name, const char *pane_name, int pane, const lv_area_t *tabs)
{
    char what[200];
    lv_obj_t *l = column_of(pane, 0);
    lv_obj_t *r = column_of(pane, 1);
    lv_area_t b;
    lv_area_t c0;
    lv_area_t c1;

    body_box(&b);
    area_of(l, &c0);
    area_of(r, &c1);
    if (wide_body()) {
        snprintf(what, sizeof(what), "[%s] %s: two columns across the body, 20 px apart, none under 528 px", name,
                 pane_name);
        check(what, c0.x1 == b.x1 && c1.x2 == b.x2 && c1.x1 - c0.x2 - 1 == POCKETUI_PAD &&
                        lv_area_get_width(&c0) >= COLUMN_W && lv_area_get_width(&c1) >= COLUMN_W &&
                        LV_ABS(lv_area_get_width(&c0) - lv_area_get_width(&c1)) <= 1);
        snprintf(what, sizeof(what), "[%s] %s: both from under the tabs to the foot less the corners, each "
                                     "scrolling itself under a finger", name, pane_name);
        check(what, c0.y1 == tabs->y2 + 1 + POCKETUI_PAD && c1.y1 == c0.y1 && c0.y2 == b.y2 - foot_inset() &&
                        c1.y2 == c0.y2 && has_flag(l, LV_OBJ_FLAG_SCROLLABLE) &&
                        has_flag(r, LV_OBJ_FLAG_SCROLLABLE) && has_flag(l, LV_OBJ_FLAG_CLICKABLE) &&
                        has_flag(r, LV_OBJ_FLAG_CLICKABLE));
    } else {
        snprintf(what, sizeof(what), "[%s] %s: one column the body's width under the tabs, the second under the "
                                     "first", name, pane_name);
        check(what, c0.x1 == b.x1 && c0.x2 == b.x2 && c1.x1 == b.x1 && c1.x2 == b.x2 &&
                        c0.y1 == tabs->y2 + 1 + POCKETUI_PAD && c1.y1 == c0.y2 + 1 + POCKETUI_PAD);
        snprintf(what, sizeof(what), "[%s] %s: neither column scrolls or is pressed", name, pane_name);
        check(what, l && r && !has_flag(l, LV_OBJ_FLAG_SCROLLABLE) && !has_flag(r, LV_OBJ_FLAG_SCROLLABLE) &&
                        !has_flag(l, LV_OBJ_FLAG_CLICKABLE) && !has_flag(r, LV_OBJ_FLAG_CLICKABLE) &&
                        scroll_y(l) == 0 && scroll_y(r) == 0);
    }
}

static void check_main(const char *name)
{
    char what[200];
    lv_obj_t *main_screen = screen_of(SCREEN_MAIN);
    lv_obj_t *face_card;
    lv_obj_t *unset_card;
    lv_area_t b;
    lv_area_t tabs;
    lv_area_t f;
    lv_area_t u;
    lv_area_t c0;
    lv_area_t c1;
    lv_area_t h;
    int wide = wide_body();

    body_box(&b);
    area_of(kid(main_screen, 0), &tabs);
    snprintf(what, sizeof(what), "[%s] the tabs across the top of the body, 64 px tall", name);
    check(what, tabs.x1 == b.x1 && tabs.x2 == b.x2 && tabs.y1 == b.y1 &&
                    lv_area_get_height(&tabs) == POCKETUI_TOUCH_MIN);

    /* The clock face, with the time and without it. */
    tap_obj(find_labelled(app_body, "Clock"));
    face_card = kid(pane_of(PANE_CLOCK), 0);
    unset_card = kid(pane_of(PANE_CLOCK), 1);
    area_of(face_card, &f);
    snprintf(what, sizeof(what), "[%s] a face with a time to show fills the pane, to the foot less the corners",
             name);
    check(what, f.x1 == b.x1 && f.x2 == b.x2 && f.y1 == tabs.y2 + 1 + POCKETUI_PAD &&
                    f.y2 == b.y2 - foot_inset() && has_flag(unset_card, LV_OBJ_FLAG_HIDDEN));
    snprintf(what, sizeof(what), "[%s] Clock", name);
    check_screen(what, main_screen);

    g_wall_unset = 1;
    pump(250);
    area_of(face_card, &f);
    area_of(unset_card, &u);
    check_str("without a time, the face says so", text_of(hero_in(pane_of(PANE_CLOCK))), "--:--");
    if (wide) {
        snprintf(what, sizeof(what), "[%s] without a time, the face and the explanation side by side, halves of "
                                     "the width", name);
        check(what, f.x1 == b.x1 && u.x2 == b.x2 && u.x1 == f.x2 + 1 + POCKETUI_PAD && u.y1 == f.y1 &&
                        f.y1 == tabs.y2 + 1 + POCKETUI_PAD && lv_area_get_width(&f) >= COLUMN_W &&
                        lv_area_get_width(&u) >= COLUMN_W && f.y2 < b.y2 - foot_inset());
    } else {
        snprintf(what, sizeof(what), "[%s] without a time, the face shrinks and the explanation is under it", name);
        check(what, f.x1 == b.x1 && f.x2 == b.x2 && u.x1 == b.x1 && u.x2 == b.x2 &&
                        u.y1 == f.y2 + 1 + POCKETUI_PAD && u.y2 < b.y2 - foot_inset());
    }
    snprintf(what, sizeof(what), "[%s] the explanation is read whole", name);
    check(what, laid_out_whole(pane_of(PANE_CLOCK)) == 0 && in_view(kid(unset_card, 1)));

    /* The alarm list with the time-not-set notice over it: a drag that starts
     * in the gap between the two still scrolls the list. */
    tap_obj(find_labelled(app_body, "Alarm"));
    check_columns(name, "Alarm, time not set", PANE_ALARM, &tabs);
    if (wide) {
        lv_obj_t *col = column_of(PANE_ALARM, 0);
        lv_area_t n;
        lv_area_t list;
        uint32_t sig_before = 0;
        uint32_t sig_after = 0;
        int i;

        area_of(kid(col, 0), &n);
        area_of(kid(col, 1), &list);
        for (i = 0; i < clock_alarm_count(clock_runtime_engine()); i++) {
            sig_before = sig_before * 2u + clock_alarm_at(clock_runtime_engine(), i)->enabled;
        }
        drag_at((n.x1 + n.x2) / 2, n.y2 + 1 + POCKETUI_PAD / 2, -120);
        for (i = 0; i < clock_alarm_count(clock_runtime_engine()); i++) {
            sig_after = sig_after * 2u + clock_alarm_at(clock_runtime_engine(), i)->enabled;
        }
        snprintf(what, sizeof(what), "[%s] a drag from the gap over the list scrolls its column and switches "
                                     "nothing", name);
        check(what, list.y1 == n.y2 + 1 + POCKETUI_PAD && scroll_y(col) > 0 && sig_after == sig_before);
        lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
    }
    snprintf(what, sizeof(what), "[%s] Alarm, time not set, eight alarms", name);
    check_screen(what, main_screen);
    g_wall_unset = 0;
    pump(250);

    /* The alarms, full. */
    check_columns(name, "Alarm", PANE_ALARM, &tabs);
    area_of(column_of(PANE_ALARM, 0), &c0);
    snprintf(what, sizeof(what), "[%s] the list heads the first column and scrolls with it; the full notice "
                                 "heads the second", name);
    check(what, scroller_of(find_labelled(app_body, "05:00")) == (wide ? column_of(PANE_ALARM, 0) : frame_of()) &&
                    lv_obj_get_parent(find_text_anywhere(app_body, "That is all eight alarms. Delete one to add "
                                                                   "another.")) == column_of(PANE_ALARM, 1) &&
                    !label_present(app_body, "Add alarm"));
    snprintf(what, sizeof(what), "[%s] Alarm, eight alarms", name);
    check_screen(what, main_screen);

    /* The stopwatch, with laps. */
    tap_obj(find_labelled(app_body, "Watch"));
    tap_obj(find_labelled(app_body, "Start"));
    tap_obj(find_labelled(app_body, "Lap"));
    tap_obj(find_labelled(app_body, "Lap"));
    tap_obj(find_labelled(app_body, "Lap"));
    tap_obj(find_labelled(app_body, "Lap"));
    tap_obj(find_labelled(app_body, "Lap"));
    tap_obj(find_labelled(app_body, "Pause"));
    check_columns(name, "Watch", PANE_WATCH, &tabs);
    area_of(column_of(PANE_WATCH, 0), &c0);
    area_of(column_of(PANE_WATCH, 1), &c1);
    area_of(hero_card(pane_of(PANE_WATCH)), &h);
    {
        lv_area_t laps;

        area_of(kid(column_of(PANE_WATCH, 1), 1), &laps);
        snprintf(what, sizeof(what), "[%s] the laps grow to the foot less the corners, and scroll themselves", name);
        check(what, laps.y2 == b.y2 - foot_inset() && laps.x2 == c1.x2 &&
                        scroller_of(kid(kid(column_of(PANE_WATCH, 1), 1), 0)) == kid(column_of(PANE_WATCH, 1), 1));
    }
    if (wide) {
        snprintf(what, sizeof(what), "[%s] the running time fills its column", name);
        check(what, h.x1 == c0.x1 && h.x2 == c0.x2 && h.y1 == c0.y1 && h.y2 == c0.y2);
    } else {
        snprintf(what, sizeof(what), "[%s] the running time is as tall as it needs", name);
        check(what, h.x1 == b.x1 && h.x2 == b.x2 && lv_area_get_height(&h) < 200);
    }
    snprintf(what, sizeof(what), "[%s] Watch, five laps", name);
    check_screen(what, main_screen);
    tap_obj(find_labelled(app_body, "Reset"));

    /* The countdown, set and running. */
    tap_obj(find_labelled(app_body, "Timer"));
    check_columns(name, "Timer", PANE_TIMER, &tabs);
    area_of(column_of(PANE_TIMER, 0), &c0);
    area_of(hero_card(pane_of(PANE_TIMER)), &h);
    snprintf(what, sizeof(what), "[%s] the countdown %s", name, wide ? "fills its column" : "is as tall as it needs");
    check(what, wide ? (h.x1 == c0.x1 && h.x2 == c0.x2 && h.y1 == c0.y1 && h.y2 == c0.y2)
                     : (h.x1 == b.x1 && h.x2 == b.x2 && lv_area_get_height(&h) < 200));
    snprintf(what, sizeof(what), "[%s] Timer, set", name);
    check_screen(what, main_screen);
    tap_obj(find_labelled(app_body, "Start"));
    check("the countdown runs", clock_runtime_engine()->timer.state == CLOCK_TIMER_RUNNING);
    snprintf(what, sizeof(what), "[%s] Timer, running", name);
    check_screen(what, main_screen);
    tap_obj(find_labelled(app_body, "Cancel"));
}

/* The new-alarm form, with the keyboard down and up and a label refused. */
static void check_form(const char *name)
{
    char what[200];
    lv_obj_t *form;
    lv_obj_t *hero;
    lv_obj_t *controls;
    lv_obj_t *wrap;
    lv_obj_t *field;
    lv_obj_t *cancel;
    lv_obj_t *add;
    lv_area_t b;
    lv_area_t hr;
    lv_area_t cr;
    lv_area_t fw;
    lv_area_t ar;
    lv_area_t cn;
    lv_area_t ad;
    int wide = wide_body();
    int i;

    tap_obj(find_labelled(app_body, "Alarm"));
    tap_seen(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    check("the form is open", !has_flag(form, LV_OBJ_FLAG_HIDDEN));
    hero = kid(form_when(), 0);
    controls = form_controls();
    wrap = kid(form_what(), 0);
    field = form_field();
    cancel = kid(form_actions(), 0);
    add = kid(form_actions(), 1);
    body_box(&b);
    area_of(hero, &hr);
    area_of(controls, &cr);
    area_of(wrap, &fw);
    area_of(form_actions(), &ar);
    area_of(cancel, &cn);
    area_of(add, &ad);
    if (wide) {
        snprintf(what, sizeof(what), "[%s] the label field heads the form, with Cancel and Add beside it in a "
                                     "288 px rail", name);
        check(what, fw.x1 == b.x1 && fw.y1 == b.y1 && ar.x2 == b.x2 && ar.y1 == b.y1 &&
                        lv_area_get_width(&ar) == RAIL_W && ar.x1 == fw.x2 + 1 + POCKETUI_PAD &&
                        lv_area_get_width(&cn) == 140 && lv_area_get_height(&cn) == PAIRED_BUTTON_H &&
                        lv_area_get_width(&ad) == 140 && lv_area_get_height(&ad) == PAIRED_BUTTON_H &&
                        lv_area_get_width(&fw) >= COLUMN_W && lv_obj_get_height(field) == POCKETUI_ROW_H);
        snprintf(what, sizeof(what), "[%s] under it the time beside Hour, Minute and Repeat, halves of the width, "
                                     "as tall as each other", name);
        check(what, hr.x1 == b.x1 && cr.x2 == b.x2 && cr.x1 == hr.x2 + 1 + POCKETUI_PAD &&
                        hr.y1 == fw.y2 + 1 + POCKETUI_PAD && cr.y1 == hr.y1 && hr.y2 == cr.y2 &&
                        lv_area_get_height(&cr) == 3 * POCKETUI_TOUCH_MIN + 2 * POCKETUI_PAD &&
                        lv_area_get_width(&hr) >= COLUMN_W && lv_area_get_width(&cr) >= COLUMN_W);
        snprintf(what, sizeof(what), "[%s] the form scrolls, and opens at its top", name);
        check(what, has_flag(form, LV_OBJ_FLAG_SCROLLABLE) && scroll_y(form) == 0);
    } else {
        lv_area_t hour;
        lv_area_t minute;
        lv_area_t repeat;

        area_of(kid(controls, 0), &hour);
        area_of(kid(controls, 1), &minute);
        area_of(kid(controls, 2), &repeat);
        snprintf(what, sizeof(what), "[%s] the form down the body in its portrait order, and it does not scroll",
                 name);
        check(what, hr.x1 == b.x1 && hr.x2 == b.x2 && hr.y1 == b.y1 && hour.y1 == hr.y2 + 1 + POCKETUI_PAD &&
                        minute.y1 == hour.y2 + 1 + POCKETUI_PAD && repeat.y1 == minute.y2 + 1 + POCKETUI_PAD &&
                        fw.y1 == repeat.y2 + 1 + POCKETUI_PAD && ar.y1 == fw.y2 + 1 + POCKETUI_PAD &&
                        fw.x1 == b.x1 && fw.x2 == b.x2 && ar.x1 == b.x1 && ar.x2 == b.x2 &&
                        !has_flag(form, LV_OBJ_FLAG_SCROLLABLE));
    }
    snprintf(what, sizeof(what), "[%s] New alarm", name);
    check_screen(what, form);

    /* The keyboard up: the field, Cancel and Add in view above it. */
    tap_seen(field);
    snprintf(what, sizeof(what), "[%s] above the keyboard the focused field, Cancel and Add are all in view", name);
    check(what, pocketos_shell_keyboard_visible() && pos_input_focused() == field && in_view(field) &&
                    in_view(cancel) && in_view(add) && scroll_y(form) == 0);
    type_text("Tea");
    check_str("typing reaches the field", lv_textarea_get_text(field), "Tea");
    snprintf(what, sizeof(what), "[%s] New alarm, keyboard up", name);
    check_screen(what, form);
    if (wide) {
        /* The time is below the fold above a landscape keyboard; a finger
         * scrolls the form to it. */
        const char *before = text_of(hero_in(form));
        char was[8];
        lv_area_t now_b;

        snprintf(was, sizeof(was), "%s", before);
        body_box(&now_b);
        drag_at(now_b.x1 + lv_area_get_width(&now_b) / 4, now_b.y2 - 8, -(lv_area_get_height(&now_b) - 20));
        check("a drag scrolls the form towards the time", scroll_y(form) > 0);
        tap_seen(form_cell(FORM_HOUR, STEP_PLUS));
        check("and Hour is pressed there", strcmp(text_of(hero_in(form)), was) != 0);
        check("with the field still focused", pos_input_focused() == field);
    }

    /* A label too long to store, refused with the form part-way scrolled:
     * the reason is read under the field, above the keyboard. */
    tap_key("?123");
    for (i = 0; i < 11; i++) {
        tap_key("\xC3\xA6"); /* æ */
    }
    tap_key("ABC");
    if (wide) {
        lv_obj_scroll_to_y(form, 30, LV_ANIM_OFF);
        pump(20);
    }
    area_of(add, &ad);
    finger_point.x = (ad.x1 + ad.x2) / 2;
    finger_point.y = ad.y2 - 6;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    drain();
    snprintf(what, sizeof(what), "[%s] a label too long to store is refused: field and reason in view above the "
                                 "keyboard, the form at its top", name);
    check(what, !has_flag(form, LV_OBJ_FLAG_HIDDEN) && pocketos_shell_keyboard_visible() &&
                    !has_flag(kid(wrap, 1), LV_OBJ_FLAG_HIDDEN) && in_view(kid(wrap, 1)) && in_view(field) &&
                    in_view(add) && scroll_y(form) == 0 && laid_out_whole(wrap) == 0);
    check_str("the reason", text_of(kid(wrap, 1)), "That label is too long to store.");
    snprintf(what, sizeof(what), "[%s] New alarm, label refused", name);
    check_screen(what, form);

    for (i = 0; i < 11; i++) {
        tap_key("BKSP");
    }
    tap_seen(add);
    check("with the label shortened, Add adds it and the form closes",
          has_flag(form, LV_OBJ_FLAG_HIDDEN) && !pocketos_shell_keyboard_visible() &&
              clock_alarm_count(clock_runtime_engine()) == CLOCK_MAX_ALARMS);
}

static void check_confirm(const char *name)
{
    char what[200];
    lv_obj_t *panel;
    lv_area_t b;
    lv_area_t p;

    tap_obj(find_labelled(app_body, "Alarm"));
    tap_seen(alarm_row_delete("05:00"));
    panel = kid(screen_of(SCREEN_CONFIRM), 0);
    body_box(&b);
    area_of(panel, &p);
    if (wide_body()) {
        snprintf(what, sizeof(what), "[%s] the confirmation at its portrait width, centred at the top", name);
        check(what, lv_area_get_width(&p) == COLUMN_W && p.x1 - b.x1 == b.x2 - p.x2 && p.y1 == b.y1);
    } else {
        snprintf(what, sizeof(what), "[%s] the confirmation across the top of the body", name);
        check(what, p.x1 == b.x1 && p.x2 == b.x2 && p.y1 == b.y1);
    }
    snprintf(what, sizeof(what), "[%s] and whole in view", name);
    check(what, in_view(panel));
    snprintf(what, sizeof(what), "[%s] Delete this alarm?", name);
    check_screen(what, screen_of(SCREEN_CONFIRM));
    tap_obj(find_labelled(app_body, "Cancel"));
    check("cancelled, the list is back", label_present(app_body, "05:00"));
}

static void check_orientation(const char *name, enum pos_rotation rotation, int32_t corner, const char *mode)
{
    char what[200];
    char why[128];

    use_display(rotation, corner);
    pos_theme_apply(NULL, mode, why, sizeof(why));
    seed_full_list();
    app_start();
    pump(200);
    snprintf(what, sizeof(what), "[%s] the shape is chosen from the body", name);
    check(what, wide_body() == (rotation == POS_ROTATION_270));
    check_main(name);
    /* Seven, so that Add alarm is offered. */
    clock_alarm_remove(clock_runtime_engine(), CLOCK_MAX_ALARMS - 1);
    pump(200);
    check_form(name);
    check_confirm(name);
    app_stop();
    pos_theme_apply(NULL, "normal", why, sizeof(why));
}

int main(void)
{
    lv_indev_t *finger;
    struct clock_engine disk;
    struct clock_now now;
    lv_obj_t *form;
    lv_obj_t *timer_pane;
    lv_obj_t *field;
    char before[24];
    char kept[16];

    snprintf(root, sizeof(root), "/tmp/pocketclock-app-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

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
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    g_keyboard = pos_keyboard_create(lv_screen_active());
    /* Sections 1 to 14 run on the unit's panel as it stands in portrait. */
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* What the shell does: the one alert sheet, then the one runtime, with
     * the sheet listening for a ring. */
    shell_alarm_create(lv_screen_active());
    clock_runtime_init(shell_alarm_sync);
    pump(60);

    /* ---- 1. the four tabs ---------------------------------------------- */

    app_start();
    check("the Clock tab is there", label_present(app_body, "Clock"));
    check("the Alarm tab", label_present(app_body, "Alarm"));
    check("the stopwatch tab", label_present(app_body, "Watch"));
    check("the Timer tab", label_present(app_body, "Timer"));
    check("the clock pane is the one showing",
          pane_of(PANE_CLOCK) && !has_flag(pane_of(PANE_CLOCK), LV_OBJ_FLAG_HIDDEN));
    check("and the others are not", has_flag(pane_of(PANE_TIMER), LV_OBJ_FLAG_HIDDEN));
    check("the keyboard is not up", !pocketos_shell_keyboard_visible());

    /* ---- 2. the clock face tells the truth about the clock ------------- */

    clock_now_read(&now);
    if (now.wall.valid) {
        char expect[16];

        clock_format_wall(&now.wall, expect, sizeof(expect));
        check_str("a board that knows the time shows it",
                  text_of(hero_in(pane_of(PANE_CLOCK))), expect);
        check("and does not show the unset state",
              !label_present(app_body, "Time not set"));
    } else {
        check_str("a board that does not know the time shows none",
                  text_of(hero_in(pane_of(PANE_CLOCK))), "--:--");
        check("and says so in words", label_present(app_body, "Time not set"));
    }

    /* ---- 3. the alarm tab is honest about what an alert can do --------- */

    tap_obj(find_labelled(app_body, "Alarm"));
    check("the alarm list starts empty", label_present(app_body, "No alarms yet"));
    check("and offers to add one", label_present(app_body, "Add alarm"));
    check("the alert capability is stated on screen",
          find_text_anywhere(app_body, clock_alert_why()) != NULL);
    check("so is what an alarm does and does not survive",
          find_text_anywhere(app_body,
                             "Alarms ring with Clock closed. They do not ring "
                             "with the device switched off.") != NULL);

    /* ---- 4. add one, tapping every control ----------------------------- */

    tap_obj(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    check("the form opens", label_present(app_body, "Repeat: Once"));
    check("with an hour row", strcmp(text_of(form_cell(FORM_HOUR, STEP_VALUE)),
                                     "Hour") == 0);
    check("and a minute row", strcmp(text_of(form_cell(FORM_MINUTE, STEP_VALUE)),
                                     "Minute") == 0);

    set_form_time("08:15");
    check_str("the steppers reach the time that was wanted",
              text_of(hero_in(form)), "08:15");
    tap_obj(form_cell(FORM_HOUR, STEP_MINUS));
    check_str("and step back down", text_of(hero_in(form)), "07:15");
    tap_obj(form_cell(FORM_HOUR, STEP_PLUS));

    tap_obj(find_labelled(app_body, "Repeat: Once"));
    check("the repeat cycles", label_present(app_body, "Repeat: Daily"));

    field = form_field();
    check("the form has a label field",
          lv_obj_check_type(field, &lv_textarea_class));
    tap_obj(field);
    check("tapping it brings the keyboard up", pocketos_shell_keyboard_visible());
    check("and focuses the field", pos_input_focused() == field);
    type_text("Tea");
    check_str("what is typed reaches the label", lv_textarea_get_text(field), "Tea");

    tap_obj(find_labelled(app_body, "Add"));
    check("adding puts the keyboard away", !pocketos_shell_keyboard_visible());
    check("the alarm is in the list", label_present(app_body, "08:15"));
    check("with its repeat and label", label_present(app_body, "Daily  Tea"));
    check("and it is on", label_present(app_body, "On"));
    check("the empty state is gone", !label_present(app_body, "No alarms yet"));

    check("it was written to the store", stored(&disk) == 0);
    check("as one alarm", clock_alarm_count(&disk) == 1);
    check("at the time that was set", clock_alarm_at(&disk, 0)->hour == 8 &&
                                      clock_alarm_at(&disk, 0)->minute == 15);
    check_str("with the label that was typed", clock_alarm_at(&disk, 0)->label, "Tea");
    check("and daily", clock_alarm_at(&disk, 0)->repeat == CLOCK_REPEAT_DAILY);

    /* ---- 5. tapping the row switches it off, and that is saved too ----- */

    tap_obj(find_labelled(app_body, "08:15"));
    check("tapping the row switches it off", label_present(app_body, "Off"));
    check("in words, not by a colour alone", !label_present(app_body, "On"));
    stored(&disk);
    check("and the store followed", !clock_alarm_at(&disk, 0)->enabled);
    tap_obj(find_labelled(app_body, "08:15"));
    check("and back on again", label_present(app_body, "On"));

    /* ---- 6. deleting asks first (DS 17.5) ------------------------------ */

    tap_obj(alarm_row_delete("08:15"));
    check("the confirmation opens", label_present(app_body, "Delete this alarm?"));
    check("the list is not shown behind it", !label_present(app_body, "Add alarm"));
    tap_obj(find_labelled(app_body, "Cancel"));
    check("cancelling leaves the alarm alone", label_present(app_body, "08:15"));
    stored(&disk);
    check("and the store untouched", clock_alarm_count(&disk) == 1);

    tap_obj(alarm_row_delete("08:15"));
    tap_obj(find_labelled(app_body, "Delete"));
    check("confirming removes it", !label_present(app_body, "08:15"));
    check("and the empty state is back", label_present(app_body, "No alarms yet"));
    stored(&disk);
    check("and it is gone from the store", clock_alarm_count(&disk) == 0);

    /* ---- 7. an alarm survives leaving the app, and a power cut --------- */

    tap_obj(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    set_form_time("06:30");
    snprintf(kept, sizeof(kept), "%s", text_of(hero_in(form)));
    tap_obj(find_labelled(app_body, "Add"));
    check("an alarm with no label can be added", label_present(app_body, kept));
    check("and needs no label to show its repeat", label_present(app_body, "Once"));

    /* Closing the app leaves the runtime holding it. */
    app_stop();
    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("it is still there after the app is closed and reopened",
          label_present(app_body, kept));

    /* And a power cut takes the runtime with it, so this time the alarm has
     * to come back off the disk. */
    app_stop();
    reboot_runtime();
    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("and after the shell itself has restarted",
          label_present(app_body, kept));
    stored(&disk);
    check("one alarm on disk", clock_alarm_count(&disk) == 1);
    tap_obj(alarm_row_delete(kept));
    tap_obj(find_labelled(app_body, "Delete"));
    check("and can be deleted again afterwards",
          label_present(app_body, "No alarms yet"));

    /* ---- 8. the stopwatch ---------------------------------------------- */

    tap_obj(find_labelled(app_body, "Watch"));
    check_str("the stopwatch starts at zero",
              text_of(hero_in(pane_of(PANE_WATCH))), "00:00.00");
    check("and offers Start", label_present(app_body, "Start"));
    check("and Reset", label_present(app_body, "Reset"));

    tap_obj(find_labelled(app_body, "Start"));
    check("starting turns it into Pause", label_present(app_body, "Pause"));
    check("and Reset into Lap", label_present(app_body, "Lap"));
    /* The stopwatch reads the real monotonic clock, which pumping LVGL ticks
     * does not move: a thousand pumps can still be under a hundredth of a
     * second of real time. So this waits for three real hundredths to have
     * passed - thirty milliseconds, not the seconds the brief ruled out -
     * and then asks what the display says. */
    {
        struct clock_now t0;
        struct clock_now t1;

        clock_now_read(&t0);
        do {
            pump(20);
            clock_now_read(&t1);
        } while (t1.mono_ms - t0.mono_ms < 30);
        snprintf(before, sizeof(before), "%s",
                 text_of(hero_in(pane_of(PANE_WATCH))));
    }
    check("a running stopwatch is no longer at zero",
          strcmp(before, "00:00.00") != 0);

    tap_obj(find_labelled(app_body, "Lap"));
    check("a lap is listed", label_present(app_body, "1"));
    tap_obj(find_labelled(app_body, "Pause"));
    check("pausing offers Start again", label_present(app_body, "Start"));
    check("and Reset", label_present(app_body, "Reset"));
    snprintf(before, sizeof(before), "%s", text_of(hero_in(pane_of(PANE_WATCH))));
    pump(400);
    check_str("and a paused stopwatch stays where it stopped",
              text_of(hero_in(pane_of(PANE_WATCH))), before);
    tap_obj(find_labelled(app_body, "Reset"));
    check_str("reset returns it to zero",
              text_of(hero_in(pane_of(PANE_WATCH))), "00:00.00");
    check("and clears the laps", !label_present(app_body, "1"));

    /* ---- 9. the timer -------------------------------------------------- */

    tap_obj(find_labelled(app_body, "Timer"));
    timer_pane = pane_of(PANE_TIMER);
    check_str("the minute row starts at zero",
              text_of(stepper_cell(timer_setter(), 0, STEP_VALUE)),
              "Minutes 00");
    check_str("and the second row",
              text_of(stepper_cell(timer_setter(), 1, STEP_VALUE)),
              "Seconds 00");

    tap_obj(stepper_cell(timer_setter(), 0, STEP_PLUS));
    check_str("a minute is added",
              text_of(stepper_cell(timer_setter(), 0, STEP_VALUE)),
              "Minutes 01");
    tap_obj(stepper_cell(timer_setter(), 1, STEP_PLUS_BIG));
    check_str("and ten seconds",
              text_of(stepper_cell(timer_setter(), 1, STEP_VALUE)),
              "Seconds 10");
    check_str("the countdown shows what was set",
              text_of(hero_in(timer_pane)), "01:10");
    tap_obj(stepper_cell(timer_setter(), 0, STEP_MINUS_BIG));
    check_str("stepping below zero clamps rather than wrapping round",
              text_of(stepper_cell(timer_setter(), 0, STEP_VALUE)),
              "Minutes 00");
    tap_obj(stepper_cell(timer_setter(), 0, STEP_PLUS));

    tap_obj(find_labelled(app_body, "Start"));
    check("starting hides the setter",
          has_flag(timer_setter(), LV_OBJ_FLAG_HIDDEN));
    check("and offers Pause", label_present(app_body, "Pause"));
    check("and Cancel", label_present(app_body, "Cancel"));
    stored(&disk);
    check("the duration is a setting and was saved",
          disk.timer.duration_ms == 70000);
    check("but what is left of the countdown is not",
          disk.timer.remaining_ms == disk.timer.duration_ms);

    tap_obj(find_labelled(app_body, "Pause"));
    check("pausing offers Start again", label_present(app_body, "Start"));
    tap_obj(find_labelled(app_body, "Cancel"));
    check("cancelling brings the setter back",
          timer_setter() && !has_flag(timer_setter(), LV_OBJ_FLAG_HIDDEN));
    check_str("with the duration that was set",
              text_of(stepper_cell(timer_setter(), 0, STEP_VALUE)),
              "Minutes 01");

    /* ---- 10. the alert is the shell's, and rings with the app shut ----- */

    /* This is the thing the whole runtime exists for, so it is tested with
     * PocketClock closed: no app, no app timer, nothing of the Clock UI on
     * screen at all. The clock is injected, because 07:30 is not worth
     * waiting for. */
    app_stop();
    check("no app is running", lv_obj_get_child_count(g_content) == 0);
    check("and nothing is alerting", !shell_alarm_visible());

    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now;

        clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, "Wake up", NULL);
        clock_runtime_save();

        now = at(20260911, 7, 0, 5, 1000);
        clock_runtime_step_at(&now);
        pump(60);
        check("still quiet before its time", !shell_alarm_visible());

        now = at(20260911, 7, 30, 5, 1800000);
        clock_runtime_step_at(&now);
        pump(60);
        check("the alarm rings with PocketClock closed", shell_alarm_visible());
        check("and says what it is",
              find_text_anywhere(lv_screen_active(), "Alarm") != NULL);
        check("and which alarm it was",
              find_text_anywhere(lv_screen_active(), "07:30  Wake up") != NULL);
        check("with Stop on it", on_screen("Stop") != NULL);
        check("and a snooze", on_screen("Snooze 9 min") != NULL);
        check("and what an alert can do on this board",
              find_text_anywhere(lv_screen_active(), clock_alert_why()) != NULL);

        /* Ticking through the same minute must not produce a second alert. */
        clock_runtime_step_at(&now);
        clock_runtime_step_at(&now);
        pump(60);
        check("and it is still one alert", shell_alarm_visible());

        tap_obj(on_screen("Stop"));
        check("Stop puts it away", !shell_alarm_visible());
        check("and acknowledges it", e->ringing == CLOCK_RING_NONE);

        now = at(20260911, 7, 31, 5, 1860000);
        clock_runtime_step_at(&now);
        pump(60);
        check("an acknowledged alarm does not come back",
              !shell_alarm_visible());
    }

    /* ---- 11. and it interrupts whatever is on screen -------------------- */

    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("PocketClock is up", label_present(app_body, "Add alarm"));
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now = at(20260912, 7, 30, 6, 90000000);

        clock_runtime_step_at(&now);
        pump(60);
        check("the alert covers the app it is over", shell_alarm_visible());
        check("and the app is not the one drawing it",
              find_text_anywhere(app_body, "Snooze 9 min") == NULL);
        check("PocketClock has no ringing screen of its own",
              find_text_anywhere(app_body, "Stop") == NULL);

        tap_obj(on_screen("Stop"));
        check("Stop from over the app works too", !shell_alarm_visible());
        check("and acknowledged it", e->ringing == CLOCK_RING_NONE);
        check("PocketClock is where it was", label_present(app_body, "Add alarm"));
    }

    /* A one-shot alarm switches itself off when the shell acknowledges it.
     * The list on screen was drawn before that happened, and it has to
     * notice - otherwise the owner is looking at a row that says On for an
     * alarm that will never ring again. */
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now;

        clock_alarm_add(e, 9, 0, CLOCK_REPEAT_ONCE, "once only", NULL);
        pump(200);
        check("the new alarm appears without the app being told",
              label_present(app_body, "09:00"));
        check("switched on", label_present(app_body, "On"));
        check("and none are off yet", !label_present(app_body, "Off"));

        now = at(20260912, 9, 0, 6, 95000000);
        clock_runtime_step_at(&now);
        pump(60);
        check("it rings over the app", shell_alarm_visible());
        tap_obj(on_screen("Stop"));
        pump(200);
        check("and the list on screen notices it switched itself off",
              label_present(app_body, "Off"));
        check("without anyone having touched the app",
              !shell_alarm_visible());
    }

    /* ---- 12. the countdown finishes in the background too -------------- */

    app_stop();
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now = unset_now(500000);

        check("a countdown is set", clock_timer_set(e, 0, 1, 0));
        check("and started", clock_timer_start(e, &now));

        now = unset_now(500000 + 59000);
        clock_runtime_step_at(&now);
        pump(60);
        check("it has not finished", !shell_alarm_visible());

        now = unset_now(500000 + 60000);
        clock_runtime_step_at(&now);
        pump(60);
        check("the timer rings with PocketClock closed", shell_alarm_visible());
        check("and says so",
              find_text_anywhere(lv_screen_active(), "Timer finished") != NULL);
        check("there is nothing to snooze on a countdown",
              on_screen("Snooze 9 min") == NULL);
        check("but there is a Stop", on_screen("Stop") != NULL);
        check("none of which needed a wall clock",
              !clock_runtime_now()->wall.valid);

        tap_obj(on_screen("Stop"));
        check("Stop dismisses it", !shell_alarm_visible());
        check("and returns the timer to idle", e->timer.state == CLOCK_TIMER_IDLE);
    }

    /* ---- 13. switching an alarm back on after its time ------------------ */

    /* An alarm switched off before its time and on again after it rang the
     * moment its row was tapped (P1-3 of the v0.0.8 review). The app reads
     * the real clock on its refresh, so the alarm sits at the real current
     * minute - which counts as gone by - and the runtime is stepped with a
     * real reading, the way the shell's tick steps it. */
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now real;
        char hm[8];

        clock_now_read(&real);
        if (!real.wall.valid) {
            printf("note: the host clock is not set, so the re-enable check cannot run\n");
        } else {
            while (clock_alarm_count(e) > 0) {
                clock_alarm_remove(e, 0);
            }
            /* The clock has been real since before this alarm was set, as it
             * is on a board whose time was set at boot. */
            clock_runtime_step_at(&real);
            clock_alarm_add(e, real.wall.hour, real.wall.minute, CLOCK_REPEAT_DAILY,
                            "back on", NULL);
            clock_format_hm(real.wall.hour, real.wall.minute, hm, sizeof(hm));

            app_start();
            tap_obj(find_labelled(app_body, "Alarm"));
            tap_obj(find_labelled(app_body, hm));
            check("a tap switches it off", label_present(app_body, "Off"));
            tap_obj(find_labelled(app_body, hm));
            check("and a second tap on again, after its time",
                  label_present(app_body, "On"));
            clock_now_read(&real);
            clock_runtime_step_at(&real);
            pump(60);
            check("switched on after its time, it does not ring on the spot",
                  !shell_alarm_visible());
            check("and nothing is ringing", e->ringing == CLOCK_RING_NONE);
            check("it stays on, for its next time",
                  clock_alarm_at(e, 0) && clock_alarm_at(e, 0)->enabled);
            app_stop();
            while (clock_alarm_count(e) > 0) {
                clock_alarm_remove(e, 0);
            }
            clock_runtime_save();
        }
    }

    /* ---- 14. opening and closing repeatedly ---------------------------- */

    app_start();
    app_stop();
    check("the app can be opened and closed without leaving a timer behind",
          lv_obj_get_child_count(g_content) == 0);

    /* ---- 15. portrait is where it was --------------------------------- */

    /* The v0.0.10 places, in Normal type, with square corners and with the
     * unit's 30 px ones: nothing but the foot of what reaches it moves. */
    {
        const int32_t corners[2] = { 0, PANEL_CORNER };
        int c;

        for (c = 0; c < 2; c++) {
            use_display(POS_ROTATION_0, corners[c]);
            seed_full_list();
            clock_alarm_remove(clock_runtime_engine(), CLOCK_MAX_ALARMS - 1);
            app_start();
            check_rect("portrait: the tabs", kid(screen_of(SCREEN_MAIN), 0), 20, V010_ROW(152), 547, V010_ROW(215));
            tap_obj(find_labelled(app_body, "Alarm"));
            check_rect("portrait: the alarm list", column_of(PANE_ALARM, 0), 20, V010_ROW(236), 547, V010_ROW(685));
            check_rect("portrait: Add alarm under it", find_labelled(app_body, "Add alarm"), 20, V010_ROW(706), 547, V010_ROW(769));
            tap_obj(find_labelled(app_body, "Add alarm"));
            check_rect("portrait: the new alarm's time", kid(form_when(), 0), 20, V010_ROW(152), 547, V010_ROW(261));
            check_rect("portrait: Hour", kid(form_controls(), 0), 20, V010_ROW(282), 547, V010_ROW(345));
            check_rect("portrait: Minute", kid(form_controls(), 1), 20, V010_ROW(366), 547, V010_ROW(429));
            check_rect("portrait: Repeat", kid(form_controls(), 2), 20, V010_ROW(450), 547, V010_ROW(513));
            check_rect("portrait: the label field", form_field(), 20, V010_ROW(534), 547, V010_ROW(597));
            check_rect("portrait: Cancel and Add", form_actions(), 20, V010_ROW(618), 547, V010_ROW(673));
            tap_obj(find_labelled(app_body, "Cancel"));
            tap_obj(alarm_row_delete("05:00"));
            check_rect("portrait: the confirmation", kid(screen_of(SCREEN_CONFIRM), 0), 20, V010_ROW(152), 547, V010_ROW(331));
            tap_obj(find_labelled(app_body, "Cancel"));
            tap_obj(find_labelled(app_body, "Clock"));
            check_rect("portrait: the clock face grows to the foot, less the corners", kid(pane_of(PANE_CLOCK), 0),
                       20, V010_ROW(236), 547, 1211 - (corners[c] ? 10 : 0));
            app_stop();
        }
    }

    /* ---- 16. every screen, both orientations, both corners, both modes - */

    check_orientation("portrait", POS_ROTATION_0, PANEL_CORNER, "normal");
    check_orientation("landscape", POS_ROTATION_270, PANEL_CORNER, "normal");
    check_orientation("portrait, square corners", POS_ROTATION_0, 0, "normal");
    check_orientation("landscape, square corners", POS_ROTATION_270, 0, "normal");
    check_orientation("portrait, Outdoor", POS_ROTATION_0, PANEL_CORNER, "outdoor");
    check_orientation("landscape, Outdoor", POS_ROTATION_270, PANEL_CORNER, "outdoor");

    /* ---- 17. the width the wide shape needs, and a pixel less ----------- */

    /* A landscape body exactly two portrait columns and the gutter wide gets
     * them side by side; one pixel narrower stacks them. */
    {
        lv_area_t c0;
        lv_area_t c1;

        use_panel(2 * COLUMN_W + POCKETUI_PAD + 2 * POCKETUI_PAD, POS_ROTATION_270, PANEL_CORNER);
        seed_full_list();
        app_start();
        tap_obj(find_labelled(app_body, "Alarm"));
        area_of(column_of(PANE_ALARM, 0), &c0);
        area_of(column_of(PANE_ALARM, 1), &c1);
        check("a body 1076 px wide takes two 528 px columns side by side",
              wide_body() && c1.y1 == c0.y1 && lv_area_get_width(&c0) == COLUMN_W &&
                  lv_area_get_width(&c1) == COLUMN_W);
        use_panel(2 * COLUMN_W + POCKETUI_PAD + 2 * POCKETUI_PAD - 1, POS_ROTATION_270, PANEL_CORNER);
        area_of(column_of(PANE_ALARM, 0), &c0);
        area_of(column_of(PANE_ALARM, 1), &c1);
        check("a body 1075 px wide keeps them one under the other",
              !wide_body() && c1.y1 > c0.y2 && c0.x1 == c1.x1);
        /* Wide enough, but taller than it is wide - a square panel, say - is
         * still a tall body. */
        use_panel_sized(PANEL_H, PANEL_H + 280, POS_ROTATION_0, PANEL_CORNER);
        area_of(column_of(PANE_ALARM, 0), &c0);
        area_of(column_of(PANE_ALARM, 1), &c1);
        check("a body 1192 px wide and taller still keeps them one under the other",
              !wide_body() && c1.y1 > c0.y2 && c0.x1 == c1.x1 && lv_area_get_width(&c0) == PANEL_H - 40);
        app_stop();
    }

    /* ---- 18. turning the display under the open app --------------------- */

    /* A change of shape moves objects and nothing else: what is running keeps
     * running, what is typed stays typed and focused, a confirmation stays
     * for the alarm it was about, no second set of controls appears, and
     * nothing is written to the store. */
    {
        struct clock_engine *e = clock_runtime_engine();
        char path[256];
        char disk_before[1024];
        char disk_after[1024];
        FILE *f;
        size_t n;
        int objects;
        int turn;
        lv_obj_t *label_field;

        use_display(POS_ROTATION_0, PANEL_CORNER);
        seed_full_list();
        clock_alarm_remove(e, CLOCK_MAX_ALARMS - 1);
        clock_alarm_remove(e, CLOCK_MAX_ALARMS - 2);
        clock_runtime_save();
        clock_store_path(path, sizeof(path));
        f = fopen(path, "rb");
        n = f ? fread(disk_before, 1, sizeof(disk_before) - 1, f) : 0;
        disk_before[n] = '\0';
        if (f) {
            fclose(f);
        }
        app_start();

        /* The stopwatch, running, with laps. */
        tap_obj(find_labelled(app_body, "Watch"));
        tap_obj(find_labelled(app_body, "Start"));
        tap_obj(find_labelled(app_body, "Lap"));
        tap_obj(find_labelled(app_body, "Lap"));
        objects = count_objects(app_body);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        check("turned to landscape, the stopwatch is still running",
              e->sw.state == CLOCK_SW_RUNNING && label_present(app_body, "Pause") &&
                  label_present(app_body, "Lap"));
        check("with its two laps listed once",
              lv_obj_get_child_count(kid(column_of(PANE_WATCH, 1), 1)) == 2u && e->sw.lap_count == 2);
        check("on the Watch tab, still selected",
              !has_flag(pane_of(PANE_WATCH), LV_OBJ_FLAG_HIDDEN) &&
                  lv_obj_has_state(kid(kid(screen_of(SCREEN_MAIN), 0), PANE_WATCH), LV_STATE_CHECKED));
        check("and no control made twice", count_objects(app_body) == objects);
        use_display(POS_ROTATION_0, PANEL_CORNER);
        check("turned back, still running with the same laps, and the same objects",
              e->sw.state == CLOCK_SW_RUNNING && e->sw.lap_count == 2 && count_objects(app_body) == objects &&
                  lv_obj_get_child_count(kid(column_of(PANE_WATCH, 1), 1)) == 2u);
        tap_obj(find_labelled(app_body, "Pause"));
        tap_obj(find_labelled(app_body, "Reset"));

        /* The countdown, running. */
        tap_obj(find_labelled(app_body, "Timer"));
        tap_obj(find_labelled(app_body, "Start"));
        use_display(POS_ROTATION_270, PANEL_CORNER);
        check("turned, the countdown is still running with its setter put away",
              e->timer.state == CLOCK_TIMER_RUNNING && label_present(app_body, "Pause") &&
                  has_flag(timer_setter(), LV_OBJ_FLAG_HIDDEN) && in_view(find_labelled(app_body, "Cancel")));
        tap_seen(find_labelled(app_body, "Cancel"));
        check("and Cancel, pressed in landscape, stops it", e->timer.state == CLOCK_TIMER_IDLE);

        /* Scrolled in landscape, then turned: portrait starts from the top,
         * and nothing that scrolls only in landscape scrolls in portrait. */
        tap_obj(find_labelled(app_body, "Alarm"));
        {
            lv_area_t col;
            lv_area_t cn;

            area_of(column_of(PANE_ALARM, 0), &col);
            drag_at((col.x1 + col.x2) / 2, col.y2 - 20, -150);
            check("in landscape a finger scrolls the alarm list, and switches nothing",
                  scroll_y(column_of(PANE_ALARM, 0)) > 0 && clock_alarm_at(e, 0)->enabled &&
                      !clock_alarm_at(e, 1)->enabled);
            tap_obj(find_labelled(app_body, "Add alarm"));
            tap_obj(form_field());
            lv_obj_scroll_to_y(screen_of(SCREEN_ADD), 30, LV_ANIM_OFF);
            pump(20);
            area_of(kid(form_actions(), 0), &cn);
            finger_point.x = (cn.x1 + cn.x2) / 2;
            finger_point.y = cn.y2 - 6;
            finger_state = LV_INDEV_STATE_PRESSED;
            pump(60);
            finger_state = LV_INDEV_STATE_RELEASED;
            pump(60);
            drain();
            check("Cancel, pressed on a part-scrolled form, closes it",
                  has_flag(screen_of(SCREEN_ADD), LV_OBJ_FLAG_HIDDEN) && !pocketos_shell_keyboard_visible());
            tap_obj(find_labelled(app_body, "Add alarm"));
            check("and the next new alarm opens at the top of the form, its field in view",
                  scroll_y(screen_of(SCREEN_ADD)) == 0 && in_view(form_field()));
            tap_obj(find_labelled(app_body, "Cancel"));
        }
        use_display(POS_ROTATION_0, PANEL_CORNER);
        {
            lv_area_t list;

            area_of(kid(column_of(PANE_ALARM, 0), 1), &list);
            check("turned back to portrait, the alarm list starts at its top again",
                  scroll_y(column_of(PANE_ALARM, 0)) == 0 && list.y1 == V010_ROW(236));
            check("and neither its columns nor the form scroll or take a finger",
                  !has_flag(column_of(PANE_ALARM, 0), LV_OBJ_FLAG_SCROLLABLE) &&
                      !has_flag(column_of(PANE_ALARM, 1), LV_OBJ_FLAG_SCROLLABLE) &&
                      !has_flag(column_of(PANE_ALARM, 0), LV_OBJ_FLAG_CLICKABLE) &&
                      !has_flag(column_of(PANE_ALARM, 1), LV_OBJ_FLAG_CLICKABLE) &&
                      !has_flag(screen_of(SCREEN_ADD), LV_OBJ_FLAG_SCROLLABLE) &&
                      scroll_y(screen_of(SCREEN_ADD)) == 0);
        }

        /* A new alarm half made, the keyboard up and the label typed. */
        tap_obj(find_labelled(app_body, "Alarm"));
        tap_obj(find_labelled(app_body, "Add alarm"));
        set_form_time("09:45");
        tap_obj(find_labelled(app_body, "Repeat: Once"));
        label_field = form_field();
        tap_obj(label_field);
        type_text("Tea");
        objects = count_objects(app_body);
        for (turn = 0; turn < 2; turn++) {
            char what[160];

            use_display(turn == 0 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            snprintf(what, sizeof(what), "turned to %s, the form keeps its time, repeat, label, caret and focus",
                     turn == 0 ? "landscape" : "portrait");
            check(what, !has_flag(screen_of(SCREEN_ADD), LV_OBJ_FLAG_HIDDEN) &&
                            strcmp(text_of(hero_in(screen_of(SCREEN_ADD))), "09:45") == 0 &&
                            label_present(app_body, "Repeat: Daily") &&
                            strcmp(lv_textarea_get_text(label_field), "Tea") == 0 &&
                            lv_textarea_get_cursor_pos(label_field) == 3 && pos_input_focused() == label_field &&
                            pocketos_shell_keyboard_visible());
            snprintf(what, sizeof(what), "and the field is in view above the keyboard, with nothing made twice");
            check(what, in_view(label_field) && count_objects(app_body) == objects);
        }
        use_display(POS_ROTATION_270, PANEL_CORNER);
        tap_seen(find_labelled(app_body, "Add"));
        check("Add, pressed in landscape, adds that alarm",
              !pocketos_shell_keyboard_visible() && label_present(app_body, "09:45") &&
                  label_present(app_body, "Daily  Tea"));
        {
            struct clock_engine disk2;
            int i;
            int found = 0;

            stored(&disk2);
            for (i = 0; i < clock_alarm_count(&disk2); i++) {
                const struct clock_alarm *al = clock_alarm_at(&disk2, i);

                found += al->hour == 9 && al->minute == 45 && al->repeat == CLOCK_REPEAT_DAILY &&
                         strcmp(al->label, "Tea") == 0;
            }
            check("and the store has it, once", found == 1 && clock_alarm_count(&disk2) == 7);
        }

        /* A confirmation, for one alarm, through a turn. */
        tap_seen(alarm_row_delete("09:45"));
        check("the confirmation opens in landscape", label_present(app_body, "Delete this alarm?"));
        use_display(POS_ROTATION_0, PANEL_CORNER);
        check("and is still open in portrait", label_present(app_body, "Delete this alarm?"));
        tap_obj(find_labelled(app_body, "Delete"));
        {
            struct clock_engine disk2;

            stored(&disk2);
            check("Delete removes the alarm it was opened for, and only that one",
                  !label_present(app_body, "09:45") && label_present(app_body, "05:00") &&
                      clock_alarm_count(&disk2) == 6);
        }

        /* Many turns, nothing new, nothing written. */
        tap_obj(find_labelled(app_body, "Clock"));
        objects = count_objects(app_body);
        f = fopen(path, "rb");
        n = f ? fread(disk_before, 1, sizeof(disk_before) - 1, f) : 0;
        disk_before[n] = '\0';
        if (f) {
            fclose(f);
        }
        for (turn = 0; turn < 6; turn++) {
            use_display(turn % 2 ? POS_ROTATION_0 : POS_ROTATION_270, turn % 3 ? PANEL_CORNER : 0);
        }
        f = fopen(path, "rb");
        n = f ? fread(disk_after, 1, sizeof(disk_after) - 1, f) : 0;
        disk_after[n] = '\0';
        if (f) {
            fclose(f);
        }
        check("six turns later there are exactly as many objects", count_objects(app_body) == objects);
        check("and the store is byte for byte what it was", strcmp(disk_before, disk_after) == 0 && n > 0);
        app_stop();
        use_display(POS_ROTATION_0, PANEL_CORNER);
    }

    wipe();
    printf("clock_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
