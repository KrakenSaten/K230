/*
 * PocketNotes in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Nothing here sets the field's text to make a note: every character is
 * tapped on the keyboard, travels the logical stream, and lands in the
 * focused field, the way it will on the panel. What is then read back off
 * the disk is what the store actually kept.
 *
 * The app is hosted the way the shell hosts it, but the shell itself is not
 * here, so the three app.h keyboard entry points are implemented below
 * against the real pos_keyboard. That is exactly what the shell does, and it
 * keeps the app honest: it can only ask, and it never sees a keyboard.
 *
 * The display is the reference panel with its 30 px rounded corners, as the
 * shell opens it, in portrait and in landscape (DS 21, 22): sections 17 on
 * check every screen's layout in both, and the body changing shape under a
 * note that is open.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/notes_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "notes_store.h"
#include "notes_view.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_keyboard.h"

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
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* The status bar the shell gives this app in the display's orientation
 * (ui/shell/chrome.h, DS section 30), so the frame built here is the one
 * shell.c builds: 56 px in portrait, 32 px under an app in landscape. */
#define STATUS_H chrome_height(chrome_resolve(app_notes.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_notes;

static int failed;
static int checks;
static char root[128];
static char text_buf[NOTES_MAX_BYTES + 1];
static struct notes_entry list_buf[NOTES_MAX_NOTES];

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

/* pocketlog's one entry point, so what the app says on the way out can be
 * read here without a log directory to go with it. */
static char g_logged[256];

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_logged, sizeof(g_logged), fmt, ap);
    va_end(ap);
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)on_done;
    (void)user;
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE ? POS_KB_RETURN_NEWLINE
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

/* Type it the way a person would: the alpha layer shows lower case, so a
 * capital needs Shift first. One-shot Shift clears itself after the letter,
 * which is why no capital here ever leaks into the next one. */
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

/* Depth-first search for the label showing this text. */
static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    /* A hidden screen is still in the tree, and both the editor and the
     * confirmation carry a button labelled Delete. A finger cannot press
     * what is not shown, so neither may this. */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_label(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* The same search, returning the label's clickable ancestor, which is what a
 * finger would press. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    lv_obj_t *lb = find_label(obj, text);
    lv_obj_t *p = lb;

    while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
        p = lv_obj_get_parent(p);
    }
    return p ? p : lb;
}

static lv_obj_t *find_field(lv_obj_t *obj)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
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

