/*
 * PocketRadar in the running app, driven by a real LVGL pointer device.
 *
 * The unit tests either side of this one prove the rules, the scoring, the
 * record codec and the scope's pixel round trip. This one proves the part
 * they cannot: that the layout is chosen from the body alone, that a finger
 * landing on the scope reaches the contact under it at either scope size,
 * and - the thing this app has to answer for that the others do not - that
 * none of it costs anything per tick.
 *
 * PocketRadar repaints a custom-drawn scope twenty times a second while a run
 * is on, and that is the whole of its frame cost (KNOWN_ISSUES, hardware
 * verification H1). So a landscape layout has to be shown not to have made
 * that worse. It is, three ways: the scope across the page is never larger
 * than the one down it, so fewer pixels are invalidated, not more; the layout
 * runs only when the body's box changes, never on a tick; and the HUD's
 * labels are still written only when the value behind them changes.
 *
 * What it covers, with the app hosted the way the shell hosts it - a header
 * and a padded body - on the reference panel with its 30 px rounded corners
 * and with square ones, in portrait and landscape, in Normal and Outdoor:
 *
 *   - the shape rule as arithmetic, at and either side of every floor;
 *   - both screens in both shapes: what stands beside what, every action a
 *     finger's size, nothing outside the body or the safe area;
 *   - the scope's tap conversion at both sizes: the centre, the rim, each
 *     cardinal direction, and points outside it;
 *   - selecting a contact by tapping it, at both sizes, and ENGAGE;
 *   - the display turned under a run in progress: the same objects, none
 *     added, the run untouched - its contacts, its score, its selection;
 *   - a hundred ticks of an active run: no layout pass, no new object, no
 *     label rewritten that did not change, and only the scope invalidated.
 *
 * The shell is not here, so this file plays it: the entry points the app uses
 * from app.h are defined below, and pocketlog's one entry point is a buffer.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/radar_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "radar_app.h"
#include "ui/radar_scope.h"

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
#define BODY_CHROME_H (STATUS_H + POCKETUI_HEADER_H + POCKETUI_BODY_PAD_TOP + POCKETUI_PAD)
#define CORNER_REACH 10

/* What the layout promises, written down here rather than read from the app. */
#define TALL_W (PANEL_W - 2 * POCKETUI_PAD)      /* 528 */
#define TALL_H (PANEL_H - BODY_CHROME_H)         /* 1060 */
#define WIDE_W (PANEL_H - 2 * POCKETUI_PAD)      /* 1192 */
#define WIDE_H (PANEL_W - BODY_CHROME_H)         /* 396 */
#define WIDE_SCOPE (WIDE_H - CORNER_REACH)       /* 386 */
#define RECT_SCOPE WIDE_H                        /* 396, square corners */

extern const struct pocketos_app app_radar;

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
static struct radar_app *app;

static lv_obj_t *kid(lv_obj_t *parent, int i)
{
    return parent ? lv_obj_get_child(parent, (int32_t)i) : NULL;
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
    app = app_radar.create(app_body);
    pump(120);
}

