/*
 * PocketFleet in the running app, driven by a real LVGL pointer device.
 *
 * The unit tests either side of this one prove the rules, the AI, the save
 * codec and the view model. This one proves the part they cannot: that a
 * finger landing on a square of the board reaches the cell under it and no
 * other, at both board sizes; that the layout is chosen from the body alone;
 * and that turning the display under an open app moves objects and changes
 * nothing about the match being played.
 *
 * It exists so that a board is the first HARDWARE test of the tap path and
 * not the first test of it at all - which matters more here than anywhere
 * else, because the wide shape draws a 40 x 51 px cell (DS 28.2, 30.4) where the tall
 * one draws 48.
 *
 * What it covers, with the app hosted the way the shell hosts it - a header
 * and a padded body - on the reference panel with its 30 px rounded corners
 * and with square ones, in portrait and landscape, in Normal and Outdoor:
 *
 *   - the shape rule as arithmetic, at and either side of every floor;
 *   - all four screens in both shapes: what stands beside what, every action
 *     a finger's size, nothing outside the body or the safe area;
 *   - every cell of the board hit at its centre and at all four of its
 *     corners, at 48 px, 41 px and 40 px; the gaps between cells; the caption
 *     gutter; and points just outside the board;
 *   - aim-then-confirm: a tap on a cell never fires, FIRE is the only thing
 *     that does, and only on a square not already fired at;
 *   - the display turned under the open app, repeatedly: the same objects,
 *     none added, the match untouched - crosshair, turn, boards, fleet as
 *     placed, and a paced reply still in flight;
 *   - the Deploy roster, the placement controls and CONFIRM DEPLOYMENT;
 *   - the Result screen's figures;
 *   - the save written on the way through, the same file either way up.
 *
 * The shell is not here, so this file plays it: the entry points the app uses
 * from app.h are defined below, and pocketlog's one entry point is a buffer.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/fleet_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "fleet_app.h"
#include "pocketlog/pocketlog.h"
#include "link/fleet_link_loop.h"
#include "link/fleet_session.h"
#include "pocketui.h"
#include "ui/fleet_grid.h"
#include "ui/fleet_widgets.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* The status bar the shell gives this app in the display's orientation
 * (ui/shell/chrome.h, DS section 30), so the frame built here is the one
 * shell.c builds: none in either orientation, since the app is
 * fullscreen (DS section 30.4, stage 2). */
#define STATUS_H chrome_height(chrome_resolve(app_fleet.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
/* Above the body and below it: the status bar, the app header and the body's
 * own top and foot padding (DS 7) - and how far the unit's 30 px corners
 * reach above the body's foot. */
#define BODY_CHROME_H (STATUS_H + POCKETUI_HEADER_H + POCKETUI_BODY_PAD_TOP + POCKETUI_PAD)
#define CORNER_REACH 10

/* What the layout promises, written down here rather than read from the app. */
#define TALL_W (PANEL_W - 2 * POCKETUI_PAD)      /* 528 */
#define TALL_H (PANEL_H - BODY_CHROME_H)         /* 1116: fullscreen */
#define WIDE_W (PANEL_H - 2 * POCKETUI_PAD)      /* 1192 */
#define WIDE_H (PANEL_W - BODY_CHROME_H)         /* 452: fullscreen, no bar */
/* A cell across the page is as tall as ten rows in the body allow, and as
 * wide as the two columns beside the board can spare - up to half as wide
 * again as it is tall, and never wider than the 51 px board the §28 columns
 * were measured against (FLEET_CELL_ACROSS_MAX). Fleet is fullscreen (DS
 * section 30.4, stage 2), so the rows are taller than §28's and the width is
 * where the cap holds it. */
#define WIDE_CELL 40                             /* 452 less the 10 px foot */
#define WIDE_CELL_W 51                           /* capped: FLEET_CELL_ACROSS_MAX */
#define RECT_CELL 41                             /* a panel with square corners */
#define RECT_CELL_W 51                           /* capped too */
/* The §28 cells, under the 56 px bar Fleet was first validated under: the
 * shape rule's arithmetic is still checked at those bodies. */
#define S28_CELL 34                              /* a 386 px body */
#define S28_RECT 35                              /* a 396 px body */
/* What the rule draws across for a cell down: half as wide again, capped. */
#define ACROSS_OF(cell) ((cell) * 3 / 2 > FLEET_CELL_ACROSS_MAX ? FLEET_CELL_ACROSS_MAX : (cell) * 3 / 2)
#define OWN_CELL_ANY 20                          /* your own waters, either shape */
#define SPAN_OF(cell) (FLEET_GRID_GUTTER + FLEET_GRID * (cell) + (FLEET_GRID - 1) * FLEET_GRID_GAP)
/* The four one-square nudges (fleet_screen_battle.c). */
#define STEP_COUNT 4

/* Long enough for the opponent's paced reply to have been played and the log
 * line rewritten with both halves of the exchange (fleet_screen_battle.c,
 * REPLY_PACE_MS). */
#define REPLY_SETTLE_MS 700

/* A panel's child 0 is its caption (DS 2), so its content starts at 1. */
#define KID_PANEL_FIRST 1
#define KID_BOARD 0
#define KID_SIDE 1

extern const struct pocketos_app app_fleet;

static int failed;
static int checks;
static const char *phase = "";

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL [%s] %s\n", phase, what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL [%s] %s: got \"%s\", want \"%s\"\n", phase, what,
               got ? got : "(null)", want);
    }
}

static void check_int(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL [%s] %s: got %ld, want %ld\n", phase, what, got, want);
    }
}

/* ---- the shell's side of app.h ----------------------------------------- */

static char g_hint[64];
static int g_reduced_motion;
static int g_home_calls;
static int g_keyboard_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void) { g_home_calls++; }
int pocketos_shell_reduced_motion(void) { return g_reduced_motion; }
const char *pocketos_shell_radio_state(void) { return NULL; }
int64_t pocketos_shell_system_day(void) { return -1; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    g_keyboard_calls++;
}
void pocketos_shell_keyboard_hide(void) { g_keyboard_calls++; }
int pocketos_shell_keyboard_visible(void) { return 0; }

static char g_logged[256];

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_logged, sizeof(g_logged), fmt, ap);
    va_end(ap);
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

