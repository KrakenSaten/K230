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
 * else, because the wide shape draws a 34 px cell (DS 28.2) where the tall
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
 *     corners, at 48 px, 35 px and 34 px; the gaps between cells; the caption
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
#define STATUS_H POCKETUI_STATUS_BAR_H
/* Above the body and below it: the status bar, the app header and the body's
 * own top and foot padding (DS 7) - and how far the unit's 30 px corners
 * reach above the body's foot. */
#define BODY_CHROME_H (STATUS_H + POCKETUI_HEADER_H + POCKETUI_BODY_PAD_TOP + POCKETUI_PAD)
#define CORNER_REACH 10

/* What the layout promises, written down here rather than read from the app. */
#define TALL_W (PANEL_W - 2 * POCKETUI_PAD)      /* 528 */
#define TALL_H (PANEL_H - BODY_CHROME_H)         /* 1060 */
#define WIDE_W (PANEL_H - 2 * POCKETUI_PAD)      /* 1192 */
#define WIDE_H (PANEL_W - BODY_CHROME_H)         /* 396 */
#define WIDE_CELL 34                             /* 396 less the 10 px foot */
#define RECT_CELL 35                             /* a panel with square corners */
#define SPAN_OF(cell) (FLEET_GRID_GUTTER + FLEET_GRID * (cell) + (FLEET_GRID - 1) * FLEET_GRID_GAP)

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
static lv_obj_t *battle_act(void) { return kid(battle_side(), 0); }
static lv_obj_t *battle_waters(void) { return kid(battle_side(), 1); }
static lv_obj_t *battle_target_panel(void) { return kid(battle_act(), 0); }
static lv_obj_t *battle_fire(void) { return kid(battle_act(), 1); }
static lv_obj_t *battle_waters_panel(void) { return kid(battle_waters(), 0); }
static lv_obj_t *battle_own(void) { return kid(battle_waters_panel(), KID_PANEL_FIRST); }
static lv_obj_t *battle_cell_value(void)
{
    return kid(kid(battle_target_panel(), KID_PANEL_FIRST), 0);
}
static lv_obj_t *battle_note(void) { return kid(battle_target_panel(), KID_PANEL_FIRST + 1); }
static lv_obj_t *battle_log(void) { return kid(battle_waters_panel(), KID_PANEL_FIRST + 1); }

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
static lv_obj_t *command_deploy(void) { return kid(kid(screen_of(FLEET_SCREEN_COMMAND), 1), 0); }
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

/* ---- the board, as the grid draws it ----------------------------------- */

/* fleet_grid.c's cell_area(), restated here so the test does not ask the code
 * under test where a cell is. */
static void cell_box(lv_obj_t *grid, int labels, int row, int col, lv_area_t *out)
{
    lv_area_t g;
    int cell = fleet_grid_cell(grid);
    int gutter = labels ? FLEET_GRID_GUTTER : 0;

    box_of(grid, &g);
    out->x1 = g.x1 + gutter + col * (cell + FLEET_GRID_GAP);
    out->y1 = g.y1 + gutter + row * (cell + FLEET_GRID_GAP);
    out->x2 = out->x1 + cell - 1;
    out->y2 = out->y1 + cell - 1;
}

/* ---- the shape rule, as arithmetic ------------------------------------- */