static int label_present(lv_obj_t *obj, const char *text)
{
    return find_labelled(obj, text) != NULL;
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

/* The shell's own frame (ui/shell/shell.c, app_open): a header row, then a
 * body that grows into what is left, padded the same way. Whether a long
 * list fits is a question about exactly these pixels. */
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

/* The display the shell would open: a panel with corner squares of `corner`
 * px, turned to `rotation`, the geometry handed to PocketUI and the content
 * area below the status bar sized to it, less the keyboard's sheet when that
 * is up (shell.c). Called with the app open, it is the body changing shape
 * under a running app. */
static void use_panel(int32_t w, int32_t h, enum pos_rotation rotation, int32_t corner)
{
    struct pos_panel panel = {
        .width = w,
        .height = h,
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
                    g.height - STATUS_H - (pos_keyboard_is_shown(g_keyboard) ? POS_KB_H : 0));
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    use_panel(PANEL_W, PANEL_H, rotation, corner);
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
    app_priv = app_notes.create(app_body);
    pump(60);
}

/* The shell's app_close(): hide the keyboard, destroy, delete the root. */
static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_notes.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* ---- what is on disk, and what a finger can reach ---------------------- */

/* A file's bytes, read straight off the disk rather than through the store:
 * the point is to see what is actually there. Returns how many, or -1. */
static long read_raw(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (!f) {
        return -1;
    }
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

/* Back-date a file, so a rewrite shows up as a changed time. */
static void set_mtime(const char *path, time_t when)
{
    struct timespec times[2];

    times[0].tv_sec = when;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    if (utimensat(AT_FDCWD, path, times, 0) != 0) {
        printf("FAIL cannot set the time of %s\n", path);
        failed++;
        checks++;
    }
}

static int has_mtime(const char *path, time_t when)
{
    struct stat st;

    return stat(path, &st) == 0 && st.st_mtime == when;
}

/* Whether obj lies wholly inside clip's box: on screen, where a finger can
 * reach it, rather than drawn past an edge that cuts it off. */
static int inside(lv_obj_t *obj, lv_obj_t *clip)
{
    lv_area_t o;
    lv_area_t c;

    if (!obj || !clip) {
        return 0;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &o);
    lv_obj_get_coords(clip, &c);
    return o.x1 >= c.x1 && o.x2 <= c.x2 && o.y1 >= c.y1 && o.y2 <= c.y2;
}

/* A finger drawn dy pixels across obj in small moves, the way a scroll
 * reaches LVGL from the panel, then time for the scroll to come to rest. */
static void drag(lv_obj_t *obj, int32_t dy)
{
    lv_area_t a;
    int32_t y0;
    int step;

    if (!obj) {
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    y0 = a.y1 + lv_area_get_height(&a) * 3 / 4;
    finger_point.y = y0;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (step = 1; step <= 20; step++) {
        finger_point.y = y0 + dy * step / 20;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(1500);
}

/* ---- the editor's field and its error caption -------------------------- */

/* DS §17.1: an error "MUST carry a caption below the field", and a caption
 * that is cut off carries nothing. The caption lives in the field's wrapper,
 * after the field, so it is only seen if the field leaves it room there: a
 * field taking all of its wrapper's height pushed the caption out under the
 * wrapper's edge, and only the red border was left on the panel. So it must
 * lie below the field and wholly inside every ancestor up to the screen,
 * clear of the keyboard when that is up, without the body having had to
 * scroll to make room (DS §17.1, scrolling). */
static void check_caption(const char *what, const char *text)
{
    lv_obj_t *lb = find_label(app_body, text);
    lv_obj_t *field = find_field(app_body);
    lv_area_t a = { 0 };
    lv_area_t f = { 0 };
    lv_area_t w = { 0 };
    lv_obj_t *p;
    int ok = lb && field;

    if (ok) {
        lv_obj_update_layout(lb);
        lv_obj_get_coords(lb, &a);
        lv_obj_get_coords(field, &f);
        lv_obj_get_coords(lv_obj_get_parent(field), &w);
        ok = lv_area_get_height(&a) > 0 && a.y1 > f.y2;
        for (p = lv_obj_get_parent(lb); ok && p; p = lv_obj_get_parent(p)) {
            ok = inside(lb, p);
        }
        if (ok && pos_keyboard_is_shown(g_keyboard)) {
            lv_area_t k;

            lv_obj_get_coords(g_keyboard, &k);
            ok = a.y2 < k.y1;
        }
        ok = ok && lv_obj_get_scroll_y(app_body) == 0 &&
             lv_obj_get_scroll_bottom(app_body) <= 0;
    }
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s: caption %d..%d x %d..%d, field y %d..%d, wrapper y %d..%d\n",
               what, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2, (int)f.y1, (int)f.y2,
               (int)w.y1, (int)w.y2);
    }
}

/* With no error shown the field is the whole of its wrapper, exactly as
 * before the caption had room made for it. */
static int fills_wrapper(lv_obj_t *field)
{
    lv_area_t f;
    lv_area_t w;

    if (!field) {
        return 0;
    }
    lv_obj_update_layout(field);
    lv_obj_get_coords(field, &f);
    lv_obj_get_coords(lv_obj_get_parent(field), &w);
    return f.x1 == w.x1 && f.x2 == w.x2 && f.y1 == w.y1 && f.y2 == w.y2;
}

/* ---- layout: where things are, in either orientation -------------------- */

/* NULL-safe, so that a layout that lost an object fails its checks and the
 * run goes on, rather than stopping in an LVGL assert that never returns. */
static void area_of(lv_obj_t *obj, lv_area_t *a)
{
    if (!obj) {
        lv_area_set(a, 0, 0, -1, -1);
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static lv_obj_t *parent_of(lv_obj_t *obj)
{
    return obj ? lv_obj_get_parent(obj) : NULL;
}

static const char *text_of(lv_obj_t *field)
{
    return field ? lv_textarea_get_text(field) : "(no field)";
}

static int within(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static int is_rect(const lv_area_t *a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    return a->x1 == x1 && a->y1 == y1 && a->x2 == x2 && a->y2 == y2;
}

/* On the display and outside every rounded corner's square (pos_display.h). */
static int is_safe(const lv_area_t *a)
{
    return pos_display_rect_is_safe(pocketui_display_geometry(), a->x1, a->y1, a->x2, a->y2);
}

/* Where the app may put anything: the body's content box. */
static void body_box(lv_area_t *b)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, b);
}

/* How far the foot corner squares reach above the body's foot: what the
 * layout must leave clear at the foot. */
static int32_t foot_inset(void)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    lv_area_t b;

    body_box(&b);
    return LV_MAX(0, b.y2 - (g->height - LV_MAX(g->corners.bottom_left, g->corners.bottom_right)) + 1);
}

static void report_rect(const char *what, lv_obj_t *obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a;

    checks++;
    if (!obj) {
        failed++;
        printf("FAIL %s: no object\n", what);
        return;
    }
    area_of(obj, &a);
    if (!is_rect(&a, x1, y1, x2, y2)) {
        failed++;
        printf("FAIL %s: is %d..%d x %d..%d, want %d..%d x %d..%d\n", what, (int)a.x1, (int)a.x2,
               (int)a.y1, (int)a.y2, (int)x1, (int)x2, (int)y1, (int)y2);
    }
}

/* The controls of one screen, checked together: every one in the body's
 * content box and the safe area, at least min_h tall and the touch minimum
 * wide, none on another, none under the keyboard's sheet; and the body not
 * scrolling (DS 17.1). */
struct control {
    const char *name;
    lv_obj_t *obj;
    int32_t min_h;
};

static void check_controls(const char *what, const struct control *c, int n)
{
    char msg[160];
    lv_area_t box;
    lv_area_t sheet;
    lv_area_t a;
    lv_area_t b;
    int i;
    int j;

    body_box(&box);
    area_of(g_keyboard, &sheet);
    for (i = 0; i < n; i++) {
        snprintf(msg, sizeof(msg), "%s: %s exists", what, c[i].name);
        check(msg, c[i].obj != NULL);
        if (!c[i].obj) {
            return;
        }
        area_of(c[i].obj, &a);
        snprintf(msg, sizeof(msg), "%s: %s is in the body", what, c[i].name);
        check(msg, within(&a, &box));
        snprintf(msg, sizeof(msg), "%s: %s is in the safe area, clear of the rounded corners", what,
                 c[i].name);
        check(msg, is_safe(&a));
        snprintf(msg, sizeof(msg), "%s: %s is at least %d px tall and %d px wide", what, c[i].name,
                 (int)c[i].min_h, POCKETUI_TOUCH_MIN);
        check(msg, lv_area_get_height(&a) >= c[i].min_h && lv_area_get_width(&a) >= POCKETUI_TOUCH_MIN);
        if (pos_keyboard_is_shown(g_keyboard)) {
            snprintf(msg, sizeof(msg), "%s: %s is clear of the keyboard", what, c[i].name);
            check(msg, !overlaps(&a, &sheet));
        }
        for (j = i + 1; j < n; j++) {
            if (!c[j].obj) {
                continue;
            }
            area_of(c[j].obj, &b);
            snprintf(msg, sizeof(msg), "%s: %s and %s do not overlap", what, c[i].name, c[j].name);
            check(msg, !overlaps(&a, &b));
        }
    }
    snprintf(msg, sizeof(msg), "%s: the body does not scroll", what);
    check(msg, lv_obj_get_scroll_top(app_body) <= 0 && lv_obj_get_scroll_bottom(app_body) <= 0);
}

/* Whether the field is scrolled no further than its text: a note that fits is
 * shown from its top, and one that does not ends at the field's foot, never
 * with blank field past its end. */
static int not_past_the_end(lv_obj_t *field)
{
    if (!field) {
        return 0;
    }
    lv_obj_update_layout(field);
    return lv_obj_get_scroll_y(field) == 0 || lv_obj_get_scroll_bottom(field) >= 0;
}

/* Whether the caret's line lies inside the field's box, where it can be seen
 * - not scrolled past the field, and not in a part of it that its container
 * cuts off. Worked out from the field's own scroll rather than read off its
 * label: after the editor has been hidden behind the confirmation and shown
 * again, LVGL leaves the label's coordinates stale until the next draw, on
 * master as here (the panel itself settles on the right picture). */
static int caret_in_view(lv_obj_t *field)
{
    const lv_font_t *font = field ? lv_obj_get_style_text_font(field, LV_PART_MAIN) : NULL;
    lv_point_t p;
    lv_area_t content;
    lv_area_t f;
    lv_area_t w;
    int32_t y1;

    if (!field) {
        return 0;
    }
    pump(1000); /* a long scroll to the caret is animated */
    lv_obj_update_layout(field);
    lv_label_get_letter_pos(lv_textarea_get_label(field), lv_textarea_get_cursor_pos(field), &p);
    lv_obj_get_content_coords(field, &content);
    lv_obj_get_coords(field, &f);
    lv_obj_get_coords(lv_obj_get_parent(field), &w);
    y1 = content.y1 - lv_obj_get_scroll_y(field) + p.y;
    return y1 >= f.y1 && y1 + lv_font_get_line_height(font) - 1 <= LV_MIN(f.y2, w.y2);
}

/* Twenty "Note NN" notes, Note 01 the oldest, and one with a title longer
 * than any row. */
static void write_many(void)
{
    char path[256];
    time_t base = time(NULL) - 100000;
    int i;

    for (i = 1; i <= 20; i++) {
        char body[32];

        snprintf(body, sizeof(body), "Note %02d\nbody", i);
        notes_store_write((uint32_t)i, body);
        notes_store_path((uint32_t)i, path, sizeof(path));
        set_mtime(path, base + i * 60);
    }
    notes_store_write(21, "A title far longer than any row can hold in either orientation, "
                          "on and on\nx");
}

static lv_obj_t *list_card(void)
{
    /* body > the app's frame > the list screen > the rows or the empty state */
    return lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(app_body, 0), 0), 0);
}

static void check_list(const char *what, int rows)
{
    char msg[160];
    lv_obj_t *card = list_card();
    lv_obj_t *new_note = find_labelled(app_body, "New note");
    struct control c[] = {
        { "the list", card, POCKETUI_ROW_H },
        { "New note", new_note, POCKETUI_TOUCH_MIN },
    };
    uint32_t i;

    check_controls(what, c, 2);
    if (!rows || !card) {
        return;
    }
    /* Every row a full-width 64 px hit area, its title and its time side by
     * side, the title cut short rather than running into the time. */
    for (i = 0; i < lv_obj_get_child_count(card); i++) {
        lv_obj_t *row = lv_obj_get_child(card, i);
        lv_area_t r;
        lv_area_t t;
        lv_area_t d;

        area_of(row, &r);
        area_of(lv_obj_get_child(row, 0), &t);
        area_of(lv_obj_get_child(row, 1), &d);
        if (lv_area_get_height(&r) != POCKETUI_ROW_H || !within(&t, &r) || !within(&d, &r) ||
            overlaps(&t, &d)) {
            snprintf(msg, sizeof(msg), "%s: row %u is 64 px, its title and time inside it and apart",
                     what, (unsigned)i);
            check(msg, 0);
            return;
        }
    }
    snprintf(msg, sizeof(msg), "%s: every row is 64 px, its title and time inside it and apart", what);
    check(msg, 1);
}