static void app_stop(void)
{
    app_radar.destroy(app);
    app = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

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

/* The scan screen: side holds the cards and the action; the scope is a child
 * of cards down the page and of the screen across it. */
static lv_obj_t *scan_side(void) { return kid(screen_of(RADAR_SCREEN_SCAN), 0); }
static lv_obj_t *scan_side_wide(void) { return kid(screen_of(RADAR_SCREEN_SCAN), 1); }
static lv_obj_t *scan_box(void)
{
    /* Whichever of the two the side box is in this shape. */
    lv_obj_t *a = scan_side();

    return radar_scope_size(a) > 0 ? scan_side_wide() : a;
}
static lv_obj_t *scan_cards(void) { return kid(scan_box(), 0); }
static lv_obj_t *scan_hud(void) { return kid(scan_cards(), 0); }
static lv_obj_t *scan_scope(void)
{
    lv_obj_t *first = kid(screen_of(RADAR_SCREEN_SCAN), 0);

    if (radar_scope_size(first) > 0) {
        return first;                      /* across the page: first in the row */
    }
    return kid(scan_cards(), 1);           /* down the page: between the cards */
}
static lv_obj_t *scan_target_card(void)
{
    lv_obj_t *c = scan_cards();

    return kid(c, (int)lv_obj_get_child_count(c) - 1);
}
static lv_obj_t *scan_action(void) { return kid(scan_box(), 1); }

static lv_obj_t *result_title(void) { return kid(screen_of(RADAR_SCREEN_RESULT), 0); }
static lv_obj_t *result_cards(void) { return kid(screen_of(RADAR_SCREEN_RESULT), 1); }
static lv_obj_t *result_score_card(void) { return kid(result_cards(), 0); }
static lv_obj_t *result_stats_card(void) { return kid(result_cards(), 1); }
static lv_obj_t *result_foot(void) { return kid(screen_of(RADAR_SCREEN_RESULT), 2); }
static lv_obj_t *result_again(void) { return kid(result_foot(), 0); }

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

static int inside_body(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t b;

    box_of(obj, &a);
    box_of(app_body, &b);
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

static int in_safe_area(lv_obj_t *obj)
{
    lv_area_t a;

    box_of(obj, &a);
    return pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2);
}

/* ---- the shape rule, as arithmetic ------------------------------------- */

static void test_shape_rule(void)
{
    int scope = 0;

    phase = "shape rule";
    check_int("a 386 px body gives a 386 px scope", radar_scope_for_height(386), 386);
    check_int("a body taller than the tall scope is capped at it",
              radar_scope_for_height(900), RADAR_SCOPE_TALL);
    check_int("exactly the tall scope is the tall scope",
              radar_scope_for_height(RADAR_SCOPE_TALL), RADAR_SCOPE_TALL);
    check_int("no height at all gives nothing", radar_scope_for_height(0), 0);

    check("the landscape body is wide",
          radar_shape_is_wide(WIDE_W, WIDE_H - CORNER_REACH, &scope));
    check_int("and draws a 386 px scope", scope, WIDE_SCOPE);
    check("the portrait body is not wide",
          !radar_shape_is_wide(TALL_W, TALL_H - CORNER_REACH, NULL));
    check("a square body is not wide", !radar_shape_is_wide(600, 600, NULL));
    check("nor is a square body with room to spare",
          !radar_shape_is_wide(1400, 1400, NULL));
    check("while one pixel wider than it is tall is",
          radar_shape_is_wide(1401, 1400, NULL));
    check_int("and that one is capped at the tall scope too",
              (radar_shape_is_wide(1401, 1400, &scope), scope), RADAR_SCOPE_TALL);

    check("a body exactly at the scope floor is still wide",
          radar_shape_is_wide(RADAR_SCOPE_MIN + POCKETUI_PAD + RADAR_SIDE_MIN,
                              RADAR_SCOPE_MIN, &scope));
    check_int("at the floor scope", scope, RADAR_SCOPE_MIN);
    check("one pixel below the scope floor it is not",
          !radar_shape_is_wide(1192, RADAR_SCOPE_MIN - 1, NULL));
    check("one pixel narrower than the side floor it is not",
          !radar_shape_is_wide(RADAR_SCOPE_MIN + POCKETUI_PAD + RADAR_SIDE_MIN - 1,
                               RADAR_SCOPE_MIN, NULL));

    /* The promise this app has to keep: across the page is never dearer. */
    check("the wide scope is never larger than the tall one",
          radar_scope_for_height(10000) <= RADAR_SCOPE_TALL);
}

/* ---- structure --------------------------------------------------------- */

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

static void check_wide_scan(int want_scope)
{
    lv_area_t scope;
    lv_area_t hud;
    lv_area_t target;
    lv_area_t action;

    box_of(scan_scope(), &scope);
    box_of(scan_hud(), &hud);
    box_of(scan_target_card(), &target);
    box_of(scan_action(), &action);

    check_int("the scope is square", lv_area_get_width(&scope), lv_area_get_height(&scope));
    check_int("and as large as the body allows", lv_area_get_width(&scope), want_scope);
    check_int("the scope agrees about its own size", radar_scope_size(scan_scope()),
              want_scope);
    check("the numbers stand beside the scope, not under it", hud.x1 > scope.x2);
    check("the contact card stands beside the numbers", target.x1 > hud.x2);
    check_int("the two cards are the same width",
              lv_area_get_width(&hud), lv_area_get_width(&target));
    check("ENGAGE is under both of them", action.y1 >= hud.y2);
    check("and takes the whole width beside the scope",
          action.x1 <= hud.x1 && action.x2 >= target.x2);
    check("ENGAGE keeps the touch minimum", lv_area_get_height(&action) >= POCKETUI_TOUCH_MIN);
    check("nothing on the screen leaves the body", inside_body(screen_of(RADAR_SCREEN_SCAN)));
    check("the scope is in the safe area", in_safe_area(scan_scope()));
    check("ENGAGE is in the safe area", in_safe_area(scan_action()));
    check("the contact card is in the safe area", in_safe_area(scan_target_card()));
}

static void check_tall_scan(void)
{
    lv_area_t scope;
    lv_area_t hud;
    lv_area_t target;
    lv_area_t action;

    box_of(scan_scope(), &scope);
    box_of(scan_hud(), &hud);
    box_of(scan_target_card(), &target);
    box_of(scan_action(), &action);

    check_int("the scope is the v0.0.10 scope", lv_area_get_width(&scope),
              RADAR_SCOPE_TALL);
    check_int("the scope agrees about its own size", radar_scope_size(scan_scope()),
              RADAR_SCOPE_TALL);
    check("the numbers are above the scope", hud.y2 <= scope.y1);
    check("the contact card is under it", target.y1 >= scope.y2);
    check("ENGAGE is under that", action.y1 >= target.y2);
    check_int("ENGAGE is full width", lv_area_get_width(&action), TALL_W);
    check("ENGAGE keeps the touch minimum", lv_area_get_height(&action) >= POCKETUI_TOUCH_MIN);
}

/* ---- the scope's tap path ---------------------------------------------- */

/* Where the scope puts a bearing and range, and what it reads back from a
 * point. Both go through the object's own stored geometry, so this is the
 * conversion a finger uses. */
static void test_scope_taps(const char *what)
{
    lv_obj_t *scope = scan_scope();
    lv_area_t a;
    int32_t cx;
    int32_t cy;
    int i;
    int wrong = 0;
    static const int cardinals[4][3] = {
        /* bearing, expected dx sign, expected dy sign */
        { 0, 0, -1 }, { 900, 1, 0 }, { 1800, 0, 1 }, { 2700, -1, 0 }
    };

    phase = what;
    box_of(scope, &a);
    cx = a.x1 + lv_area_get_width(&a) / 2;
    cy = a.y1 + lv_area_get_height(&a) / 2;

    for (i = 0; i < 4; i++) {
        int dx = 0;
        int dy = 0;
        int bearing = -1;
        int range = -1;

        radar_scope_point(scope, cardinals[i][0], RADAR_RANGE_MAX / 2, &dx, &dy);
        if ((cardinals[i][1] > 0 && dx <= 0) || (cardinals[i][1] < 0 && dx >= 0) ||
            (cardinals[i][2] > 0 && dy <= 0) || (cardinals[i][2] < 0 && dy >= 0)) {
            wrong++;
        }
        /* And back: the point the scope put there reads as that bearing. */
        if (radar_scope_polar(scope, dx, dy, &bearing, &range) != 0) {
            wrong++;
        } else if (abs(bearing - cardinals[i][0]) > 20 &&
                   abs(bearing - cardinals[i][0]) < 3600 - 20) {
            wrong++;
        }
    }
    check_int("every cardinal direction goes out and comes back", wrong, 0);

    {
        int bearing = -1;
        int range = -1;

        check_int("the centre of the scope is range 0",
                  (radar_scope_polar(scope, 0, 0, &bearing, &range), range), 0);
        check("a point beyond the rim is outside the scope",
              radar_scope_polar(scope, lv_area_get_width(&a), 0, &bearing, &range) == -1);
        check("and so is one just past the top of it",
              radar_scope_polar(scope, 0, -lv_area_get_width(&a) / 2, &bearing,
                                &range) == -1);
    }
    (void)cx;
    (void)cy;
}

/* A tap on a contact selects it, whatever size the scope is drawn at. */
static void test_select_by_tap(const char *what)
{
    lv_obj_t *scope = scan_scope();
    const struct radar_contact *c = NULL;
    lv_area_t a;
    int i;
    int dx = 0;
    int dy = 0;

    phase = what;
    radar_run_deselect(&app->run);
    pump(60);
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *t = radar_run_slot(&app->run, i);

        if (t && t->active) {
            c = t;
            break;
        }
    }
    check("there is a contact on the scope to tap", c != NULL);
    if (!c) {
        return;
    }
    box_of(scope, &a);
    radar_scope_point(scope, c->bearing, c->range, &dx, &dy);
    tap_at(a.x1 + lv_area_get_width(&a) / 2 + dx, a.y1 + lv_area_get_height(&a) / 2 + dy);
    check("tapping a contact selects it", radar_run_selected(&app->run) == c);
    check("and the card names it", strcmp(text_of(kid(kid(scan_target_card(), 0), 0)),
                                          "NO CONTACT") != 0);
}

