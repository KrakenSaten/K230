/*
 * Files in the running app, driven by a real LVGL pointer device and a real
 * touch keyboard, against a real (temporary) directory tree.
 *
 * Every operation is done the way a finger does it - a row tapped to select,
 * tapped again to open, a name typed on the keyboard, Paste here pressed in
 * another folder - and what is then checked is what is on disk, as well as
 * what the screen says about it.
 *
 * The app is hosted the way the shell hosts it (ui/shell/shell.c app_open):
 * the reference panel with its 30 px rounded corners, portrait and
 * landscape, the status chrome the shell would give it, and the keyboard
 * entry points of app.h implemented here against the real pos_keyboard.
 *
 * HOME points Files at the tree; POCKETOS_STATE_DIR makes one folder in it
 * Doors' own data, which the default policy must keep read-only.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/files_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "files_fs.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define STATUS_H chrome_height(chrome_resolve(app_files.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_files;

static int failed;
static int checks;
static char root[128];
static char home[256];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the shell's side of app.h ------------------------------------------------ */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static void (*g_done)(void *user);
static void *g_done_user;
static char g_logged[256];

void pocketos_shell_set_status_hint(const char *text) { (void)text; }
void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_logged, sizeof(g_logged), fmt, ap);
    va_end(ap);
}

/* The shell's own dispatch of Done (shell.c on_keyboard_done). */
static void on_kb_done(void *user)
{
    (void)user;
    if (g_done) {
        g_done(g_done_user);
    }
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    g_done = on_done;
    g_done_user = user;
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE ? POS_KB_RETURN_NEWLINE
                                                                   : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, pocketui_display_geometry()->height - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, pocketui_display_geometry()->height - STATUS_H);
    g_done = NULL;
    g_done_user = NULL;
}

int pocketos_shell_keyboard_visible(void)
{
    return pos_keyboard_is_shown(g_keyboard);
}

/* ---- display and finger -------------------------------------------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 4];
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
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
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
    lv_obj_t *k = pos_keyboard_key(g_keyboard, label);

    if (!k) {
        printf("FAIL no key %s\n", label);
        failed++;
        checks++;
        return;
    }
    tap_obj(k);
}

/* Typed as a person would: capitals with Shift, digits and punctuation from
 * the symbol layer and back. */
static void type_text(const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        one[0] = *s;
        if (*s == ' ') {
            tap_key("SPACE");
            continue;
        }
        if ((*s >= '0' && *s <= '9') || strchr(".-,:;()$&@?!/", *s)) {
            tap_key("?123");
            tap_key(one);
            tap_key("ABC");
            continue;
        }
        if (*s >= 'A' && *s <= 'Z') {
            tap_key("SHIFT");
        }
        tap_key(one);
    }
}

static void clear_field(void)
{
    int i;

    for (i = 0; i < 40; i++) {
        tap_key("BKSP");
    }
}

/* ---- finding things -------------------------------------------------------------- */

static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

/* The shown label whose text is text (or, with part, contains it). A hidden
 * screen is still in the tree; a finger cannot press what is not shown. */
static lv_obj_t *find_label_ex(lv_obj_t *obj, const char *text, int part)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && (part ? strstr(t, text) != NULL : strcmp(t, text) == 0)) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_label_ex(lv_obj_get_child(obj, i), text, part);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static lv_obj_t *find_label(const char *text)
{
    return find_label_ex(app_body, text, 0);
}

static int shows(const char *text)
{
    return find_label(text) != NULL;
}

static int shows_part(const char *text)
{
    return find_label_ex(app_body, text, 1) != NULL;
}

/* What a finger presses for a label: the button it is on, or the list row.
 * Every plain LVGL container is clickable too, so neither "clickable" nor
 * "has an event" picks out a target: a button is a button, and a row is a
 * row-high object with an event in a scrolling list. A disabled button is
 * still returned, so a tap on it can be seen to do nothing. */