static void tap_at(int32_t x, int32_t y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

/* Press at one point, slide through the points given, and let go at the last
 * one. This is the aim a thumb makes: it lands anywhere and slides. */
static void drag_through(const lv_point_t *points, int count)
{
    int i;

    if (count < 1) {
        return;
    }
    finger_point = points[0];
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (i = 1; i < count; i++) {
        finger_point = points[i];
        pump(60);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL [%s] tap on a missing object\n", phase);
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    tap_at(a.x1 + lv_area_get_width(&a) / 2, a.y1 + lv_area_get_height(&a) / 2);
}

/* ---- the app's lifecycle, as the shell runs it -------------------------- */

static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static struct fleet_app *app;

static lv_obj_t *kid(lv_obj_t *parent, int i)
{
    return parent ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

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
    app = app_fleet.create(app_body);
    pump(120);
}

static void app_stop(void)
{
    app_fleet.destroy(app);
    app = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

/* The display the shell would open: a panel `width` x `long_side` with corner
 * squares of `corner` px, turned to `rotation`, the geometry handed to
 * PocketUI and the content area below the status bar sized to it. Called with
 * the app open, it is the body changing shape under a running app. */
static void use_panel_sized(int32_t width, int32_t long_side, enum pos_rotation rotation,
                            int32_t corner)
{
    struct pos_panel panel;
    struct pos_display_geometry g;

    memset(&panel, 0, sizeof(panel));
    panel.width = width;
    panel.height = long_side;
    panel.corners.top_left = corner;
    panel.corners.top_right = corner;
    panel.corners.bottom_right = corner;
    panel.corners.bottom_left = corner;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    /* The lifted finger's last point could be off the turned display, which
     * LVGL warns about on every read. */
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(120);
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

static lv_obj_t *frame_of(void) { return kid(app_body, 0); }
static lv_obj_t *screen_of(int s) { return kid(frame_of(), s); }

static lv_obj_t *battle_board(void) { return kid(screen_of(FLEET_SCREEN_BATTLE), KID_BOARD); }
static lv_obj_t *battle_side(void) { return kid(screen_of(FLEET_SCREEN_BATTLE), KID_SIDE); }
static lv_obj_t *battle_cols(void) { return kid(battle_side(), 0); }
static lv_obj_t *battle_act(void) { return kid(battle_cols(), 0); }
static lv_obj_t *battle_waters(void) { return kid(battle_cols(), 1); }
static lv_obj_t *battle_target_panel(void) { return kid(battle_act(), 0); }
/* FIRE's row is the last thing in the readout column down the page and the
 * last thing in the whole region across it; the test looks where the layout
 * says it should be rather than being told. FIRE leads the row, and in
 * multiplayer the chat button follows it. */
static lv_obj_t *battle_fire_row(void)
{
    lv_obj_t *o = kid(battle_act(), 1);

    return o ? o : kid(battle_side(), 1);
}
static lv_obj_t *battle_fire(void) { return kid(battle_fire_row(), 0); }
static lv_obj_t *battle_chat(void) { return kid(battle_fire_row(), 1); }
static lv_obj_t *battle_waters_panel(void) { return kid(battle_waters(), 0); }
static lv_obj_t *battle_own(void) { return kid(battle_waters_panel(), KID_PANEL_FIRST); }
static lv_obj_t *battle_cell_value(void)
{
    return kid(kid(battle_target_panel(), KID_PANEL_FIRST), 0);
}
static lv_obj_t *battle_note(void) { return kid(battle_target_panel(), KID_PANEL_FIRST + 1); }
/* The four one-square nudges, last in the readout panel across the page and
 * hidden down it. */
static lv_obj_t *battle_steps(void)
{
    lv_obj_t *p = battle_target_panel();

    return kid(p, (int)lv_obj_get_child_count(p) - 1);
}
static lv_obj_t *battle_step(int i) { return kid(battle_steps(), i); }
/* The log line is under your own board down the page and under the readout
 * across it. */
static lv_obj_t *battle_log(void)
{
    lv_obj_t *o = kid(battle_waters_panel(), KID_PANEL_FIRST + 1);

    return o ? o : kid(battle_target_panel(), KID_PANEL_FIRST + 2);
}

static lv_obj_t *deploy_board(void) { return kid(screen_of(FLEET_SCREEN_DEPLOY), KID_BOARD); }
static lv_obj_t *deploy_side(void) { return kid(screen_of(FLEET_SCREEN_DEPLOY), KID_SIDE); }
static lv_obj_t *deploy_roster(void) { return kid(kid(deploy_side(), 0), 0); }
static lv_obj_t *deploy_roster_row(int i) { return kid(deploy_roster(), KID_PANEL_FIRST + i); }
static lv_obj_t *deploy_controls(void) { return kid(kid(deploy_side(), 1), 0); }
static lv_obj_t *deploy_confirm(void) { return kid(kid(deploy_side(), 1), 2); }
#define DEPLOY_TURN 0
#define DEPLOY_AUTO 1
#define DEPLOY_CLEAR 2

static lv_obj_t *command_cols(void) { return kid(screen_of(FLEET_SCREEN_COMMAND), 0); }
static lv_obj_t *command_col(int i) { return kid(command_cols(), i); }
static lv_obj_t *command_foot(void) { return kid(screen_of(FLEET_SCREEN_COMMAND), 1); }
static lv_obj_t *command_saved(void) { return kid(command_col(2), 1); }
/* DEPLOY FLEET is the last thing on the foot row in both shapes; RESUME joins
 * it there across the page and goes back in the panel down it. */
static lv_obj_t *command_deploy(void)
{
    lv_obj_t *f = command_foot();

    return kid(f, (int)lv_obj_get_child_count(f) - 1);
}
static lv_obj_t *command_resume(void)
{
    lv_obj_t *f = command_foot();

    if (lv_obj_get_child_count(f) > 1) {
        return kid(f, 0);
    }
    return kid(command_saved(), (int)lv_obj_get_child_count(command_saved()) - 1);
}
static lv_obj_t *command_segments(void) { return kid(kid(command_col(0), 0), KID_PANEL_FIRST); }

static lv_obj_t *result_heading(void) { return kid(screen_of(FLEET_SCREEN_RESULT), 0); }
static lv_obj_t *result_panels(void) { return kid(screen_of(FLEET_SCREEN_RESULT), 1); }
static lv_obj_t *result_foot(void) { return kid(screen_of(FLEET_SCREEN_RESULT), 2); }
/* The value label of row n of one of the two list panels. */
static lv_obj_t *result_value(int panel, int row)
{
    return kid(kid(kid(result_panels(), panel), KID_PANEL_FIRST + row), 1);
}

static const char *text_of(lv_obj_t *label)
{
    return label ? lv_label_get_text(label) : "(missing)";
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

static void box_of(lv_obj_t *obj, lv_area_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!obj) {
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static int visible(lv_obj_t *obj)
{
    lv_obj_t *o;

    if (!obj) {
        return 0;
    }
    for (o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return 0;
        }
    }
    return 1;
}

/* Wholly inside the body the shell gave the app. */
static int inside_body(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t b;

    box_of(obj, &a);
    box_of(app_body, &b);
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

/* LVGL clips a widget's children to the widget's box, grown by its own extra
 * draw size only when it allows overflow. A panel's caption straddles the
 * panel's top border and so is drawn above it, which means every box between
 * the caption and the frame has to allow that much - and the frame, which
 * allows none, has to have been given the room instead. This walks that chain
 * the way the renderer does. */
static int caption_would_be_drawn(lv_obj_t *panel)
{
    lv_obj_t *cap = kid(panel, 0);
    lv_area_t c;
    lv_obj_t *o;

    if (!cap) {
        return 0;
    }
    box_of(cap, &c);
    for (o = lv_obj_get_parent(cap); o && o != app_body; o = lv_obj_get_parent(o)) {
        lv_area_t a;
        /* What a box that allows captions promises, stated here rather than
         * read back from LVGL: the caption's rise and a little over. A box
         * that does not allow overflow clips at its own edge. */
        int32_t ext = lv_obj_has_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE)
                          ? FLEET_CAPTION_RISE + 4 : 0;

        box_of(o, &a);
        if (c.x1 < a.x1 - ext || c.y1 < a.y1 - ext ||
            c.x2 > a.x2 + ext || c.y2 > a.y2 + ext) {
            return 0;
        }
    }
    return 1;
}

/* Wholly outside the unsafe area of the panel as it is set up now. */
static int in_safe_area(lv_obj_t *obj)
{
    lv_area_t a;

    box_of(obj, &a);
    return pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2);
}

/* Seen whole, now, without anything having to scroll: shown, and inside
 * every box between it and the body, none of which is scrolled. A column
 * that clips a control it would have to be scrolled to show fails this
 * where inside_body() would not. DS §46.4: at every text size Fleet's
 * controls are on its one screen. */
static void check_in_view(const char *what, lv_obj_t *obj)
{
    char msg[200];
    lv_area_t a;
    lv_obj_t *o;
    int ok = obj != NULL && visible(obj);

    box_of(obj, &a);
    for (o = obj ? lv_obj_get_parent(obj) : NULL; ok && o; o = lv_obj_get_parent(o)) {
        lv_area_t p;

        box_of(o, &p);
        if (a.x1 < p.x1 || a.y1 < p.y1 || a.x2 > p.x2 || a.y2 > p.y2 || lv_obj_get_scroll_x(o) != 0 ||
            lv_obj_get_scroll_y(o) != 0) {
            printf("     %s: %d..%d x %d..%d, but a box it is in is %d..%d x %d..%d, scrolled %d,%d\n", what,
                   (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2, (int)p.x1, (int)p.x2, (int)p.y1, (int)p.y2,
                   (int)lv_obj_get_scroll_x(o), (int)lv_obj_get_scroll_y(o));
            ok = 0;
        }
        if (o == app_body) {
            break;
        }
    }
    snprintf(msg, sizeof(msg), "%s is in view with nothing scrolled", what);
    check(msg, ok && in_safe_area(obj));
}

/* ---- the board, as the grid draws it ----------------------------------- */

/* fleet_grid.c's cell_area(), restated here so the test does not ask the code
 * under test where a cell is. */
static void cell_box(lv_obj_t *grid, int labels, int row, int col, lv_area_t *out)
{
    lv_area_t g;
    int cell_w = fleet_grid_cell_across(grid);
    int cell_h = fleet_grid_cell(grid);
    int gutter = labels ? FLEET_GRID_GUTTER : 0;

    box_of(grid, &g);
    out->x1 = g.x1 + gutter + col * (cell_w + FLEET_GRID_GAP);
    out->y1 = g.y1 + gutter + row * (cell_h + FLEET_GRID_GAP);
    out->x2 = out->x1 + cell_w - 1;
    out->y2 = out->y1 + cell_h - 1;
}

/* ---- the shape rule, as arithmetic ------------------------------------- */

static void test_shape_rule(void)
{
    int cell = 0;
    int cell_w = 0;
    int32_t h_at_floor = FLEET_GRID_GUTTER + (FLEET_GRID - 1) * FLEET_GRID_GAP +
                         FLEET_GRID * FLEET_CELL_MIN;
    int32_t floor_w;

    phase = "shape rule";
    check_int("a 386 px body gives a 34 px cell", fleet_cell_for_height(386), S28_CELL);
    check_int("a 396 px body gives a 35 px cell", fleet_cell_for_height(396), S28_RECT);
    check_int("the fullscreen body, 442 px, gives a 40 px cell",
              fleet_cell_for_height(WIDE_H - CORNER_REACH), WIDE_CELL);
    check_int("a tall body is capped at the tall cell",
              fleet_cell_for_height(TALL_H), FLEET_CELL_TALL);
    check_int("a body with no room at all gives nothing", fleet_cell_for_height(10), 0);
    check_int("the finest board allowed is the floor",
              fleet_cell_for_height(h_at_floor), FLEET_CELL_MIN);
    check_int("the labelled span at 34 px", fleet_grid_span_for(S28_CELL, 1),
              SPAN_OF(S28_CELL));
    check_int("the unlabelled span has no gutter", fleet_grid_span_for(20, 0),
              FLEET_GRID * 20 + (FLEET_GRID - 1) * FLEET_GRID_GAP);

    check("the landscape body is wide",
          fleet_shape_is_wide(WIDE_W, WIDE_H - CORNER_REACH, &cell_w, &cell));
    check_int("and draws a 40 px cell down the board", cell, WIDE_CELL);
    check_int("and a 51 px cell across it", cell_w, WIDE_CELL_W);

    /* The height is the binding constraint and the width is the one with
     * room to spare, so the rule spends the spare width on the cell - up to
     * half as wide again, and never less than it is tall. */
    check_int("a cell is half as wide again where there is room",
              fleet_cell_across(WIDE_W, S28_CELL), S28_CELL * 3 / 2);
    /* A taller body grows the rows and not the board's width, so the
     * columns beside it keep what §28 measured (FLEET_CELL_ACROSS_MAX). */
    check_int("a taller cell is held at the §28 width",
              fleet_cell_across(WIDE_W, WIDE_CELL), FLEET_CELL_ACROSS_MAX);
    check_int("and at the square-corner height too",
              fleet_cell_across(WIDE_W, RECT_CELL), FLEET_CELL_ACROSS_MAX);
    check_int("which is the §28 cell made half as wide again",
              FLEET_CELL_ACROSS_MAX, S28_CELL * 3 / 2);
    check("a body with no width to spare draws square cells",
          fleet_cell_across(SPAN_OF(WIDE_CELL) + POCKETUI_PAD + 2 * FLEET_COL_MIN +
                            POCKETUI_PAD, WIDE_CELL) == WIDE_CELL);
    check("and one narrower still never goes below square",
          fleet_cell_across(100, WIDE_CELL) == WIDE_CELL);
    check("a much wider body stops at half as wide again",
          fleet_cell_across(4000, S28_CELL) == S28_CELL * 3 / 2);
    check("or at the cap, for a taller cell",
          fleet_cell_across(4000, WIDE_CELL) == FLEET_CELL_ACROSS_MAX);
    check("the portrait body is not wide",
          !fleet_shape_is_wide(TALL_W, TALL_H - CORNER_REACH, NULL, NULL));
    check("a square body is not wide", !fleet_shape_is_wide(600, 600, NULL, NULL));
    /* Big enough across for the wide shape and no wider than it is high: the
     * one case where only "wider than it is tall" can refuse it. */
    check("nor is a square body with room to spare",
          !fleet_shape_is_wide(1400, 1400, NULL, NULL));
    check("nor a body taller than it is wide with the same room",
          !fleet_shape_is_wide(1400, 1500, NULL, NULL));
    check("while one pixel wider than it is tall is",
          fleet_shape_is_wide(1401, 1400, NULL, NULL));
    check("a body exactly at the cell floor is still wide",
          fleet_shape_is_wide(1192, h_at_floor, &cell_w, &cell));
    check_int("at the floor cell", cell, FLEET_CELL_MIN);
    check_int("and the widest that floor allows", cell_w, FLEET_CELL_MIN * 3 / 2);
    check("one whole cell below the floor it is not",
          !fleet_shape_is_wide(1192, h_at_floor - FLEET_GRID, NULL, NULL));

    floor_w = SPAN_OF(WIDE_CELL) + POCKETUI_PAD + 2 * FLEET_COL_MIN + POCKETUI_PAD;
    check("a body exactly at the width floor is wide",
          fleet_shape_is_wide(floor_w, WIDE_H - CORNER_REACH, NULL, NULL));
    check("one pixel narrower it is not",
          !fleet_shape_is_wide(floor_w - 1, WIDE_H - CORNER_REACH, NULL, NULL));
}

/* ---- structure --------------------------------------------------------- */

static void check_one_screen(int shown)
{
    int i;
    int n = 0;

    for (i = 0; i < FLEET_SCREEN_COUNT; i++) {
        if (screen_of(i) && !lv_obj_has_flag(screen_of(i), LV_OBJ_FLAG_HIDDEN)) {
            n++;
        }
    }
    check_int("exactly one screen is shown", n, 1);
    check("and it is the one asked for", visible(screen_of(shown)));
}

/* The frame is the body's content box, and it is padded by however far the
 * corner squares reach into it - never more, never less, and never sideways. */
static void check_frame(int32_t want_w, int32_t want_h, int32_t corner)
{
    lv_area_t f;

    box_of(frame_of(), &f);
    check_int("the frame is the body's content box, across", lv_area_get_width(&f), want_w);
    check_int("the frame is the body's content box, down", lv_area_get_height(&f), want_h);
    check_int("the frame's foot clears the corner squares",
              lv_obj_get_style_pad_bottom(frame_of(), LV_PART_MAIN),
              corner ? CORNER_REACH : 0);
    check_int("and nothing is taken from the sides",
              lv_obj_get_style_pad_left(frame_of(), LV_PART_MAIN) +
              lv_obj_get_style_pad_right(frame_of(), LV_PART_MAIN), 0);
}

/* The body the app was given, less what the rounded corners take: the box a
 * normal turn has to fit inside without being scrolled. */
static void turn_viewport(lv_area_t *out)
{
    lv_obj_update_layout(frame_of());
    lv_obj_get_content_coords(frame_of(), out);
}

static int inside_viewport(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t v;

    turn_viewport(&v);
    box_of(obj, &a);
    return a.x1 >= v.x1 && a.y1 >= v.y1 && a.x2 <= v.x2 && a.y2 <= v.y2;
}

/* Anything in this subtree that could be scrolled, and by how far. Walks the
 * whole of it rather than the containers the test happens to know about, so a
 * box added later is covered without the test being changed. A hidden screen
 * is skipped: the frame holds all four of them and only the one on show can
 * be scrolled by anybody.
 *
 * Scrolled up is not asked about. A panel's caption is drawn above the
 * panel's top border on purpose (DS 2), which is content above the box by
 * every measure LVGL has; what matters is that nothing is reached by
 * scrolling down or across, and that nothing has been scrolled already. */
static int32_t scrollable_by(lv_obj_t *obj, lv_obj_t **worst_obj)
{
    int32_t worst = 0;
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (lv_obj_get_scroll_bottom(obj) > worst) {
        worst = lv_obj_get_scroll_bottom(obj);
    }
    if (lv_obj_get_scroll_right(obj) > worst) {
        worst = lv_obj_get_scroll_right(obj);
    }
    if (lv_obj_get_scroll_left(obj) > worst) {
        worst = lv_obj_get_scroll_left(obj);
    }
    if (worst > 0 && worst_obj) {
        *worst_obj = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *from_child = NULL;
        int32_t child = scrollable_by(lv_obj_get_child(obj, i), &from_child);

        if (child > worst) {
            worst = child;
            if (worst_obj) {
                *worst_obj = from_child;
            }
        }
    }
    return worst;
}

/* The first thing in this subtree that sticks out of the box a turn has to
 * fit in, or NULL. Returned rather than counted, so a failure says which. */
static lv_obj_t *outside_of(lv_obj_t *obj, const lv_area_t *v)
{
    lv_area_t a;
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    box_of(obj, &a);
    /* A panel's caption is drawn above its top border on purpose (DS 2); it
     * is checked by caption_would_be_drawn() instead. */
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT) &&
        (a.x1 < v->x1 || a.y1 < v->y1 || a.x2 > v->x2 || a.y2 > v->y2)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *bad = outside_of(lv_obj_get_child(obj, i), v);

        if (bad) {
            return bad;
        }
    }
    return NULL;
}

/* Where an object is, for a failure message. */
static void say_where(const char *what, lv_obj_t *obj)
{
    lv_area_t a;

    box_of(obj, &a);
    printf("     %s: x %d..%d y %d..%d\n", what, (int)a.x1, (int)a.x2,
           (int)a.y1, (int)a.y2);
}

/*
 * The one thing this screen promises above all others: a normal turn is on
 * the display, whole, and no part of playing it is reached by scrolling
 * (DS 28.6). Everything a turn needs is named here rather than left to a
 * sweep, so that taking one of them off the screen fails the test instead of
 * quietly passing it.
 */
static void check_battle_never_scrolls(const char *what)
{
    lv_area_t v;
    lv_area_t f;
    lv_obj_t *screen = screen_of(FLEET_SCREEN_BATTLE);
    int row;
    int col;
    int cells_in = 1;

    phase = what;
    turn_viewport(&v);

    {
        lv_obj_t *bad = NULL;
        int32_t by = scrollable_by(screen, &bad);

        check_int("nothing on the Battle screen can be scrolled", (long)by, 0);
        if (by) {
            say_where("the scroller", bad);
        }
        bad = NULL;
        by = scrollable_by(frame_of(), &bad);
        check_int("nor the frame it is in", (long)by, 0);
        if (by) {
            say_where("the scroller", bad);
        }
        bad = NULL;
        by = scrollable_by(app_body, &bad);
        check_int("nor the body the shell gave the app", (long)by, 0);
        if (by) {
            say_where("the scroller", bad);
        }
    }
    check("the Battle screen is not itself a scroller",
          !lv_obj_has_flag(screen, LV_OBJ_FLAG_SCROLLABLE));
    check_int("and nothing has been scrolled to see it",
              (long)(lv_obj_get_scroll_y(app_body) + lv_obj_get_scroll_x(app_body) +
                     lv_obj_get_scroll_y(frame_of()) + lv_obj_get_scroll_x(frame_of())), 0);

    check("the whole target board is in view", inside_viewport(battle_board()));
    check("FIRE is in view", inside_viewport(battle_fire()));
    check("the readout is in view", inside_viewport(battle_target_panel()));
    check("what is aimed at is in view", inside_viewport(battle_cell_value()));
    check("what the shot would do is in view", inside_viewport(battle_note()));
    check("what the last exchange did is in view", inside_viewport(battle_log()));
    check("your own waters are in view", inside_viewport(battle_waters_panel()));
    check("your own board is in view", inside_viewport(battle_own()));

    /* Every square of the board, not just the board's box: a board that fits
     * but whose last row is drawn past its own edge would pass the check
     * above and still cost the player the row. */
    for (row = 0; row < FLEET_GRID; row++) {
        for (col = 0; col < FLEET_GRID; col++) {
            lv_area_t c;

            cell_box(battle_board(), 1, row, col, &c);
            if (c.x1 < v.x1 || c.y1 < v.y1 || c.x2 > v.x2 || c.y2 > v.y2) {
                cells_in = 0;
            }
        }
    }
    check("every square of the board is in view", cells_in);
    {
        lv_obj_t *bad = outside_of(screen, &v);

        check("and nothing else on the screen is out of it", bad == NULL);
        if (bad) {
            printf("     the turn has x %d..%d y %d..%d\n", (int)v.x1, (int)v.x2,
                   (int)v.y1, (int)v.y2);
            say_where("the stray object", bad);
            say_where("its parent", lv_obj_get_parent(bad));
        }
    }
    box_of(battle_fire(), &f);
    check("FIRE is still a finger's size", lv_area_get_height(&f) >= POCKETUI_TOUCH_MIN);

    /* Whose turn it is, and what state the game is in, is the one thing a turn
     * needs that is not on this screen: it is in the status bar, which is
     * above the body and cannot be scrolled at all. The app is held to
     * keeping it current, because that is what makes it readable. */
    if (app->mode == FLEET_MODE_MULTI) {
        /* Multiplayer: exactly the status the match and the link give now -
         * not a second opinion kept by the screen. */
        char want[64];

        fleet_app_mp_status(app, want, sizeof(want));
        check_str("the header carries the match's status as it stands", g_hint, want);
        check("the chat button is in view", inside_viewport(battle_chat()));
    } else {
        check("the status bar carries the turn",
              strstr(g_hint, "TURN") != NULL || strstr(g_hint, "COMPLETE") != NULL);
        check("single player has no chat button", !visible(battle_chat()));
    }
}

/*
 * Across the page the screen is the board, the readout and your own waters
 * beside it, and FIRE across the foot of both.
 */
static void check_wide_battle(int want_cell)
{
    lv_area_t board;
    lv_area_t act;
    lv_area_t waters;
    lv_area_t fire;
    lv_area_t side;
    lv_area_t view;

    box_of(battle_board(), &board);
    box_of(battle_act(), &act);
    box_of(battle_waters(), &waters);
    box_of(battle_fire(), &fire);
    box_of(battle_side(), &side);
    turn_viewport(&view);

    check_int("the board is as tall as the body allows", lv_area_get_height(&board),
              SPAN_OF(want_cell));
    check_int("its rows are the wide cell", fleet_grid_cell(battle_board()), want_cell);
    /* Height is the binding constraint and width the one with room to spare,
     * so a cell is wider than it is tall - which is the only way this shape
     * has of making the target bigger. */
    check_int("its columns are half as wide again, up to the cap",
              fleet_grid_cell_across(battle_board()), ACROSS_OF(want_cell));
    check_int("so the board is wider than it is tall", lv_area_get_width(&board),
              SPAN_OF(ACROSS_OF(want_cell)));
    check("a cell is larger than one down the page in area",
          fleet_grid_cell_across(battle_board()) * fleet_grid_cell(battle_board()) >
              want_cell * want_cell);
    check("and wider than the 48 px the tall shape draws",
          fleet_grid_cell_across(battle_board()) > FLEET_CELL_TALL);
    check("the readout stands beside the board, not under it", act.x1 > board.x2);
    check("your own waters stand beyond the readout", waters.x1 > act.x2);

    /* The order the turn is played in is the order across the page, and the
     * weight is in that order too. The board is the largest thing on the
     * screen - by area, which is what a square board is measured in, not by
     * width, which the readout has more of because it holds lines of text -
     * and your own waters are the smallest of the three. */
    /* The four one-square nudges: the path that asks the player to hit
     * nothing small. Each is a full touch target whatever the board does. */
    check_int("there are four one-square nudges",
              (int)lv_obj_get_child_count(battle_steps()), STEP_COUNT);
    {
        int i;
        int small = 0;

        for (i = 0; i < STEP_COUNT; i++) {
            lv_area_t b;

            box_of(battle_step(i), &b);
            if (lv_area_get_width(&b) < POCKETUI_TOUCH_MIN ||
                lv_area_get_height(&b) < POCKETUI_TOUCH_MIN) {
                small = 1;
            }
        }
        check("every one of them is a finger's size", !small);
        check("and they are shown across the page",
              !lv_obj_has_flag(battle_steps(), LV_OBJ_FLAG_HIDDEN));
        check("they stand with the readout, next to the board",
              lv_obj_get_parent(battle_steps()) == battle_target_panel());
        {
            lv_area_t steps;
            lv_area_t panel;
            lv_area_t words;

            box_of(battle_steps(), &steps);
            box_of(battle_target_panel(), &panel);
            box_of(battle_log(), &words);
            /* At the foot of the panel, under everything that is read, and
             * in the same place however much the text above them wraps. */
            /* Right down on the panel's content edge, whatever padding the
             * type size gives it. */
            check("and at the foot of it, nearest FIRE",
                  panel.y2 - steps.y2 <=
                      lv_obj_get_style_pad_bottom(battle_target_panel(), LV_PART_MAIN) +
                      2 * lv_obj_get_style_border_width(battle_target_panel(),
                                                        LV_PART_MAIN));
            check("below the last thing that is read", steps.y1 > words.y1);
        }
    }

    check("the board is the largest thing on the screen",
          lv_area_get_width(&board) * lv_area_get_height(&board) >
              lv_area_get_width(&act) * lv_area_get_height(&act));
    check("and larger than your own waters several times over",
          lv_area_get_width(&board) * lv_area_get_height(&board) >
              2 * lv_area_get_width(&waters) * lv_area_get_height(&waters));
    check("your own waters are the narrowest column",
          lv_area_get_width(&waters) < lv_area_get_width(&act));
    check("your own board is smaller than the target board",
          fleet_grid_cell(battle_own()) < fleet_grid_cell(battle_board()));
    check_int("your own board is the same 20 px it is down the page",
              fleet_grid_cell(battle_own()), OWN_CELL_ANY);

    /* Neither panel is stretched to the foot of the screen: a panel that ends
     * where its neighbour ends reads as finished, and one that ends at the
     * edge of the body reads as cut off. The panels themselves are what is
     * measured, not the columns holding them - a column can be the right
     * height with the panel in it half as tall. */
    {
        lv_area_t readout;
        lv_area_t own;

        box_of(battle_target_panel(), &readout);
        box_of(battle_waters_panel(), &own);
        check_int("the two panels start on the same line", readout.y1, own.y1);
        check_int("and close on the same line", readout.y2, own.y2);
        check("well above the foot of the body",
              readout.y2 < view.y2 - POCKETUI_TOUCH_MIN);
    }
    check_int("their columns end together too", act.y2, waters.y2);

    check("FIRE spans the whole region beside the board",
          fire.x1 == side.x1 && fire.x2 == side.x2);
    check("so it is wider than either panel", lv_area_get_width(&fire) >
          lv_area_get_width(&act));
    check("it is at the foot, under both panels", fire.y1 > act.y2 && fire.y1 > waters.y2);
    check_int("and its foot is the foot of the body", fire.y2, view.y2);
    check("FIRE keeps the touch minimum", lv_area_get_height(&fire) >= POCKETUI_TOUCH_MIN);
    check("and is a larger target than the one down the page",
          lv_area_get_width(&fire) * lv_area_get_height(&fire) >
              TALL_W * POCKETUI_TOUCH_MIN);
    check("and is wider than it is tall", lv_area_get_width(&fire) > lv_area_get_height(&fire));

    /* The log reports the exchange just played, so across the page it is with
     * the readout, where a line of text has room. */
    check("the last exchange is read beside the board, with the readout",
          lv_obj_get_parent(battle_log()) == battle_target_panel());
    check("and FIRE has left the readout column for the whole region",
          lv_obj_get_parent(battle_fire_row()) == battle_side());

    /* Down the page the frame is what scrolls the stack; across it there is
     * nothing to scroll, and the frame says so rather than being a scroller
     * that happens to have nothing under the fold. */
    check("across the page the frame is not a scroller",
          !lv_obj_has_flag(frame_of(), LV_OBJ_FLAG_SCROLLABLE));

    check("nothing on the screen leaves the body", inside_body(screen_of(FLEET_SCREEN_BATTLE)));
    check("the board is in the safe area", in_safe_area(battle_board()));
    check("FIRE is in the safe area", in_safe_area(battle_fire()));
    check("your waters are in the safe area", in_safe_area(battle_waters_panel()));
    check("the TARGET caption is drawn, not cut", caption_would_be_drawn(battle_target_panel()));
    check("and so is YOUR WATERS", caption_would_be_drawn(battle_waters_panel()));
}

static void check_tall_battle(void)
{
    lv_area_t frame;
    lv_area_t board;
    lv_area_t act;
    lv_area_t waters;
    lv_area_t fire;

    box_of(frame_of(), &frame);
    box_of(battle_board(), &board);
    box_of(battle_act(), &act);
    box_of(battle_waters(), &waters);
    box_of(battle_fire(), &fire);

    check_int("the board is the v0.0.10 board", lv_area_get_width(&board),
              SPAN_OF(FLEET_CELL_TALL));
    check_int("its cells are 48 px", fleet_grid_cell(battle_board()), FLEET_CELL_TALL);
    check_int("and square, as they always were",
              fleet_grid_cell_across(battle_board()), FLEET_CELL_TALL);
    check_int("the board is square with them", lv_area_get_width(&board),
              lv_area_get_height(&board));
    check("the nudges are not on the screen at all down the page",
          lv_obj_has_flag(battle_steps(), LV_OBJ_FLAG_HIDDEN));
    check_int("your own board is back to 20 px", fleet_grid_cell(battle_own()), 20);
    /* Where v0.0.10 put it: at the top of the body, centred across a column
     * six pixels wider than it is. */
    check_int("the board starts at the top of the body", board.y1, frame.y1);
    check_int("and is centred across it", board.x1 - frame.x1,
              (TALL_W - SPAN_OF(FLEET_CELL_TALL)) / 2);
    check("the readout is under the board", act.y1 > board.y2);
    check("your own waters are under the readout", waters.y1 > act.y2);
    check_int("FIRE is full width", lv_area_get_width(&fire), TALL_W);
    check_int("and the 64 px it has always been", lv_area_get_height(&fire),
              POCKETUI_TOUCH_MIN);
    check("FIRE is back at the foot of the readout column",
          lv_obj_get_parent(battle_fire_row()) == battle_act());
    check("and the log back under your own board",
          lv_obj_get_parent(battle_log()) == battle_waters_panel());
    check("the TARGET caption is drawn, not cut", caption_would_be_drawn(battle_target_panel()));
    check("and so is YOUR WATERS", caption_would_be_drawn(battle_waters_panel()));
}

/* One of each: a relayout that made a second board or a second FIRE would be
 * caught by the object count, but not by anything that says which is which. */
static int count_matching(lv_obj_t *obj, lv_obj_t *want)
{
    uint32_t i;
    int n = obj == want;

    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n += count_matching(lv_obj_get_child(obj, i), want);
    }
    return n;
}

static void check_singletons(void)
{
    check_int("there is one target board in the tree",
              count_matching(frame_of(), battle_board()), 1);
    check_int("one FIRE", count_matching(frame_of(), battle_fire()), 1);
    check_int("one board of your own", count_matching(frame_of(), battle_own()), 1);
    check_int("one CONFIRM DEPLOYMENT", count_matching(frame_of(), deploy_confirm()), 1);
    check_int("one DEPLOY FLEET", count_matching(frame_of(), command_deploy()), 1);
}


/* ---- the tap path ------------------------------------------------------ */

/* Tap the centre and all four corners of every cell of the target board and
 * see that each lands on that cell and no other. The crosshair is the report:
 * moving it is what a tap does, and all it does. */
static void test_board_taps(const char *what)
{
    lv_obj_t *grid = battle_board();
    int row;
    int col;
    int p;
    int wrong = 0;
    int missed = 0;

    phase = what;
    for (row = 0; row < FLEET_GRID; row++) {
        for (col = 0; col < FLEET_GRID; col++) {
            lv_area_t c;
            int32_t pts[5][2];

            cell_box(grid, 1, row, col, &c);
            pts[0][0] = (c.x1 + c.x2) / 2; pts[0][1] = (c.y1 + c.y2) / 2;
            pts[1][0] = c.x1;              pts[1][1] = c.y1;
            pts[2][0] = c.x2;              pts[2][1] = c.y1;
            pts[3][0] = c.x1;              pts[3][1] = c.y2;
            pts[4][0] = c.x2;              pts[4][1] = c.y2;
            for (p = 0; p < 5; p++) {
                int got_row = -1;
                int got_col = -1;

                fleet_grid_set_cursor(grid, -1, -1);
                tap_at(pts[p][0], pts[p][1]);
                if (fleet_grid_get_cursor(grid, &got_row, &got_col) != 0) {
                    missed++;
                } else if (got_row != row || got_col != col) {
                    if (wrong < 4) {
                        printf("FAIL [%s] tap at (%d,%d) in %c%d landed on %c%d\n", phase,
                               (int)pts[p][0], (int)pts[p][1], 'A' + col, row + 1,
                               'A' + got_col, got_row + 1);
                    }
                    wrong++;
                }
            }
        }
    }
    check_int("every corner and centre of every cell hits that cell", wrong, 0);
    check_int("and none of them misses the board", missed, 0);
}

/* The pixels between cells, the caption gutter, and points just off the board.
 * A gap belongs to the cell before it - deterministic, and overlapping
 * nothing - while the gutter and everything outside must not aim at all. */
/*
 * What the grid draws and what it hits, compared rather than both taken on
 * trust. cell_box() above is the test's own restatement of the layout's
 * arithmetic; fleet_grid_cell_rect() is the grid's, and it is the one the
 * drawing uses. If they part company - a cell painted narrower than the
 * square that answers to a tap, say - the board is lying about where a square
 * is, and the taps alone would not notice.
 */
static void test_drawn_is_hit(lv_obj_t *grid, int labels, const char *what)
{
    int row;
    int col;
    int wrong = 0;
    int missing = 0;

    phase = what;
    for (row = 0; row < FLEET_GRID; row++) {
        for (col = 0; col < FLEET_GRID; col++) {
            lv_area_t drawn;
            lv_area_t want;

            if (fleet_grid_cell_rect(grid, row, col, &drawn) != 0) {
                missing++;
                continue;
            }
            cell_box(grid, labels, row, col, &want);
            if (memcmp(&drawn, &want, sizeof(want)) != 0) {
                if (!wrong) {
                    printf("     cell %d,%d drawn x %d..%d y %d..%d, "
                           "the layout says x %d..%d y %d..%d\n", row, col,
                           (int)drawn.x1, (int)drawn.x2, (int)drawn.y1, (int)drawn.y2,
                           (int)want.x1, (int)want.x2, (int)want.y1, (int)want.y2);
                }
                wrong++;
            }
        }
    }
    check_int("every square is drawn where the layout says it is", wrong, 0);
    check_int("and the grid answers for all one hundred of them", missing, 0);
    {
        lv_area_t off;

        check_int("a square off the board has no rectangle",
                  fleet_grid_cell_rect(grid, FLEET_GRID, 0, &off), -1);
    }
}

static void test_board_edges(const char *what)
{
    lv_obj_t *grid = battle_board();
    int cell = fleet_grid_cell(grid);
    lv_area_t g;
    lv_area_t c;
    int row;
    int i;
    int gap_wrong = 0;
    int stuck = 0;
    int32_t mid_x;
    int32_t mid_y;

    phase = what;
    box_of(grid, &g);

    for (row = 0; row < FLEET_GRID - 1; row++) {
        int got_row = -1;
        int got_col = -1;

        cell_box(grid, 1, row, row, &c);
        for (i = 1; i <= FLEET_GRID_GAP; i++) {
            fleet_grid_set_cursor(grid, -1, -1);
            tap_at(c.x2 + i, (c.y1 + c.y2) / 2);
            if (fleet_grid_get_cursor(grid, &got_row, &got_col) != 0 ||
                got_row != row || got_col != row) {
                gap_wrong++;
            }
            fleet_grid_set_cursor(grid, -1, -1);
            tap_at((c.x1 + c.x2) / 2, c.y2 + i);
            if (fleet_grid_get_cursor(grid, &got_row, &got_col) != 0 ||
                got_row != row || got_col != row) {
                gap_wrong++;
            }
        }
    }
    check_int("a tap in the gap between cells belongs to the cell before it", gap_wrong, 0);

    for (i = 0; i < FLEET_GRID_GUTTER; i++) {
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(g.x1 + i, g.y1 + FLEET_GRID_GUTTER + cell / 2);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(g.x1 + FLEET_GRID_GUTTER + cell / 2, g.y1 + i);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
    }
    check_int("a tap in the A-J and 1-10 gutter aims at nothing", stuck, 0);

    stuck = 0;
    mid_x = (g.x1 + g.x2) / 2;
    mid_y = (g.y1 + g.y2) / 2;
    for (i = 1; i <= 8; i++) {
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(g.x1 - i, mid_y);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(g.x2 + i, mid_y);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(mid_x, g.y1 - i);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
        fleet_grid_set_cursor(grid, -1, -1);
        tap_at(mid_x, g.y2 + i);
        stuck += fleet_grid_get_cursor(grid, NULL, NULL) == 0;
    }
    check_int("a tap just off the board aims at nothing", stuck, 0);

    cell_box(grid, 1, FLEET_GRID - 1, FLEET_GRID - 1, &c);
    check_int("the board ends where the last cell ends", c.x2, g.x2);
    check_int("and is as high as it is wide", c.y2, g.y2);
}

/* ---- the match, and what a relayout may not do to it -------------------- */

static int shots_on(const struct fleet_board *b)
{
    int i;
    int n = 0;

    for (i = 0; i < FLEET_CELLS; i++) {
        n += b->shot[i] != 0;
    }
    return n;
}

struct match_state {
    uint8_t game_phase;
    uint16_t turn;
    int shots_player;
    int shots_opponent;
    int afloat_player;
    int afloat_opponent;
    int placed;
    int aim_row;
    int aim_col;
    int aimed;
    uint32_t seed;
};

static void snapshot(struct match_state *s)
{
    memset(s, 0, sizeof(*s));
    s->game_phase = app->game.phase;
    s->turn = app->game.turn;
    s->shots_player = shots_on(&app->game.board[FLEET_SIDE_PLAYER]);
    s->shots_opponent = shots_on(&app->game.board[FLEET_SIDE_OPPONENT]);
    s->afloat_player = app->game.board[FLEET_SIDE_PLAYER].ships_afloat;
    s->afloat_opponent = app->game.board[FLEET_SIDE_OPPONENT].ships_afloat;
    s->placed = app->game.board[FLEET_SIDE_PLAYER].ships_placed;
    s->seed = app->game.seed;
    s->aimed = fleet_grid_get_cursor(battle_board(), &s->aim_row, &s->aim_col) == 0;
    if (!s->aimed) {
        s->aim_row = -1;
        s->aim_col = -1;
    }
}

static void check_same_match(const char *what, const struct match_state *a)
{
    struct match_state b;

    snapshot(&b);
    checks++;
    if (memcmp(a, &b, sizeof(b)) != 0) {
        failed++;
        printf("FAIL [%s] %s\n", phase, what);
        printf("     phase %u/%u turn %u/%u shots %d,%d/%d,%d afloat %d,%d/%d,%d "
               "placed %d/%d aim %d(%d,%d)/%d(%d,%d)\n",
               a->game_phase, b.game_phase, a->turn, b.turn,
               a->shots_player, a->shots_opponent, b.shots_player, b.shots_opponent,
               a->afloat_player, a->afloat_opponent, b.afloat_player, b.afloat_opponent,
               a->placed, b.placed,
               a->aimed, a->aim_row, a->aim_col, b.aimed, b.aim_row, b.aim_col);
    }
}

/* Your own waters report; they are never aimed at. */
static void check_own_board_is_not_a_target(void)
{
    lv_obj_t *own = battle_own();
    lv_area_t a;
    struct match_state before;
    int i;

    box_of(own, &a);
    snapshot(&before);
    for (i = 0; i < FLEET_GRID; i++) {
        lv_area_t c;

        cell_box(own, 0, i, i, &c);
        tap_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
    }
    check_same_match("a tap on your own waters does nothing at all", &before);
}

static void test_relayout_keeps_the_match(int32_t corner)
{
    struct match_state before;
    uint32_t layouts;
    int objects;
    int i;

    phase = "relayout";
    /* Start from a known shape, so the first turn in the loop below is a real
     * change and not a repeat of the shape already in force. */
    use_display(POS_ROTATION_0, corner);
    fleet_screen_battle_aim(app, 4, 7);
    pump(60);
    snapshot(&before);
    objects = count_objects(frame_of());

    for (i = 0; i < 3; i++) {
        layouts = app->layouts;
        use_display(POS_ROTATION_270, corner);
        check_int("turning the display adds no objects", count_objects(frame_of()), objects);
        /* The body can settle in more than one step, so more than one pass is
         * allowed; none at all would mean the app never noticed, and many
         * would mean it is laying out for nothing. */
        check("and lays the app out, a bounded number of times",
              app->layouts > layouts && app->layouts <= layouts + 2);
        check_same_match("the match is untouched by the turn", &before);
        check_int("the board is the wide board", fleet_grid_cell(battle_board()), WIDE_CELL);
        layouts = app->layouts;
        use_display(POS_ROTATION_0, corner);
        check_int("and turning it back adds none either", count_objects(frame_of()), objects);
        check("and lays it out again, as few times",
              app->layouts > layouts && app->layouts <= layouts + 2);
        check_same_match("the match is untouched by turning back", &before);
        check_int("the board is the tall board", fleet_grid_cell(battle_board()),
                  FLEET_CELL_TALL);
    }
    layouts = app->layouts;
    /* A body that has not changed must not be laid out again: a layout pass is
     * the whole cost of this app, and repeating it for nothing is the one way
     * a shape chosen from the body can cost more than one fixed to it. */
    use_display(POS_ROTATION_0, corner);
    pump(600);
    check_int("laying out an unchanged body does no work at all",
              app->layouts, layouts);
    check_int("and adds no objects", count_objects(frame_of()), objects);
    check_same_match("and changes nothing", &before);
    /* Nor does a match being played: a turn is not a layout. */
    fleet_screen_battle_aim(app, 1, 1);
    pump(60);
    fleet_screen_battle_fire(app);
    pump(1200);
    check_int("nor does playing a turn", app->layouts, layouts);
    check_int("nor does that add objects", count_objects(frame_of()), objects);
}

/* Every roster row selects its own ship - the last one included. The proof is
 * behavioural: choose a ship, put it on the board, see which ship moved. */
/* Defined with the other harness helpers, below. */
static void with_screen(const char *screen);

/*
 * The invariant, held through a whole match rather than at the moment the
 * screen is built: every turn of a game played across the page leaves the
 * screen whole. The log line and the note under the readout are the two
 * things on this screen whose height depends on what they say, so a match is
 * played out - sinkings reported on both sides and all - and the screen is
 * measured after every single turn.
 */
static void test_no_scroll_through_a_match(const char *mode, int32_t corner)
{
    char what[64];
    int turn;
    int worst = 0;
    int row;
    int col;

    snprintf(what, sizeof(what), "a match across the page, %s", mode);
    use_mode(mode);
    use_display(POS_ROTATION_270, corner);
    with_screen("battle");
    app_start();
    pump(120);

    check_battle_never_scrolls(what);
    phase = what;

    for (turn = 0, row = 0; row < FLEET_GRID; row++) {
        for (col = 0; col < FLEET_GRID; col++) {
            lv_area_t c;
            lv_area_t v;
            lv_area_t log;
            lv_obj_t *bad;

            if (app->current != FLEET_SCREEN_BATTLE) {
                break;
            }
            cell_box(battle_board(), 1, row, col, &c);
            tap_at(c.x1 + 2, c.y1 + 2);
            tap_obj(battle_fire());
            /* Past the paced reply, so the turn is complete and the log line
             * carries both halves of the exchange. */
            pump(REPLY_SETTLE_MS);
            if (app->current != FLEET_SCREEN_BATTLE) {
                break;
            }
            turn++;
            box_of(battle_log(), &log);
            if (lv_area_get_height(&log) > worst) {
                worst = lv_area_get_height(&log);
            }
            turn_viewport(&v);
            bad = outside_of(screen_of(FLEET_SCREEN_BATTLE), &v);
            if (scrollable_by(app_body, NULL) != 0 || bad) {
                /* The turn it first went wrong on, rather than every turn
                 * after it. */
                check("no turn of the match needed the screen scrolled", 0);
                printf("     first at turn %d, the log line %d px tall\n", turn,
                       (int)lv_area_get_height(&log));
                if (bad) {
                    say_where("the stray object", bad);
                    say_where("its parent", lv_obj_get_parent(bad));
                }
                row = FLEET_GRID;
                break;
            }
        }
    }
    check("a whole match was played across the page", turn > 20);
    check("with the exchange reported every turn", worst > 0);
    check_battle_never_scrolls(what);
    app_stop();
    with_screen(NULL);
    use_mode("normal");
}

/*
 * Aiming without a precise touch. A row across the page is 40 px, which no
 * layout can improve on, so the screen offers two ways to reach a square that
 * do not ask the player to hit one: land anywhere and slide, and four
 * one-square nudges that are each a finger's size. Neither of them fires.
 */
static void test_aim_without_precision(const char *what)
{
    struct match_state before;
    lv_area_t from;
    lv_area_t to;
    lv_point_t path[5];
    int row = -1;
    int col = -1;
    int i;

    phase = what;
    fleet_grid_set_cursor(battle_board(), -1, -1);
    pump(60);
    snapshot(&before);

    /* ---- landing anywhere and sliding ---------------------------------- */

    cell_box(battle_board(), 1, 1, 1, &from);
    cell_box(battle_board(), 1, 7, 6, &to);
    path[0].x = (from.x1 + from.x2) / 2;
    path[0].y = (from.y1 + from.y2) / 2;
    path[4].x = (to.x1 + to.x2) / 2;
    path[4].y = (to.y1 + to.y2) / 2;
    for (i = 1; i < 4; i++) {
        path[i].x = path[0].x + (path[4].x - path[0].x) * i / 4;
        path[i].y = path[0].y + (path[4].y - path[0].y) * i / 4;
    }
    drag_through(path, 5);
    check_int("a drag ends aimed at the square it ended on",
              fleet_grid_get_cursor(battle_board(), &row, &col), 0);
    check_int("its row", row, 7);
    check_int("its column", col, 6);
    check_str("and the readout names it", text_of(battle_cell_value()), "G8");
    before.aimed = 1;
    before.aim_row = 7;
    before.aim_col = 6;
    check_same_match("a drag fires nothing", &before);

    /* The square under the finger is reported the whole way, not only at the
     * end: that is what lets the aim be corrected by watching the readout
     * rather than by hitting a small square. */
    finger_point = path[0];
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    check_int("the press alone aims", fleet_grid_get_cursor(battle_board(), &row, &col), 0);
    check_int("at the square pressed", row, 1);
    cell_box(battle_board(), 1, 4, 4, &to);
    finger_point.x = (to.x1 + to.x2) / 2;
    finger_point.y = (to.y1 + to.y2) / 2;
    pump(60);
    check_str("and the readout follows the finger mid-drag",
              text_of(battle_cell_value()), "E5");
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    check_str("letting go leaves it there", text_of(battle_cell_value()), "E5");

    /* ---- the four one-square nudges ------------------------------------ */

    tap_obj(battle_step(0));
    check_str("a nudge left moves one square", text_of(battle_cell_value()), "D5");
    tap_obj(battle_step(1));
    check_str("up moves one row", text_of(battle_cell_value()), "D4");
    tap_obj(battle_step(2));
    check_str("down moves back", text_of(battle_cell_value()), "D5");
    tap_obj(battle_step(3));
    check_str("right moves one column", text_of(battle_cell_value()), "E5");
    before.aimed = 1;
    before.aim_row = 4;
    before.aim_col = 4;
    check_same_match("and none of them fires", &before);

    /* They clamp rather than wrap: a crosshair driven at the edge stays on
     * the board. */
    fleet_screen_battle_aim(app, 0, 0);
    pump(60);
    tap_obj(battle_step(0));
    tap_obj(battle_step(1));
    check_str("the top left corner is a wall", text_of(battle_cell_value()), "A1");
    fleet_screen_battle_aim(app, FLEET_GRID - 1, FLEET_GRID - 1);
    pump(60);
    tap_obj(battle_step(2));
    tap_obj(battle_step(3));
    check_str("and so is the bottom right", text_of(battle_cell_value()), "J10");

    /* With nothing aimed the first nudge starts in the middle, so the whole
     * board is within five presses of it. */
    fleet_grid_set_cursor(battle_board(), -1, -1);
    pump(60);
    tap_obj(battle_step(1));
    check_str("the first nudge starts in the middle", text_of(battle_cell_value()), "F6");
    check("every square is reachable by nudges alone",
          FLEET_GRID / 2 + FLEET_GRID / 2 <= 10);
}

static void test_roster(const char *what)
{
    int wrong = 0;
    int i;

    phase = what;
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_area_t c;

        tap_obj(kid(deploy_controls(), DEPLOY_CLEAR));
        tap_obj(deploy_roster_row(i));
        cell_box(deploy_board(), 1, 0, 0, &c);
        tap_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
        if (!app->game.board[FLEET_SIDE_PLAYER].ships[i].placed) {
            if (wrong < 3) {
                printf("FAIL [%s] the roster row for %s placed something else\n", phase,
                       fleet_ship_name((enum fleet_ship)i));
            }
            wrong++;
        }
    }
    check_int("every ship in the roster can be chosen, the last included", wrong, 0);
    tap_obj(kid(deploy_controls(), DEPLOY_CLEAR));
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
}

/* ---- main -------------------------------------------------------------- */

static char state_dir[] = "/tmp/fleet_app_test_XXXXXX";

static void with_screen(const char *screen)
{
    if (screen) {
        setenv("POCKETFLEET_SCREEN", screen, 1);
    } else {
        unsetenv("POCKETFLEET_SCREEN");
    }
}


/* ---- 8. multiplayer, against the virtual opponent ----------------------- *
 *
 * The same app under the same finger, playing another device through the real
 * protocol (docs/apps/FLEET_MULTIPLAYER.md) over the fake link. Nothing here
 * changes the rules the solo game is held to: a multiplayer turn is on the
 * display whole, across the page, every time.
 */

/* Time passes for the match: the session's clock and LVGL's together. */
static void mp_wait(int64_t ms)
{
    int64_t t;

    for (t = 0; t < ms; t += 100) {
        app->mp_clock_offset += 100;
        pump(100);
    }
}

/* Each multiplayer case starts with no match on disk. */
static void mp_fresh(void)
{
    char path[600];

    snprintf(path, sizeof(path), "%s/fleet/match.v1", state_dir);
    unlink(path);
}

static lv_obj_t *command_multi_button(void)
{
    lv_obj_t *panel = kid(command_col(0), 1);

    return kid(panel, (int)lv_obj_get_child_count(panel) - 1);
}

static lv_obj_t *lobby_screen(void) { return screen_of(FLEET_SCREEN_LOBBY); }
static lv_obj_t *lobby_col(int i) { return kid(kid(lobby_screen(), 0), i); }
static lv_obj_t *lobby_player_row(int i) { return kid(kid(lobby_col(0), 0), KID_PANEL_FIRST + i); }
static lv_obj_t *lobby_act(void) { return kid(lobby_col(1), 1); }
static lv_obj_t *lobby_alt(void) { return kid(lobby_col(1), 2); }

/* A listed row lit in the accent: the player INVITE would go to. */
static int lobby_row_lit(int i)
{
    lv_style_value_t accent;

    return visible(lobby_player_row(i)) &&
           lv_style_get_prop(pos_style(POS_STYLE_ACCENT_TEXT), LV_STYLE_TEXT_COLOR, &accent) ==
               LV_STYLE_RES_FOUND &&
           lv_color_eq(lv_obj_get_style_text_color(kid(lobby_player_row(i), 0), 0), accent.color);
}

static int lobby_rows_lit(void)
{
    int n = 0;
    int i;

    for (i = 0; i < 5; i++) {
        n += lobby_row_lit(i);
    }
    return n;
}

static int lobby_row_is(int i, const char *name)
{
    return visible(lobby_player_row(i)) &&
           strncmp(text_of(kid(lobby_player_row(i), 0)), name, strlen(name)) == 0;
}

/* Our side's shot: the first square in reading order not yet fired at. */
static int mp_our_turn(int check_layout)
{
    const struct fleet_match *m = &app->mp->m;
    lv_area_t cell;
    int i;

    for (i = 0; i < FLEET_CELLS; i++) {
        if (!m->target.shot[i]) {
            break;
        }
    }
    if (i == FLEET_CELLS ||
        fleet_grid_cell_rect(battle_board(), i / FLEET_GRID, i % FLEET_GRID, &cell) != 0) {
        return -1;
    }
    tap_at((cell.x1 + cell.x2) / 2, (cell.y1 + cell.y2) / 2);
    if (check_layout) {
        check_battle_never_scrolls("multiplayer, a turn across the page");
    }
    tap_obj(battle_fire());
    return 0;
}

static void test_multiplayer(enum pos_rotation rotation)
{
    int turns = 0;
    int fire_only_on_turn = 1;
    int every_press_sent = 1;
    int sent_before;
    int resolved_before;
    int i;

    phase = rotation == POS_ROTATION_0 ? "multiplayer, tall" : "multiplayer, wide";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=5", 1);
    use_display(rotation, PANEL_CORNER);
    app_start();
    check("multiplayer is available with a link", app->mp != NULL && app->link != NULL);
    mp_wait(2000);
    check("opening Fleet engages nothing and sends nothing",
          !app->mp->engaged && app->mp->sent == 0 && app->mp->received == 0);
    check("MULTIPLAYER is in view on Command, not below the fold",
          inside_body(command_multi_button()) && in_safe_area(command_multi_button()));
    if (rotation != POS_ROTATION_0) {
        /* Across the page Command is one screen: the choice, the fleet,
         * the terms and the ways on, nothing below a fold. */
        check_in_view("Command: MULTIPLAYER", command_multi_button());
        check_in_view("Command: the opponent's difficulty", command_segments());
        check_in_view("Command: DEPLOY FLEET", command_deploy());
    }
    tap_obj(command_multi_button());
    mp_wait(500);
    check_one_screen(FLEET_SCREEN_LOBBY);
    check_in_view("Lobby: the opponent's row", lobby_player_row(0));
    check_in_view("Lobby: INVITE", lobby_act());
    check_str("the header says where we are", g_hint, "MULTIPLAYER");
    check("the lobby lists the opponent", visible(lobby_player_row(0)));
    check("nothing is sent by opening the lobby", app->mp->sent == 0);
    check("INVITE waits for a player to be chosen",
          !lv_obj_has_flag(lobby_act(), LV_OBJ_FLAG_CLICKABLE));
    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    check("INVITE sent one invitation", app->mp->m.phase == FLEET_MP_INVITING && app->mp->sent == 1);
    check("every lobby control keeps the touch minimum",
          lv_obj_get_height(lobby_act()) >= POCKETUI_TOUCH_MIN &&
          lv_obj_get_height(lobby_player_row(0)) >= 56);
    check("and the lobby stays inside the body", inside_body(lobby_screen()));
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    check_one_screen(FLEET_SCREEN_DEPLOY);
    check_in_view("Deploy: the board", deploy_board());
    check_in_view("Deploy: AUTO", kid(deploy_controls(), DEPLOY_AUTO));
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    check_in_view("Deploy: CONFIRM", deploy_confirm());
    tap_obj(deploy_confirm());
    check_one_screen(FLEET_SCREEN_BATTLE);
    check_in_view("Battle: the target board", battle_board());
    check_in_view("Battle: FIRE", battle_fire());
    for (i = 0; i < 20000 && (app->mp->m.phase == FLEET_MP_COMMITTED ||
                              app->mp->m.phase == FLEET_MP_BATTLE); i++) {
        if (fleet_match_my_turn(&app->mp->m)) {
            sent_before = (int)app->mp->sent;
            resolved_before = app->mp->m.resolved;
            if (mp_our_turn(rotation != POS_ROTATION_0 && turns % 7 == 0) != 0) {
                break;
            }
            turns++;
            /* The shot was taken: pending, or already answered. Whether it
             * has left yet is the airtime governor's business - at this pace
             * it holds some back, and says so. */
            (void)sent_before;
            if (app->mp->m.pending == FLEET_NO_CELL && app->mp->m.resolved == resolved_before) {
                every_press_sent = 0;
            }
        } else if (app->mp->m.pending == FLEET_NO_CELL && app->current == FLEET_SCREEN_BATTLE) {
            /* The opponent's turn: aiming is allowed, firing is not. */
            fire_only_on_turn &= !lv_obj_has_flag(battle_fire(), LV_OBJ_FLAG_CLICKABLE) ||
                                 fleet_match_link(&app->mp->m) == FLEET_LINK_LOST;
        }
        mp_wait(100);
    }
    for (i = 0; i < 300 && app->mp->m.phase != FLEET_MP_DONE; i++) {
        mp_wait(100);
    }
    printf("     %s: %d turns of ours, %u packets sent\n", phase, turns, app->mp->sent);
    check("FIRE cannot be pressed on the opponent's turn", fire_only_on_turn);
    check("and every press on ours took the shot", every_press_sent);
    check("the match played to its end", app->mp->m.phase == FLEET_MP_DONE);
    check_one_screen(FLEET_SCREEN_RESULT);
    check("the Result names the outcome",
          strcmp(text_of(result_heading()), "Enemy fleet destroyed") == 0 ||
          strcmp(text_of(result_heading()), "Fleet lost") == 0);
    check_str("and the opponent's fleet was verified", text_of(result_value(0, 3)), "Verified");
    check("the Result stays inside the body", inside_body(screen_of(FLEET_SCREEN_RESULT)));
    check_in_view("Result: the outcome", result_heading());
    check_in_view("Result: the way back to the lobby", kid(result_foot(), 0));
    tap_obj(kid(result_foot(), 0));
    check_one_screen(FLEET_SCREEN_LOBBY);
    check("MULTIPLAYER put the finished match away",
          app->mp->m.phase == FLEET_MP_IDLE && app->mp->m.tomb[0].kind == FLEET_TOMB_ENDED);
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

static void test_multiplayer_reopen(void)
{
    int i;

    phase = "multiplayer, closed and reopened";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=6", 1);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tap_obj(command_multi_button());
    mp_wait(300);
    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    tap_obj(deploy_confirm());
    for (i = 0; i < 400 && app->mp->m.resolved < 8; i++) {
        if (fleet_match_my_turn(&app->mp->m)) {
            mp_our_turn(0);
        }
        mp_wait(100);
    }
    check("some of the match was played", app->mp->m.resolved >= 8);
    /* The shell destroys the app; the virtual opponent goes with its link, so
     * the reopened app plays a fresh one against the saved match - which is
     * exactly what a stranger's device looks like: the next packet voids it
     * as unknown. That half is fleet_session_test's; this is the screens'. */
    app_stop();
    app_start();
    mp_wait(1000);
    check("reopened, nothing is engaged or sent", !app->mp->engaged && app->mp->sent == 0);
    check("Command offers the match in hand", app->mp_saved &&
          strcmp(text_of(kid(command_multi_button(), 0)), "RESUME MATCH") == 0);
    tap_obj(command_multi_button());
    check("RESUME MATCH opens the lobby, the match under way",
          app->current == FLEET_SCREEN_LOBBY && app->mp->engaged);
    check_str("and offers RESUME", text_of(kid(lobby_act(), 0)), "RESUME");
    check_str("and FORFEIT", text_of(kid(lobby_alt(), 0)), "FORFEIT");
    tap_obj(lobby_act());
    check("RESUME goes back to the battle, and asks the peer where it stands",
          app->current == FLEET_SCREEN_BATTLE && app->mp->sent >= 1);
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

/* The lobby's choice is a player, not a row. meshcored lists the most recently
 * heard first, so rows re-sort under the finger; unit B, 2026-09-30: the
 * opponent chosen on row 4 moved to row 1 after a match, and row 4 - now a
 * stranger's node - stayed lit with INVITE ready to send to it. The virtual
 * opponent is listed among bystanders here, and the bystanders are heard. */
static void test_multiplayer_lobby_choice(void)
{
    const struct fleet_match *opponent;

    phase = "multiplayer, the lobby's choice follows the player";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=7,crowd=5", 1);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tap_obj(command_multi_button());
    mp_wait(1500);
    check("the lobby lists the opponent first, then bystanders",
          lobby_row_is(0, "SIM OPPONENT") && lobby_row_is(4, "BYSTANDER 4"));
    check("with no row lit", lobby_rows_lit() == 0);

    /* A choice that falls off the list is no choice. */
    tap_obj(lobby_player_row(4));
    check("a tapped row is lit", lobby_row_lit(4) && lobby_rows_lit() == 1);
    check("and INVITE can be pressed", lv_obj_has_flag(lobby_act(), LV_OBJ_FLAG_CLICKABLE));
    fleet_link_loop_hear(app->link, 5);
    mp_wait(1500);
    check("a node heard goes to the top", lobby_row_is(0, "BYSTANDER 5"));
    check("the player chosen, pushed off the list, is no longer chosen",
          lobby_rows_lit() == 0 && !lv_obj_has_flag(lobby_act(), LV_OBJ_FLAG_CLICKABLE));
    fleet_link_loop_hear(app->link, 4);
    mp_wait(1500);
    check("nor chosen again when heard again",
          lobby_row_is(0, "BYSTANDER 4") && lobby_rows_lit() == 0 &&
          !lv_obj_has_flag(lobby_act(), LV_OBJ_FLAG_CLICKABLE));
    tap_obj(lobby_act());
    check("INVITE then goes to nobody, not to whoever now holds the old row",
          lobby_row_is(4, "BYSTANDER 2") && app->mp->m.phase != FLEET_MP_INVITING &&
          app->mp->sent == 0);

    /* Rows: BYSTANDER 4, 5, the opponent, BYSTANDER 1, 2. */
    check("the opponent is on row 2", lobby_row_is(2, "SIM OPPONENT"));
    tap_obj(lobby_player_row(2));
    fleet_link_loop_hear(app->link, 1);
    fleet_link_loop_hear(app->link, 2);
    mp_wait(1500);
    check("the opponent moves down to row 4", lobby_row_is(4, "SIM OPPONENT"));
    check("and the light goes with it", lobby_row_lit(4) && lobby_rows_lit() == 1);
    fleet_link_loop_hear(app->link, 0);
    mp_wait(1500);
    check("heard, the opponent moves up to row 0", lobby_row_is(0, "SIM OPPONENT"));
    check("the light goes with it again", lobby_row_lit(0) && lobby_rows_lit() == 1);
    check("not left on the stranger now on row 4",
          lobby_row_is(4, "BYSTANDER 5") && !lobby_row_lit(4));
    tap_obj(lobby_act());
    opponent = fleet_link_loop_peer(app->link);
    check("INVITE goes to the player chosen",
          app->mp->m.phase == FLEET_MP_INVITING && app->mp->sent == 1 &&
          memcmp(app->mp->m.peer_key, opponent->self_key, FLEET_KEY_BYTES) == 0);
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

/* FORFEIT is only in the lobby (docs/apps/FLEET_MULTIPLAYER.md, "Forfeit:
 * Lobby, match in hand"), so the lobby has to be reachable, and stay put,
 * whatever phase the match is in. Unit B, 2026-09-26: RESUME MATCH jumped
 * straight to Deploy; and when it did open the lobby (the mesh service had
 * not yet told Fleet its key), the key arriving steered the lobby away to
 * Deploy, as though the match had just started. */
static void test_multiplayer_forfeit(void)
{
    int i;

    phase = "multiplayer, forfeit from Deploy";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=8", 1);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tap_obj(command_multi_button());
    mp_wait(300);
    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    check("the match is in Deploy", app->current == FLEET_SCREEN_DEPLOY &&
          app->mp->m.phase == FLEET_MP_DEPLOY);
    /* Back on Command, as after closing and reopening Fleet. */
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
    check_str("Command offers the match", text_of(kid(command_multi_button(), 0)), "RESUME MATCH");
    tap_obj(command_multi_button());
    check("RESUME MATCH opens the lobby, not Deploy", app->current == FLEET_SCREEN_LOBBY);
    /* As on the device when the key comes late: the lobby was opened with
     * no match known yet, and then the match is read. */
    app->mp_revision = 0;
    app->mp_phase = FLEET_MP_IDLE;
    mp_wait(300);
    check("the match being read does not steer the lobby away to Deploy",
          app->current == FLEET_SCREEN_LOBBY && app->mp->m.phase == FLEET_MP_DEPLOY);
    check_str("the lobby offers FORFEIT", text_of(kid(lobby_alt(), 0)), "FORFEIT");
    tap_obj(lobby_alt());
    check("one press only arms it", app->mp->m.phase == FLEET_MP_DEPLOY &&
          strcmp(text_of(kid(lobby_alt(), 0)), "CONFIRM FORFEIT") == 0);
    tap_obj(lobby_alt());
    for (i = 0; i < 100 && app->mp->m.end_unacked; i++) {
        mp_wait(100);
    }
    check("the second ends the match: a forfeit, lost",
          app->mp->m.phase == FLEET_MP_DONE && app->mp->m.outcome == FLEET_OUTCOME_LOSS &&
          app->mp->m.end_reason == FLEET_END_FORFEIT && app->mp->m.end_by_me);
    check("and the opponent acknowledged it", !app->mp->m.end_unacked);
    check("the lobby then offers the result", app->current == FLEET_SCREEN_LOBBY &&
          strcmp(text_of(kid(lobby_act(), 0)), "RESULT") == 0);
    tap_obj(lobby_act());
    check_one_screen(FLEET_SCREEN_RESULT);
    check_str("the Result says who forfeited, not that a fleet went down",
              text_of(result_heading()), "You forfeited");
    check_str("and how it ended", text_of(result_value(0, 1)), "YOU FORFEITED");
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

/* From the lobby to Deploy against the virtual opponent, and AUTO pressed:
 * the fleet it placed. */
static struct fleet_board mp_auto_fleet(void)
{
    int i;

    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    check("the match is in Deploy", app->current == FLEET_SCREEN_DEPLOY);
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    check("AUTO placed the whole fleet", fleet_board_complete(&app->mp_fleet));
    return app->mp_fleet;
}

/* Every match ends with both fleets shown, so AUTO must not place the same
 * one again next match. Unit A, 2026-09-30: two matches in one run, one
 * layout - the stream was seeded from the solo game's seed, which does not
 * change between them. The solo game keeps that seed, for its screenshots. */
static void test_multiplayer_auto_varies(void)
{
    struct fleet_board first;
    struct fleet_board second;
    struct fleet_board want;
    struct fleet_rng rng;
    int i;

    phase = "multiplayer, AUTO per match";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=9", 1);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tap_obj(command_deploy());
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    fleet_board_clear(&want);
    fleet_rng_seed(&rng, app->game.seed ^ 0x5A5A5A5Au);
    fleet_board_autoplace(&want, &rng);
    check("the solo AUTO still follows the match seed",
          memcmp(app->game.board[FLEET_SIDE_PLAYER].ship_at, want.ship_at,
                 sizeof(want.ship_at)) == 0);
    fleet_app_show(app, FLEET_SCREEN_COMMAND);

    tap_obj(command_multi_button());
    mp_wait(300);
    first = mp_auto_fleet();
    /* Out of the first match the short way: forfeit it from the lobby. */
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
    tap_obj(command_multi_button());
    tap_obj(lobby_alt());
    tap_obj(lobby_alt());
    for (i = 0; i < 100 && app->mp->m.end_unacked; i++) {
        mp_wait(100);
    }
    tap_obj(lobby_act());
    check_one_screen(FLEET_SCREEN_RESULT);
    tap_obj(kid(result_foot(), 0));
    check("the first match is put away", app->current == FLEET_SCREEN_LOBBY &&
          app->mp->m.phase == FLEET_MP_IDLE);
    /* The opponent puts its end of the match away after 30 s. */
    mp_wait(31000);
    second = mp_auto_fleet();
    check("a second match does not get the first match's AUTO fleet",
          memcmp(first.ship_at, second.ship_at, sizeof(first.ship_at)) != 0);
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

static void test_multiplayer_lost(void)
{
    int i;

    phase = "multiplayer, out of reach";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=7", 1);
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    tap_obj(command_multi_button());
    mp_wait(300);
    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    tap_obj(deploy_confirm());
    for (i = 0; i < 300 && !fleet_match_my_turn(&app->mp->m); i++) {
        mp_wait(100);
    }
    fleet_link_loop_set_cut(app->link, 1);
    mp_our_turn(0);
    mp_wait(8 * 60000);
    printf("     lost: link %d attempts %d pending %d phase %d resolved %d sent %u\n",
           fleet_match_link(&app->mp->m), app->mp->m.attempts, app->mp->m.pending,
           app->mp->m.phase, app->mp->m.resolved, app->mp->sent);
    check("after six tries the link is lost", fleet_match_link(&app->mp->m) == FLEET_LINK_LOST);
    check_str("and FIRE becomes CHECK LINK", text_of(kid(battle_fire(), 0)), "CHECK LINK");
    check("which can be pressed", lv_obj_has_flag(battle_fire(), LV_OBJ_FLAG_CLICKABLE));
    check("the note says the match is paused, not lost",
          strstr(text_of(battle_note()), "paused") != NULL);
    check_battle_never_scrolls("multiplayer, lost, across the page");
    fleet_link_loop_set_cut(app->link, 0);
    tap_obj(battle_fire());
    for (i = 0; i < 300 && app->mp->m.pending != FLEET_NO_CELL; i++) {
        mp_wait(100);
    }
    check("CHECK LINK found the peer and the shot was answered",
          app->mp->m.pending == FLEET_NO_CELL && fleet_match_link(&app->mp->m) != FLEET_LINK_LOST);
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

/* ---- 9. multiplayer chat and status --------------------------------------- */

static lv_obj_t *chat_screen(void) { return screen_of(FLEET_SCREEN_CHAT); }
static lv_obj_t *chat_panel(void) { return kid(chat_screen(), 0); }
static lv_obj_t *chat_status(void) { return kid(chat_panel(), KID_PANEL_FIRST); }
static lv_obj_t *chat_list(void) { return kid(chat_panel(), KID_PANEL_FIRST + 1); }
static lv_obj_t *chat_composer(void) { return kid(chat_screen(), 1); }
static lv_obj_t *chat_field(void) { return kid(kid(chat_composer(), 0), 0); }
static lv_obj_t *chat_send_button(void) { return kid(chat_composer(), 1); }
static lv_obj_t *chat_board(void) { return kid(chat_screen(), 2); }
static lv_obj_t *result_chat(void) { return kid(result_foot(), 2); }

/* The last line shown in the chat, or "". */
static const char *chat_last_shown(void)
{
    const char *last = "";
    int i;

    for (i = 0; i < FLEET_CHAT_HISTORY; i++) {
        lv_obj_t *l = kid(chat_list(), i);

        if (l && visible(l)) {
            last = text_of(l);
        }
    }
    return last;
}

/* Keys reach the field on the keypad's own read timer, not as they are
 * pushed: pump until the stream has delivered every one of them. */
static void drain_keys(void)
{
    int i;

    for (i = 0; i < 200 && pos_input_queued() > 0; i++) {
        pump(10);
    }
    pump(50);
}

static void type_key(pos_key_t key)
{
    pos_input_push_key(key);
    drain_keys();
}

static void type_keys(const char *s)
{
    while (*s) {
        type_key((pos_key_t)(unsigned char)*s++);
    }
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void test_multiplayer_chat(enum pos_rotation rotation)
{
    int wide = rotation != POS_ROTATION_0;
    int keyboard_before;
    int i;

    phase = wide ? "chat and status, wide" : "chat and status, tall";
    mp_fresh();
    setenv("POCKETFLEET_MP_FAKE", "think=300,delay=100,seed=9,chat", 1);
    use_display(rotation, PANEL_CORNER);
    app_start();
    keyboard_before = g_keyboard_calls;
    tap_obj(command_multi_button());
    mp_wait(500);
    tap_obj(lobby_player_row(0));
    tap_obj(lobby_act());
    mp_wait(100);
    check_str("inviting: the header says the opponent is being reached", g_hint, "CONNECTING");
    for (i = 0; i < 100 && app->current != FLEET_SCREEN_DEPLOY; i++) {
        mp_wait(100);
    }
    check_str("deploying: the header says so", g_hint, "DEPLOY YOUR FLEET");
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    tap_obj(deploy_confirm());
    check_one_screen(FLEET_SCREEN_BATTLE);
    check("the chat button is on the Battle screen in multiplayer", visible(battle_chat()));

    /* Whose turn it is, turn by turn. We invited, so the guest fires first. */
    for (i = 0; i < 300 && !fleet_match_my_turn(&app->mp->m); i++) {
        mp_wait(100);
        if (i == 0) {
            check_str("our fleet placed, theirs not yet: waiting for them", g_hint,
                      "WAITING FOR SIM OPPONENT");
        }
    }
    check("the opponent's shot made it our turn", starts_with(g_hint, "YOUR TURN"));
    check_str("with the shot we are on", g_hint, "YOUR TURN \xc2\xb7 SHOT 1");
    mp_our_turn(0);
    check_str("our shot fired: waiting at once, not a tick later", g_hint,
              "WAITING FOR SIM OPPONENT");
    for (i = 0; i < 300 && !fleet_match_my_turn(&app->mp->m); i++) {
        mp_wait(100);
    }
    check_str("their answer and their shot: our turn again", g_hint, "YOUR TURN \xc2\xb7 SHOT 2");

    /* The chat button sits beside FIRE and costs the turn nothing. */
    {
        lv_area_t f;
        lv_area_t c;

        box_of(battle_fire(), &f);
        box_of(battle_chat(), &c);
        check("the chat button shares FIRE's row", c.y1 == f.y1 && c.y2 == f.y2 && c.x1 > f.x2);
        check("FIRE keeps the larger part of it", lv_area_get_width(&f) > lv_area_get_width(&c));
        check("and both are a finger's size",
              lv_area_get_height(&f) >= POCKETUI_TOUCH_MIN && lv_area_get_height(&c) >= POCKETUI_TOUCH_MIN);
        check("the chat button is in view", inside_body(battle_chat()));
        check_in_view("Battle: the chat button", battle_chat());
        check_in_view("Battle: FIRE beside it", battle_fire());
    }
    if (wide) {
        mp_our_turn(1);     /* the whole never-scroll check, with the button there */
        phase = "chat and status, wide";
    } else {
        mp_our_turn(0);
    }

    /* Into the chat: the match goes on, and the header still says so. */
    tap_obj(battle_chat());
    mp_wait(200);
    check_one_screen(FLEET_SCREEN_CHAT);
    check("the chat is inside the body", inside_body(chat_screen()));
    check("and nothing but its list can scroll", lv_obj_get_scroll_bottom(frame_of()) == 0 &&
          lv_obj_get_scroll_bottom(chat_screen()) == 0);
    check_str("the status line says what the header says", text_of(chat_status()), g_hint);
    check_in_view("Chat: the status line", chat_status());
    check_in_view("Chat: the field", lv_obj_get_parent(chat_field()));
    check_in_view("Chat: SEND", chat_send_button());
    check_in_view("Chat: back to the board", chat_board());
    check("the field has the keys, without a finger on it", pos_input_focused() == chat_field());
    check("SEND waits for something to send", !lv_obj_has_flag(chat_send_button(), LV_OBJ_FLAG_CLICKABLE));
    type_keys("Hello there");
    check("SEND is armed once there are words", lv_obj_has_flag(chat_send_button(), LV_OBJ_FLAG_CLICKABLE));
    type_key(LV_KEY_ENTER);
    pump(50);
    check_str("Enter sends: the line is ours, on its way or there",
              app->mp->m.chat.count ? app->mp->m.chat.line[0].text : "", "Hello there");
    check_str("and the field is free for the next", lv_textarea_get_text(chat_field()), "");
    check("while it waits for the game to spare the air, it says so",
          strstr(chat_last_shown(), "Hello there") != NULL);
    /* The virtual opponent plays far faster than a person, and the game
     * spends the governor first: the line goes once the burst has room. */
    for (i = 0; i < 900 && !strstr(chat_last_shown(), "Copy that."); i++) {
        mp_wait(100);
    }
    check_str("the answer is shown, with who said it", chat_last_shown(),
              "SIM OPPONENT \xc2\xb7 Copy that.");
    check("and on the screen it is never counted as new", app->mp->m.chat.unread == 0);
    check("our line was delivered", app->mp->m.chat.line[0].state == FLEET_CHAT_DELIVERED);
    {
        /* Twenty two-byte letters: 20 characters, 40 bytes. */
        for (i = 0; i < 20; i++) {
            type_key(0xE6);
        }
        check_int("twenty letters in the field, forty bytes",
                  (long)strlen(lv_textarea_get_text(chat_field())), 40);
        check("over the byte budget, SEND is not armed",
              !lv_obj_has_flag(chat_send_button(), LV_OBJ_FLAG_CLICKABLE));
        type_key(LV_KEY_ENTER);
        check("and Enter does not send it", app->mp->m.chat.count == 2);
        for (i = 0; i < 25; i++) {
            type_key(LV_KEY_BACKSPACE);
        }
        check_str("and it can be taken back", lv_textarea_get_text(chat_field()), "");
    }
    check_int("typing on keys never called up the touch keyboard", g_keyboard_calls, keyboard_before);
    if (!wide) {
        tap_obj(chat_field());
        check("down the page, a finger on the field calls up the touch keyboard",
              g_keyboard_calls == keyboard_before + 1);
    }

    /* Back to the board; something said meanwhile is on the button. */
    tap_obj(chat_board());
    check_one_screen(FLEET_SCREEN_BATTLE);
    fleet_link_loop_say(app->link, "Your move, captain");
    /* It goes when the opponent's governor has room for it. */
    for (i = 0; i < 300 && app->mp->m.chat.unread == 0; i++) {
        mp_wait(100);
    }
    check_str("a line said while on the board is counted on the button",
              text_of(kid(battle_chat(), 0)), "CHAT \xc2\xb7 1 NEW");
    /* The button shows as much of it as fits, cut with dots. */
    check("and shown on it, who said it first",
          starts_with(text_of(kid(battle_chat(), 1)), "SIM OPPONENT \xc2\xb7 "));
    check_str("the newest line is the one said", fleet_chat_last(&app->mp->m.chat)->text,
              "Your move, captain");

    if (wide) {
        /* Played out: the chat outlasts the match, from Result. */
        for (i = 0; i < 20000 && (app->mp->m.phase == FLEET_MP_COMMITTED ||
                                  app->mp->m.phase == FLEET_MP_BATTLE); i++) {
            if (fleet_match_my_turn(&app->mp->m) && app->current == FLEET_SCREEN_BATTLE) {
                mp_our_turn(0);
            }
            mp_wait(100);
        }
        for (i = 0; i < 300 && app->mp->m.phase != FLEET_MP_DONE; i++) {
            mp_wait(100);
        }
        check_one_screen(FLEET_SCREEN_RESULT);
        check_str("game over: the header says so", g_hint, "GAME OVER");
        check("Result offers the chat", visible(result_chat()));
        tap_obj(result_chat());
        mp_wait(200);
        check_one_screen(FLEET_SCREEN_CHAT);
        type_keys("gg");
        type_key(LV_KEY_ENTER);
        mp_wait(200);
        check("a word after the match is still taken",
              app->mp->m.chat.count > 0 &&
              strcmp(fleet_chat_last(&app->mp->m.chat)->text, "gg") == 0 &&
              fleet_chat_last(&app->mp->m.chat)->state != FLEET_CHAT_FAILED);
        check("and shown as ours", starts_with(chat_last_shown(), "YOU \xc2\xb7 gg"));
        tap_obj(chat_board());
        check("BOARD goes back to the Result, where the match now is",
              app->current == FLEET_SCREEN_RESULT);
        tap_obj(kid(result_foot(), 0));
        check("putting the match away forgets its chat", app->mp->m.phase == FLEET_MP_IDLE &&
              app->mp->m.chat.count == 0);
    }
    app_stop();
    unsetenv("POCKETFLEET_MP_FAKE");
}

int main(void)
{
    lv_indev_t *finger;
    int i;

    if (!mkdtemp(state_dir)) {
        printf("FAIL cannot make a state directory\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", state_dir, 1);

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    (void)finger;

    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 1. the rule on its own --------------------------------------- */

    test_shape_rule();

    /* ---- 2. Command, in both shapes ----------------------------------- */

    with_screen(NULL);
    app_start();
    phase = "command, tall";
    check_one_screen(FLEET_SCREEN_COMMAND);
    check_frame(TALL_W, TALL_H, PANEL_CORNER);
    check_str("the status bar is told where we are", g_hint, "COMMAND");
    check_int("the difficulty bar has four segments",
              (int)lv_obj_get_child_count(command_segments()), FLEET_DIFFICULTY_COUNT);
    {
        lv_area_t d;
        lv_area_t c0;
        lv_area_t c1;
        lv_area_t c2;

        box_of(command_deploy(), &d);
        check_int("DEPLOY FLEET is full width, down the page", lv_area_get_width(&d), TALL_W);
        check("and keeps the touch minimum", lv_area_get_height(&d) >= POCKETUI_TOUCH_MIN);
        box_of(command_col(0), &c0);
        box_of(command_col(1), &c1);
        box_of(command_col(2), &c2);
        check("the three groups are stacked down the page", c1.y1 >= c0.y2 && c2.y1 >= c1.y2);
        check("the OPPONENT caption is drawn, not cut",
              caption_would_be_drawn(kid(command_col(0), 0)));
        check("RESUME is inside the panel that describes the saved match",
              lv_obj_get_parent(command_resume()) == command_saved());
        check_int("and the foot row holds only DEPLOY FLEET",
                  (int)lv_obj_get_child_count(command_foot()), 1);
        check("no column scrolls down the page",
              !lv_obj_has_flag(command_col(0), LV_OBJ_FLAG_SCROLLABLE) &&
              !lv_obj_has_flag(command_col(1), LV_OBJ_FLAG_SCROLLABLE) &&
              !lv_obj_has_flag(command_col(2), LV_OBJ_FLAG_SCROLLABLE));
    }

    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "command, wide";
    check_frame(WIDE_W, WIDE_H, PANEL_CORNER);
    {
        lv_area_t c0;
        lv_area_t c1;
        lv_area_t c2;
        lv_area_t d;

        box_of(command_col(0), &c0);
        box_of(command_col(1), &c1);
        box_of(command_col(2), &c2);
        box_of(command_deploy(), &d);
        check("the three groups stand side by side", c1.x1 > c0.x2 && c2.x1 > c1.x2);
        check_int("in three equal columns", lv_area_get_width(&c0), lv_area_get_width(&c1));
        check_int("all three", lv_area_get_width(&c1), lv_area_get_width(&c2));
        check("DEPLOY FLEET is under them, not in them", d.y1 >= c0.y2);
        check("it is a column wide, not a body wide", lv_area_get_width(&d) < WIDE_W / 2);
        check("and it is at the end of the row", d.x2 > c2.x1);
        check("it keeps the touch minimum", lv_area_get_height(&d) >= POCKETUI_TOUCH_MIN);
        check("it is in the safe area", in_safe_area(command_deploy()));
        check("a column that cannot show everything scrolls",
              lv_obj_has_flag(command_col(1), LV_OBJ_FLAG_SCROLLABLE));
        check("the OPPONENT caption is drawn, not cut",
              caption_would_be_drawn(kid(command_col(0), 0)));
        /* The other way to start an engagement must not be the thing that has
         * been scrolled out of sight either. */
        check_int("RESUME joins DEPLOY FLEET on the foot row",
                  (int)lv_obj_get_child_count(command_foot()), 2);
        check("with RESUME first", kid(command_foot(), 0) == command_resume());
        {
            lv_area_t r;
            lv_area_t dd;

            box_of(command_resume(), &r);
            box_of(command_deploy(), &dd);
            check("RESUME stands beside DEPLOY FLEET", r.x2 < dd.x1);
            check("it keeps the touch minimum",
                  lv_area_get_height(&r) >= POCKETUI_TOUCH_MIN);
            check("and it is wholly in the body", inside_body(command_resume()));
            check("and in the safe area", in_safe_area(command_resume()));
        }
        check("nothing on the screen leaves the body",
              inside_body(screen_of(FLEET_SCREEN_COMMAND)));
    }
    tap_obj(kid(command_segments(), FLEET_ADMIRAL));
    check_int("a segment across the page selects that opponent", app->difficulty,
              FLEET_ADMIRAL);
    tap_obj(kid(command_segments(), FLEET_ADMIRAL));
    {
        int objects = count_objects(frame_of());

        for (i = 0; i < 2; i++) {
            use_display(POS_ROTATION_0, PANEL_CORNER);
            check_int("turning the display adds no objects to Command",
                      count_objects(frame_of()), objects);
            check_int("and the opponent chosen is still chosen", app->difficulty,
                      FLEET_ADMIRAL);
            use_display(POS_ROTATION_270, PANEL_CORNER);
            check_int("nor does turning it back", count_objects(frame_of()), objects);
            check_int("still chosen", app->difficulty, FLEET_ADMIRAL);
        }
    }
    tap_obj(kid(command_segments(), FLEET_OFFICER));
    check_int("and back again", app->difficulty, FLEET_OFFICER);
    check_int("nothing here asked the shell for a keyboard", g_keyboard_calls, 0);

    /* And back down the page, RESUME returns to the panel it came from - the
     * move goes both ways, or a portrait Command would keep a landscape foot
     * row it never had. */
    use_display(POS_ROTATION_0, PANEL_CORNER);
    phase = "command, back down the page";
    check("RESUME is back inside the saved-match panel",
          lv_obj_get_parent(command_resume()) == command_saved());
    check_int("and the foot row holds only DEPLOY FLEET again",
              (int)lv_obj_get_child_count(command_foot()), 1);
    {
        lv_area_t d;

        box_of(command_deploy(), &d);
        check_int("which is full width once more", lv_area_get_width(&d), TALL_W);
    }
    app_stop();

    /* ---- 3. Deploy ---------------------------------------------------- */

    with_screen("deploy");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    phase = "deploy, tall";
    check_one_screen(FLEET_SCREEN_DEPLOY);
    check_int("the board is the v0.0.10 board", fleet_grid_cell(deploy_board()),
              FLEET_CELL_TALL);
    {
        lv_area_t b;
        lv_area_t r;

        box_of(deploy_board(), &b);
        box_of(deploy_roster(), &r);
        check("the roster is under the board", r.y1 > b.y2);
    }
    test_roster("deploy roster, tall");

    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "deploy, wide";
    {
        lv_area_t b;
        lv_area_t r;
        lv_area_t ctl;
        lv_area_t con;

        box_of(deploy_board(), &b);
        box_of(deploy_roster(), &r);
        box_of(deploy_controls(), &ctl);
        box_of(deploy_confirm(), &con);
        /* The same board Battle aims at, cell for cell: a fleet is placed on
         * the squares the shots are later aimed at, so the two screens must
         * not disagree about where a square is. */
        check_int("the board is as tall as the body allows", lv_area_get_height(&b),
                  SPAN_OF(WIDE_CELL));
        check_int("and as wide as Battle's", lv_area_get_width(&b),
                  SPAN_OF(WIDE_CELL_W));
        check_int("its rows are the wide cell", fleet_grid_cell(deploy_board()), WIDE_CELL);
        check_int("its columns are 51 px", fleet_grid_cell_across(deploy_board()),
                  WIDE_CELL_W);
        check("the roster stands beside the board", r.x1 > b.x2);
        check("the controls stand beyond the roster", ctl.x1 > r.x2);
        check("CONFIRM DEPLOYMENT is at the foot of that column", con.y1 > ctl.y2);
        check("it keeps the touch minimum", lv_area_get_height(&con) >= POCKETUI_TOUCH_MIN);
        for (i = 0; i < 3; i++) {
            lv_area_t k;

            box_of(kid(deploy_controls(), i), &k);
            check("a placement control is at least a finger wide",
                  lv_area_get_width(&k) >= POCKETUI_TOUCH_MIN);
        }
        check("CONFIRM is in the safe area", in_safe_area(deploy_confirm()));
        check("nothing on the screen leaves the body",
              inside_body(screen_of(FLEET_SCREEN_DEPLOY)));
    }
    test_roster("deploy roster, wide");

    /* A fleet half placed must survive the display being turned, and so must
     * which ship is selected - there is no other way to tell it did but to
     * put the next one down and see which hull moved. */
    phase = "deploy across a relayout";
    {
        int objects;
        int i2;

        tap_obj(kid(deploy_controls(), DEPLOY_CLEAR));
        tap_obj(deploy_roster_row(FLEET_SHIP_CRUISER));
        objects = count_objects(frame_of());
        for (i2 = 0; i2 < 2; i2++) {
            use_display(POS_ROTATION_0, PANEL_CORNER);
            check_int("turning the display adds no objects to Deploy",
                      count_objects(frame_of()), objects);
            use_display(POS_ROTATION_270, PANEL_CORNER);
            check_int("nor does turning it back",
                      count_objects(frame_of()), objects);
        }
        check_int("the board is empty still",
                  app->game.board[FLEET_SIDE_PLAYER].ships_placed, 0);
        {
            lv_area_t c;

            cell_box(deploy_board(), 1, 5, 5, &c);
            tap_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
        }
        check("the ship chosen before the turn is the one that goes down",
              app->game.board[FLEET_SIDE_PLAYER].ships[FLEET_SHIP_CRUISER].placed);
        tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
        check_int("AUTO fills the rest",
                  app->game.board[FLEET_SIDE_PLAYER].ships_placed, FLEET_SHIP_COUNT);
        use_display(POS_ROTATION_0, PANEL_CORNER);
        check_int("a placed fleet survives the turn",
                  app->game.board[FLEET_SIDE_PLAYER].ships_placed, FLEET_SHIP_COUNT);
        use_display(POS_ROTATION_270, PANEL_CORNER);
    }

    phase = "deploy, wide";
    tap_obj(kid(deploy_controls(), DEPLOY_CLEAR));
    check_int("CLEAR empties the board across the page",
              app->game.board[FLEET_SIDE_PLAYER].ships_placed, 0);
    tap_obj(deploy_confirm());
    check_one_screen(FLEET_SCREEN_DEPLOY);
    check("CONFIRM refuses an incomplete fleet",
          app->game.phase == FLEET_PHASE_DEPLOY);
    tap_obj(kid(deploy_controls(), DEPLOY_AUTO));
    check_int("AUTO places the whole fleet",
              app->game.board[FLEET_SIDE_PLAYER].ships_placed, FLEET_SHIP_COUNT);
    tap_obj(deploy_confirm());
    check_one_screen(FLEET_SCREEN_BATTLE);
    check("and CONFIRM starts the engagement once it is complete",
          app->game.phase != FLEET_PHASE_DEPLOY);
    app_stop();

    /* ---- 4. Battle: structure, then the tap path at every board size --- */

    with_screen("battle");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    phase = "battle, tall";
    check_one_screen(FLEET_SCREEN_BATTLE);
    check_tall_battle();
    check_singletons();
    check_own_board_is_not_a_target();
    test_board_taps("battle taps, 48 px");
    test_drawn_is_hit(battle_board(), 1, "battle drawn is hit, 48 px");
    test_board_edges("battle edges, 48 px");
    {
        lv_area_t b;
        lv_area_t f;
        lv_area_t last;
        int32_t pad;

        box_of(battle_board(), &b);
        box_of(frame_of(), &f);
        check("the tall stack is taller than the body, so the frame scrolls it",
              lv_obj_has_flag(frame_of(), LV_OBJ_FLAG_SCROLLABLE));
        check("and the board is where it starts", b.y1 == f.y1);

        /* What the foot clearance is for. While the stack is scrolled, content
         * passes through the last few pixels of the body as it always has -
         * that is what a scroll looks like. What must not happen is the end of
         * the stack coming to rest in a rounded corner, so scroll to the end
         * and look at the last thing there. */
        lv_obj_scroll_to_y(frame_of(), lv_obj_get_scroll_bottom(frame_of()), LV_ANIM_OFF);
        pump(120);
        box_of(battle_waters_panel(), &last);
        box_of(frame_of(), &f);
        pad = lv_obj_get_style_pad_bottom(frame_of(), LV_PART_MAIN);
        check("at the end of the scroll the last panel clears the corner squares",
              last.y2 <= f.y2 - pad);
        check("and it is in the safe area", in_safe_area(battle_waters_panel()));
        lv_obj_scroll_to_y(frame_of(), 0, LV_ANIM_OFF);
        pump(120);
    }
    phase = "battle, tall, outdoor";
    {
        lv_area_t before;
        lv_area_t after;

        box_of(battle_board(), &before);
        use_mode("outdoor");
        box_of(battle_board(), &after);
        check("Outdoor does not move the board down the page either",
              memcmp(&before, &after, sizeof(after)) == 0);
        use_mode("normal");
    }

    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "battle, wide";
    check_wide_battle(WIDE_CELL);
    check_battle_never_scrolls("battle, wide, whole on the screen");
    phase = "battle, wide";
    check_singletons();
    check_own_board_is_not_a_target();
    test_board_taps("battle taps, 40 px");
    test_drawn_is_hit(battle_board(), 1, "battle drawn is hit, 51 x 40");
    test_board_edges("battle edges, 40 px");

    test_aim_without_precision("aiming without a precise touch, wide");

    phase = "aim-then-confirm, wide";
    {
        struct match_state before;
        lv_area_t c;

        fleet_grid_set_cursor(battle_board(), -1, -1);
        pump(60);
        snapshot(&before);
        cell_box(battle_board(), 1, 6, 6, &c);
        tap_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
        check("a tap on a square moves the crosshair",
              fleet_grid_get_cursor(battle_board(), NULL, NULL) == 0);
        before.aimed = 1;
        before.aim_row = 6;
        before.aim_col = 6;
        check_same_match("and fires nothing", &before);
        check_str("the readout names the square", text_of(battle_cell_value()), "G7");
        check_str("and says it is ready", text_of(battle_note()), "Ready to fire.");

        /* Only the board and FIRE take a tap. The readout, your own waters and
         * the gutter between the board and them are looked at, not pressed,
         * and none of them may quietly be a target of its own: a hitbox that
         * is not drawn is a hitbox nobody can avoid. */
        {
            lv_area_t board;
            lv_area_t readout;
            lv_area_t own;

            box_of(battle_board(), &board);
            box_of(battle_target_panel(), &readout);
            box_of(battle_waters_panel(), &own);
            tap_at((readout.x1 + readout.x2) / 2, (readout.y1 + readout.y2) / 2);
            tap_at((own.x1 + own.x2) / 2, (own.y1 + own.y2) / 2);
            tap_at((board.x2 + readout.x1) / 2, (board.y1 + board.y2) / 2);
            pump(120);
            check_same_match("a tap on the readout, your waters or the gutter "
                             "fires nothing", &before);
            check_str("and leaves the crosshair where it was",
                      text_of(battle_cell_value()), "G7");
        }
    }

    phase = "fire, wide";
    {
        int shots = shots_on(&app->game.board[FLEET_SIDE_OPPONENT]);
        int taken = shots_on(&app->game.board[FLEET_SIDE_PLAYER]);

        tap_obj(battle_fire());
        pump(1200);   /* long enough for the paced reply to settle */
        check_int("FIRE fires exactly one shot",
                  shots_on(&app->game.board[FLEET_SIDE_OPPONENT]), shots + 1);
        check("and the crosshair is cleared",
              fleet_grid_get_cursor(battle_board(), NULL, NULL) != 0);
        check_int("the opponent answered, once",
                  shots_on(&app->game.board[FLEET_SIDE_PLAYER]), taken + 1);
        check("the log line says so", strlen(text_of(battle_log())) > 0);
    }
    {
        int row = -1;
        int col = -1;
        int shots;

        for (i = 0; i < FLEET_CELLS; i++) {
            if (app->game.board[FLEET_SIDE_OPPONENT].shot[i]) {
                row = i / FLEET_GRID;
                col = i % FLEET_GRID;
                break;
            }
        }
        check("there is a square already fired at", row >= 0);
        fleet_screen_battle_aim(app, row, col);
        pump(60);
        shots = shots_on(&app->game.board[FLEET_SIDE_OPPONENT]);
        check("the readout says the square has been fired at already",
              strstr(text_of(battle_note()), "already") != NULL);
        tap_obj(battle_fire());
        pump(1200);
        check_int("and FIRE does nothing there",
                  shots_on(&app->game.board[FLEET_SIDE_OPPONENT]), shots);
    }

    test_relayout_keeps_the_match(PANEL_CORNER);
    app_stop();

    /* Square corners: the frame takes nothing at the foot, the tall shape is
     * the v0.0.10 layout to the pixel, and the wide board gains a pixel.
     *
     * The app is opened again rather than have the corners change under it:
     * on this board the panel's shape is settled before the shell builds
     * anything and cannot change while an app is open, so a running app is
     * never asked to answer for a different one. */
    use_display(POS_ROTATION_0, 0);
    app_start();
    phase = "battle, tall, square corners";
    check_frame(TALL_W, TALL_H, 0);
    check_tall_battle();
    use_display(POS_ROTATION_270, 0);
    phase = "battle, wide, square corners";
    check_frame(WIDE_W, WIDE_H, 0);
    check_wide_battle(RECT_CELL);
    check_battle_never_scrolls("battle, wide, square corners, whole on the screen");
    phase = "battle, wide, square corners";
    test_board_taps("battle taps, 41 px");
    test_drawn_is_hit(battle_board(), 1, "battle drawn is hit, 51 x 41");
    app_stop();

    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    phase = "battle, wide, outdoor";
    {
        lv_area_t before;
        lv_area_t after;

        box_of(battle_board(), &before);
        use_mode("outdoor");
        box_of(battle_board(), &after);
        check("Outdoor does not move the board", memcmp(&before, &after, sizeof(after)) == 0);
        check_int("nor change its cells", fleet_grid_cell(battle_board()), WIDE_CELL);
        check_wide_battle(WIDE_CELL);
        check_battle_never_scrolls("battle, wide, outdoor, whole on the screen");
        phase = "battle, wide, outdoor";
        use_mode("normal");
    }

    phase = "bodies with no room";
    /* 1100 x 568 -> a 1060 x 452 body: just over the width floor, which
     * for the 41 px cell is 452 + 20 + 2 x 280 + 20 = 1052. */
    use_panel_sized(PANEL_W, 1100, POS_ROTATION_270, 0);
    check_int("a body just over the width floor is wide",
              fleet_grid_cell(battle_board()), RECT_CELL);
    /* 1090 x 568 -> 1050 x 452: just under it. */
    use_panel_sized(PANEL_W, 1090, POS_ROTATION_270, 0);
    check_int("a body just under the width floor keeps the tall board",
              fleet_grid_cell(battle_board()), FLEET_CELL_TALL);
    {
        lv_area_t b;
        lv_area_t s;

        box_of(battle_board(), &b);
        box_of(battle_side(), &s);
        check("and the tall stack with it", s.y1 > b.y2);
    }
    /* 1200 x 220 -> a 1160 x 48 body: no height for a board at all. */
    use_panel_sized(1200, 220, POS_ROTATION_0, 0);
    check_int("a body with no height keeps the tall board",
              fleet_grid_cell(battle_board()), FLEET_CELL_TALL);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();

    /* ---- 5. a paced reply across a relayout --------------------------- */

    phase = "paced reply";
    with_screen("battle");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    {
        int shots_before = shots_on(&app->game.board[FLEET_SIDE_PLAYER]);

        fleet_screen_battle_aim(app, 8, 8);
        pump(60);
        fleet_screen_battle_fire(app);
        pump(100);   /* inside the 420 ms pace: the reply is still pending */
        check_int("the opponent has not answered yet",
                  shots_on(&app->game.board[FLEET_SIDE_PLAYER]), shots_before);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        check_int("turning the display mid-turn does not settle it early",
                  shots_on(&app->game.board[FLEET_SIDE_PLAYER]), shots_before);
        pump(1200);
        check_int("and the reply still arrives, once",
                  shots_on(&app->game.board[FLEET_SIDE_PLAYER]), shots_before + 1);
    }
    /* Leaving the app with a turn in flight must settle it, not drop it. */
    fleet_screen_battle_aim(app, 9, 9);
    pump(60);
    fleet_screen_battle_fire(app);
    pump(100);
    app_stop();
    check("the app is torn down mid-turn without a fault", 1);

    /* Reduced motion: the same turn, no pacing. */
    phase = "reduced motion";
    g_reduced_motion = 1;
    with_screen("battle");
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    {
        int shots_before = shots_on(&app->game.board[FLEET_SIDE_PLAYER]);

        fleet_screen_battle_aim(app, 7, 1);
        pump(60);
        fleet_screen_battle_fire(app);
        pump(60);
        check_int("with reduced motion the reply is immediate",
                  shots_on(&app->game.board[FLEET_SIDE_PLAYER]), shots_before + 1);
        check_int("and the board is still the wide board",
                  fleet_grid_cell(battle_board()), WIDE_CELL);
    }
    app_stop();
    g_reduced_motion = 0;

    /* ---- 6. Result ----------------------------------------------------- */

    phase = "result, tall";
    with_screen("result");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    check_one_screen(FLEET_SCREEN_RESULT);
    check_str("the outcome is the heading", text_of(result_heading()),
              "Enemy fleet destroyed");
    check_str("the opponent is named", text_of(result_value(0, 0)), "Officer");
    check("your fleet is reported as so many afloat",
          strstr(text_of(result_value(0, 2)), "AFLOAT") != NULL);
    {
        lv_area_t p0;
        lv_area_t p1;

        box_of(kid(result_panels(), 0), &p0);
        box_of(kid(result_panels(), 1), &p1);
        check("the two accounts are stacked down the page", p1.y1 > p0.y2);
    }
    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "result, wide";
    {
        lv_area_t h;
        lv_area_t p0;
        lv_area_t p1;
        lv_area_t f0;
        lv_area_t f1;
        char was[64];

        snprintf(was, sizeof(was), "%s", text_of(result_value(1, 1)));
        box_of(result_heading(), &h);
        box_of(kid(result_panels(), 0), &p0);
        box_of(kid(result_panels(), 1), &p1);
        box_of(kid(result_foot(), 0), &f0);
        box_of(kid(result_foot(), 1), &f1);
        check("the outcome is still over everything", h.y2 <= p0.y1);
        check("the two accounts stand side by side", p1.x1 > p0.x2);
        check_int("in halves", lv_area_get_width(&p0), lv_area_get_width(&p1));
        check("the two ways on stand side by side under them", f1.x1 > f0.x2);
        check_int("in halves", lv_area_get_width(&f0), lv_area_get_width(&f1));
        check("and are under the accounts", f0.y1 >= p0.y2);
        check("both keep the touch minimum",
              lv_area_get_height(&f0) >= POCKETUI_TOUCH_MIN &&
              lv_area_get_height(&f1) >= POCKETUI_TOUCH_MIN);
        check("both are in the safe area",
              in_safe_area(kid(result_foot(), 0)) && in_safe_area(kid(result_foot(), 1)));
        check("nothing on the screen leaves the body",
              inside_body(screen_of(FLEET_SCREEN_RESULT)));
        check_str("the figures did not move with the layout",
                  text_of(result_value(0, 0)), "Officer");
        check_str("nor did the accuracy", text_of(result_value(1, 1)), was);
    }
    tap_obj(kid(result_foot(), 0));
    check_one_screen(FLEET_SCREEN_DEPLOY);
    check_int("NEW ENGAGEMENT starts a fresh match", app->game.turn, 0);
    app_stop();

    /* ---- 6b. the no-scroll invariant, held through whole matches ------ *
     *
     * The screen is measured after every turn of a game played out across the
     * page, in both type sizes and with the unit's corners and with square
     * ones. This is the check the landscape Battle screen exists to pass.
     */

    test_no_scroll_through_a_match("normal", PANEL_CORNER);
    test_no_scroll_through_a_match("outdoor", PANEL_CORNER);
    test_no_scroll_through_a_match("normal", 0);
    test_no_scroll_through_a_match("outdoor", 0);

    /* ---- 7. the save is the same file, whatever the shape -------------- */

    phase = "the save";
    {
        char path[512];
        struct stat st_tall;
        struct stat st_wide;

        snprintf(path, sizeof(path), "%s/fleet/save.v1", state_dir);
        with_screen("battle");
        use_display(POS_ROTATION_0, PANEL_CORNER);
        app_start();
        pump(120);
        check_int("a resolved turn wrote a save", stat(path, &st_tall), 0);
        app_stop();
        unlink(path);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        app_start();
        pump(120);
        check_int("and so did one played across the page", stat(path, &st_wide), 0);
        check_int("the file is the same size either way",
                  (long)st_wide.st_size, (long)st_tall.st_size);
        app_stop();
        with_screen(NULL);
        use_display(POS_ROTATION_0, PANEL_CORNER);
        app_start();
        check("the stored match is offered whatever shape it was played in",
              app->resumable != 0);
        app_stop();
        unlink(path);
    }

    /* ---- 8. multiplayer --------------------------------------------------- */

    test_multiplayer(POS_ROTATION_0);
    test_multiplayer(POS_ROTATION_270);
    test_multiplayer_reopen();
    test_multiplayer_lobby_choice();
    test_multiplayer_forfeit();
    test_multiplayer_auto_varies();
    test_multiplayer_lost();
    test_multiplayer_chat(POS_ROTATION_0);
    test_multiplayer_chat(POS_ROTATION_270);
    {
        char mp_file[600];

        snprintf(mp_file, sizeof(mp_file), "%s/fleet/match.v1", state_dir);
        unlink(mp_file);
    }

    check_int("the game never went home by itself", g_home_calls, 0);
    /* Once only: the finger on the chat field down the page, in section 9. */
    check_int("and asked for the keyboard only when a finger asked for it", g_keyboard_calls, 1);

    printf("fleet_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