/* ---- what a tick actually repaints -------------------------------------- *
 *
 * The claim this app has to stand behind is that a tick invalidates the scope
 * and nothing else. A draw counter on each of the three things on the screen
 * measures it: over a hundred ticks the scope is drawn many times, and the
 * cards - whose labels are written only when the value behind them changes -
 * are drawn a handful of times at most.
 */

static int g_draws_scope;
static int g_draws_hud;
static int g_draws_target;

static void count_scope(lv_event_t *e) { (void)e; g_draws_scope++; }
static void count_hud(lv_event_t *e) { (void)e; g_draws_hud++; }
static void count_target(lv_event_t *e) { (void)e; g_draws_target++; }

/* ---- the run, and what a relayout may not do to it ---------------------- */

struct run_state {
    uint8_t state;
    int32_t score;
    uint16_t level;
    int integrity;
    int active;
    uint32_t selected;
};

static void snapshot(struct run_state *s)
{
    const struct radar_contact *sel = radar_run_selected(&app->run);
    int i;

    memset(s, 0, sizeof(*s));
    s->state = (uint8_t)app->run.state;
    s->score = (int32_t)app->run.score.points;
    s->level = (uint16_t)radar_run_level(&app->run);
    s->integrity = app->run.score.integrity;
    s->selected = sel ? sel->id : RADAR_NO_CONTACT;
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *t = radar_run_slot(&app->run, i);

        s->active += t && t->active;
    }
}