static void check_editor(const char *what)
{
    char msg[160];
    lv_obj_t *field = find_field(app_body);
    const lv_font_t *font = field ? lv_obj_get_style_text_font(field, LV_PART_MAIN) : NULL;
    struct control c[] = {
        { "the field", field, 3 * (font ? lv_font_get_line_height(font) : 0) },
        { "Done", find_labelled(app_body, "Done"), 56 },
        { "Delete", find_labelled(app_body, "Delete"), 56 },
    };
    lv_area_t f;
    lv_area_t w;

    check_controls(what, c, 3);
    if (!field) {
        return;
    }
    /* The field whole inside what holds it: a field taller than its box is
     * drawn cut off, and scrolls its caret into the cut. */
    area_of(field, &f);
    area_of(lv_obj_get_parent(field), &w);
    snprintf(msg, sizeof(msg), "%s: the field is not cut off by its container", what);
    check(msg, within(&f, &w));
    snprintf(msg, sizeof(msg), "%s: the field shows three lines of its type or more", what);
    check(msg, lv_area_get_height(&f) >= 3 * lv_font_get_line_height(font));
}

static void check_confirm(const char *what)
{
    lv_obj_t *title = find_label(app_body, "Delete this note?");
    struct control c[] = {
        { "the dialog", title ? lv_obj_get_parent(title) : NULL, 56 },
    };
    struct control b[] = {
        { "Cancel", find_labelled(app_body, "Cancel"), 56 },
        { "Delete", find_labelled(app_body, "Delete"), 56 },
    };

    check_controls(what, c, 1);
    check_controls(what, b, 2);
}

/* Which shape the screen on show is in: its actions beside its content (1)
 * or not (0). */
static int beside(lv_obj_t *content, lv_obj_t *action)
{
    lv_area_t c;
    lv_area_t a;

    if (!content || !action) {
        return -1;
    }
    area_of(content, &c);
    area_of(action, &a);
    return a.x1 > c.x2 && a.y1 < c.y2 ? 1 : a.y1 > c.y2 || a.y2 < c.y1 ? 0 : -1;
}

/* Every screen of the app on one display and in one mode: the list long and
 * scrolled and empty, the editor above the keyboard with a long note typed
 * into it, the confirmation, and a note the editor only shows, with the
 * keyboard down. */
