/*
 * PocketCalendar: a month, and which day it is. A calendar, not a scheduler
 * - there are no events, no reminders and nothing to sync, and
 * tests/calendar_lint.sh fails the build if any appear.
 *
 * This file is the screen and nothing else. Every date decision belongs to
 * cal_date.c and cal_view.c, which are pure and tested; what is here is the
 * grid of cells, the three buttons and the once-a-second question to the
 * shell. It reads no clock: the system date arrives through
 * pocketos_shell_system_day(), so there is one reader of the wall clock and
 * one rule about whether the board knows the date (app.h).
 *
 * No motion. A month change is a relabelling of the cells that are already
 * there, applied immediately, which is what DS §12 asks of a reduced-motion
 * build and is no worse for anyone else.
 *
 * LAYOUT. One screen in two shapes, chosen from the box the app is given and
 * chosen again whenever that box changes size: tall, the portrait layout it
 * always had, and wide, the month beside what is said about it (see "the
 * layout" below). The orientation is the system's (DS section 21.2); nothing
 * here asks for it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "cal_view.h"

#include "app.h"
#include "pocketui.h"

#include <string.h>

/* The grid fills the body exactly: 7 columns of 72 px with 4 px between them
 * is 7*72 + 6*4 = 528, which is 568 less the body's 20 px side padding
 * (DS §7). 72 px square clears the 64 px minimum touch target of C1 with
 * room for a fingertip that is not centred. */
#define CELL_SIZE 72
#define CELL_GAP 4
#define WEEKDAY_ROW_H 32
/* Between the weekday headers and the first row of days. The body's own 20 px
 * row gap separates the blocks; inside this one the headers belong to the
 * grid and sit close to it. */
#define HEADER_GAP 8
/* Prev and next. 72 wide like the shell's back slab, 64 tall rather than the
 * 56 of DS §7's paired buttons, so month stepping meets the same 64 px
 * minimum as every other target on this screen. */
#define NAV_W 72
#define NAV_H 64
/* The number sits above centre and the accent dot below it, so today carries
 * a shape as well as a colour (DS §2 does not let colour mean anything by
 * itself). Both are offsets rather than sizes, so the two body fonts of DS §6
 * - 16 px in Normal, 20 px in Outdoor - both fit the 72 px cell. */
#define CELL_TEXT_RISE 8
#define CELL_DOT_DROP 4

/* The portrait body on the reference panel: 568 less the 20 px body padding
 * each side (DS §7). The wide shape is only chosen when two of these and the
 * gutter between them fit, so neither the month nor what is said about it is
 * narrower there than in portrait. */
#define CAL_COLUMN_W 528
/* The wide shape's weeks. The landscape body is too short for six rows of 72
 * px, or of 64 (6 * 64 + 5 * 4 = 404 px before the weekday headings, in a
 * 396 px body), so there the weeks share the height instead: cells wider
 * than tall, and at least as tall as DS §7's paired buttons. Rows and
 * columns are both 4 px apart, and the headings keep 24 px of their 32. */
#define WIDE_WEEKDAY_ROW_H 24
#define WIDE_CELL_MIN_H 56
#define WIDE_MONTH_MIN_H (WIDE_WEEKDAY_ROW_H + CAL_ROWS * (CELL_GAP + WIDE_CELL_MIN_H))

struct cal_app {
    struct cal_view view;
    lv_obj_t *frame;    /* the app's own box in the body: everything below */
    /* What the layout in force was chosen from, and the one place that
     * decides whether a pass is needed at all (pocketui.h). */
    struct pocketui_layout_guard layout_guard;
    bool wide;
    lv_obj_t *nav;   /* previous, the month's name and next */
    lv_obj_t *month_label;
    lv_obj_t *month; /* the weekday headings over the six weeks */
    lv_obj_t *headings;
    lv_obj_t *heading[CAL_COLS];
    lv_obj_t *week[CAL_ROWS];
    lv_obj_t *panel; /* the notice and the date in words */
    lv_obj_t *cell[CAL_CELLS];
    lv_obj_t *cell_label[CAL_CELLS];
    lv_obj_t *cell_dot[CAL_CELLS];
    lv_obj_t *selected_label; /* the date in words */
    lv_obj_t *notice;         /* "Date not set", shown instead */
    lv_obj_t *notice_title;
    lv_obj_t *today_button;
    /* What the cells were last painted as, so a tick that changes nothing
     * touches nothing. */
    struct cal_cell drawn[CAL_CELLS];
    bool drawn_valid;
    bool today_enabled;
};

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The disable treatment every app uses: the disabled role replaces the
 * primary one and the object stops taking taps, so a button that cannot act
 * also cannot be pressed (DS §9). */