static lv_obj_t *pressable(lv_obj_t *lb)
{
    lv_obj_t *p;

    for (p = lb; p; p = lv_obj_get_parent(p)) {
        lv_obj_t *parent = lv_obj_get_parent(p);

        if (lv_obj_check_type(p, &lv_button_class)) {
            return p;
        }
        if (parent && lv_obj_get_event_count(p) > 0 && lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE) &&
            lv_obj_get_height(p) == POCKETUI_ROW_H) {
            return p;
        }
    }
    return NULL;
}

/* Every shown label, for a lookup that missed (FILES_TEST_DEBUG=1). */
static void dump_labels(lv_obj_t *obj)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        printf(" [%s]", lv_label_get_text(obj));
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        dump_labels(lv_obj_get_child(obj, i));
    }
}

/* The first shown label with this text that is on something to press, so a
 * screen's title "Rename" is passed over for its Rename button. */
static lv_obj_t *find_pressable(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0 && pressable(obj)) {
            return pressable(obj);
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_pressable(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static lv_obj_t *find_labelled(const char *text)
{
    lv_obj_t *hit;

    lv_obj_update_layout(app_body);
    hit = find_pressable(app_body, text);
    if (!hit && getenv("FILES_TEST_DEBUG")) {
        printf("note: nothing to press says \"%s\"; shown:", text);
        dump_labels(app_body);
        printf("\n");
    }
    return hit;
}

/* A row, by the name it shows. */
static lv_obj_t *row(const char *name)
{
    return find_labelled(name);
}

static lv_obj_t *find_field(lv_obj_t *obj)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_field(lv_obj_get_child(obj, i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* A button by its label that can be pressed now. */
static int enabled(const char *label)
{
    lv_obj_t *b = find_labelled(label);

    return b && lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE) && lv_obj_check_type(b, &lv_button_class);
}

/* The path bar: the label that shows the folder, whichever it is. */
static int path_ends_with(const char *tail)
{
    return shows_part(tail) && find_label_ex(app_body, "/", 1) != NULL;
}

/* ---- the app, hosted --------------------------------------------------------------- */

static void use_display(enum pos_rotation rotation)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { PANEL_CORNER, PANEL_CORNER, PANEL_CORNER, PANEL_CORNER },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width,
                    g.height - STATUS_H - (pos_keyboard_is_shown(g_keyboard) ? POS_KB_H : 0));
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
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
    app_priv = app_files.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_files.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

/* Pump until the status line says one of the things a finished job says,
 * letting the worker run in real time. */
static int wait_for(const char *text)
{
    int i;

    for (i = 0; i < 2500; i++) {
        pump(10);
        if (shows_part(text)) {
            return 1;
        }
        usleep(2000);
    }
    printf("note: waited for \"%s\" in vain\n", text);
    return 0;
}

/* ---- the disk ---------------------------------------------------------------------- */

static const char *at(const char *rel)
{
    static char buf[4][600];
    static int k;

    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", home, rel);
    return buf[k];
}

static void put(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        printf("FAIL cannot write fixture %s\n", path);
        failed++;
        return;
    }
    fwrite(text, 1, len, f);
    fclose(f);
}

static const char *get(const char *path)
{
    static char buf[256];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        return "(missing)";
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static void sh(const char *fmt, const char *arg)
{
    char cmd[800];

    snprintf(cmd, sizeof(cmd), fmt, arg, arg);
    if (system(cmd) != 0) {
        printf("note: \"%s\" failed\n", cmd);
    }
}

/* ---- layout ------------------------------------------------------------------------- */

static void body_box(lv_area_t *b)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, b);
}

/* Every button shown, and every list row, where a finger can use it: inside
 * the body, outside the rounded corners, as tall as a touch target, and no
 * two buttons on top of each other. */
static int n_buttons;
static lv_area_t buttons[64];

