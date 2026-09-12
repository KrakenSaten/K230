/*
 * PocketCalendar in the running app, driven by a real LVGL pointer device.
 *
 * The unit tests either side of this one prove the arithmetic and the state
 * machine. This one proves the part they cannot: that a finger landing on a
 * cell reaches them, that the cell it lands on is the one under it, and that
 * an empty cell is not a target. It exists so that a board is the first
 * HARDWARE test of the tap path and not the first test of it at all.
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
#define STATUS_H POCKETUI_STATUS_BAR_H

/* The dates this test drives the app with. September 2026 starts on a
 * Tuesday, so cell 0 is a leading blank and the 12th lands in cell 12 -
 * which is the Monday-first offset arriving on the glass. */
#define DAY_2026_09_12 20260912LL
#define DAY_2026_09_13 20260913LL
#define SEPT_LEAD 1
#define SEPT_DAYS 30

/* The app's own layout, as cal_app.c builds it under the body: the month
 * row, the grid block, the panel and the button. Reaching for a cell by
 * position is the only way to ask about one that has no text in it. */
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

static uint8_t draw_buf[PANEL_W * 40 * 2];
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

static lv_obj_t *app_body;
static void *app_priv;

static lv_obj_t *kid(lv_obj_t *parent, int i)
{
    return parent ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

static lv_obj_t *nav_of(int which) { return kid(kid(app_body, KID_NAV), which); }
static lv_obj_t *grid_block(void) { return kid(app_body, KID_GRID); }
/* Child 0 of the block is the weekday headings; the six day rows follow. */
static lv_obj_t *cell_at(int i)
{
    return kid(kid(grid_block(), 1 + i / CAL_COLS), i % CAL_COLS);
}
static lv_obj_t *cell_label(int i) { return kid(cell_at(i), CELL_LABEL); }
static lv_obj_t *cell_dot(int i) { return kid(cell_at(i), CELL_DOT); }
static lv_obj_t *today_button(void) { return kid(app_body, KID_TODAY); }

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
static const char *selected_text(void)
{
    lv_obj_t *panel = kid(app_body, KID_PANEL);
    lv_obj_t *label = kid(panel, (int)lv_obj_get_child_count(panel) - 1);

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

/* ---- the app's lifecycle, as the shell runs it -------------------------- */

static lv_obj_t *g_content;

static void app_start(void)
{
    app_body = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_size(app_body, LV_PCT(100), LV_PCT(100));
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
    lv_obj_delete(app_body);
    app_body = NULL;
    pump(60);
}

int main(void)
{
    lv_display_t *disp;
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

    /* ---- 1. it opens, and the month is on the glass -------------------- */

    g_system_day = DAY_2026_09_12;
    app_start();
    check_str("the app opens on the system date's month", month_text(), "September 2026");
    check("the weekday headings are there, Monday first",
          strcmp(text_of(kid(kid(grid_block(), 0), 0)), "Mon") == 0);
    check_str("and Sunday is the last column",
              text_of(kid(kid(grid_block(), 0), CAL_COLS - 1)), "Sun");
    check("the grid has six rows of seven",
          (int)lv_obj_get_child_count(grid_block()) == 1 + CAL_ROWS);
    {
        int wrong = 0;

        for (i = 0; i < CAL_ROWS; i++) {
            if ((int)lv_obj_get_child_count(kid(grid_block(), 1 + i)) != CAL_COLS) {
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
    {
        int clickable_cells = 0;

        for (i = 0; i < CAL_CELLS; i++) {
            if (clickable(cell_at(i))) {
                clickable_cells++;
            }
        }
        check("exactly the month's days are targets", clickable_cells == SEPT_DAYS);
    }

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

    printf("cal_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
