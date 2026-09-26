/*
 * PocketCalendar in the running app, driven by a real LVGL pointer device.
 *
 * The unit tests either side of this one prove the arithmetic and the state
 * machine. This one proves the part they cannot: that a finger landing on a
 * cell reaches them, that the cell it lands on is the one under it, and that
 * an empty cell is not a target. It exists so that a board is the first
 * HARDWARE test of the tap path and not the first test of it at all.
 *
 * It also proves the layout (DS section 21.3 and Calendar's own amendment):
 * the app hosted the way the shell hosts it - a header and a padded body -
 * on the reference panel with its 30 px rounded corners and with square ones,
 * in portrait and landscape, in Normal and Outdoor type; portrait placed
 * exactly where v0.0.10 placed it; every target a finger's size, inside the
 * body and the safe area and on no other target; every month on screen whole;
 * and the display turned under the open app without losing the month, the
 * selection or today, and without a second copy of anything.
 *
 * The shell is not here, so this file plays it - and the one entry point the
 * app uses, pocketos_shell_system_day(), is implemented below over a variable
 * the test sets. That is the whole reason the app asks the shell for the date
 * instead of reading a clock: an unset clock, the moment it is set, and the
 * moment it stops being valid are all reachable here without touching the
 * host's clock or waiting for midnight.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/calendar_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "cal_view.h"
#include "pocketui.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* Where the shell's content area starts for this app in the display's
 * orientation (ui/shell/chrome.h, DS sections 30 and 36), so the frame built
 * here is the one shell.c builds: the top edge, since no chrome reserves a
 * row there any more. */
#define STATUS_H chrome_height(chrome_resolve(app_calendar.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
/* Rows measured under the retired bars - 56 px in portrait (v0.0.10), 32 in
 * landscape (DS section 30) - moved up with the frame's top: nothing else
 * about those places changed. */
#define V010_ROW(y) ((y) - 56 + STATUS_H)
#define V010_LROW(y) ((y) - 32 + STATUS_H)
/* Above the body in landscape and below it: the status bar, the header and
 * the body's own top and foot padding (DS 7) - and how far the unit's 30 px
 * corners reach above the body's foot. */
#define BODY_CHROME_H (STATUS_H + POCKETUI_HEADER_H + POCKETUI_BODY_PAD_TOP + POCKETUI_PAD)
#define CORNER_REACH 10

/* What the layout promises, written down here rather than read from the app:
 * the portrait body's width (DS 7), the 20 px gutter, and in the wide shape
 * cells at least as tall as DS 7's paired buttons under 24 px headings, 4 px
 * apart both ways. */
#define COLUMN_W 528
#define WIDE_MIN_W (2 * COLUMN_W + POCKETUI_PAD)
#define WIDE_CELL_MIN_H 56
#define WIDE_HEADINGS_H 24
#define WIDE_MIN_H (WIDE_HEADINGS_H + CAL_ROWS * (4 + WIDE_CELL_MIN_H))

/* The dates this test drives the app with. September 2026 starts on a
 * Tuesday, so cell 0 is a leading blank and the 12th lands in cell 12 -
 * which is the Monday-first offset arriving on the glass. */
#define DAY_2026_09_12 20260912LL
#define DAY_2026_09_13 20260913LL
/* A Wednesday in September: the longest date in words there is. */
#define DAY_2026_09_30 20260930LL
#define SEPT_LEAD 1
#define SEPT_DAYS 30

/* The app's own layout, as cal_app.c builds it under the body: one frame,
 * and in it the month row, the grid block, the panel and the button.
 * Reaching for a cell by position is the only way to ask about one that has
 * no text in it. */
#define KID_NAV 0
#define KID_GRID 1
#define KID_PANEL 2
#define KID_TODAY 3
#define NAV_PREV 0
#define NAV_MONTH 1
#define NAV_NEXT 2
#define CELL_LABEL 0
#define CELL_DOT 1

extern const struct pocketos_app app_calendar;

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

/* ---- the shell's side of app.h ----------------------------------------- */

/* What the shell's tick would have seen. -1 is an unset wall clock, which on
 * this board is the state it boots into. */
static int64_t g_system_day = DAY_2026_09_12;

int64_t pocketos_shell_system_day(void)
{
    return g_system_day;
}

void pocketos_shell_set_status_hint(const char *text) { (void)text; }
void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
}
void pocketos_shell_keyboard_hide(void) { }
int pocketos_shell_keyboard_visible(void) { return 0; }

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

/* ---- finding things in the app's tree ---------------------------------- */

static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