static void test_shape_rule(void)
{
    int cell = 0;
    int32_t h_at_floor = FLEET_GRID_GUTTER + (FLEET_GRID - 1) * FLEET_GRID_GAP +
                         FLEET_GRID * FLEET_CELL_MIN;
    int32_t floor_w;

    phase = "shape rule";
    check_int("a 386 px body gives a 34 px cell", fleet_cell_for_height(386), WIDE_CELL);
    check_int("a 396 px body gives a 35 px cell", fleet_cell_for_height(396), RECT_CELL);
    check_int("a tall body is capped at the tall cell",
              fleet_cell_for_height(TALL_H), FLEET_CELL_TALL);
    check_int("a body with no room at all gives nothing", fleet_cell_for_height(10), 0);
    check_int("the finest board allowed is the floor",
              fleet_cell_for_height(h_at_floor), FLEET_CELL_MIN);
    check_int("the labelled span at 34 px", fleet_grid_span_for(WIDE_CELL, 1),
              SPAN_OF(WIDE_CELL));
    check_int("the unlabelled span has no gutter", fleet_grid_span_for(20, 0),
              FLEET_GRID * 20 + (FLEET_GRID - 1) * FLEET_GRID_GAP);

    check("the landscape body is wide",
          fleet_shape_is_wide(WIDE_W, WIDE_H - CORNER_REACH, &cell));
    check_int("and draws a 34 px cell", cell, WIDE_CELL);
    check("the portrait body is not wide",
          !fleet_shape_is_wide(TALL_W, TALL_H - CORNER_REACH, NULL));
    check("a square body is not wide", !fleet_shape_is_wide(600, 600, NULL));
    /* Big enough across for the wide shape and no wider than it is high: the
     * one case where only "wider than it is tall" can refuse it. */
    check("nor is a square body with room to spare",
          !fleet_shape_is_wide(1400, 1400, NULL));
    check("nor a body taller than it is wide with the same room",
          !fleet_shape_is_wide(1400, 1500, NULL));
    check("while one pixel wider than it is tall is",
          fleet_shape_is_wide(1401, 1400, NULL));
    check("a body exactly at the cell floor is still wide",
          fleet_shape_is_wide(1192, h_at_floor, &cell));
    check_int("at the floor cell", cell, FLEET_CELL_MIN);
    check("one whole cell below the floor it is not",
          !fleet_shape_is_wide(1192, h_at_floor - FLEET_GRID, NULL));

    floor_w = SPAN_OF(WIDE_CELL) + POCKETUI_PAD + 2 * FLEET_COL_MIN + POCKETUI_PAD;
    check("a body exactly at the width floor is wide",
          fleet_shape_is_wide(floor_w, WIDE_H - CORNER_REACH, NULL));
    check("one pixel narrower it is not",
          !fleet_shape_is_wide(floor_w - 1, WIDE_H - CORNER_REACH, NULL));
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

static void check_wide_battle(int want_cell)
{
    lv_area_t board;
    lv_area_t act;
    lv_area_t waters;
    lv_area_t fire;

    box_of(battle_board(), &board);
    box_of(battle_act(), &act);
    box_of(battle_waters(), &waters);
    box_of(battle_fire(), &fire);

    check_int("the board is square", lv_area_get_width(&board), lv_area_get_height(&board));
    check_int("and as large as the body allows", lv_area_get_width(&board),
              SPAN_OF(want_cell));
    check_int("its cells are the wide cell", fleet_grid_cell(battle_board()), want_cell);
    check("the readout stands beside the board, not under it", act.x1 > board.x2);
    check("your own waters stand beyond the readout", waters.x1 > act.x2);
    check_int("the two columns are the same width",
              lv_area_get_width(&act), lv_area_get_width(&waters));
    check("FIRE is at the foot of the column next to the board",
          fire.x1 >= act.x1 && fire.x2 <= act.x2 && fire.y2 >= act.y2 - 1);
    check("FIRE keeps the touch minimum", lv_area_get_height(&fire) >= POCKETUI_TOUCH_MIN);
    check("and is wider than it is tall", lv_area_get_width(&fire) > lv_area_get_height(&fire));
    check("your own board is the lesser of the two",
          fleet_grid_cell(battle_own()) < fleet_grid_cell(battle_board()));
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
    check_int("your own board is back to 20 px", fleet_grid_cell(battle_own()), 20);
    /* Where v0.0.10 put it: at the top of the body, centred across a column
     * six pixels wider than it is. */
    check_int("the board starts at the top of the body", board.y1, frame.y1);
    check_int("and is centred across it", board.x1 - frame.x1,
              (TALL_W - SPAN_OF(FLEET_CELL_TALL)) / 2);
    check("the readout is under the board", act.y1 > board.y2);
    check("your own waters are under the readout", waters.y1 > act.y2);
    check_int("FIRE is full width", lv_area_get_width(&fire), TALL_W);
    check("FIRE keeps the touch minimum", lv_area_get_height(&fire) >= POCKETUI_TOUCH_MIN);
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
        check_int("the board is square", lv_area_get_width(&b), lv_area_get_height(&b));
        check_int("and as large as the body allows", lv_area_get_width(&b), SPAN_OF(WIDE_CELL));
        check_int("its cells are 34 px", fleet_grid_cell(deploy_board()), WIDE_CELL);
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
    check_singletons();
    check_own_board_is_not_a_target();
    test_board_taps("battle taps, 34 px");
    test_board_edges("battle edges, 34 px");

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
    test_board_taps("battle taps, 35 px");
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
        use_mode("normal");
    }

    phase = "bodies with no room";
    /* 1040 x 568 -> a 1000 x 396 body: just over the width floor. */
    use_panel_sized(PANEL_W, 1040, POS_ROTATION_270, 0);
    check_int("a body just over the width floor is wide",
              fleet_grid_cell(battle_board()), RECT_CELL);
    /* 1030 x 568 -> 990 x 396: just under it. */
    use_panel_sized(PANEL_W, 1030, POS_ROTATION_270, 0);
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

    check_int("the game never went home by itself", g_home_calls, 0);
    check_int("and never asked for the keyboard", g_keyboard_calls, 0);

    printf("fleet_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