static void walk_targets(lv_obj_t *obj, const lv_area_t *body, lv_obj_t *list_clip, const char *what,
                         int *bad)
{
    uint32_t i;
    lv_area_t a;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    if (lv_obj_check_type(obj, &lv_button_class)) {
        int k;

        if (lv_area_get_height(&a) < 56 || a.x1 < body->x1 || a.x2 > body->x2 || a.y1 < body->y1 ||
            a.y2 > body->y2 ||
            !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
            printf("FAIL %s: button \"%s\" at %d,%d-%d,%d is not a usable target in %d,%d-%d,%d\n", what,
                   lv_obj_get_child_count(obj) ? lv_label_get_text(lv_obj_get_child(obj, 0)) : "?",
                   (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, (int)body->x1, (int)body->y1, (int)body->x2,
                   (int)body->y2);
            (*bad)++;
        }
        for (k = 0; k < n_buttons; k++) {
            const lv_area_t *o = &buttons[k];

            if (a.x1 <= o->x2 && o->x1 <= a.x2 && a.y1 <= o->y2 && o->y1 <= a.y2) {
                printf("FAIL %s: two buttons overlap at %d,%d\n", what, (int)a.x1, (int)a.y1);
                (*bad)++;
            }
        }
        if (n_buttons < 64) {
            buttons[n_buttons++] = a;
        }
    }
    if (list_clip && lv_obj_get_parent(obj) == list_clip && lv_obj_get_event_count(obj) > 0 &&
        lv_area_get_height(&a) != POCKETUI_ROW_H) {
        printf("FAIL %s: a row is %d px tall\n", what, (int)lv_area_get_height(&a));
        (*bad)++;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        walk_targets(lv_obj_get_child(obj, i), body, list_clip, what, bad);
    }
}

static void check_targets(const char *what)
{
    lv_area_t body;
    lv_obj_t *any_row = row("docs");
    int bad = 0;
    char msg[160];

    body_box(&body);
    n_buttons = 0;
    walk_targets(app_body, &body, any_row ? lv_obj_get_parent(any_row) : NULL, what, &bad);
    snprintf(msg, sizeof(msg), "%s: every button and row is a usable touch target", what);
    check(msg, bad == 0);
}

static int above(lv_obj_t *a, lv_obj_t *b)
{
    lv_area_t x;
    lv_area_t y;

    lv_obj_update_layout(a);
    lv_obj_get_coords(a, &x);
    lv_obj_get_coords(b, &y);
    return x.y2 <= y.y1;
}

static int left_of(lv_obj_t *a, lv_obj_t *b)
{
    lv_area_t x;
    lv_area_t y;

    lv_obj_update_layout(a);
    lv_obj_get_coords(a, &x);
    lv_obj_get_coords(b, &y);
    return x.x2 <= y.x1;
}

static int same_row(lv_obj_t *a, lv_obj_t *b)
{
    lv_area_t x;
    lv_area_t y;

    lv_obj_update_layout(a);
    lv_obj_get_coords(a, &x);
    lv_obj_get_coords(b, &y);
    return x.y1 == y.y1;
}

/* Every label in a list row lies inside the row, one line each: a label left
 * to size its own height wraps a long name instead of ending it in an
 * ellipsis, and on unit A a 235-character name took three lines, ran out of
 * its row and hid its caption (Files gate, finding F1). */
static int text_inside_row(lv_obj_t *r)
{
    lv_area_t ra;
    uint32_t i;
    uint32_t j;

    if (!r) {
        return 0;
    }
    lv_obj_update_layout(r);
    lv_obj_get_coords(r, &ra);
    for (i = 0; i < lv_obj_get_child_count(r); i++) {
        lv_obj_t *c = lv_obj_get_child(r, i);

        for (j = 0; j < (lv_obj_check_type(c, &lv_label_class) ? 1 : lv_obj_get_child_count(c)); j++) {
            lv_obj_t *lb = lv_obj_check_type(c, &lv_label_class) ? c : lv_obj_get_child(c, j);
            lv_area_t la;
            const lv_font_t *f = lv_obj_get_style_text_font(lb, LV_PART_MAIN);

            if (!lv_obj_check_type(lb, &lv_label_class)) {
                continue;
            }
            lv_obj_get_coords(lb, &la);
            if (la.y1 < ra.y1 || la.y2 > ra.y2 || la.x2 > ra.x2 ||
                lv_area_get_height(&la) > lv_font_get_line_height(f)) {
                printf("note: \"%.24s\" is %d px tall at %d..%d in a row %d..%d\n", lv_label_get_text(lb),
                       (int)lv_area_get_height(&la), (int)la.y1, (int)la.y2, (int)ra.y1, (int)ra.y2);
                return 0;
            }
        }
    }
    return 1;
}

static int rows_hold_their_text(void)
{
    lv_obj_t *docs = row("docs");
    lv_obj_t *list = docs ? lv_obj_get_parent(docs) : NULL;
    uint32_t i;

    if (!list) {
        return 0;
    }
    for (i = 0; i < lv_obj_get_child_count(list); i++) {
        if (!text_inside_row(lv_obj_get_child(list, i))) {
            return 0;
        }
    }
    return 1;
}

/* ---- the fixture --------------------------------------------------------------------- */

static char longname[241];

/* The long name is only ever shown cut with an ellipsis, and LVGL writes the
 * ellipsis into the label's own text, so it is found by its first 40
 * characters, which no other name has. */
static lv_obj_t *long_row(void)
{
    char prefix[41];

    snprintf(prefix, sizeof(prefix), "%.40s", longname);
    return pressable(find_label_ex(app_body, prefix, 1));
}

/* The same name in the landscape details pane: the one that is not in a row. */
static lv_obj_t *long_label_in_pane(void)
{
    char prefix[41];
    lv_obj_t *side = find_label("Can be changed");
    lv_obj_t *card = side ? lv_obj_get_parent(side) : NULL;

    snprintf(prefix, sizeof(prefix), "%.40s", longname);
    return card ? find_label_ex(card, prefix, 1) : NULL;
}

static void fixture(void)
{
    sh("rm -rf '%s'; mkdir -p '%s'", root);
    snprintf(home, sizeof(home), "%s/home", root);
    mkdir(home, 0755);
    mkdir(at("docs"), 0755);
    put(at("docs/readme.txt"), "Hello from Files\nsecond line\n", 29);
    put(at("notes.txt"), "abc", 3);
    put(at("data.bin"), "a\0b", 3);
    put(at("big.log"), "0123456789012345678901234567890123456789", 40);
    mkdir(at("doors-data"), 0755);
    put(at("doors-data/settings.conf"), "keep", 4);
    memset(longname, 'L', 230);
    memcpy(longname + 230, "-name.txt", 10);
    put(at(longname), "long", 4);
    setenv("HOME", home, 1);
    setenv("POCKETOS_STATE_DIR", at("doors-data"), 1);
}

/* ---- the journeys ---------------------------------------------------------------------- */

static void browse_and_open(void)
{
    lv_obj_t *docs = row("docs");
    lv_obj_t *notes = row("notes.txt");

    check("Files opens in the home folder", path_ends_with("/home"));
    check("and lists its folders and files", docs && notes && row("doors-data") && row("data.bin"));
    check("folders come first", docs && notes && above(docs, notes) && above(row("doors-data"), notes));
    check("a row says size and time", shows_part("3 B \xC2\xB7 "));
    check("nothing is selected yet, so nothing can be done to anything",
          !enabled("Open") && !enabled("Rename") && !enabled("Delete"));
    check("but a folder can be made here", enabled("New folder"));
    check("a long name is shown, not refused", long_row() != NULL);
    check("and cut to one line inside its row, with its caption under it", text_inside_row(long_row()));
    check("every row's text stays inside its row", rows_hold_their_text());

    tap_obj(row("notes.txt"));
    check("a tap selects", enabled("Open") && enabled("Rename") && enabled("Copy") && enabled("Move") &&
                               enabled("Delete"));
    tap_obj(row("notes.txt"));
    check("a second tap opens a text file", shows("abc") && shows("Close") && !shows("New folder"));
    tap_obj(find_labelled("Close"));
    check("Close is back in the list", shows("New folder") && row("notes.txt"));

    tap_obj(row("data.bin"));
    tap_obj(find_labelled("Open"));
    check("a file that is not text is not shown", !shows("Close") && shows_part("not a text file"));

    tap_obj(row("docs"));
    tap_obj(row("docs"));
    check("a folder is entered", path_ends_with("/docs") && row("readme.txt") && !row("notes.txt"));
    tap_obj(row("readme.txt"));
    tap_obj(find_labelled("Open"));
    check("Open shows the text", shows("Hello from Files\nsecond line\n"));
    tap_obj(find_labelled("Close"));
    tap_obj(find_labelled(LV_SYMBOL_UP));
    check("Up goes back", path_ends_with("/home") && row("notes.txt"));
    check("with the folder it came from selected", enabled("Open") && enabled("Rename"));
}

static void make_and_rename(void)
{
    lv_obj_t *field;

    tap_obj(find_labelled("New folder"));
    field = find_field(app_body);
    check("New folder asks for a name, with the keyboard up",
          field && pocketos_shell_keyboard_visible() && pos_input_focused() == field);
    tap_obj(find_labelled("Create"));
    check("an empty name is refused, and says why", shows("Type a name"));
    type_text("Projects");
    tap_key("DONE");
    check("Done on the keyboard creates it", exists(at("Projects")));
    check("and goes back to the list, keyboard down", !pocketos_shell_keyboard_visible() && row("Projects"));
    check("and says so", shows_part("Created \xE2\x80\x9CProjects\xE2\x80\x9D"));

    tap_obj(find_labelled("New folder"));
    type_text("Projects");
    tap_obj(find_labelled("Create"));
    check("a name that is taken is refused in the field", shows("Something with that name is already there"));
    clear_field();
    type_text("a-b");
    tap_key("?123");
    tap_key("/");
    tap_key("ABC");
    tap_obj(find_labelled("Create"));
    check("a slash is refused", shows("A name cannot contain /") && !exists(at("a-b")));
    tap_obj(find_labelled("Cancel"));
    check("Cancel makes nothing", !pocketos_shell_keyboard_visible() && row("Projects") && !row("a-b"));

    /* Enter from a physical keyboard reaches the same logical stream as the
     * touch keyboard's Done (DS 17.4), and commits the same way, once. */
    tap_obj(find_labelled("New folder"));
    type_text("Base");
    pos_input_push_key(LV_KEY_ENTER);
    drain();
    check("Enter from any keyboard creates it", exists(at("Base")) && !pocketos_shell_keyboard_visible() &&
                                                  shows_part("Created \xE2\x80\x9C" "Base"));
    check("and only once: no error shown over it", !shows_part("already there"));
    rmdir(at("Base"));

    tap_obj(row("notes.txt"));
    tap_obj(find_labelled("Rename"));
    field = find_field(app_body);
    check("Rename starts from the name it has", field && strcmp(lv_textarea_get_text(field), "notes.txt") == 0);
    clear_field();
    type_text("todo.txt");
    tap_obj(find_labelled("Rename"));
    check("Rename renames it on disk", exists(at("todo.txt")) && !exists(at("notes.txt")) &&
                                           strcmp(get(at("todo.txt")), "abc") == 0);
    check("and it is selected under its new name", row("todo.txt") && enabled("Rename"));

    tap_obj(row("big.log"));
    tap_obj(find_labelled("Rename"));
    clear_field();
    type_text("todo.txt");
    tap_obj(find_labelled("Rename"));
    check("renaming onto a taken name replaces nothing",
          shows("Something with that name is already there") &&
              strcmp(get(at("todo.txt")), "abc") == 0 && exists(at("big.log")));
    tap_obj(find_labelled("Cancel"));
}

static void copy_move_delete(void)
{
    /* Copy into another folder. */
    tap_obj(row("todo.txt"));
    tap_obj(find_labelled("Copy"));
    check("Copy carries the file: Paste here in place of the actions",
          shows("Copy \xE2\x80\x9Ctodo.txt\xE2\x80\x9D") && enabled("Paste here") && !find_label("Rename"));
    tap_obj(row("docs"));
    tap_obj(row("docs"));
    check("while it is carried, folders still open", path_ends_with("/docs"));
    tap_obj(find_labelled("Paste here"));
    check("Paste here copies it", wait_for("Copied"));
    check("the copy has the bytes and the original stays",
          strcmp(get(at("docs/todo.txt")), "abc") == 0 && strcmp(get(at("todo.txt")), "abc") == 0);
    check("and the actions are back", shows("Rename") && !shows("Paste here"));

    /* A copy into its own folder is a duplicate with a number. */
    tap_obj(row("readme.txt"));
    tap_obj(find_labelled("Copy"));
    tap_obj(find_labelled("Paste here"));
    check("a copy beside the original gets a number", wait_for("as \xE2\x80\x9Creadme (2).txt"));
    check("and replaces nothing", exists(at("docs/readme (2).txt")) &&
                                      strcmp(get(at("docs/readme.txt")), "Hello from Files\nsecond line\n") == 0);

    /* Move it up a folder. The copy is already selected: a tap on its row
     * now would open it. */
    check("the copy is selected when it arrives", enabled("Move") && !shows("Close"));
    tap_obj(find_labelled("Move"));
    tap_obj(find_labelled(LV_SYMBOL_UP));
    tap_obj(find_labelled("Paste here"));
    check("Move moves it", wait_for("Moved"));
    check("it is here and gone from there", exists(at("readme (2).txt")) && !exists(at("docs/readme (2).txt")));

    /* Paste where it is (it arrives selected), and cancel. */
    tap_obj(find_labelled("Move"));
    tap_obj(find_labelled("Paste here"));
    check("moving it to where it is says so", wait_for("already here") && exists(at("readme (2).txt")));
    tap_obj(row("todo.txt"));
    tap_obj(find_labelled("Copy"));
    tap_obj(find_labelled("Cancel"));
    check("Cancel drops what was carried", !shows("Paste here") && shows("Rename"));

    /* Delete: confirmation, cancel, then for real. */
    tap_obj(row("readme (2).txt"));
    tap_obj(find_labelled("Delete"));
    check("Delete asks first", shows("Delete \xE2\x80\x9Creadme (2).txt\xE2\x80\x9D?"));
    check("and nothing is gone yet", exists(at("readme (2).txt")));
    tap_obj(find_labelled("Cancel"));
    check("Cancel keeps it", exists(at("readme (2).txt")) && row("readme (2).txt"));
    tap_obj(find_labelled("Delete"));
    tap_obj(find_labelled("Delete"));
    check("the second Delete removes it", wait_for("Deleted") && !exists(at("readme (2).txt")));
    check("and its row", !row("readme (2).txt"));

    put(at("Projects/inside.txt"), "x", 1);
    tap_obj(row("Projects"));
    tap_obj(find_labelled("Delete"));
    check("a folder's confirmation says everything in it goes",
          shows_part("everything in it"));
    tap_obj(find_labelled("Delete"));
    check("a folder is deleted with what is in it", wait_for("Deleted") && !exists(at("Projects")));
}

static void read_only_places(void)
{
    tap_obj(row("doors-data"));
    check("Doors' data can be looked at and copied", enabled("Open") && enabled("Copy"));
    check("but not renamed, moved or deleted", !enabled("Rename") && !enabled("Move") && !enabled("Delete"));
    tap_obj(find_labelled("Delete"));
    check("a disabled Delete does nothing", !shows_part("Delete \xE2\x80\x9C") && exists(at("doors-data")));
    tap_obj(row("doors-data"));
    check("inside it nothing can be made", path_ends_with("/doors-data") && !enabled("New folder") &&
                                               shows_part("Read-only"));
    tap_obj(row("settings.conf"));
    check("nor changed", enabled("Open") && !enabled("Delete") && !enabled("Rename"));
    tap_obj(find_labelled(LV_SYMBOL_UP));

    /* Up to the top of the filesystem: browsable, read-only. */
    tap_obj(find_labelled(LV_SYMBOL_UP));
    tap_obj(find_labelled(LV_SYMBOL_UP));
    tap_obj(find_labelled(LV_SYMBOL_UP));
    check("the top of the filesystem can be browsed", row("tmp") && row("etc"));
    check("and nothing can be made there", !enabled("New folder") && shows_part("Read-only"));
    tap_obj(row("etc"));
    check("nor can a system folder be changed", enabled("Open") && !enabled("Delete") && !enabled("Rename"));
}

static void missing_and_refused(void)
{
    /* A folder that is gone by the time it is opened. */
    tap_obj(row("docs"));
    sh("rm -rf '%s/home/docs' # %s", root);
    tap_obj(find_labelled("Open"));
    check("a folder that has gone is not entered, and that is said",
          path_ends_with("/home") && shows_part("no longer there"));

    /* The folder on screen goes while something is made in it. */
    mkdir(at("gone"), 0755);
    tap_obj(find_labelled(LV_SYMBOL_UP));
    /* Up selects the folder it came from, so one tap goes back into it. */
    tap_obj(row("home"));
    tap_obj(row("gone"));
    tap_obj(row("gone"));
    check("in the folder that is about to go", path_ends_with("/gone"));
    rmdir(at("gone"));
    tap_obj(find_labelled("New folder"));
    type_text("x");
    tap_obj(find_labelled("Create"));
    check("making a folder in it fails, in the field", shows("It is no longer there") && !exists(at("gone")));
    tap_obj(find_labelled("Cancel"));
    tap_obj(find_labelled(LV_SYMBOL_UP));
    check("and Up leads out of it", path_ends_with("/home") && row("todo.txt"));

    if (geteuid() != 0) {
        mkdir(at("locked"), 0755);
        put(at("todo.txt"), "abc", 3);
        tap_obj(find_labelled(LV_SYMBOL_UP));
        tap_obj(row("home"));
        chmod(at("locked"), 0);
        tap_obj(row("locked"));
        tap_obj(row("locked"));
        check("a folder that may not be read is not entered", path_ends_with("/home") &&
                                                                  shows_part("Permission denied"));
        chmod(at("locked"), 0755);
    }
}

/* ---- the layouts ------------------------------------------------------------------------ */

static void portrait_layout(void)
{
    lv_obj_t *up = find_labelled(LV_SYMBOL_UP);
    lv_obj_t *sort = find_labelled("Sort: Name");
    lv_obj_t *docs = row("docs");
    lv_obj_t *open = find_labelled("Open");

    check("portrait: the path bar, then Sort, then the list, then the actions",
          up && sort && docs && open && above(up, sort) && above(sort, docs) && above(docs, open));
    check("portrait: no details pane", !shows("Nothing selected") && !shows("Tap a file or folder to see it here."));
    check("portrait: the five actions in one row",
          same_row(open, find_labelled("Delete")) && left_of(open, find_labelled("Rename")));
    check_targets("portrait");
}

static void landscape_layout(void)
{
    lv_obj_t *up = find_labelled(LV_SYMBOL_UP);
    lv_obj_t *sort = find_labelled("Sort: Name");
    lv_obj_t *docs = row("docs");
    lv_obj_t *name;
    lv_obj_t *open;

    check("landscape: Sort and New folder move up into the path bar",
          up && sort && same_row(up, sort) && left_of(up, sort) && same_row(sort, find_labelled("New folder")));
    check("landscape: a details pane beside the list", shows("Nothing selected") && docs &&
                                                        left_of(docs, find_label("Nothing selected")));
    tap_obj(long_row());
    name = long_label_in_pane();
    check("landscape: every row's text stays inside its row", rows_hold_their_text());
    check("selecting shows the entry in the pane", name != NULL && shows("TXT file \xC2\xB7 4 B") &&
                                                       shows_part("Modified ") && shows("Can be changed"));
    open = find_labelled("Open");
    check("with the actions under it", open && above(find_label("Can be changed"), open) &&
                                           left_of(docs, open) && above(open, find_labelled("Rename")) &&
                                           same_row(find_labelled("Rename"), find_labelled("Copy")));
    check_targets("landscape");

    /* The name entry above a landscape keyboard. */
    tap_obj(find_labelled("New folder"));
    {
        lv_obj_t *field = find_field(app_body);
        lv_obj_t *create = find_labelled("Create");

        check("landscape: New folder puts field and buttons in one row above the keyboard",
              field && create && pocketos_shell_keyboard_visible() && left_of(field, create));
        check_targets("landscape, name entry over the keyboard");
        tap_obj(create);
        check("landscape: the error caption is shown there too", shows("Type a name"));
        check_targets("landscape, name entry with its caption");
    }
    tap_obj(find_labelled("Cancel"));

    /* Carrying survives a change of shape. */
    tap_obj(row("todo.txt"));
    tap_obj(find_labelled("Copy"));
    check("landscape: Paste here waits in the pane", enabled("Paste here"));
    use_display(POS_ROTATION_0);
    check("turned to portrait, the carried file is still carried",
          shows("Copy \xE2\x80\x9Ctodo.txt\xE2\x80\x9D") && enabled("Paste here"));
    check_targets("portrait, carrying");
    tap_obj(find_labelled("Cancel"));
    use_display(POS_ROTATION_90);

    /* Sorting, on the wide list. */
    tap_obj(find_labelled("Sort: Name"));
    check("Sort cycles to type", shows("Sort: Type"));
    tap_obj(find_labelled("Sort: Type"));
    check("then size, largest file first", shows("Sort: Size") && row("big.log") && row("todo.txt") &&
                                               above(row("big.log"), row("todo.txt")));
    tap_obj(find_labelled("Sort: Size"));
    tap_obj(find_labelled("Sort: Date"));
    check("then back to name", shows("Sort: Name"));
}

/* Closing the app with a copy running leaves no thread and no half copy. */
static void close_while_copying(void)
{
    char big[64 * 1024];
    FILE *f;
    int i;

    memset(big, 'z', sizeof(big));
    f = fopen(at("huge.bin"), "wb");
    for (i = 0; f && i < 256; i++) { /* 16 MB */
        fwrite(big, 1, sizeof(big), f);
    }
    if (f) {
        fclose(f);
    }
    mkdir(at("dest"), 0755);
    app_start();
    tap_obj(row("huge.bin"));
    tap_obj(find_labelled("Copy"));
    tap_obj(row("dest"));
    tap_obj(row("dest"));
    tap_obj(find_labelled("Paste here"));
    app_stop();
    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "ls -A '%s' | grep -q files-partial", at("dest"));
        check("closing mid-copy leaves no partial copy behind", system(cmd) != 0);
    }
    check("the original is whole", exists(at("huge.bin")));
}

int main(void)
{
    lv_indev_t *finger;

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(root, sizeof(root), "/tmp/files-app-%ld", (long)getpid());
    fixture();

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
    g_keyboard = pos_keyboard_create(lv_screen_active());
    pos_keyboard_set_done_cb(g_keyboard, on_kb_done, NULL);

    /* Portrait: the journeys. */
    use_display(POS_ROTATION_0);
    app_start();
    portrait_layout();
    browse_and_open();
    make_and_rename();
    copy_move_delete();
    read_only_places();
    app_stop();

    app_start();
    missing_and_refused();
    app_stop();

    /* Landscape. */
    mkdir(at("docs"), 0755);
    use_display(POS_ROTATION_90);
    app_start();
    landscape_layout();
    app_stop();

    close_while_copying();

    sh("chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s'", root);
    printf("files_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