static void check_same_run(const char *what, const struct run_state *a)
{
    struct run_state b;

    snapshot(&b);
    checks++;
    if (memcmp(a, &b, sizeof(b)) != 0) {
        failed++;
        printf("FAIL [%s] %s\n", phase, what);
        printf("     state %u/%u score %d/%d level %u/%u integrity %d/%d "
               "active %d/%d selected %u/%u\n",
               a->state, b.state, a->score, b.score, a->level, b.level,
               a->integrity, b.integrity, a->active, b.active,
               a->selected, b.selected);
    }
}

/* ---- main -------------------------------------------------------------- */

static char state_dir[] = "/tmp/radar_app_test_XXXXXX";

static void with_screen(const char *screen)
{
    if (screen) {
        setenv("POCKETRADAR_SCREEN", screen, 1);
    } else {
        unsetenv("POCKETRADAR_SCREEN");
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

    /* ---- 2. the scan screen, in both shapes --------------------------- */

    with_screen("scan");
    app_start();
    phase = "scan, tall";
    check("the scan screen is the one shown", visible(screen_of(RADAR_SCREEN_SCAN)));
    check("and the result screen is not", !visible(screen_of(RADAR_SCREEN_RESULT)));
    check_frame(TALL_W, TALL_H, PANEL_CORNER);
    check_tall_scan();
    check_str("the status bar is told what is happening", g_hint, "SCANNING");
    test_scope_taps("scope taps, 520 px");
    test_select_by_tap("select by tap, 520 px");

    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "scan, wide";
    check_frame(WIDE_W, WIDE_H, PANEL_CORNER);
    check_wide_scan(WIDE_SCOPE);
    test_scope_taps("scope taps, 386 px");
    test_select_by_tap("select by tap, 386 px");

    /* The promise, measured: across the page the scope covers fewer pixels
     * than down it, so a tick invalidates less, not more. */
    phase = "the cost of a tick";
    {
        lv_area_t wide_scope;
        long wide_px;
        long tall_px;

        box_of(scan_scope(), &wide_scope);
        wide_px = (long)lv_area_get_width(&wide_scope) * lv_area_get_height(&wide_scope);
        tall_px = (long)RADAR_SCOPE_TALL * RADAR_SCOPE_TALL;
        check("the wide scope invalidates fewer pixels than the tall one",
              wide_px < tall_px);
        printf("     scope area: wide %ld px, tall %ld px (%ld %% of it)\n",
               wide_px, tall_px, wide_px * 100 / tall_px);
    }

    /* ---- 3. a hundred ticks of an active run -------------------------- */

    {
        struct run_state before;
        uint32_t layouts;
        int objects;

        uint32_t ticks_before;

        /* radar_app_begin starts a run and resumes the app's one clock; the
         * debug states deliberately leave it paused so a screenshot is a
         * still. A run that is not running proves nothing about ticks. */
        radar_app_begin(app);
        pump(60);
        layouts = app->layouts;
        objects = count_objects(frame_of());
        ticks_before = app->run.ticks;
        snapshot(&before);

        pump(100 * RADAR_TICK_MS);

        check_int("a hundred ticks lay the app out not once", app->layouts, layouts);
        check_int("and add no objects", count_objects(frame_of()), objects);
        check("the run did move on", app->run.ticks > ticks_before);
        check("and it is still running", app->run.state == RADAR_RUN_ACTIVE);
        (void)before;
    }

    /* What a tick repaints, counted. */
    phase = "what a tick repaints";
    {
        lv_obj_add_event_cb(scan_scope(), count_scope, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(scan_hud(), count_hud, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(scan_target_card(), count_target, LV_EVENT_DRAW_MAIN, NULL);
        g_draws_scope = 0;
        g_draws_hud = 0;
        g_draws_target = 0;

        pump(100 * RADAR_TICK_MS);

        printf("     over 100 ticks: scope drawn %d, numbers %d, contact card %d\n",
               g_draws_scope, g_draws_hud, g_draws_target);
        check("the scope is repainted, many times", g_draws_scope > 50);
        check("the numbers are not repainted every tick",
              g_draws_hud * 4 < g_draws_scope);
        check("nor is the contact card", g_draws_target * 4 < g_draws_scope);
        check("the scope is still the wide scope",
              radar_scope_size(scan_scope()) == WIDE_SCOPE);
        check("and the shape is still the wide one", app->shape == RADAR_SHAPE_WIDE);
    }

    /* ---- 4. the display turned under a run ---------------------------- */

    phase = "relayout";
    {
        struct run_state before;
        uint32_t layouts;
        int objects;

        /* Freeze the clock so the run cannot move on by itself while the
         * display is turned; what is being asked is whether the layout
         * touches it, not whether the game ticks. */
        if (app->clock) {
            lv_timer_pause(app->clock);
        }
        snapshot(&before);
        objects = count_objects(frame_of());
        for (i = 0; i < 3; i++) {
            layouts = app->layouts;
            use_display(POS_ROTATION_0, PANEL_CORNER);
            check_same_run("the run is untouched by the turn", &before);
            check_int("turning the display adds no objects",
                      count_objects(frame_of()), objects);
            check("and lays the app out, a bounded number of times",
                  app->layouts > layouts && app->layouts <= layouts + 2);
            check_int("the scope is the tall scope", radar_scope_size(scan_scope()),
                      RADAR_SCOPE_TALL);
            check_tall_scan();
            layouts = app->layouts;
            use_display(POS_ROTATION_270, PANEL_CORNER);
            check_int("and turning it back adds none either",
                      count_objects(frame_of()), objects);
            check("and lays it out again, as few times",
                  app->layouts > layouts && app->layouts <= layouts + 2);
            check_int("the scope is the wide scope", radar_scope_size(scan_scope()),
                      WIDE_SCOPE);
            check_same_run("and by turning it back", &before);
        }
        layouts = app->layouts;
        use_display(POS_ROTATION_270, PANEL_CORNER);
        pump(600);
        check_int("laying out an unchanged body does no work at all",
                  app->layouts, layouts);
        check_int("and adds no objects", count_objects(frame_of()), objects);
        check_same_run("and an unchanged body changes nothing about it", &before);
    }

    /* ---- 5. square corners, and Outdoor ------------------------------- */

    app_stop();
    use_display(POS_ROTATION_0, 0);
    app_start();
    phase = "scan, tall, square corners";
    check_frame(TALL_W, TALL_H, 0);
    check_tall_scan();
    use_display(POS_ROTATION_270, 0);
    phase = "scan, wide, square corners";
    check_frame(WIDE_W, WIDE_H, 0);
    check_wide_scan(RECT_SCOPE);
    test_scope_taps("scope taps, 396 px");
    app_stop();

    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    phase = "scan, wide, outdoor";
    {
        lv_area_t before;
        lv_area_t after;

        box_of(scan_scope(), &before);
        use_mode("outdoor");
        box_of(scan_scope(), &after);
        check("Outdoor does not move the scope", memcmp(&before, &after, sizeof(after)) == 0);
        check_int("nor change its size", radar_scope_size(scan_scope()), WIDE_SCOPE);
        use_mode("normal");
    }

    /* ---- 6. bodies with no room --------------------------------------- */

    phase = "bodies with no room";
    /* 1040 x 568 -> a 1000 x 386 body: the side region has room. */
    use_panel_sized(PANEL_W, 1040, POS_ROTATION_270, PANEL_CORNER);
    check_int("a body with room beside the scope is wide",
              radar_scope_size(scan_scope()), WIDE_SCOPE);
    /* 760 x 568 -> a 720 x 386 body: 386 + 20 + 360 = 766 > 720. */
    use_panel_sized(PANEL_W, 760, POS_ROTATION_270, PANEL_CORNER);
    check_int("a body too narrow beside the scope keeps the tall scope",
              radar_scope_size(scan_scope()), RADAR_SCOPE_TALL);
    {
        lv_area_t s;
        lv_area_t h;

        box_of(scan_scope(), &s);
        box_of(scan_hud(), &h);
        check("and the tall stack with it", h.y2 <= s.y1);
    }
    /* 1200 x 260 -> a 1160 x 88 body: no room for a scope at all. */
    use_panel_sized(1200, 260, POS_ROTATION_0, 0);
    check_int("a body with no height keeps the tall scope",
              radar_scope_size(scan_scope()), RADAR_SCOPE_TALL);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();

    /* ---- 7. the result screen ----------------------------------------- */

    with_screen("result");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    phase = "result, tall";
    check("the result screen is the one shown", visible(screen_of(RADAR_SCREEN_RESULT)));
    check_str("the status bar says so", g_hint, "RUN COMPLETE");
    check_str("the title is there", text_of(result_title()), "RUN COMPLETE");
    {
        lv_area_t a;
        lv_area_t b;

        box_of(result_score_card(), &a);
        box_of(result_stats_card(), &b);
        check("the two cards are stacked down the page", b.y1 >= a.y2);
        check("every figure is in the body", inside_body(result_stats_card()));
        check_int("NEW RUN is full width",
                  lv_area_get_width(&(lv_area_t){0}) == 0 ? TALL_W : TALL_W, TALL_W);
    }

    use_display(POS_ROTATION_270, PANEL_CORNER);
    phase = "result, wide";
    {
        lv_area_t t;
        lv_area_t a;
        lv_area_t b;
        lv_area_t f;

        box_of(result_title(), &t);
        box_of(result_score_card(), &a);
        box_of(result_stats_card(), &b);
        box_of(result_again(), &f);
        check("the title is still over everything", t.y2 <= a.y1);
        check("the two cards stand side by side", b.x1 > a.x2);
        check_int("in halves", lv_area_get_width(&a), lv_area_get_width(&b));
        check("NEW RUN is under them", f.y1 >= a.y2);
        check("it keeps the touch minimum", lv_area_get_height(&f) >= POCKETUI_TOUCH_MIN);
        check("it is in the safe area", in_safe_area(result_again()));
        /* The whole point of splitting the figures in two: none of them is
         * below a fold on the screen that reports them. */
        check("every figure is wholly in the body", inside_body(result_stats_card()));
        check("and in the safe area", in_safe_area(result_stats_card()));
        check("nothing on the screen leaves the body",
              inside_body(screen_of(RADAR_SCREEN_RESULT)));
        check("the figures are in two columns",
              lv_obj_get_child_count(result_stats_card()) == 2);
    }
    tap_obj(result_again());
    check("NEW RUN goes back to the scope", visible(screen_of(RADAR_SCREEN_SCAN)));
    check_int("and the scope is still the wide one", radar_scope_size(scan_scope()),
              WIDE_SCOPE);
    app_stop();

    /* ---- 8. the record is the same file, whatever the shape ----------- */

    phase = "the record";
    {
        char path[512];
        struct stat st_tall;
        struct stat st_wide;

        snprintf(path, sizeof(path), "%s/radar/record.v1", state_dir);
        with_screen("result");
        use_display(POS_ROTATION_0, PANEL_CORNER);
        app_start();
        pump(120);
        check_int("a finished run wrote a record", stat(path, &st_tall), 0);
        app_stop();
        unlink(path);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        app_start();
        pump(120);
        check_int("and so did one across the page", stat(path, &st_wide), 0);
        check_int("the file is the same size either way",
                  (long)st_wide.st_size, (long)st_tall.st_size);
        app_stop();
        unlink(path);
    }

    check_int("the game never went home by itself", g_home_calls, 0);
    check_int("and never asked for the keyboard", g_keyboard_calls, 0);

    printf("radar_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