static lv_obj_t *kid(lv_obj_t *parent, int i)
{
    return parent ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

static lv_obj_t *frame_of(void) { return kid(app_body, 0); }
static lv_obj_t *nav_row(void) { return kid(frame_of(), KID_NAV); }
static lv_obj_t *nav_of(int which) { return kid(nav_row(), which); }
static lv_obj_t *grid_block(void) { return kid(frame_of(), KID_GRID); }
static lv_obj_t *headings(void) { return kid(grid_block(), 0); }
static lv_obj_t *week_of(int row) { return kid(grid_block(), 1 + row); }
/* Child 0 of the block is the weekday headings; the six day rows follow. */
static lv_obj_t *cell_at(int i)
{
    return kid(week_of(i / CAL_COLS), i % CAL_COLS);
}
static lv_obj_t *cell_label(int i) { return kid(cell_at(i), CELL_LABEL); }
static lv_obj_t *cell_dot(int i) { return kid(cell_at(i), CELL_DOT); }
static lv_obj_t *panel_of(void) { return kid(frame_of(), KID_PANEL); }
static lv_obj_t *today_button(void) { return kid(frame_of(), KID_TODAY); }

static const char *text_of(lv_obj_t *label)
{
    return label ? lv_label_get_text(label) : "(missing)";
}

static const char *month_text(void) { return text_of(nav_of(NAV_MONTH)); }

/* Depth-first search for a visible label with this text. A hidden subtree is
 * still in the tree and a finger cannot reach it, so neither may this - which
 * is what makes it the right question to ask about the "Date not set"
 * notice. */
static lv_obj_t *find_visible(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_visible(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int shows(const char *text)
{
    return find_visible(app_body, text) != NULL;
}

/* The panel holds the notice and the selected date; whichever is showing is
 * the one to read. The date is the last child. */
static lv_obj_t *selected_label(void)
{
    lv_obj_t *panel = panel_of();

    return kid(panel, (int)lv_obj_get_child_count(panel) - 1);
}

static const char *selected_text(void)
{
    lv_obj_t *label = selected_label();

    if (!label || lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
        return "";
    }
    return text_of(label);
}

/* How many cells carry the today dot, and which one. The dot is the app's
 * non-colour half of the today mark, so counting it is exactly the question
 * "does the screen claim a today". */
static int dots_shown(int *cell)
{
    int i, n = 0;

    for (i = 0; i < CAL_CELLS; i++) {
        if (!lv_obj_has_flag(cell_dot(i), LV_OBJ_FLAG_HIDDEN)) {
            n++;
            if (cell) {
                *cell = i;
            }
        }
    }
    return n;
}

static int clickable(lv_obj_t *obj)
{
    return obj && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

static int clickable_cells(void)
{
    int i, n = 0;

    for (i = 0; i < CAL_CELLS; i++) {
        n += clickable(cell_at(i));
    }
    return n;
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

/* ---- the app's lifecycle, as the shell runs it -------------------------- */

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
    app_priv = app_calendar.create(app_body);
    pump(60);
}

/* The shell's once-a-second tick, which is the only thing that tells the app
 * the date has changed. */
static void app_tick(void)
{
    app_calendar.tick(app_priv);
    pump(60);
}

static void app_stop(void)
{
    app_calendar.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

/* The display the shell would open: a panel `width` x `long_side` with corner
 * squares of `corner` px, turned to `rotation`, the geometry handed to
 * PocketUI and the content area below the status bar sized to it. Called
 * with the app open, it is the body changing shape under a running app. */
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
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    use_panel_sized(PANEL_W, PANEL_H, rotation, corner);
}

static void use_mode(const char *mode)
{
    char why[128];

    pos_theme_apply(NULL, mode, why, sizeof(why));
    pump(60);
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

static int rect_is(lv_obj_t *obj, int32_t x1, int32_t x2, int32_t y1, int32_t y2)
{
    lv_area_t a;

    area_of(obj, &a);
    if (a.x1 == x1 && a.x2 == x2 && a.y1 == y1 && a.y2 == y2) {
        return 1;
    }
    printf("     at %d..%d x %d..%d, want %d..%d x %d..%d\n", (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
           (int)x1, (int)x2, (int)y1, (int)y2);
    return 0;
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

/* The wide shape's rule, read off the body: wider than tall once the foot
 * has cleared the corners, room for two portrait-wide halves and the gutter,
 * and room for six weeks of cells at least as tall as a paired button. */
static int expect_wide(void)
{
    lv_area_t b;
    int32_t w;
    int32_t h;

    body_box(&b);
    w = lv_area_get_width(&b);
    h = lv_area_get_height(&b) - foot_inset();
    return w > h && w >= WIDE_MIN_W && h >= WIDE_MIN_H;
}

/* The shape on the glass: the panel beside the month rather than under it. */
static int laid_wide(void)
{
    lv_area_t m;
    lv_area_t p;

    area_of(grid_block(), &m);
    area_of(panel_of(), &p);
    return p.x1 > m.x2;
}

/* The top and the height of what a label draws of its text: the glyphs'
 * boxes, placed the way lv_draw_label places them, over every character. */
static void ink_of(lv_obj_t *label, int32_t *top, int32_t *bottom)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const char *t = lv_label_get_text(label);
    lv_area_t a;

    area_of(label, &a);
    *top = LV_COORD_MAX;
    *bottom = LV_COORD_MIN;
    for (; t && *t; t++) {
        lv_font_glyph_dsc_t g;
        uint32_t letter = (unsigned char)*t;
        int32_t y1;

        if (letter >= 0x80) {
            /* Only the bullet is multi-byte here: U+2022, three bytes. */
            letter = 0x2022;
            t += 2;
        }
        if (!lv_font_get_glyph_dsc(font, &g, letter, 0) || g.box_h == 0) {
            continue;
        }
        y1 = a.y1 + (font->line_height - font->base_line) - g.box_h - g.ofs_y;
        *top = LV_MIN(*top, y1);
        *bottom = LV_MAX(*bottom, y1 + g.box_h - 1);
    }
}

/* Every visible thing a finger can press: clickable, with a handler, and not
 * the frame, whose only handler is the one that lays it out. */
static int collect_targets(lv_obj_t *obj, lv_obj_t **out, int n, int max)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return n;
    }
    if (obj != frame_of() && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        lv_obj_get_event_count(obj) > 0 && n < max) {
        out[n++] = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n = collect_targets(lv_obj_get_child(obj, i), out, n, max);
    }
    return n;
}

/* Every visible label inside its parent's box, and no two visible children
 * of a flex row or a grid on top of each other. */
static int laid_out_whole(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t j;
    int bad = 0;
    lv_layout_t layout;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
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
    layout = lv_obj_get_style_layout(obj, 0);
    if (layout == LV_LAYOUT_FLEX || layout == LV_LAYOUT_GRID) {
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

/* ---- the whole screen, checked ----------------------------------------- */

/* One screen as it stands, in whichever shape the body gives it:
 *
 * - the shape is the one the body's size calls for, and the body itself
 *   never scrolls (the app is exactly its content box);
 * - the frame clears the corners at its foot and nowhere else;
 * - every target a finger's size - the arrows and Today 64 x 64, a day 72 px
 *   square when tall and at least 64 wide by 56 tall when wide - on no other
 *   target, and wholly on show inside the body and the safe area;
 * - every label drawn whole and no two neighbours on top of each other;
 * - the panel saying all it says without scrolling;
 * - the month a true grid: seven columns under their headings, six weeks of
 *   one height, and today's dot and number both in their cell, apart;
 * - and the pieces where the shape puts them. */
static void check_screen(const char *name)
{
    static lv_obj_t *t[64];
    char what[200];
    lv_area_t body;
    lv_area_t fr;
    lv_area_t nav;
    lv_area_t month;
    lv_area_t panel;
    lv_area_t today;
    lv_area_t last;
    int32_t inset = foot_inset();
    bool wide = expect_wide();
    int n;
    int i;
    int j;
    int small = 0;
    int overlap = 0;
    int unsafe = 0;
    int cols_bad = 0;
    int rows_bad = 0;
    int32_t min_h = LV_COORD_MAX;
    int32_t max_h = 0;
    int32_t min_w = LV_COORD_MAX;
    int32_t max_w = 0;
    int dot_cell = -1;

    body_box(&body);
    snprintf(what, sizeof(what), "[%s] the body holds one frame, exactly its content box", name);
    area_of(frame_of(), &fr);
    check(what, lv_obj_get_child_count(app_body) == 1 && fr.x1 == body.x1 && fr.x2 == body.x2 &&
                    fr.y1 == body.y1 && fr.y2 == body.y2);
    snprintf(what, sizeof(what), "[%s] and the body is not scrolled", name);
    check(what, lv_obj_get_scroll_y(app_body) == 0);
    snprintf(what, sizeof(what), "[%s] the shape is the one the body calls for (%s)", name, wide ? "wide" : "tall");
    check(what, laid_wide() == wide);
    snprintf(what, sizeof(what), "[%s] the frame's foot gives way to the corners (%d px), and only its foot", name,
             (int)inset);
    check(what, lv_obj_get_style_pad_bottom(frame_of(), 0) == inset && lv_obj_get_style_pad_top(frame_of(), 0) == 0 &&
                    lv_obj_get_style_pad_left(frame_of(), 0) == 0 &&
                    lv_obj_get_style_pad_right(frame_of(), 0) == 0);
    snprintf(what, sizeof(what), "[%s] the frame is not scrolled", name);
    check(what, lv_obj_get_scroll_y(frame_of()) == 0);
    if (wide) {
        snprintf(what, sizeof(what), "[%s] and does not scroll in the wide shape, where the panel scrolls itself if it must",
                 name);
        check(what, !lv_obj_has_flag(frame_of(), LV_OBJ_FLAG_SCROLLABLE) &&
                        lv_obj_has_flag(panel_of(), LV_OBJ_FLAG_SCROLLABLE));
    } else {
        snprintf(what, sizeof(what), "[%s] the tall shape: the frame scrolls if it must, the panel never", name);
        check(what, lv_obj_has_flag(frame_of(), LV_OBJ_FLAG_SCROLLABLE) &&
                        !lv_obj_has_flag(panel_of(), LV_OBJ_FLAG_SCROLLABLE));
    }

    n = collect_targets(frame_of(), t, 0, 64);
    snprintf(what, sizeof(what), "[%s] every target there is: two arrows, the month's days and Today when it acts",
             name);
    check(what, n == 2 + clickable_cells() + clickable(today_button()));
    lv_obj_get_content_coords(frame_of(), &fr);
    for (i = 0; i < n; i++) {
        lv_area_t a;
        int32_t w;
        int32_t h;
        bool is_cell = lv_obj_get_parent(t[i]) != nav_row() && t[i] != today_button();

        area_of(t[i], &a);
        w = lv_area_get_width(&a);
        h = lv_area_get_height(&a);
        if (is_cell ? (wide ? (w < POCKETUI_TOUCH_MIN || h < WIDE_CELL_MIN_H) : (w != 72 || h != 72))
                    : (w < POCKETUI_TOUCH_MIN || h < POCKETUI_TOUCH_MIN)) {
            printf("     target %dx%d at %d,%d is not a finger's size\n", (int)w, (int)h, (int)a.x1, (int)a.y1);
            small++;
        }
        for (j = i + 1; j < n; j++) {
            lv_area_t b;

            area_of(t[j], &b);
            if (overlaps(&a, &b)) {
                overlap++;
            }
        }
        if (!within(&a, &body) || !within(&a, &fr) ||
            !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
            printf("     target %d..%d x %d..%d is not inside the body and the safe area\n", (int)a.x1, (int)a.x2,
                   (int)a.y1, (int)a.y2);
            unsafe++;
        }
    }
    snprintf(what, sizeof(what), "[%s] every target is a finger's size", name);
    check(what, small == 0);
    snprintf(what, sizeof(what), "[%s] no target is on another", name);
    check(what, overlap == 0);
    snprintf(what, sizeof(what), "[%s] every target is on show, in the body and the safe area", name);
    check(what, unsafe == 0);
    snprintf(what, sizeof(what), "[%s] everything is laid out whole", name);
    check(what, laid_out_whole(frame_of()) == 0);
    snprintf(what, sizeof(what), "[%s] the panel says all it says without scrolling", name);
    check(what, lv_obj_get_scroll_y(panel_of()) == 0 && lv_obj_get_scroll_bottom(panel_of()) <= 0);
    snprintf(what, sizeof(what), "[%s] the month's name is one line", name);
    check(what, lv_obj_get_height(nav_of(NAV_MONTH)) ==
                    lv_font_get_line_height(lv_obj_get_style_text_font(nav_of(NAV_MONTH), LV_PART_MAIN)));

    /* The month as a grid. */
    for (i = 0; i < CAL_CELLS; i++) {
        lv_area_t c;
        lv_area_t col;
        lv_area_t row;

        area_of(cell_at(i), &c);
        area_of(kid(headings(), i % CAL_COLS), &col);
        area_of(cell_at(i - i % CAL_COLS), &row);
        if (c.x1 != col.x1 || c.x2 != col.x2) {
            cols_bad++;
        }
        if (c.y1 != row.y1 || c.y2 != row.y2) {
            rows_bad++;
        }
        min_h = LV_MIN(min_h, lv_area_get_height(&c));
        max_h = LV_MAX(max_h, lv_area_get_height(&c));
        min_w = LV_MIN(min_w, lv_area_get_width(&c));
        max_w = LV_MAX(max_w, lv_area_get_width(&c));
    }
    snprintf(what, sizeof(what), "[%s] seven columns, each under its heading", name);
    check(what, cols_bad == 0);
    snprintf(what, sizeof(what), "[%s] six weeks, each in one row", name);
    check(what, rows_bad == 0);
    snprintf(what, sizeof(what), "[%s] every cell the same size, to the pixel LVGL shares out (%d-%d x %d-%d)", name,
             (int)min_w, (int)max_w, (int)min_h, (int)max_h);
    check(what, max_h - min_h <= 1 && max_w - min_w <= 1);
    for (i = 1; i < CAL_ROWS; i++) {
        lv_area_t up;
        lv_area_t down;

        area_of(week_of(i - 1), &up);
        area_of(week_of(i), &down);
        if (down.y1 - up.y2 - 1 != (wide ? 4 : 8)) {
            rows_bad++;
        }
    }
    snprintf(what, sizeof(what), "[%s] the weeks are %d px apart", name, wide ? 4 : 8);
    check(what, rows_bad == 0);
    snprintf(what, sizeof(what), "[%s] the headings are %d px tall", name, wide ? WIDE_HEADINGS_H : 32);
    check(what, lv_obj_get_height(headings()) == (wide ? WIDE_HEADINGS_H : 32));

    if (dots_shown(&dot_cell) == 1) {
        lv_area_t c;
        int32_t num_top;
        int32_t num_bottom;
        int32_t dot_top;
        int32_t dot_bottom;

        area_of(cell_at(dot_cell), &c);
        ink_of(cell_label(dot_cell), &num_top, &num_bottom);
        ink_of(cell_dot(dot_cell), &dot_top, &dot_bottom);
        snprintf(what, sizeof(what), "[%s] today's number and dot are drawn inside the cell, the dot below (%d..%d, %d..%d in %d..%d)",
                 name, (int)num_top, (int)num_bottom, (int)dot_top, (int)dot_bottom, (int)c.y1, (int)c.y2);
        check(what, num_top >= c.y1 && dot_bottom <= c.y2 && dot_top > num_bottom + 1);
    }

    area_of(nav_row(), &nav);
    area_of(grid_block(), &month);
    area_of(panel_of(), &panel);
    area_of(today_button(), &today);
    area_of(week_of(CAL_ROWS - 1), &last);
    if (wide) {
        int32_t half = (lv_area_get_width(&body) - POCKETUI_PAD) / 2;

        snprintf(what, sizeof(what), "[%s] the month takes the first half, from the top to the foot less the corners",
                 name);
        check(what, month.x1 == body.x1 && lv_area_get_width(&month) == half && month.y1 == body.y1 &&
                        month.y2 == body.y2 - inset && last.y2 == month.y2);
        snprintf(what, sizeof(what), "[%s] and the second half, past the gutter, the rest", name);
        check(what, nav.x1 == month.x2 + POCKETUI_PAD + 1 && nav.x2 == body.x2 && panel.x1 == nav.x1 &&
                        panel.x2 == nav.x2 && today.x1 == nav.x1 && today.x2 == nav.x2 &&
                        lv_area_get_width(&nav) >= COLUMN_W);
        snprintf(what, sizeof(what), "[%s] the arrows at its top, Today at its foot, the panel between", name);
        check(what, nav.y1 == body.y1 && lv_area_get_height(&nav) == 64 && today.y2 == body.y2 - inset &&
                        lv_area_get_height(&today) == 64 && panel.y1 == nav.y2 + POCKETUI_PAD + 1 &&
                        panel.y2 == today.y1 - POCKETUI_PAD - 1);
    } else {
        snprintf(what, sizeof(what), "[%s] one under the other across the body, 20 px apart, from its top", name);
        check(what, nav.y1 == body.y1 && month.y1 == nav.y2 + POCKETUI_PAD + 1 &&
                        panel.y1 == month.y2 + POCKETUI_PAD + 1 && today.y1 == panel.y2 + POCKETUI_PAD + 1 &&
                        nav.x1 == body.x1 && nav.x2 == body.x2 && month.x1 == body.x1 && month.x2 == body.x2 &&
                        panel.x1 == body.x1 && panel.x2 == body.x2 && today.x1 == body.x1 && today.x2 == body.x2);
    }
}

/* The screens that matter, in one orientation, one corner and one mode: an
 * ordinary month with today selected, a month of six weeks, a month of four,
 * the longest date in words, and a board that does not know the date with a
 * day picked anyway - the panel at its tallest. */
static void check_orientation(const char *name, enum pos_rotation rotation, int32_t corner, const char *mode)
{
    char what[200];
    int i;

    use_display(rotation, corner);
    use_mode(mode);

    g_system_day = DAY_2026_09_12;
    app_start();
    snprintf(what, sizeof(what), "%s, September 2026", name);
    check_screen(what);

    tap_obj(nav_of(NAV_PREV));
    tap_obj(cell_at(5 + 20)); /* August 2026 starts on a Saturday: the 21st */
    check_str("August 2026, and a day in it", selected_text(), "Friday 21 August 2026");
    snprintf(what, sizeof(what), "%s, August 2026: six weeks", name);
    check_screen(what);
    check_str("its last day is in the sixth week", text_of(cell_label(5 + 30)), "31");

    for (i = 0; i < 6; i++) {
        tap_obj(nav_of(NAV_NEXT));
    }
    check_str("across the year to February 2027", month_text(), "February 2027");
    snprintf(what, sizeof(what), "%s, February 2027: four weeks", name);
    check_screen(what);
    check_str("which starts in the first cell", text_of(cell_label(0)), "1");
    check_str("and ends on the 28th, in the fourth week", text_of(cell_label(27)), "28");
    check("the leading and trailing blanks are no targets", clickable_cells() == 28);
    app_stop();

    g_system_day = DAY_2026_09_30;
    app_start();
    check_str("the longest date in words", selected_text(), "Wednesday 30 September 2026 - Today");
    snprintf(what, sizeof(what), "%s, the longest date in words", name);
    check_screen(what);
    app_stop();

    g_system_day = -1;
    app_start();
    tap_obj(cell_at(14));
    check("no date, and a day picked: the notice", shows("Date not set"));
    check_str("and the day", selected_text(), "Monday 15 January 2024");
    snprintf(what, sizeof(what), "%s, no date and a day picked", name);
    check_screen(what);
    app_stop();

    g_system_day = DAY_2026_09_12;
    use_mode("normal");
}

/* Every object's rectangle, in tree order, so that a layout can be compared
 * with another to the pixel. */
static int snapshot(lv_obj_t *obj, lv_area_t *out, int n, int max)
{
    uint32_t i;

    if (n < max) {
        area_of(obj, &out[n]);
        n++;
    }
    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n = snapshot(lv_obj_get_child(obj, i), out, n, max);
    }
    return n;
}

int main(void)
{
    lv_indev_t *finger;
    int i;
    int dot_cell = -1;

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
    pump(60);
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 1. it opens, and the month is on the glass -------------------- */

    g_system_day = DAY_2026_09_12;
    app_start();
    check_str("the app opens on the system date's month", month_text(), "September 2026");
    check("the weekday headings are there, Monday first",
          strcmp(text_of(kid(headings(), 0)), "Mon") == 0);
    check_str("and Sunday is the last column", text_of(kid(headings(), CAL_COLS - 1)), "Sun");
    check("the grid has six rows of seven",
          (int)lv_obj_get_child_count(grid_block()) == 1 + CAL_ROWS);
    {
        int wrong = 0;

        for (i = 0; i < CAL_ROWS; i++) {
            if ((int)lv_obj_get_child_count(week_of(i)) != CAL_COLS) {
                wrong++;
            }
        }
        check("every row is seven cells", wrong == 0);
    }
    /* September 2026 starts on a Tuesday: cell 0 is blank and the days run
     * from cell 1. This is the whole Monday-first claim, on screen. */
    check_str("the leading cell is empty", text_of(cell_label(0)), "");
    check_str("the first falls in the Tuesday column", text_of(cell_label(SEPT_LEAD)), "1");
    check_str("and the last day is where the month ends",
              text_of(cell_label(SEPT_LEAD + SEPT_DAYS - 1)), "30");
    check_str("the cell after it is empty",
              text_of(cell_label(SEPT_LEAD + SEPT_DAYS)), "");
    check_str("the last cell of the grid is empty", text_of(cell_label(CAL_CELLS - 1)), "");
    check("exactly one cell is marked today", dots_shown(&dot_cell) == 1);
    check("and it is the twelfth's cell", dot_cell == 12);
    check_str("the day it marks", text_of(cell_label(12)), "12");
    check_str("the date is written out", selected_text(),
              "Saturday 12 September 2026 - Today");
    check("no notice is up", !shows("Date not set"));
    check("Today can be pressed", clickable(today_button()));

    /* ---- 2. a finger on previous and next ------------------------------ */

    tap_obj(nav_of(NAV_PREV));
    check_str("a tap on previous steps back a month", month_text(), "August 2026");
    check("today is not in that month, and nothing claims to be",
          dots_shown(NULL) == 0);
    check_str("the selected date is unchanged by browsing", selected_text(),
              "Saturday 12 September 2026 - Today");
    tap_obj(nav_of(NAV_NEXT));
    check_str("a tap on next comes back", month_text(), "September 2026");
    check("and today is marked again", dots_shown(NULL) == 1);
    tap_obj(nav_of(NAV_NEXT));
    check_str("another goes on to October", month_text(), "October 2026");
    /* Twelve back and twelve forward is where a year boundary would show. */
    for (i = 0; i < 12; i++) {
        tap_obj(nav_of(NAV_PREV));
    }
    check_str("twelve taps back cross into the previous year", month_text(),
              "October 2025");
    for (i = 0; i < 12; i++) {
        tap_obj(nav_of(NAV_NEXT));
    }
    check_str("and twelve forward return", month_text(), "October 2026");
    tap_obj(nav_of(NAV_PREV));
    check_str("back to September", month_text(), "September 2026");

    /* ---- 3. a finger on a day ------------------------------------------ */

    tap_obj(cell_at(SEPT_LEAD + 19)); /* the 20th */
    check_str("a tap on a day selects it", selected_text(), "Sunday 20 September 2026");
    check("and today is still marked, on its own cell", dots_shown(&dot_cell) == 1);
    check("which is not the one just tapped", dot_cell == 12);
    tap_obj(cell_at(SEPT_LEAD)); /* the 1st */
    check_str("a tap on the first selects it", selected_text(),
              "Tuesday 1 September 2026");
    tap_obj(cell_at(SEPT_LEAD + SEPT_DAYS - 1)); /* the 30th */
    check_str("and a tap on the last", selected_text(), "Wednesday 30 September 2026");

    /* ---- 4. the cells that are not days -------------------------------- */

    check("a leading blank takes no taps", !clickable(cell_at(0)));
    tap_obj(cell_at(0));
    check_str("so tapping one selects nothing", selected_text(),
              "Wednesday 30 September 2026");
    check("a trailing blank takes no taps either",
          !clickable(cell_at(SEPT_LEAD + SEPT_DAYS)));
    tap_obj(cell_at(SEPT_LEAD + SEPT_DAYS));
    check_str("and nor does that", selected_text(), "Wednesday 30 September 2026");
    tap_obj(cell_at(CAL_CELLS - 1));
    check_str("nor the last cell of the grid", selected_text(),
              "Wednesday 30 September 2026");
    check("exactly the month's days are targets", clickable_cells() == SEPT_DAYS);

    /* ---- 5. a finger on Today ------------------------------------------ */

    tap_obj(nav_of(NAV_PREV));
    tap_obj(nav_of(NAV_PREV));
    check_str("browsed away", month_text(), "July 2026");
    tap_obj(today_button());
    check_str("Today returns to the system date's month", month_text(), "September 2026");
    check("marks it", dots_shown(&dot_cell) == 1);
    check("on the right cell", dot_cell == 12);
    check_str("and selects it", selected_text(), "Saturday 12 September 2026 - Today");

    /* ---- 6. the blank cells survive a month change --------------------- */

    /* October 2026 starts on a Thursday, so the leading run is longer than
     * September's. A cell that was a day and is now blank has to stop being
     * a target, and the days have to move to their new columns. */
    tap_obj(nav_of(NAV_NEXT));
    check_str("October is on screen", month_text(), "October 2026");
    check_str("its first is in the Thursday column", text_of(cell_label(3)), "1");
    check_str("and the cells before it are empty", text_of(cell_label(2)), "");
    check("which no longer take taps", !clickable(cell_at(2)));
    tap_obj(cell_at(2));
    check_str("so the selection is untouched", selected_text(),
              "Saturday 12 September 2026 - Today");
    tap_obj(cell_at(3));
    check_str("while the first of October does select", selected_text(),
              "Thursday 1 October 2026");
    app_stop();

    /* ---- 7. a board that does not know the date ------------------------ */

    g_system_day = -1;
    app_start();
    check_str("with no date the app opens on the fallback month", month_text(),
              "January 2024");
    check("the notice is up", shows("Date not set"));
    check("no cell claims to be today", dots_shown(NULL) == 0);
    check_str("and no date is written", selected_text(), "");
    check("Today cannot be pressed", !clickable(today_button()));
    tap_obj(today_button());
    check_str("and a tap on it does nothing", month_text(), "January 2024");

    /* Browsing still works, which is the point of having a fallback month
     * rather than a blank screen. */
    tap_obj(nav_of(NAV_NEXT));
    check_str("next month works with no date", month_text(), "February 2024");
    tap_obj(nav_of(NAV_PREV));
    tap_obj(nav_of(NAV_PREV));
    check_str("and back across the year boundary", month_text(), "December 2023");
    tap_obj(nav_of(NAV_NEXT));
    check_str("returning to the fallback", month_text(), "January 2024");

    /* January 2024 starts on a Monday, so cell 0 is the first. */
    check_str("the first is in the Monday column", text_of(cell_label(0)), "1");
    tap_obj(cell_at(0));
    check_str("a day can be selected with no date set", selected_text(),
              "Monday 1 January 2024");
    check("and it still claims no today", dots_shown(NULL) == 0);
    check("the notice stays up", shows("Date not set"));
    check("and Today stays disabled", !clickable(today_button()));
    /* Over a long browse, in both directions, nothing invents a today. */
    {
        int claimed = 0;

        for (i = 0; i < 14; i++) {
            tap_obj(nav_of(NAV_NEXT));
            if (dots_shown(NULL) != 0) {
                claimed++;
            }
        }
        for (i = 0; i < 28; i++) {
            tap_obj(nav_of(NAV_PREV));
            if (dots_shown(NULL) != 0) {
                claimed++;
            }
        }
        check("no month browsed without a date claims a today", claimed == 0);
        check_str("and the browse ends where the arithmetic says", month_text(),
                  "November 2022");
    }

    /* ---- 8. the clock is set, with the app open ------------------------ */

    /* The ordinary path on this board: the app is open, SNTP lands, and the
     * next tick is the first the app hears of it. */
    g_system_day = DAY_2026_09_12;
    app_tick();
    check_str("the app moves to the real month by itself", month_text(),
              "September 2026");
    check("and marks today", dots_shown(&dot_cell) == 1);
    check("on the right cell", dot_cell == 12);
    check_str("and selects it", selected_text(), "Saturday 12 September 2026 - Today");
    check("the notice is gone", !shows("Date not set"));
    check("and Today can be pressed", clickable(today_button()));
    tap_obj(cell_at(SEPT_LEAD + 4));
    check_str("the recovered app takes taps", selected_text(),
              "Saturday 5 September 2026");

    /* A tick that changes nothing must leave the screen alone. */
    app_tick();
    check_str("an unchanged tick changes nothing", selected_text(),
              "Saturday 5 September 2026");
    check_str("nor the month", month_text(), "September 2026");

    /* ---- 9. midnight --------------------------------------------------- */

    g_system_day = DAY_2026_09_13;
    app_tick();
    check("the today mark moves", dots_shown(&dot_cell) == 1);
    check("to the next day's cell", dot_cell == 13);
    check_str("and the selection does not", selected_text(),
              "Saturday 5 September 2026");

    /* ---- 10. and the date going away ----------------------------------- */

    g_system_day = -1;
    app_tick();
    check("no cell is today any more", dots_shown(NULL) == 0);
    check("the notice is back", shows("Date not set"));
    check("Today is disabled again", !clickable(today_button()));
    check_str("the month is left where the user was", month_text(), "September 2026");
    check_str("and so is the selection", selected_text(), "Saturday 5 September 2026");
    tap_obj(nav_of(NAV_NEXT));
    check_str("browsing still works", month_text(), "October 2026");
    app_stop();

    /* ---- 11. opened and closed, again and again ------------------------ */

    g_system_day = DAY_2026_09_12;
    for (i = 0; i < 5; i++) {
        app_start();
        tap_obj(nav_of(NAV_NEXT));
        tap_obj(cell_at(3));
        tap_obj(today_button());
        app_stop();
    }
    check("five rounds leave nothing behind", lv_obj_get_child_count(g_content) == 0u);

    /* And the app that comes up after all that is a new one, on today, with
     * no trace of what the last one was showing: nothing is stored, so every
     * open starts here. */
    app_start();
    check_str("a fresh open lands on today's month", month_text(), "September 2026");
    check("with today marked", dots_shown(&dot_cell) == 1);
    check("on its cell", dot_cell == 12);
    check_str("and selected", selected_text(), "Saturday 12 September 2026 - Today");
    tap_obj(cell_at(SEPT_LEAD + 9));
    check_str("and it still takes taps", selected_text(), "Thursday 10 September 2026");
    app_stop();
    check("and closes clean", lv_obj_get_child_count(g_content) == 0u);

    /* ---- 12. portrait is where v0.0.10 put it -------------------------- */

    /* The v0.0.10 places, in Normal and Outdoor type, with square corners and
     * with the unit's: the corners reach 10 px above the body's foot, and
     * nothing in portrait comes near it. Measured on v0.0.10 in the
     * simulator. */
    {
        const int32_t corners[2] = { 0, PANEL_CORNER };
        int c;
        int m;

        for (m = 0; m < 2; m++) {
            use_mode(m ? "outdoor" : "normal");
            for (c = 0; c < 2; c++) {
                char what[160];
                int wrong = 0;

                use_display(POS_ROTATION_0, corners[c]);
                g_system_day = DAY_2026_09_12;
                app_start();
                snprintf(what, sizeof(what), "portrait, %s, corners %d: the month row", m ? "Outdoor" : "Normal",
                         (int)corners[c]);
                check(what, rect_is(nav_row(), 20, 547, V010_ROW(152), V010_ROW(215)) &&
                                rect_is(nav_of(NAV_PREV), 20, 91, V010_ROW(152), V010_ROW(215)) &&
                                rect_is(nav_of(NAV_NEXT), 476, 547, V010_ROW(152), V010_ROW(215)));
                snprintf(what, sizeof(what), "portrait, %s, corners %d: the grid", m ? "Outdoor" : "Normal",
                         (int)corners[c]);
                check(what, rect_is(grid_block(), 20, 547, V010_ROW(236), V010_ROW(747)) &&
                                rect_is(headings(), 20, 547, V010_ROW(236), V010_ROW(267)));
                for (i = 0; i < CAL_CELLS; i++) {
                    int32_t x = 20 + 76 * (i % CAL_COLS);
                    int32_t y = V010_ROW(276) + 80 * (i / CAL_COLS);

                    wrong += !rect_is(cell_at(i), x, x + 71, y, y + 71);
                }
                snprintf(what, sizeof(what), "portrait, %s, corners %d: every cell", m ? "Outdoor" : "Normal",
                         (int)corners[c]);
                check(what, wrong == 0);
                snprintf(what, sizeof(what), "portrait, %s, corners %d: the panel and Today", m ? "Outdoor" : "Normal",
                         (int)corners[c]);
                check(what, m ? rect_is(panel_of(), 20, 547, V010_ROW(768), V010_ROW(837)) &&
                                    rect_is(today_button(), 20, 547, V010_ROW(858), V010_ROW(921))
                              : rect_is(panel_of(), 20, 547, V010_ROW(768), V010_ROW(835)) &&
                                    rect_is(today_button(), 20, 547, V010_ROW(856), V010_ROW(919)));
                snprintf(what, sizeof(what), "portrait, %s, corners %d: the frame never scrolls", m ? "Outdoor" : "Normal",
                         (int)corners[c]);
                check(what, lv_obj_get_scroll_bottom(frame_of()) <= 0 && !lv_obj_has_flag(panel_of(), LV_OBJ_FLAG_SCROLLABLE));
                app_stop();
            }
        }
        use_mode("normal");
    }

    /* ---- 13. every screen, both orientations, both corners, both modes - */

    check_orientation("portrait", POS_ROTATION_0, PANEL_CORNER, "normal");
    check_orientation("landscape", POS_ROTATION_270, PANEL_CORNER, "normal");
    check_orientation("portrait, square corners", POS_ROTATION_0, 0, "normal");
    check_orientation("landscape, square corners", POS_ROTATION_270, 0, "normal");
    check_orientation("portrait, Outdoor", POS_ROTATION_0, PANEL_CORNER, "outdoor");
    check_orientation("landscape, Outdoor", POS_ROTATION_270, PANEL_CORNER, "outdoor");
    check_orientation("portrait, Outdoor, square corners", POS_ROTATION_0, 0, "outdoor");
    check_orientation("landscape, Outdoor, square corners", POS_ROTATION_270, 0, "outdoor");

    /* The landscape numbers the amendment gives, on the unit's panel. */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    g_system_day = DAY_2026_09_12;
    app_start();
    /* From row 96: the header and padding straight from the top edge (DS
     * section 36; row 128 under the 32 px COMPACT bar of section 30), so the
     * month is 56 px taller than the 386 the amendment was written for, and
     * the panel between the arrows and Today takes the same 56. */
    check("landscape: the month is 586 x 442, its cells 80 wide and at least 56 tall",
          rect_is(grid_block(), 20, 605, V010_LROW(128), 537) && lv_obj_get_height(cell_at(0)) >= WIDE_CELL_MIN_H &&
              lv_obj_get_width(cell_at(0)) >= 80);
    check("landscape: the arrows, the panel and Today in the second half",
          rect_is(nav_row(), 626, 1211, V010_LROW(128), V010_LROW(191)) &&
              rect_is(panel_of(), 626, 1211, V010_LROW(212), 453) && rect_is(today_button(), 626, 1211, 474, 537));
    app_stop();

    /* ---- 14. the display turned under the open app --------------------- */

    {
        static lv_area_t before[512];
        static lv_area_t after[512];
        int n_before;
        int n_after;
        int objects;
        char kept_month[32];
        char kept_selected[64];
        int turn;

        use_display(POS_ROTATION_0, PANEL_CORNER);
        g_system_day = DAY_2026_09_12;
        app_start();
        tap_obj(cell_at(SEPT_LEAD + 19));
        tap_obj(nav_of(NAV_NEXT));
        objects = count_objects(app_body);
        n_before = snapshot(app_body, before, 0, 512);
        snprintf(kept_month, sizeof(kept_month), "%s", month_text());
        snprintf(kept_selected, sizeof(kept_selected), "%s", selected_text());
        check_str("before turning: October, with the 20th of September picked", kept_selected,
                  "Sunday 20 September 2026");

        for (turn = 0; turn < 3; turn++) {
            use_display(POS_ROTATION_270, PANEL_CORNER);
            check("turned to landscape, the app is wide", laid_wide());
            check_str("the month on screen is kept", month_text(), kept_month);
            check_str("and the selection", selected_text(), kept_selected);
            check("and today is still nowhere in October", dots_shown(NULL) == 0);
            check("the same days are targets", clickable_cells() == 31);
            check("and not one object more or less", count_objects(app_body) == objects);
            check_screen("turned to landscape, October");
            use_display(POS_ROTATION_0, PANEL_CORNER);
            check("turned back, the app is tall", !laid_wide());
            check_screen("turned back to portrait, October");
            n_after = snapshot(app_body, after, 0, 512);
            check("and every object is back where it was, to the pixel",
                  n_after == n_before && memcmp(before, after, (size_t)n_before * sizeof(before[0])) == 0);
        }

        /* Everything works in landscape, by finger. */
        use_display(POS_ROTATION_270, PANEL_CORNER);
        tap_obj(nav_of(NAV_PREV));
        check_str("previous, pressed in landscape", month_text(), "September 2026");
        check("today is marked on its cell", dots_shown(&dot_cell) == 1 && dot_cell == 12);
        tap_obj(cell_at(SEPT_LEAD + 24));
        check_str("a day, pressed in landscape", selected_text(), "Friday 25 September 2026");
        tap_obj(cell_at(0));
        check_str("a blank, pressed in landscape, selects nothing", selected_text(), "Friday 25 September 2026");
        tap_obj(nav_of(NAV_NEXT));
        tap_obj(nav_of(NAV_NEXT));
        check_str("next, pressed in landscape", month_text(), "November 2026");
        check_screen("landscape, November 2026: six weeks");
        tap_obj(today_button());
        check_str("Today, pressed in landscape", month_text(), "September 2026");
        check_str("selects today", selected_text(), "Saturday 12 September 2026 - Today");
        app_stop();

        /* A board that does not know the date, turned, then told it. */
        g_system_day = -1;
        use_display(POS_ROTATION_0, PANEL_CORNER);
        app_start();
        tap_obj(cell_at(9));
        use_display(POS_ROTATION_270, PANEL_CORNER);
        check("no date, turned to landscape: the notice is kept", shows("Date not set"));
        check_str("with the day picked", selected_text(), "Wednesday 10 January 2024");
        check("and Today still cannot be pressed", !clickable(today_button()));
        check_screen("landscape, no date, a day picked");
        g_system_day = DAY_2026_09_12;
        app_tick();
        check_str("the date arrives in landscape: the app moves to it", month_text(), "September 2026");
        check("marks today", dots_shown(&dot_cell) == 1 && dot_cell == 12);
        check("and the notice goes", !shows("Date not set"));
        check_screen("landscape, the date just set");
        app_stop();

        for (i = 0; i < 5; i++) {
            use_display(i % 2 ? POS_ROTATION_0 : POS_ROTATION_270, PANEL_CORNER);
            app_start();
            use_display(i % 2 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            tap_obj(nav_of(NAV_NEXT));
            tap_obj(today_button());
            app_stop();
        }
        check("five rounds, turned while open, leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    }

    /* ---- 15. the edges of the wide shape ------------------------------- */

    {
        char what[160];

        g_system_day = DAY_2026_09_12;
        /* A landscape body two portrait bodies and the gutter wide is wide;
         * a pixel narrower is not. */
        use_panel_sized(PANEL_W, 40 + WIDE_MIN_W, POS_ROTATION_270, PANEL_CORNER);
        app_start();
        check("a body exactly two halves and the gutter wide is wide", expect_wide() && laid_wide());
        check_screen("landscape at the width floor");
        app_stop();
        use_panel_sized(PANEL_W, 40 + WIDE_MIN_W - 1, POS_ROTATION_270, PANEL_CORNER);
        app_start();
        check("a pixel narrower is tall", !expect_wide() && !laid_wide());
        app_stop();

        /* Tall enough for six weeks of 56 px cells, less the corners, is
         * wide; a pixel shorter is not, and the tall layout scrolls. */
        use_panel_sized(BODY_CHROME_H + CORNER_REACH + WIDE_MIN_H, PANEL_H, POS_ROTATION_270, PANEL_CORNER);
        app_start();
        check("a body just tall enough for 56 px weeks is wide", expect_wide() && laid_wide());
        check_screen("landscape at the height floor");
        app_stop();
        use_panel_sized(BODY_CHROME_H + CORNER_REACH + WIDE_MIN_H - 1, PANEL_H, POS_ROTATION_270, PANEL_CORNER);
        app_start();
        check("a pixel shorter is tall", !expect_wide() && !laid_wide());
        check("and the frame scrolls", lv_obj_has_flag(frame_of(), LV_OBJ_FLAG_SCROLLABLE) &&
                                           lv_obj_get_scroll_bottom(frame_of()) > 0);
        app_stop();

        /* Wide enough, but taller than wide: tall. */
        use_panel_sized(PANEL_H, 1500, POS_ROTATION_0, PANEL_CORNER);
        app_start();
        check("a body wide enough but taller than wide is tall", !expect_wide() && !laid_wide());
        check_screen("a panel wider than it is tall, upright");
        app_stop();

        /* A short landscape body - the size above the landscape keyboard,
         * which Calendar never asks for - keeps the tall layout whole, and
         * every target can be scrolled to and is then wholly on show. */
        use_display(POS_ROTATION_270, PANEL_CORNER);
        app_start();
        lv_obj_set_height(g_content, 568 - STATUS_H - 296);
        pump(60);
        check("a 100 px landscape body is tall", !expect_wide() && !laid_wide());
        {
            static lv_obj_t *t[64];
            int n = collect_targets(frame_of(), t, 0, 64);
            int hidden = 0;
            lv_area_t fr;

            for (i = 0; i < n; i++) {
                lv_area_t a;

                lv_obj_scroll_to_view_recursive(t[i], LV_ANIM_OFF);
                area_of(t[i], &a);
                lv_obj_get_content_coords(frame_of(), &fr);
                if (!within(&a, &fr) ||
                    !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
                    hidden++;
                }
            }
            snprintf(what, sizeof(what), "every one of its %d targets can be scrolled wholly into view", n);
            check(what, n > 0 && hidden == 0);
            check("by the frame, not the body", lv_obj_get_scroll_y(app_body) == 0);
        }
        /* And back to the whole landscape body: the frame is wide again, and
         * back at its top. */
        lv_obj_set_height(g_content, 568 - STATUS_H);
        pump(60);
        check("the body back to its size is wide again", laid_wide());
        check_screen("landscape, after a short body");
        app_stop();
    }

    printf("cal_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