static void check_orientation(const char *name, enum pos_rotation rotation, int32_t corner,
                              const char *mode)
{
    char what[96];
    char why[128];
    lv_area_t box;
    lv_area_t a;
    lv_obj_t *field;
    lv_obj_t *oldest;
    int wide = rotation == POS_ROTATION_270;
    int drags;
    int i;

    wipe();
    write_many();
    /* Twelve lines: the whole of it fits the portrait field and not the
     * landscape one above the keyboard. */
    notes_store_write(22, "Twelve lines\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    use_display(rotation, corner);
    pos_theme_apply(NULL, mode, why, sizeof(why));
    app_start();

    snprintf(what, sizeof(what), "[%s] list", name);
    check_list(what, 1);
    snprintf(what, sizeof(what), "[%s] list: New note is %s the rows", name, wide ? "beside" : "below");
    check(what, beside(list_card(), find_labelled(app_body, "New note")) == wide);
    if (wide) {
        area_of(list_card(), &a);
        snprintf(what, sizeof(what), "[%s] list: the rows are no narrower than in portrait", name);
        check(what, lv_area_get_width(&a) >= 528);
    }
    oldest = find_labelled(app_body, "Note 01");
    for (drags = 0; drags < 8 && oldest && !inside(oldest, list_card()); drags++) {
        drag(list_card(), wide ? -250 : -400);
    }
    snprintf(what, sizeof(what), "[%s] list: a finger scrolls to the oldest note", name);
    check(what, oldest && inside(oldest, list_card()));
    snprintf(what, sizeof(what), "[%s] list, scrolled", name);
    check_list(what, 1);

    tap_obj(oldest);
    field = find_field(app_body);
    snprintf(what, sizeof(what), "[%s] the oldest note opens with the keyboard up", name);
    check(what, field && strcmp(lv_textarea_get_text(field), "Note 01\nbody") == 0 &&
                    pocketos_shell_keyboard_visible() && pos_input_focused() == field);
    snprintf(what, sizeof(what), "[%s] editor", name);
    check_editor(what);
    snprintf(what, sizeof(what), "[%s] editor: Done is %s the field", name, wide ? "beside" : "above");
    check(what, wide ? beside(field, find_labelled(app_body, "Done")) == 1
                     : beside(find_labelled(app_body, "Done"), field) == 0);
    if (wide) {
        body_box(&box);
        area_of(field, &a);
        snprintf(what, sizeof(what), "[%s] editor: the field has the body's full height above the keyboard",
                 name);
        check(what, a.y1 == box.y1 && a.y2 == box.y2);
        snprintf(what, sizeof(what), "[%s] editor: and is no narrower than in portrait", name);
        check(what, lv_area_get_width(&a) >= 528);
    }

    /* A long note, typed: the field scrolls and the caret stays in sight. */
    tap_key("ENTER");
    for (i = 0; i < 36; i++) { /* more lines than the portrait field shows */
        type_text("line");
        tap_key("ENTER");
    }
    type_text("end");
    snprintf(what, sizeof(what), "[%s] editor: typing a long note keeps the caret in view", name);
    check(what, caret_in_view(field));
    snprintf(what, sizeof(what), "[%s] editor: the field scrolled to keep it there", name);
    check(what, field && lv_obj_get_scroll_y(field) > 0);
    snprintf(what, sizeof(what), "[%s] editor, long note", name);
    check_editor(what);

    tap_obj(find_labelled(app_body, "Delete"));
    snprintf(what, sizeof(what), "[%s] confirmation", name);
    check_confirm(what);
    if (wide) {
        lv_obj_t *title = find_label(app_body, "Delete this note?");

        body_box(&box);
        area_of(parent_of(title), &a);
        snprintf(what, sizeof(what), "[%s] confirmation: at its portrait width, centred", name);
        check(what, lv_area_get_width(&a) == 528 && LV_ABS((a.x1 - box.x1) - (box.x2 - a.x2)) <= 1);
    }
    tap_obj(find_labelled(app_body, "Cancel"));
    snprintf(what, sizeof(what), "[%s] Cancel gives back the editor, the keyboard and the focus", name);
    check(what, label_present(app_body, "Done") && pocketos_shell_keyboard_visible() &&
                    pos_input_focused() == field);
    snprintf(what, sizeof(what), "[%s] and the caret is in view again", name);
    check(what, caret_in_view(field));
    tap_obj(find_labelled(app_body, "Done"));
    notes_store_read(1, text_buf, sizeof(text_buf));
    snprintf(what, sizeof(what), "[%s] Done stores the long note", name);
    check(what, strstr(text_buf, "line\nend") != NULL && label_present(app_body, "New note"));

    /* Opening a note never scrolls the field into blank space past the note's
     * end. With the field growing into its wrapper (pocketui), text set into
     * an editor not yet laid out scrolled for the field's three-line floor
     * and stayed there once the field grew: a note that fits opened showing
     * only its last lines over empty field. */
    tap_obj(find_labelled(app_body, "Note 01"));
    field = find_field(app_body);
    /* Reopened in the same run, a long note shows from its top, as it did
     * before this layout (0d46e34); what matters here is that it is never
     * scrolled into blank field past its end. */
    snprintf(what, sizeof(what), "[%s] reopening the long note: nothing past its end", name);
    check(what, field && strstr(lv_textarea_get_text(field), "line\nend") != NULL && not_past_the_end(field));
    tap_obj(find_labelled(app_body, "Done"));
    /* The first note opened after the app starts, when the editor has never
     * been laid out. */
    app_stop();
    app_start();
    tap_obj(find_labelled(app_body, "Twelve lines"));
    field = find_field(app_body);
    snprintf(what, sizeof(what), "[%s] a twelve-line note opens %s", name,
             wide ? "at its end, nothing past it" : "whole, from its top");
    check(what, caret_in_view(field) && not_past_the_end(field) &&
                    (wide ? lv_obj_get_scroll_y(field) > 0 : lv_obj_get_scroll_y(field) == 0));
    tap_obj(find_labelled(app_body, "Done"));

    /* The errors, met in this shape. DS 17.1: the caption is seen. A save
     * that fails keeps the editor and the keyboard up, so its caption has to
     * fit above the keyboard, which in landscape is a 100 px body; the field
     * gives up the caption's room. The store's failure is forced as the
     * sections above force it: a directory where the note's file goes. */
    {
        char path[256];
        char block[288];
        const lv_font_t *font;
        struct control c[3];

        /* A short note fits beside the caption whole, and must be seen whole.
         * The caption is created the first time an error is shown, and on
         * unit A that first caption of an app instance left a two-line note
         * scrolled out of sight above it. */
        notes_store_path(20, path, sizeof(path));
        tap_obj(find_labelled(app_body, "Note 20"));
        field = find_field(app_body);
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        unlink(path);
        mkdir(path, 0755);
        tap_obj(find_labelled(app_body, "Done"));
        snprintf(what, sizeof(what), "[%s] failed save, a short note, the first caption", name);
        check_caption(what, "This note could not be saved. It is still here; Done tries again.");
        snprintf(what, sizeof(what), "[%s] failed save, a short note: all of it in view above the caption",
                 name);
        check(what, field && strcmp(lv_textarea_get_text(field), "Note 20\nbody!") == 0 &&
                        caret_in_view(field) && lv_obj_get_scroll_y(field) == 0);
        rmdir(path);
        tap_obj(find_labelled(app_body, "Done"));
        snprintf(what, sizeof(what), "[%s] failed save, a short note: saved once the way is clear", name);
        check(what, label_present(app_body, "New note"));

        notes_store_path(1, path, sizeof(path));
        tap_obj(find_labelled(app_body, "Note 01"));
        field = find_field(app_body);
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        unlink(path);
        mkdir(path, 0755);
        tap_obj(find_labelled(app_body, "Done"));
        snprintf(what, sizeof(what), "[%s] failed save: the editor stays, the keyboard up", name);
        check(what, label_present(app_body, "Done") && pocketos_shell_keyboard_visible() &&
                        pos_input_focused() == field);
        snprintf(what, sizeof(what), "[%s] failed save, above the keyboard", name);
        check_caption(what, "This note could not be saved. It is still here; Done tries again.");
        font = field ? lv_obj_get_style_text_font(field, LV_PART_MAIN) : NULL;
        c[0] = (struct control){ "the field", field, 2 * (font ? lv_font_get_line_height(font) : 0) };
        c[1] = (struct control){ "Done", find_labelled(app_body, "Done"), 56 };
        c[2] = (struct control){ "Delete", find_labelled(app_body, "Delete"), 56 };
        snprintf(what, sizeof(what), "[%s] failed save: two lines of the note or more beside its caption", name);
        check_controls(what, c, 3);
        snprintf(what, sizeof(what), "[%s] failed save: the caret is in view", name);
        check(what, caret_in_view(field));
        rmdir(path);
        tap_obj(find_labelled(app_body, "Done"));
        snprintf(what, sizeof(what), "[%s] failed save: Done again saves and leaves", name);
        check(what, label_present(app_body, "New note") &&
                        notes_store_read(1, text_buf, sizeof(text_buf)) > 0 &&
                        text_buf[strlen(text_buf) - 1] == '!');

        /* A delete that fails comes back to the editor with the keyboard
         * down, and says so under the field. The first caption of a new app
         * instance again, on a short note, which must be seen whole. */
        app_stop();
        app_start();
        tap_obj(find_labelled(app_body, "Note 20"));
        field = find_field(app_body);
        notes_store_read(20, text_buf, sizeof(text_buf));
        notes_store_path(20, path, sizeof(path));
        unlink(path);
        mkdir(path, 0755);
        snprintf(block, sizeof(block), "%s/keep", path);
        close(open(block, O_CREAT | O_WRONLY, 0644));
        tap_obj(find_labelled(app_body, "Delete"));
        tap_obj(find_labelled(app_body, "Delete")); /* the confirmation */
        snprintf(what, sizeof(what), "[%s] failed delete, a short note: all of it in view above the caption",
                 name);
        check(what, field && caret_in_view(field) && lv_obj_get_scroll_y(field) == 0);
        unlink(block);
        rmdir(path);
        notes_store_write(20, text_buf);
        tap_obj(find_labelled(app_body, "Done"));

        notes_store_path(1, path, sizeof(path));
        notes_store_read(1, text_buf, sizeof(text_buf));
        tap_obj(find_labelled(app_body, "Note 01"));
        field = find_field(app_body);
        unlink(path);
        mkdir(path, 0755);
        snprintf(block, sizeof(block), "%s/keep", path);
        close(open(block, O_CREAT | O_WRONLY, 0644));
        tap_obj(find_labelled(app_body, "Delete"));
        tap_obj(find_labelled(app_body, "Delete")); /* the confirmation */
        snprintf(what, sizeof(what), "[%s] failed delete: the editor stays", name);
        check(what, label_present(app_body, "Done") && field &&
                        strcmp(lv_textarea_get_text(field), text_buf) == 0);
        snprintf(what, sizeof(what), "[%s] failed delete", name);
        check_caption(what, "This note could not be deleted. It is still here.");
        snprintf(what, sizeof(what), "[%s] failed delete", name);
        check_editor(what);
        unlink(block);
        rmdir(path);
        tap_obj(find_labelled(app_body, "Done"));
    }
    app_stop();

    /* A note the editor only shows: the keyboard stays down, and the field
     * and the caption saying why reach the foot of the body, clear of the
     * corners there. */
    {
        static char big[2201];

        for (i = 0; i < 2200; i++) {
            big[i] = (i % 40 == 39) ? '\n' : 'a';
        }
        memcpy(big, "Too long\n", 9);
        big[2200] = '\0';
        wipe();
        notes_store_write(5, big);
    }
    app_start();
    tap_obj(find_labelled(app_body, "Too long"));
    field = find_field(app_body);
    snprintf(what, sizeof(what), "[%s] read-only note: the keyboard stays down", name);
    check(what, field && lv_obj_has_state(field, LV_STATE_DISABLED) && !pocketos_shell_keyboard_visible());
    snprintf(what, sizeof(what), "[%s] read-only note", name);
    check_editor(what);
    snprintf(what, sizeof(what), "[%s] read-only note: why, below the field", name);
    check_caption(what, "This note is too long to edit here. It is left exactly as it is.");
    body_box(&box);
    area_of(parent_of(field), &a);
    snprintf(what, sizeof(what), "[%s] read-only note: the field and its caption end where the corners begin",
             name);
    check(what, a.y2 == box.y2 - foot_inset());
    tap_obj(find_labelled(app_body, "Done"));
    app_stop();

    wipe();
    app_start();
    snprintf(what, sizeof(what), "[%s] empty list", name);
    check_list(what, 0);
    snprintf(what, sizeof(what), "[%s] empty list: New note is %s the empty state", name,
             wide ? "beside" : "below");
    check(what, beside(list_card(), find_labelled(app_body, "New note")) == wide);
    app_stop();
    pos_theme_apply(NULL, "normal", why, sizeof(why));
}

/* ---- DS 21.2: the display turning, with a note open --------------------- */

/* The shell applies a rotation by closing the app and opening the display
 * again, so a note open at that moment is saved on the way out and the app
 * meets the new orientation by being created in it. These change the display
 * under the running app instead, which is the harder case: nothing is saved
 * or rebuilt, and everything must still be where it was. */
static void check_turning(void)
{
    lv_obj_t *field;
    lv_obj_t *oldest;
    lv_area_t a;
    int i;

    /* The editor, mid-edit, with the caret moved into the middle. */
    wipe();
    notes_store_write(1, "Alpha\nbeta");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tap_obj(find_labelled(app_body, "Alpha"));
    field = find_field(app_body);
    pos_input_push_key(LV_KEY_LEFT);
    pos_input_push_key(LV_KEY_LEFT);
    drain();
    tap_key("x");
    check_str("portrait: typed at the caret", text_of(field), "Alpha\nbexta");
    check("portrait: the caret is after it", field && lv_textarea_get_cursor_pos(field) == 9);

    use_display(POS_ROTATION_270, PANEL_CORNER);
    check("turned: the editor is still on show", label_present(app_body, "Done"));
    check_str("turned: the text is untouched", text_of(field), "Alpha\nbexta");
    check("turned: the caret has not moved", field && lv_textarea_get_cursor_pos(field) == 9);
    check("turned: the field keeps the focus", pos_input_focused() == field);
    check("turned: the keyboard is still up", pocketos_shell_keyboard_visible());
    check("turned: the caret is in view", caret_in_view(field));
    check("turned: the actions moved beside the field",
          beside(field, find_labelled(app_body, "Done")) == 1);
    check_editor("turned to landscape with a note open");
    tap_key("y");
    pos_input_push_key('z'); /* a key from the stream, as a physical keyboard sends one */
    drain();
    check_str("turned: typing goes on at the caret, tapped or from the stream",
              text_of(field), "Alpha\nbexyzta");

    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("turned back: Done is above the field again", beside(find_labelled(app_body, "Done"), field) == 0);
    check_editor("turned back to portrait with a note open");
    check_str("turned back: the text is untouched", text_of(field), "Alpha\nbexyzta");
    check("turned back: the caret has not moved", field && lv_textarea_get_cursor_pos(field) == 11);
    check("turned back: the field keeps the focus", pos_input_focused() == field);
    check("turned back: nothing was saved on the way", notes_store_read(1, text_buf, sizeof(text_buf)) == 10);
    tap_obj(find_labelled(app_body, "Done"));
    check("Done saves the note that was open, once",
          notes_store_list(list_buf, NOTES_MAX_NOTES) == 1 &&
              notes_store_read(1, text_buf, sizeof(text_buf)) == 13 &&
              strcmp(text_buf, "Alpha\nbexyzta") == 0);

    /* The confirmation, turned: still asking, and Cancel still goes back. */
    tap_obj(find_labelled(app_body, "Alpha"));
    field = find_field(app_body);
    for (i = 0; i < 36; i++) { /* long enough to scroll in either shape */
        tap_key("ENTER");
        type_text("more");
    }
    tap_obj(find_labelled(app_body, "Delete"));
    use_display(POS_ROTATION_270, PANEL_CORNER);
    check("turned: the confirmation is still asking", label_present(app_body, "Delete this note?"));
    check_confirm("confirmation, turned");
    check("and nothing was deleted", notes_store_list(list_buf, NOTES_MAX_NOTES) == 1);
    tap_obj(find_labelled(app_body, "Cancel"));
    check("Cancel after turning gives back the editor, the keyboard and the focus",
          label_present(app_body, "Done") && pocketos_shell_keyboard_visible() &&
              pos_input_focused() == field);
    check("with the caret of the long note in view", caret_in_view(field));
    check_editor("editor after a turned confirmation");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    tap_obj(find_labelled(app_body, "Done"));
    app_stop();

    /* The list, scrolled, turned. */
    wipe();
    write_many();
    app_start();
    oldest = find_labelled(app_body, "Note 01");
    for (i = 0; i < 6 && !inside(oldest, list_card()); i++) {
        drag(list_card(), -400);
    }
    use_display(POS_ROTATION_270, PANEL_CORNER);
    check("turned: the list is still on show, all of it",
          label_present(app_body, "New note") && list_card() && lv_obj_get_child_count(list_card()) == 21u);
    check_list("list, turned", 1);
    check("turned: New note moved beside the rows",
          beside(list_card(), find_labelled(app_body, "New note")) == 1);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check_list("list, turned back", 1);
    tap_obj(find_labelled(app_body, "Note 01"));
    field = find_field(app_body);
    check("and a row still opens its note", field && strcmp(lv_textarea_get_text(field), "Note 01\nbody") == 0);
    tap_obj(find_labelled(app_body, "Done"));
    app_stop();

    /* The way the shell turns the display: the app closed with a note open
     * and created again in the other orientation. */
    wipe();
    notes_store_write(1, "Gamma");
    app_start();
    tap_obj(find_labelled(app_body, "Gamma"));
    tap_key("SPACE");
    type_text("two");
    app_stop();
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    check("reopened in landscape: the edit was kept on the way out",
          notes_store_read(1, text_buf, sizeof(text_buf)) == 9 && strcmp(text_buf, "Gamma two") == 0);
    check("and the list shows it", label_present(app_body, "Gamma two"));
    tap_obj(find_labelled(app_body, "Gamma two"));
    tap_key("?123");
    tap_key("!");
    tap_key("ABC");
    app_stop();
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    check("reopened in portrait: kept again",
          notes_store_read(1, text_buf, sizeof(text_buf)) == 10 && strcmp(text_buf, "Gamma two!") == 0);
    app_stop();

    /* A wide body with no room for the rail beside a portrait-wide list keeps
     * the tall shape: an 800 x 480 panel turned, 760 px across. */
    wipe();
    write_many();
    use_panel(480, 800, POS_ROTATION_270, 0);
    app_start();
    check("a wide body under 836 px keeps New note below the rows",
          beside(list_card(), find_labelled(app_body, "New note")) == 0);
    check_list("list on an 800 x 480 display", 1);
    app_stop();
    use_display(POS_ROTATION_0, PANEL_CORNER);
    area_of(g_content, &a);
    check("back on the reference panel", lv_area_get_width(&a) == PANEL_W);
}

int main(void)
{
    lv_indev_t *finger;
    struct notes_entry list[NOTES_MAX_NOTES];
    char text[NOTES_MAX_BYTES + 1];
    lv_obj_t *field;
    int n;

    /* Line by line, so a run that stops says where. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(root, sizeof(root), "/tmp/pocketnotes-app-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

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
    /* The unit's panel, as the shell opens it in portrait. */
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 1. the empty state -------------------------------------------- */

    app_start();
    check("an empty store shows the empty state", label_present(app_body, "No notes yet"));
    check("and offers a new note", label_present(app_body, "New note"));
    check("the keyboard is not up on the list", !pocketos_shell_keyboard_visible());

    /* ---- 2. create a note and type into it ----------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    check("the editor opens", label_present(app_body, "Done"));
    check("the keyboard comes up with it", pocketos_shell_keyboard_visible());
    field = find_field(app_body);
    check("the editor has a field", field != NULL);
    check("and it is focused", pos_input_focused() == field);
    check("with no error the field is the whole of its wrapper", fills_wrapper(field));

    type_text("Shopping");
    check_str("typed characters reach the note", lv_textarea_get_text(field), "Shopping");
    check("focus stayed on the field while typing", pos_input_focused() == field);

    /* ---- 3. a newline, and the second line ----------------------------- */

    tap_key("ENTER");
    type_text("milk");
    check_str("Enter breaks the line", lv_textarea_get_text(field), "Shopping\nmilk");

    /* ---- 4. æ ø å through the symbol layer ----------------------------- */

    tap_key("ENTER");
    tap_key("?123");
    tap_key("\xC3\xA6");
    tap_key("\xC3\xB8");
    tap_key("\xC3\xA5");
    tap_key("ABC");
    check_str("Norwegian letters type like any other",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 5. Done saves and returns to the list ------------------------- */

    tap_obj(find_labelled(app_body, "Done"));
    check("the keyboard goes away with the editor",
          !pocketos_shell_keyboard_visible());
    check("the list is back", label_present(app_body, "New note"));
    check("titled by the note's first line", label_present(app_body, "Shopping"));

    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("one note was stored", n == 1);
    check("with the text that was typed",
          notes_store_read(list[0].id, text, sizeof(text)) ==
              (int)strlen("Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5"));
    check_str("byte for byte", text, "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 6. reopen it -------------------------------------------------- */

    tap_obj(find_labelled(app_body, "Shopping"));
    field = find_field(app_body);
    check_str("reopening shows what was written",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");
    check("the keyboard is up again", pocketos_shell_keyboard_visible());

    /* Punctuation lives on the symbol layer, and the layer the keyboard is
     * left on is its own business, not the app's. */
    tap_key("?123");
    tap_key("!");
    tap_key("ABC");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("editing does not make a second note", n == 1);
    notes_store_read(list[0].id, text, sizeof(text));
    check("the edit was saved",
          text[strlen(text) - 1] == '!');

    /* ---- 7. a blank note is not worth a file --------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    tap_key("SPACE");
    tap_key("ENTER");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("a note with only whitespace is not stored", n == 1);

    /* ---- 8. leaving the app saves what was being written ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    type_text("draft");
    app_stop();                 /* the shell closing the app */
    check("the keyboard is down after the app closes",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("an unsaved edit survives the app closing", n == 2);
    app_start();
    check("and is there on the next launch", label_present(app_body, "draft"));

    /* ---- 9. delete: cancel changes nothing ----------------------------- */

    tap_obj(find_labelled(app_body, "draft"));
    check("the keyboard is up in the editor", pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Delete"));
    check("the confirmation asks first", label_present(app_body, "Delete this note?"));
    check("the keyboard is dismissed for the dialog (DS §17.5)",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("nothing is deleted when the dialog opens", n == 2);

    tap_obj(find_labelled(app_body, "Cancel"));
    check("Cancel returns to the editor", label_present(app_body, "Done"));
    check("and brings the keyboard back", pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("and deleted nothing", n == 2);

    /* ---- 10. delete: confirm removes it -------------------------------- */

    tap_obj(find_labelled(app_body, "Delete"));   /* editor: open the dialog */
    tap_obj(find_labelled(app_body, "Delete"));   /* dialog: confirm */
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("confirming deletes the note", n == 1);
    check("and returns to the list", label_present(app_body, "New note"));
    check("the deleted note is gone from the list", !label_present(app_body, "draft"));
    check("the other note is untouched", label_present(app_body, "Shopping"));

    /* ---- 11. a note that is not text is shown, not edited over ---------- */

    app_stop();
    {
        char path[256];
        FILE *f;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "wb");
        if (f) {
            (void)!fwrite("bad \xFF byte", 1, 10, f);
            fclose(f);
        }
    }
    app_start();
    check("an unreadable note is listed", label_present(app_body, "Unreadable note"));
    tap_obj(find_labelled(app_body, "Unreadable note"));
    field = find_field(app_body);
    check("opening it does not offer an editable field",
          lv_obj_has_state(field, LV_STATE_DISABLED));
    check("and says why", label_present(app_body, "This note could not be read. "
                                                  "It is left exactly as it is."));
    check_caption("below the field, where it can be read",
                  "This note could not be read. It is left exactly as it is.");
    check("the keyboard stays down for it", !pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Done"));
    {
        char path[256];
        FILE *f;
        char raw[32];
        size_t got = 0;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "rb");
        if (f) {
            got = fread(raw, 1, sizeof(raw), f);
            fclose(f);
        }
        check("leaving it did not overwrite it", got == 10);
    }

    /* ---- 12. a long note scrolls rather than overflowing ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    field = find_field(app_body);
    {
        int i;

        for (i = 0; i < 40; i++) {
            type_text("line");
            tap_key("ENTER");
        }
    }
    check("a long note is all there",
          strlen(lv_textarea_get_text(field)) == 40 * 5);
    lv_obj_update_layout(field);
    check("the editor scrolls rather than growing past the body",
          lv_obj_get_height(field) <= PANEL_H - STATUS_H - POS_KB_H);
    check("and the caret is still in view: the field scrolled",
          lv_obj_get_scroll_y(field) > 0);
    check("the body itself did not become scrollable",
          lv_obj_get_scroll_bottom(app_body) <= 0);
    tap_obj(find_labelled(app_body, "Done"));

    app_stop();

    /* ---- 13. a note longer than the editor holds is shown, not cut ------ */

    /* The store takes up to 4096 bytes and the editor 2000 characters, so a
     * note written somewhere other than this app can be longer than the
     * field. Opening one used to cut it to fit on the way in and save the
     * cut version on the way out (P1-2 of the v0.0.8 review). */
    wipe();
    {
        static char big[3001];
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];
        time_t old = time(NULL) - 7200;
        int i;

        memcpy(big, "Long note\n", 10);
        for (i = 10; i < 3000; i++) {
            big[i] = (i % 50 == 49) ? '\n' : 'a';
        }
        big[3000] = '\0';
        check("a 3000-character note is one the store accepts",
              notes_store_write(60, big) == 0);
        notes_store_path(60, path, sizeof(path));
        set_mtime(path, old);

        app_start();
        tap_obj(find_labelled(app_body, "Long note"));
        field = find_field(app_body);
        check("it opens", field != NULL);
        check("read-only", field && lv_obj_has_state(field, LV_STATE_DISABLED));
        check("and says why",
              label_present(app_body, "This note is too long to edit here. "
                                      "It is left exactly as it is."));
        check_caption("below the field, where it can be read",
                      "This note is too long to edit here. It is left exactly as it is.");
        check("showing all of it, not the first 2000 characters",
              field && strcmp(lv_textarea_get_text(field), big) == 0);
        check("the keyboard stays down for it", !pocketos_shell_keyboard_visible());

        tap_obj(find_labelled(app_body, "Done"));
        check("Done goes back to the list", label_present(app_body, "New note"));
        check("and the note is byte for byte what it was",
              read_raw(path, raw, sizeof(raw)) == 3000 && memcmp(raw, big, 3000) == 0);

        tap_obj(find_labelled(app_body, "Long note"));
        app_stop(); /* the shell's Back */
        check("leaving the app from it keeps it byte for byte too",
              read_raw(path, raw, sizeof(raw)) == 3000 && memcmp(raw, big, 3000) == 0);
        check("and nothing wrote to it at all", has_mtime(path, old));
    }

    /* ---- 14. a note nobody changed is not written again ----------------- */

    {
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];
        time_t old = time(NULL) - 3600;

        notes_store_write(61, "Untouched\nsecond line");
        notes_store_path(61, path, sizeof(path));
        set_mtime(path, old);

        app_start();
        tap_obj(find_labelled(app_body, "Untouched"));
        field = find_field(app_body);
        check("an ordinary note opens editable",
              field && !lv_obj_has_state(field, LV_STATE_DISABLED));
        tap_obj(find_labelled(app_body, "Done"));
        check("Done on a note nobody changed leaves its time alone", has_mtime(path, old));

        tap_obj(find_labelled(app_body, "Untouched"));
        app_stop();
        check("and so does leaving the app from it", has_mtime(path, old));
        check("the bytes are what they were",
              read_raw(path, raw, sizeof(raw)) == 21 &&
                  memcmp(raw, "Untouched\nsecond line", 21) == 0);

        /* A change is still a change. */
        app_start();
        tap_obj(find_labelled(app_body, "Untouched"));
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        tap_obj(find_labelled(app_body, "Done"));
        check("an edited note is written", !has_mtime(path, old));
        check("with the edit in it", read_raw(path, raw, sizeof(raw)) == 22 && raw[21] == '!');
        app_stop();
    }

    /* ---- 14b. a save that fails keeps what it was given ----------------- */

    /* Done used to report "Note not saved" and then go to the list anyway,
     * with open_id cleared: the typed text was still in the field, but the
     * app no longer knew which note it belonged to, and opening the next one
     * wrote these words into that one's file. The store's own failure is
     * forced without touching permissions, so this runs the same as root and
     * as anyone else: a directory stands where the note's file goes, and
     * rename() onto a directory cannot succeed for anybody.
     * Cold review F1. */
    wipe();
    {
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];
        char why[128];

        notes_store_write(70, "Blocked\nfirst");
        notes_store_path(70, path, sizeof(path));

        app_start();
        tap_obj(find_labelled(app_body, "Blocked"));
        field = find_field(app_body);
        check("the note opens editable", field && !lv_obj_has_state(field, LV_STATE_DISABLED));
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        check_str("the edit is in the field", lv_textarea_get_text(field), "Blocked\nfirst!");

        /* Nothing can be renamed onto this. */
        unlink(path);
        mkdir(path, 0755);

        tap_obj(find_labelled(app_body, "Done"));
        check("Done on a failed save stays in the editor",
              !label_present(app_body, "New note"));
        check("with the text still in it",
              strcmp(lv_textarea_get_text(field), "Blocked\nfirst!") == 0);
        check("and says the note was not saved", !strcmp(g_hint, "Note not saved"));
        check("and says so on the field too",
              label_present(app_body, "This note could not be saved. It is still "
                                      "here; Done tries again."));
        check_caption("below the field, where it can be read",
                      "This note could not be saved. It is still here; Done tries again.");
        /* Outdoor's larger type makes the caption taller; the field gives up
         * that much more. */
        check("[outdoor] the mode applies over the error",
              pos_theme_apply(NULL, "outdoor", why, sizeof(why)) == 0);
        check_caption("[outdoor] the caption is still below the field, where it can be read",
                      "This note could not be saved. It is still here; Done tries again.");
        pos_theme_apply(NULL, "normal", why, sizeof(why));

        /* Nothing else may be opened over it: the list is not reachable, so
         * the only way another note could be opened is the app being torn
         * down, which is the case below. */
        rmdir(path);
        tap_obj(find_labelled(app_body, "Done"));
        check("Done again, with the way clear, saves and leaves",
              label_present(app_body, "New note"));
        check("the edit reached the disk",
              read_raw(path, raw, sizeof(raw)) == 14 &&
                  memcmp(raw, "Blocked\nfirst!", 14) == 0);
        check("and the hint is cleared", !strcmp(g_hint, ""));
        tap_obj(find_labelled(app_body, "Blocked"));
        check("reopened, the field has the caption's room back",
              fills_wrapper(find_field(app_body)));
        app_stop();
    }

    /* ---- 14c. the app torn down over a failed save says so -------------- */

    /* destroy() has nowhere to put the text and no one to ask, so the edit is
     * lost - but it is named in the log instead of the app closing as though
     * it had saved. */
    wipe();
    {
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];

        notes_store_write(71, "Doomed\nfirst");
        notes_store_path(71, path, sizeof(path));

        app_start();
        tap_obj(find_labelled(app_body, "Doomed"));
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        unlink(path);
        mkdir(path, 0755);

        g_logged[0] = '\0';
        app_stop(); /* the shell's Back, which destroys the app */
        check("leaving the app over a failed save logs it",
              strstr(g_logged, "notes: note 71") != NULL);
        check("and says the edits are lost", strstr(g_logged, "lost") != NULL);
        rmdir(path);
        check("nothing was written in its place",
              read_raw(path, raw, sizeof(raw)) == -1);
    }

    /* ---- 14d. a delete that fails is not reported as a delete ----------- */

    wipe();
    {
        char path[256];
        char block[288]; /* room for the path and the file kept inside it */

        notes_store_write(72, "Stubborn\nbody");
        notes_store_path(72, path, sizeof(path));

        app_start();
        tap_obj(find_labelled(app_body, "Stubborn"));
        field = find_field(app_body);

        /* unlink() on a non-empty directory fails for root too. */
        unlink(path);
        mkdir(path, 0755);
        snprintf(block, sizeof(block), "%s/keep", path);
        close(open(block, O_CREAT | O_WRONLY, 0644));

        tap_obj(find_labelled(app_body, "Delete"));
        tap_obj(find_labelled(app_body, "Delete")); /* the confirmation */
        check("a failed delete does not go to the list",
              !label_present(app_body, "New note"));
        check("it says the note was not deleted", !strcmp(g_hint, "Note not deleted"));
        check("and says so on the field",
              label_present(app_body, "This note could not be deleted. It is "
                                      "still here."));
        check_caption("below the field, where it can be read",
                      "This note could not be deleted. It is still here.");
        check("the text is not thrown away either",
              field && strcmp(lv_textarea_get_text(field), "Stubborn\nbody") == 0);

        unlink(block);
        rmdir(path);
        app_stop();
    }

    /* ---- 15. twenty notes: the list scrolls and New note stays ---------- */

    /* From sixteen notes the list outgrew a screen that does not scroll, and
     * New note, then the oldest notes, were drawn past its bottom edge where
     * no finger could reach them (P1-1 of the v0.0.8 review). Checked in the
     * shell's own frame, in Normal and in Outdoor, whose type is larger. */
    wipe();
    {
        static const char *const modes[] = { "normal", "outdoor" };
        char path[256];
        char why[128];
        char what[96];
        time_t base = time(NULL) - 100000;
        size_t m;
        int i;

        for (i = 1; i <= 20; i++) {
            char body[32];

            snprintf(body, sizeof(body), "Note %02d\nbody", i);
            notes_store_write((uint32_t)i, body);
            notes_store_path((uint32_t)i, path, sizeof(path));
            set_mtime(path, base + i * 60); /* Note 01 is the oldest */
        }

        for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            lv_obj_t *list_screen;
            lv_obj_t *oldest;
            lv_obj_t *rows;
            int drags;

            snprintf(what, sizeof(what), "[%s] the mode applies", modes[m]);
            check(what, pos_theme_apply(NULL, modes[m], why, sizeof(why)) == 0);
            app_start();
            list_screen = lv_obj_get_child(lv_obj_get_child(app_body, 0), 0);

            snprintf(what, sizeof(what), "[%s] New note is on screen with twenty notes",
                     modes[m]);
            check(what, inside(find_labelled(app_body, "New note"), list_screen));
            snprintf(what, sizeof(what), "[%s] the body itself does not scroll", modes[m]);
            check(what, lv_obj_get_scroll_bottom(app_body) <= 0);

            oldest = find_labelled(app_body, "Note 01");
            rows = oldest ? lv_obj_get_parent(oldest) : NULL;
            snprintf(what, sizeof(what), "[%s] the oldest note starts past the list's edge",
                     modes[m]);
            check(what, oldest && !inside(oldest, rows));
            for (drags = 0; drags < 6 && rows && !inside(oldest, rows); drags++) {
                drag(rows, -400);
            }
            snprintf(what, sizeof(what), "[%s] a finger scrolls the list to it", modes[m]);
            check(what, inside(oldest, rows) && inside(rows, list_screen));
            snprintf(what, sizeof(what), "[%s] with New note still on screen", modes[m]);
            check(what, inside(find_labelled(app_body, "New note"), list_screen));

            tap_obj(oldest);
            field = find_field(app_body);
            snprintf(what, sizeof(what), "[%s] and the oldest note opens", modes[m]);
            check(what, field && strcmp(lv_textarea_get_text(field), "Note 01\nbody") == 0);
            tap_obj(find_labelled(app_body, "Done"));

            tap_obj(find_labelled(app_body, "New note"));
            field = find_field(app_body);
            snprintf(what, sizeof(what), "[%s] New note opens an empty editor", modes[m]);
            check(what, field && lv_textarea_get_text(field)[0] == '\0' &&
                            pocketos_shell_keyboard_visible());
            tap_obj(find_labelled(app_body, "Done")); /* blank, so nothing is stored */
            snprintf(what, sizeof(what), "[%s] and there are still twenty notes", modes[m]);
            check(what, notes_store_list(list, NOTES_MAX_NOTES) == 20);
            app_stop();
        }
        pos_theme_apply(NULL, "normal", why, sizeof(why));
    }

    /* ---- 16. a short list looks exactly as it did ----------------------- */

    wipe();
    notes_store_write(1, "One");
    notes_store_write(2, "Two");
    app_start();
    {
        lv_obj_t *row = find_labelled(app_body, "One");
        lv_obj_t *rows = row ? lv_obj_get_parent(row) : NULL;
        lv_obj_t *button = find_labelled(app_body, "New note");
        lv_area_t r;
        lv_area_t b;

        check("a short list has both rows", rows && label_present(rows, "Two"));
        if (rows && button) {
            lv_obj_update_layout(app_body);
            lv_obj_get_coords(rows, &r);
            lv_obj_get_coords(button, &b);
            check("it is as tall as its rows and no taller",
                  lv_area_get_height(&r) ==
                      2 * POCKETUI_ROW_H + 2 * lv_obj_get_style_border_width(rows, LV_PART_MAIN));
            check("with nothing to scroll",
                  lv_obj_get_scroll_top(rows) <= 0 && lv_obj_get_scroll_bottom(rows) <= 0);
            check("and New note right under it", b.y1 - r.y2 - 1 == POCKETUI_PAD);
        }
    }
    app_stop();

    /* ---- 17. both shapes, to the pixel --------------------------------- */

    /* Portrait is the v0.0.10 layout but for two things (DS 22.2 and the
     * flex note in notes_app.c): the editor's field reaches the body's foot,
     * 20 px further than it did, and whatever reaches the foot stops where
     * the corner squares begin. Landscape puts the actions in a 288 px rail
     * beside the content. Rectangles are inclusive. */
    {
        static const int32_t corners[] = { PANEL_CORNER, 0 };
        size_t k;

        for (k = 0; k < sizeof(corners) / sizeof(corners[0]); k++) {
            int32_t c = corners[k];
            int32_t foot = 1211 - (c > 20 ? c - 20 : 0); /* portrait body foot, less the corners */
            int32_t lfoot = 547 - (c > 20 ? c - 20 : 0); /* the same in landscape */
            char what[96];

            use_display(POS_ROTATION_0, c);
            wipe();
            app_start();
            snprintf(what, sizeof(what), "portrait %d px corners: empty state", (int)c);
            report_rect(what, list_card(), 20, 152, 547, 307);
            snprintf(what, sizeof(what), "portrait %d px corners: New note under it", (int)c);
            report_rect(what, find_labelled(app_body, "New note"), 20, 328, 547, 391);
            app_stop();
            write_many();
            app_start();
            snprintf(what, sizeof(what), "portrait %d px corners: a long list", (int)c);
            report_rect(what, list_card(), 20, 152, 547, foot - 84);
            snprintf(what, sizeof(what), "portrait %d px corners: New note at the foot", (int)c);
            report_rect(what, find_labelled(app_body, "New note"), 20, foot - 63, 547, foot);
            tap_obj(find_labelled(app_body, "Note 20"));
            snprintf(what, sizeof(what), "portrait %d px corners: Done", (int)c);
            report_rect(what, find_labelled(app_body, "Done"), 20, 152, 279, 207);
            snprintf(what, sizeof(what), "portrait %d px corners: Delete", (int)c);
            report_rect(what, find_labelled(app_body, "Delete"), 288, 152, 547, 207);
            snprintf(what, sizeof(what), "portrait %d px corners: the field, down to the keyboard's 20 px",
                     (int)c);
            report_rect(what, find_field(app_body), 20, 228, 547, 915);
            tap_obj(find_labelled(app_body, "Delete"));
            snprintf(what, sizeof(what), "portrait %d px corners: the confirmation", (int)c);
            report_rect(what, parent_of(find_label(app_body, "Delete this note?")), 20, 152, 547, 331);
            tap_obj(find_labelled(app_body, "Cancel"));
            tap_obj(find_labelled(app_body, "Done"));
            app_stop();

            use_display(POS_ROTATION_270, c);
            wipe();
            app_start();
            /* From row 128, not 152: the 32 px COMPACT bar of DS section 30
             * above the same header and padding. What is pinned to the foot
             * (the list, the field's keyboard edge at 251) keeps its foot. */
            snprintf(what, sizeof(what), "landscape %d px corners: empty state", (int)c);
            report_rect(what, list_card(), 20, 128, 903, 283);
            snprintf(what, sizeof(what), "landscape %d px corners: New note in the rail", (int)c);
            report_rect(what, find_labelled(app_body, "New note"), 924, 128, 1211, 191);
            app_stop();
            write_many();
            app_start();
            snprintf(what, sizeof(what), "landscape %d px corners: a long list", (int)c);
            report_rect(what, list_card(), 20, 128, 903, lfoot);
            snprintf(what, sizeof(what), "landscape %d px corners: New note in the rail", (int)c);
            report_rect(what, find_labelled(app_body, "New note"), 924, 128, 1211, 191);
            tap_obj(find_labelled(app_body, "Note 20"));
            snprintf(what, sizeof(what), "landscape %d px corners: the field above the keyboard", (int)c);
            report_rect(what, find_field(app_body), 20, 128, 903, 251);
            snprintf(what, sizeof(what), "landscape %d px corners: Done in the rail", (int)c);
            report_rect(what, find_labelled(app_body, "Done"), 924, 128, 1063, 183);
            snprintf(what, sizeof(what), "landscape %d px corners: Delete in the rail", (int)c);
            report_rect(what, find_labelled(app_body, "Delete"), 1072, 128, 1211, 183);
            tap_obj(find_labelled(app_body, "Delete"));
            snprintf(what, sizeof(what), "landscape %d px corners: the confirmation, centred", (int)c);
            report_rect(what, parent_of(find_label(app_body, "Delete this note?")), 352, 128, 879, 307);
            tap_obj(find_labelled(app_body, "Cancel"));
            tap_obj(find_labelled(app_body, "Done"));
            app_stop();
        }
        use_display(POS_ROTATION_0, PANEL_CORNER);
    }

    /* ---- 18. every screen in both orientations and both modes ---------- */

    check_orientation("portrait", POS_ROTATION_0, PANEL_CORNER, "normal");
    check_orientation("landscape", POS_ROTATION_270, PANEL_CORNER, "normal");
    check_orientation("portrait, Outdoor", POS_ROTATION_0, PANEL_CORNER, "outdoor");
    check_orientation("landscape, Outdoor", POS_ROTATION_270, PANEL_CORNER, "outdoor");
    check_orientation("landscape, square corners", POS_ROTATION_270, 0, "normal");
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 19. the display turning with a note open ---------------------- */

    check_turning();

    /* ---- 20. opened and closed, both ways up ---------------------------- */

    wipe();
    notes_store_write(1, "Kept");
    {
        int i;

        for (i = 0; i < 6; i++) {
            use_display(i % 2 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            app_start();
            tap_obj(find_labelled(app_body, "Kept"));
            tap_obj(find_labelled(app_body, "Done"));
            app_stop();
        }
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("six rounds in both orientations leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("the keyboard is down", !pocketos_shell_keyboard_visible());
    check("and the note is as it was", notes_store_list(list_buf, NOTES_MAX_NOTES) == 1 &&
                                          notes_store_read(1, text_buf, sizeof(text_buf)) == 4);

    wipe();
    printf("notes_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