static void button_set_enabled(lv_obj_t *button, bool enabled)
{
    if (!button) {
        return;
    }
    if (enabled) {
        lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        pos_style_add(button, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(button);
}

/* ---- painting ---------------------------------------------------------- */

static void paint_cell(struct cal_app *a, int i, const struct cal_cell *c)
{
    lv_obj_t *cell = a->cell[i];
    lv_obj_t *label = a->cell_label[i];

    /* Every style this cell can carry comes off first, so painting a cell
     * that is already in the wanted state cannot stack a second copy of a
     * style on it. */
    lv_obj_remove_style(cell, pos_style(POS_STYLE_SLAB), 0);
    lv_obj_remove_style(cell, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(cell, pos_style(POS_STYLE_SELECTED), 0);
    lv_obj_remove_style(label, pos_style(POS_STYLE_TEXT_PRIMARY), 0);
    lv_obj_remove_style(label, pos_style(POS_STYLE_ACCENT_TEXT), 0);

    if (c->day == 0) {
        /* Outside the month. The cell keeps its place in the row and is
         * merely drawn as nothing: LV_OBJ_FLAG_HIDDEN would take it out of
         * the flex layout, which would close the gap and shift the rest of
         * the week left - and the leading gap is exactly what puts the first
         * of the month in the right column. It takes no taps either, because
         * there is no date there to select. */
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        set_text(label, "");
        set_hidden(a->cell_dot[i], true);
        return;
    }
    pos_style_add(cell, POS_STYLE_SLAB, 0);
    pos_style_add(cell, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text_fmt(label, "%d", (int)c->day);

    /* Today is the accent plus the dot below it. Nothing is today while the
     * system date is not set, which cal_view_grid guarantees for every cell
     * of every month browsed. */
    pos_style_add(label, c->is_today ? POS_STYLE_ACCENT_TEXT : POS_STYLE_TEXT_PRIMARY, 0);
    set_hidden(a->cell_dot[i], !c->is_today);

    /* Selected is the 2 px focus outline of DS §7, over whatever fill the
     * cell already has, so a day can be today and selected at once. */
    if (c->is_selected) {
        pos_style_add(cell, POS_STYLE_SELECTED, 0);
    }
}

static void refresh(struct cal_app *a)
{
    struct cal_cell cells[CAL_CELLS];
    char buf[CAL_SELECTED_TEXT_MAX];
    bool date_set;
    int i;

    cal_view_month_text(&a->view, buf, sizeof(buf));
    set_text(a->month_label, buf);

    cal_view_grid(&a->view, cells);
    for (i = 0; i < CAL_CELLS; i++) {
        if (a->drawn_valid && a->drawn[i].day == cells[i].day &&
            a->drawn[i].is_today == cells[i].is_today &&
            a->drawn[i].is_selected == cells[i].is_selected) {
            continue;
        }
        paint_cell(a, i, &cells[i]);
    }
    memcpy(a->drawn, cells, sizeof(cells));
    a->drawn_valid = true;

    date_set = cal_view_date_set(&a->view);
    /* The notice is up whenever the board does not know the date; the date in
     * words is up whenever a day is selected. Both can be, and while the
     * clock is unset both usually are. */
    cal_view_selected_text(&a->view, buf, sizeof(buf));
    set_text(a->selected_label, buf);
    set_hidden(a->selected_label, buf[0] == '\0');
    set_hidden(a->notice, date_set);

    if (a->today_enabled != date_set) {
        a->today_enabled = date_set;
        button_set_enabled(a->today_button, date_set);
    }
}

/* ---- events ------------------------------------------------------------ */

static void on_prev(lv_event_t *e)
{
    struct cal_app *a = lv_event_get_user_data(e);

    cal_view_prev_month(&a->view);
    refresh(a);
}

static void on_next(lv_event_t *e)
{
    struct cal_app *a = lv_event_get_user_data(e);

    cal_view_next_month(&a->view);
    refresh(a);
}

static void on_today(lv_event_t *e)
{
    struct cal_app *a = lv_event_get_user_data(e);

    cal_view_today(&a->view);
    refresh(a);
}

static void on_cell(lv_event_t *e)
{
    struct cal_app *a = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    if (i < 0 || i >= CAL_CELLS) {
        return;
    }
    /* The day this cell is currently showing. An empty cell is hidden and
     * cannot be tapped, and cal_view_select ignores a 0 anyway. */
    cal_view_select(&a->view, a->drawn[i].day);
    refresh(a);
}

/* ---- building ---------------------------------------------------------- */

/* A row of seven columns on the grid's pitch, so the weekday headers sit
 * over the days they name. */
static lv_obj_t *seven_columns(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, CELL_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void build_nav(struct cal_app *a, lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *prev;
    lv_obj_t *next;
    lv_obj_t *glyph;

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), NAV_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    a->nav = row;

    prev = lv_button_create(row);
    lv_obj_remove_style_all(prev);
    pos_style_add(prev, POS_STYLE_SLAB, 0);
    pos_style_add(prev, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(prev, NAV_W, NAV_H);
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev, on_prev, LV_EVENT_CLICKED, a);
    glyph = lv_label_create(prev);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
    pos_style_add(glyph, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(glyph);

    a->month_label = pocketui_label(row, "", POS_STYLE_TITLE);
    lv_obj_set_flex_grow(a->month_label, 1);
    lv_obj_set_style_text_align(a->month_label, LV_TEXT_ALIGN_CENTER, 0);

    next = lv_button_create(row);
    lv_obj_remove_style_all(next);
    pos_style_add(next, POS_STYLE_SLAB, 0);
    pos_style_add(next, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(next, NAV_W, NAV_H);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next, on_next, LV_EVENT_CLICKED, a);
    glyph = lv_label_create(next);
    lv_label_set_text(glyph, LV_SYMBOL_RIGHT);
    pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
    pos_style_add(glyph, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(glyph);
}

static void build_grid(struct cal_app *a, lv_obj_t *parent)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_t *headers;
    lv_obj_t *row;
    int i;

    lv_obj_remove_style_all(block);
    lv_obj_set_width(block, LV_PCT(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, HEADER_GAP, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    a->month = block;

    headers = seven_columns(block, WEEKDAY_ROW_H);
    a->headings = headers;
    for (i = 0; i < CAL_COLS; i++) {
        lv_obj_t *lb = pocketui_label(headers, cal_weekday_short(i), POS_STYLE_CAPTION);

        lv_obj_set_width(lb, CELL_SIZE);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        a->heading[i] = lb;
    }

    row = NULL;
    for (i = 0; i < CAL_CELLS; i++) {
        lv_obj_t *cell;
        lv_obj_t *dot;

        if (i % CAL_COLS == 0) {
            row = seven_columns(block, CELL_SIZE);
            a->week[i / CAL_COLS] = row;
        }
        /* The slab, the pressed state and the clickability all belong to
         * paint_cell: whether this cell is a day at all changes every time
         * the month does. */
        cell = lv_button_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(cell, (void *)(intptr_t)i);
        lv_obj_add_event_cb(cell, on_cell, LV_EVENT_CLICKED, a);

        a->cell[i] = cell;
        a->cell_label[i] = pocketui_label(cell, "", POS_STYLE_TEXT_PRIMARY);
        lv_obj_align(a->cell_label[i], LV_ALIGN_CENTER, 0, -CELL_TEXT_RISE);

        /* Today's dot. A bullet in the accent role rather than a drawn
         * rectangle, so no colour is named outside the theme engine.
         *
         * Its height is left to the glyph. Outdoor mode sets a 20 px body
         * font against Normal's 16 (DS §6), so a height fixed here for one
         * mode clips the bullet in the other - which it did. */
        dot = pocketui_label(cell, LV_SYMBOL_BULLET, POS_STYLE_ACCENT_TEXT);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -CELL_DOT_DROP);
        a->cell_dot[i] = dot;
        set_hidden(dot, true);
    }
}

static void build_footer(struct cal_app *a, lv_obj_t *parent)
{
    lv_obj_t *panel = pocketui_card(parent);
    lv_obj_t *body;

    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    a->panel = panel;

    /* The same shape PocketClock uses for a board that does not know the
     * time, said about the date (docs/apps/POCKETCLOCK.md): a warning title
     * and the reason, never a plausible-looking date the board does not have.
     *
     * It sits above the selected date rather than replacing it. A day picked
     * while the clock is unset is still a real date the user chose, and the
     * line below is the only place its year is written; hiding it would leave
     * the outline in the grid saying something the screen never finishes. */
    a->notice = lv_obj_create(panel);
    lv_obj_remove_style_all(a->notice);
    lv_obj_set_width(a->notice, LV_PCT(100));
    lv_obj_set_height(a->notice, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->notice, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->notice, 12, 0);
    lv_obj_clear_flag(a->notice, LV_OBJ_FLAG_SCROLLABLE);
    a->notice_title = pocketui_label(a->notice, "Date not set", POS_STYLE_TITLE);
    pos_style_add(a->notice_title, POS_STYLE_STATUS_WARN_TEXT, 0);
    body = pocketui_label(a->notice,
                          "This board has no battery-backed clock, so it "
                          "forgets the date at every power-off. No day is "
                          "marked as today until it is set. The months above "
                          "can still be browsed.",
                          POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));

    a->selected_label = pocketui_label(panel, "", POS_STYLE_ROW_TITLE);
    lv_label_set_long_mode(a->selected_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->selected_label, LV_PCT(100));

    a->today_button = pocketui_button(parent, "Today", on_today, a);
    lv_obj_set_height(a->today_button, POCKETUI_ROW_H);
    /* Painted by the first refresh, from whether there is a today at all. */
    a->today_enabled = true;
}

/* ---- the layout -------------------------------------------------------- *
 *
 * Everything sits in one frame that is exactly the body's content box - the
 * whole of the room the shell gives the app - laid out as one grid, and is
 * shaped from the size of that box alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel). The layout Calendar
 *   always had, one item under the other: previous, the month's name and
 *   next in a row of 64, the month's 72 px cells, the panel and Today.
 *
 *   WIDE (landscape: 1192 x 396). Height is what landscape lacks and width is
 *   what it has, so the month goes beside the rest rather than above it, in
 *   two halves of the width with the 20 px gutter between them. The month
 *   takes the first half and the full height, its six weeks sharing it, so
 *   every day of every month is on screen at once. The second half is the
 *   portrait column without the month: the row of previous, the name and
 *   next at the top, Today at the foot, and the panel between them, as tall
 *   as that leaves. The panel says the most - the notice and a selected date,
 *   in Outdoor type - in 201 px of its 218, and scrolls itself should it ever
 *   say more. Chosen only when both halves keep CAL_COLUMN_W and the weeks
 *   keep WIDE_CELL_MIN_H; under either, the tall layout is kept whole and the
 *   frame scrolls.
 *
 * The objects are built once and only shaped here - tracks, cells, sizes, and
 * which box scrolls - so the month on screen, the selection and today are
 * never touched by a change of shape.
 *
 * Whatever the shape, the content clears the panel's unsafe area (DS §21.1,
 * §22.2): the wide shape reaches the foot of the body, and would reach 10 px
 * into the 30 px corner squares of the reference panel, so the frame pads its
 * foot by however far a corner square reaches into the body
 * (pos_display_rect_insets, the rule Calculator and Notes use). The
 * tall layout ends far above the foot, so it does not move; on a panel with
 * square corners the pad is 0. */

/* Tall: one column, and in it the navigation, the month, the panel and Today. */
static const int32_t tall_cols[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
static const int32_t tall_rows[] = { NAV_H, LV_GRID_CONTENT, LV_GRID_CONTENT, POCKETUI_ROW_H,
                                     LV_GRID_TEMPLATE_LAST };
/* Wide: two halves; the month spans the second's navigation, panel and Today. */
static const int32_t wide_cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
static const int32_t wide_rows[] = { NAV_H, LV_GRID_FR(1), POCKETUI_ROW_H, LV_GRID_TEMPLATE_LAST };

/* Tall: the portrait grid of 72 px cells, the headings over them. Wide: the
 * weeks share the height and the cells the width. */
static void shape_month(struct cal_app *a)
{
    bool wide = a->wide;
    int i;

    lv_obj_set_style_pad_row(a->month, wide ? CELL_GAP : HEADER_GAP, 0);
    lv_obj_set_height(a->headings, wide ? WIDE_WEEKDAY_ROW_H : WEEKDAY_ROW_H);
    for (i = 0; i < CAL_COLS; i++) {
        lv_obj_set_flex_grow(a->heading[i], wide ? 1 : 0);
        lv_obj_set_width(a->heading[i], CELL_SIZE);
    }
    for (i = 0; i < CAL_ROWS; i++) {
        lv_obj_set_flex_grow(a->week[i], wide ? 1 : 0);
        lv_obj_set_height(a->week[i], CELL_SIZE);
    }
    for (i = 0; i < CAL_CELLS; i++) {
        lv_obj_set_flex_grow(a->cell[i], wide ? 1 : 0);
        lv_obj_set_size(a->cell[i], CELL_SIZE, wide ? LV_PCT(100) : CELL_SIZE);
    }
}

static void shape_tall(struct cal_app *a)
{
    lv_obj_set_grid_dsc_array(a->frame, tall_cols, tall_rows);
    lv_obj_set_grid_cell(a->nav, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 0, 1);
    lv_obj_set_grid_cell(a->month, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 1, 1);
    lv_obj_set_grid_cell(a->panel, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_set_grid_cell(a->today_button, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_clear_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE);
    /* The frame scrolls as the body did, which is only ever needed in a box
     * too small for the tall layout. */
    lv_obj_add_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
}

static void shape_wide(struct cal_app *a)
{
    lv_obj_set_grid_dsc_array(a->frame, wide_cols, wide_rows);
    lv_obj_set_grid_cell(a->month, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 3);
    lv_obj_set_grid_cell(a->nav, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 0, 1);
    lv_obj_set_grid_cell(a->panel, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_grid_cell(a->today_button, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_add_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to_y(a->frame, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
}

static void layout(struct cal_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;

    /* Nothing to lay out in, or nothing the layout is chosen from has
     * changed: PocketUI owns that decision for every responsive app, and
     * hands back the corner clearance the platform rule gives this box. */
    if (!pocketui_layout_begin(&a->layout_guard, a->frame, &in)) {
        return;
    }
    box = &a->layout_guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    a->wide = w > h && w >= 2 * CAL_COLUMN_W + POCKETUI_PAD && h >= WIDE_MONTH_MIN_H;
    shape_month(a);
    if (a->wide) {
        shape_wide(a);
    } else {
        shape_tall(a);
    }
}

/* The frame is the body's content box, so this is the body changing size:
 * the display turned, or this is the first layout pass after the app was
 * built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct cal_app *a, lv_obj_t *root)
{
    lv_obj_t *frame = lv_obj_create(root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the shape itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    /* The tracks before anything is placed on them: LVGL lays a grid with no
     * tracks out with a warning. */
    lv_obj_set_grid_dsc_array(frame, tall_cols, tall_rows);
    lv_obj_set_style_pad_row(frame, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(frame, POCKETUI_PAD, 0);
    lv_obj_set_scroll_dir(frame, LV_DIR_VER);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    a->frame = frame;
}

/* ---- the app ----------------------------------------------------------- */

static void *calendar_create(lv_obj_t *root)
{
    struct cal_app *a = lv_malloc_zeroed(sizeof(*a));

    if (!a) {
        return NULL;
    }
    /* Whatever the shell's last tick saw. On this board that is usually "not
     * set" for the first seconds after boot and a real date afterwards, and
     * the tick below picks the change up either way. */
    cal_view_init(&a->view, pocketos_shell_system_day());

    build_frame(a, root);
    build_nav(a, a->frame);
    build_grid(a, a->frame);
    build_footer(a, a->frame);
    refresh(a);

    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

/* Once a second, from the shell. It asks the shell what day it is rather
 * than reading a clock, and repaints only when the answer changed - which is
 * how the app recovers by itself when SNTP or an operator sets the date
 * while it is open. */
static void calendar_tick(void *priv)
{
    struct cal_app *a = priv;

    if (!a) {
        return;
    }
    if (cal_view_set_system_day(&a->view, pocketos_shell_system_day())) {
        refresh(a);
    }
}

static void calendar_destroy(void *priv)
{
    struct cal_app *a = priv;

    if (!a) {
        return;
    }
    /* The shell deletes the objects under root, there is no timer of our own
     * and nothing is stored: a calendar with no events has nothing to write
     * and no selection worth keeping. The frame outlives this by a moment,
     * in which nothing may call back into a freed app. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    lv_free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_calendar);

const struct pocketos_app app_calendar = {
    .id = "calendar",
    .name = "Calendar",
    /* The launcher draws the Doors icon (DS §20). The glyph stays as the
     * app's text icon: LVGL's symbol font has no calendar glyph, and rows of
     * bars are the nearest thing to a month grid in it. */
    .icon = LV_SYMBOL_BARS,
    .icon_mask = &pos_app_icon_calendar,
    .create = calendar_create,
    .tick = calendar_tick,
    .destroy = calendar_destroy,
};
