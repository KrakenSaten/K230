/*
 * PocketTimber input path in the running app, driven through a real LVGL
 * pointer device: the sequence that failed on the K230 during D3 (candidate 1,
 * 2026-09-09) and what a player does around it. Begin a run, select a block,
 * TEST it, draw it part way out on the track with a finger's uneven steps,
 * let go before it slips, pull it again, push it back into its seat, TEST
 * again, select another block, leave the app with a block part way out or
 * mid-press, reopen it, and everything must answer: BEGIN, the selection,
 * TEST, the track, and pushing back. The finger also wanders off the track
 * and back, and drags with a vertical wobble that scrolls the body.
 *
 * Two defects this caught:
 * - after a tap selected a block, TEST stayed unclickable, because the table
 *   screen's refresh recorded the selection for the piece card before it
 *   asked whether the controls had to follow it;
 * - pushing a block back only unlocked the turn when the extraction landed
 *   on exactly zero, which a finger never does: the block sat in its seat,
 *   locked, with TEST and every other selection refused.
 *
 * Candidate 2's bench report of drags that moved nothing turned out to be
 * test procedure, not software: the block was being dragged on the table,
 * where the design only takes taps, instead of on the pull track (the owner,
 * 2026-09-09, after dragging on the track worked on the K230). The drift
 * sections and the evdev section came out of that investigation and stay
 * as coverage: a thumb whose contact drifts out of the 64 px band keeps the
 * press (LVGL locks the press to the object by default), and LVGL's real
 * evdev parser, fed the GT9895's protocol-B event grammar through a pipe,
 * delivers the press, the motion and non-zero banked travel to the track,
 * and the engine moves the block.
 *
 * The scroll section measures, without failing, whether a tap with a vertical
 * wobble reaches a button under the shell's scrollable body: the screen as
 * built and with the body's paddings removed, with and without elastic
 * scrolling. Before the viewport was cut to what the body has left, the
 * screen overflowed the body by 28 px and such a tap was taken by the scroll.
 *
 * The app is hosted the way the shell hosts it (app root, header, the
 * scrollable body) so that scrolling and hit-testing are the ones the panel
 * sees. Needs LVGL, so it is built by ui/shell/CMakeLists.txt next to the
 * shell (host builds only) and run by tests/timber_shell_test.sh. A display
 * with a no-op flush is enough: nothing is rendered.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketui.h"
#include "timber_app.h"
#include "engine/timber_pull.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if LV_USE_EVDEV
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#endif

#define PANEL_W 568
#define PANEL_H 1232
#define TABLE_W 528
/* The status bar the shell gives Timber in portrait (ui/shell/chrome.h, DS
 * section 30): none, since Timber is fullscreen. */
#define STATUS_H chrome_height(chrome_resolve(app_timber.chrome, false, false))
/* The viewport is 672 px in the 1060 px body under the 56 px bar it was
 * measured under (DS §7, retired by §36), and takes whatever a taller body
 * has beyond that (timber_screens.c). */
#define MEASURED_UNDER_BAR_H 56
#define TABLE_H (672 + MEASURED_UNDER_BAR_H - STATUS_H)

extern const struct pocketos_app app_timber;

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

/* ---- the shell services the app calls --------------------------------- */

static char status_hint[32];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(status_hint, sizeof(status_hint), "%s", text ? text : "");
}

int pocketos_shell_reduced_motion(void)
{
    return 0;
}

void pocketos_shell_go_home(void)
{
}

/* ---- the display and the finger --------------------------------------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static lv_indev_t *finger;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

/* Run LVGL for this long: the pointer is read every LV_DEF_REFR_PERIOD and
 * the app's clock ticks every TIMBER_TICK_MS, as in the shell. */
static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void press_at(int x, int y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
}

static void move_to(int x, int y)
{
    finger_point.x = x;
    finger_point.y = y;
    pump(50);
}

static void release(void)
{
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void centre_of(lv_obj_t *obj, int *x, int *y)
{
    lv_area_t a;

    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(obj, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

static void tap(lv_obj_t *obj)
{
    int x;
    int y;

    centre_of(obj, &x, &y);
    press_at(x, y);
    release();
}

/* A tap whose finger rolls this many pixels down before it lifts. */
static void wobbly_tap(lv_obj_t *obj, int dy)
{
    int x;
    int y;

    centre_of(obj, &x, &y);
    press_at(x, y);
    move_to(x, y + dy / 2);
    move_to(x, y + dy);
    release();
}

/* ---- finding the widgets the player sees ------------------------------ */

static lv_obj_t *find_label(lv_obj_t *root, const char *const *texts, int n)
{
    uint32_t i;
    int k;

    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *t = lv_label_get_text(root);

        for (k = 0; k < n; k++) {
            if (t && strcmp(t, texts[k]) == 0) {
                return root;
            }
        }
        return NULL;
    }
    for (i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *hit = find_label(lv_obj_get_child(root, i), texts, n);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* A button by its label. */
static lv_obj_t *find_button(lv_obj_t *root, const char *text)
{
    const char *const texts[1] = { text };
    lv_obj_t *label = root ? find_label(root, texts, 1) : NULL;

    return label ? lv_obj_get_parent(label) : NULL;
}

/* The pull track: the slab that carries the track caption. */
static lv_obj_t *find_track(lv_obj_t *root)
{
    static const char *const captions[] = { "READY", "TAP A BLOCK END", "DRAG RIGHT TO PULL",
                                            "DRAG LEFT TO PULL", "TIMBER" };
    lv_obj_t *label = find_label(root, captions, 5);

    return label ? lv_obj_get_parent(label) : NULL;
}

static lv_obj_t *find_table(lv_obj_t *root)
{
    uint32_t i;

    lv_obj_update_layout(lv_screen_active());
    if (lv_obj_get_width(root) == TABLE_W && lv_obj_get_height(root) == TABLE_H) {
        return root;
    }
    for (i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *hit = find_table(lv_obj_get_child(root, i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* NULL-safe: a missing widget is a failed check, not a halted test. */
static int clickable(lv_obj_t *obj)
{
    return obj != NULL && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- hosting the app the way the shell does ---------------------------- */

static lv_obj_t *content;
static lv_obj_t *app_root;
static lv_obj_t *body;

static struct timber_app *open_app(void)
{
    lv_obj_t *header;
    struct timber_app *app;

    app_root = lv_obj_create(content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);

    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);

    body = lv_obj_create(app_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_set_style_pad_row(body, POCKETUI_PAD, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    app = app_timber.create(body);
    pump(120);
    return app;
}

static void close_app(struct timber_app *app)
{
    app_timber.destroy(app);
    lv_obj_delete(app_root);
    app_root = NULL;
    body = NULL;
    pump(60);
}

/* ---- the play ----------------------------------------------------------- */

/* A pullable block with no stiction, so travel moves it at once; the one
 * after `after`, so a second choice differs from the first. */
static int choose_block(const struct timber_app *app, int after)
{
    int id;

    for (id = after + 1; id < TIMBER_BLOCKS; id++) {
        if (timber_tower_pullable(&app->run.tower, id) &&
            timber_pull_stiction((enum timber_class)timber_run_class(&app->run, id)) == 0) {
            return id;
        }
    }
    return -1;
}

static void tap_block_end(struct timber_app *app, lv_obj_t *table, int id)
{
    struct timber_shape s;
    lv_area_t a;

    if (timber_view_block(&app->view, &app->run, id, &s) != 0) {
        return;
    }
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(table, &a);
    press_at(a.x1 + s.end_x, a.y1 + s.end_y);
    release();
}

static int extraction_of(const struct timber_app *app, int id)
{
    return app->run.tower.blocks[id].extraction;
}

/* Press on the track and draw the finger sideways in the sense the block's
 * layer needs, one step per tick; the finger stays down unless told to
 * let go. Returns the finger's x. */
static int drag_track(lv_obj_t *track, int sign, int px_per_step, int steps, int let_go)
{
    int x;
    int y;
    int i;

    centre_of(track, &x, &y);
    press_at(x, y);
    for (i = 0; i < steps; i++) {
        x += sign * px_per_step;
        move_to(x, y);
    }
    if (let_go) {
        release();
    }
    return x;
}

/* A finger's steps are never equal: 5, 3, 6 pixels. */
static void drag_track_uneven(lv_obj_t *track, int sign)
{
    static const int steps[3] = { 5, 3, 6 };
    int x;
    int y;
    int i;

    centre_of(track, &x, &y);
    press_at(x, y);
    for (i = 0; i < 3; i++) {
        x += sign * steps[i];
        move_to(x, y);
    }
    release();
}

/* Push the selected block back toward its seat in 4 px steps, as a thumb
 * would, until the engine says it is no longer out. Returns the steps used,
 * or -1 when it never came back. */
static int push_back(struct timber_app *app, lv_obj_t *track, int sign, int id)
{
    int x;
    int y;
    int i;

    centre_of(track, &x, &y);
    press_at(x, y);
    for (i = 1; i <= 40; i++) {
        x -= sign * 4;
        move_to(x, y);
        if (extraction_of(app, id) <= 0) {
            release();
            return i;
        }
    }
    release();
    return -1;
}

static int begin_and_select(struct timber_app *app, lv_obj_t **track, lv_obj_t **table, lv_obj_t **test)
{
    lv_obj_t *begin = find_button(body, "BEGIN");
    int id;

    check("BEGIN is on the screen", begin != NULL);
    if (!begin) {
        return -1;
    }
    tap(begin);
    check("BEGIN starts the run", app->run.state == TIMBER_RUN_ACTIVE);
    *track = find_track(body);
    *table = find_table(body);
    *test = find_button(body, "TEST");
    check("the track, the table and TEST are on the screen", *track && *table && *test);
    if (!*track || !*table || !*test) {
        return -1;
    }
    /* The viewport takes exactly what the fullscreen body has beyond the
     * other rows, so the body has nothing to scroll: a body that did would
     * take a vertical drag on the table for itself (timber_screens.c). */
    lv_obj_update_layout(body);
    check("the portrait body is filled exactly: nothing to scroll",
          lv_obj_get_scroll_bottom(body) <= 0 && lv_obj_get_scroll_top(body) <= 0);
    id = choose_block(app, -1);
    check("a loose pullable block exists", id >= 0);
    if (id < 0) {
        return -1;
    }
    tap_block_end(app, *table, id);
    check("a tap on a block end selects it", timber_run_selected(&app->run) == id);
    return id;
}

/* TEST is the first thing a player presses after a selection, and the one
 * that stayed dead on the bench. */
static void test_answers(struct timber_app *app, lv_obj_t *test, int id, int tests_after, const char *when)
{
    char what[96];

    snprintf(what, sizeof(what), "%s, TEST is clickable with a block selected", when);
    check(what, clickable(test));
    tap(test);
    snprintf(what, sizeof(what), "%s, TEST answers and costs a test", when);
    check(what, app->run.tower.blocks[id].tested && app->run.tests_left == tests_after);
}

/* A fresh instance: nothing of the last run, and nothing of the last finger. */
static void fresh_instance(struct timber_app *app, const char *when)
{
    char what[96];
    int id;
    int out = 0;

    snprintf(what, sizeof(what), "%s, the app opens in standby with nothing selected", when);
    check(what, app && app->run.state == TIMBER_RUN_READY && app->run.turn == TIMBER_TURN_SELECT &&
                    timber_run_selected(&app->run) == -1 && timber_run_held(&app->run) == -1);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        out += extraction_of(app, id) != 0;
    }
    snprintf(what, sizeof(what), "%s, no block is part way out", when);
    check(what, out == 0);
    snprintf(what, sizeof(what), "%s, the track holds no press and no travel", when);
    check(what, app->pressing == 0 && app->pending_px == 0);
    snprintf(what, sizeof(what), "%s, the pointer is released and scrolls nothing", when);
    check(what, lv_indev_get_state(finger) == LV_INDEV_STATE_RELEASED && lv_indev_get_scroll_obj(finger) == NULL);
    snprintf(what, sizeof(what), "%s, TEST is not clickable with nothing selected", when);
    check(what, !clickable(find_button(body, "TEST")));
    /* The screen must fit the body: a body with something to scroll takes
     * every tap or drag that rolls more than LVGL's scroll limit (10 px)
     * for itself, and a thumb on the bottom row rolls. */
    lv_obj_update_layout(lv_screen_active());
    snprintf(what, sizeof(what), "%s, the screen fits the body with nothing to scroll (%d px over)", when,
             (int)lv_obj_get_scroll_bottom(body));
    check(what, lv_obj_get_scroll_bottom(body) <= 0);
}

/* What the track sees, recorded after the app's own handlers ran. */
static struct {
    int pressed;
    int pressing;
    int released;
    int lost;
    int max_pending;
    int min_x;
    int max_x;
} seen;

static void track_probe(lv_event_t *e)
{
    struct timber_app *app = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;

    lv_indev_get_point(indev, &pt);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        seen.pressed++;
        seen.min_x = pt.x;
        seen.max_x = pt.x;
        break;
    case LV_EVENT_PRESSING:
        seen.pressing++;
        if (pt.x < seen.min_x) {
            seen.min_x = pt.x;
        }
        if (pt.x > seen.max_x) {
            seen.max_x = pt.x;
        }
        if (abs(app->pending_px) > seen.max_pending) {
            seen.max_pending = abs(app->pending_px);
        }
        break;
    case LV_EVENT_RELEASED:
        seen.released++;
        break;
    case LV_EVENT_PRESS_LOST:
        seen.lost++;
        break;
    default:
        break;
    }
}

/* ---- the sequences ------------------------------------------------------ */

/* 1. The bench sequence, from BEGIN to reopening. */
static void bench_sequence(void)
{
    struct timber_app *app = open_app();
    lv_obj_t *track = NULL;
    lv_obj_t *table = NULL;
    lv_obj_t *test = NULL;
    int id;
    int other;
    int sign;
    int e1;
    int e2;
    int steps;

    check("the app opens", app != NULL);
    if (!app) {
        return;
    }
    fresh_instance(app, "at first");
    id = begin_and_select(app, &track, &table, &test);
    if (id < 0) {
        close_app(app);
        return;
    }
    test_answers(app, test, id, 1, "in a fresh run");
    sign = timber_view_track_sign(app->run.tower.blocks[id].layer);

    /* Partial extraction with a finger's uneven steps, then let go. */
    drag_track_uneven(track, sign);
    e1 = extraction_of(app, id);
    check("an uneven drag draws the block part way out", e1 > 0 && e1 < TIMBER_SLIP_AT);
    check("a block part way out locks the turn to it", app->run.turn == TIMBER_TURN_PULLING &&
                                                          timber_run_selected(&app->run) == id);
    check("while it is out TEST and the action button are disabled", !clickable(test) && !clickable(find_button(body, "PULL")));
    other = choose_block(app, id);
    check("another loose block exists", other >= 0);
    if (other >= 0) {
        tap_block_end(app, table, other);
        check("while it is out a tap on another block end is refused", timber_run_selected(&app->run) == id);
    }
    pump(300);
    check("the block stays where the finger left it", extraction_of(app, id) == e1);

    /* Dragged again. */
    drag_track(track, sign, 4, 2, 1);
    e2 = extraction_of(app, id);
    check("a second drag keeps pulling the same block", e2 > e1 && e2 < TIMBER_SLIP_AT);

    /* Pushed back into its seat. */
    steps = push_back(app, track, sign, id);
    check("pushing back brings the block to its seat", steps > 0 && extraction_of(app, id) == 0);
    check("a block pushed back into its seat unlocks the turn", app->run.turn == TIMBER_TURN_SELECT);
    check("after the push back TEST is clickable again", clickable(test));
    tap(test);
    check("after the push back TEST answers (the block is known, no test is spent)",
          app->run.tower.blocks[id].tested && app->run.tests_left == 1);
    if (other >= 0) {
        tap_block_end(app, table, other);
        check("after the push back another block can be selected", timber_run_selected(&app->run) == other);
        test_answers(app, test, other, 0, "on the second block");
        tap_block_end(app, table, id);
        check("the first block can be selected again", timber_run_selected(&app->run) == id);
    }

    /* Out again, and left there. */
    drag_track(track, sign, 4, 3, 1);
    check("the block can be drawn out again", extraction_of(app, id) > 0 && app->run.turn == TIMBER_TURN_PULLING);
    close_app(app);

    /* Reopened. */
    app = open_app();
    fresh_instance(app, "after leaving with a block part way out");
    id = begin_and_select(app, &track, &table, &test);
    check("after reopening, BEGIN and selection answer", id >= 0);
    if (id >= 0) {
        test_answers(app, test, id, 1, "after reopening");
        sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
        drag_track_uneven(track, sign);
        check("after reopening, the track pulls", extraction_of(app, id) > 0);
        steps = push_back(app, track, sign, id);
        check("after reopening, pushing back seats the block and unlocks the turn",
              steps > 0 && extraction_of(app, id) == 0 && app->run.turn == TIMBER_TURN_SELECT && clickable(test));
    }
    close_app(app);
}

/* 2. A wandering finger, a wobbling drag, and leaving mid-press. */
static void wander_wobble_and_mid_press(void)
{
    struct timber_app *app = open_app();
    lv_obj_t *track = NULL;
    lv_obj_t *table = NULL;
    lv_obj_t *test = NULL;
    int id;
    int sign;
    int e1;
    int e2;
    int x;
    int tx;
    int ty;

    id = app ? begin_and_select(app, &track, &table, &test) : -1;
    if (id < 0) {
        if (app) {
            close_app(app);
        }
        return;
    }
    sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
    drag_track(track, sign, 4, 2, 1);
    e1 = extraction_of(app, id);

    /* A drag that starts with a vertical wobble past LVGL's scroll limit:
     * the body scrolls under the finger, and the pull must go on. */
    centre_of(track, &tx, &ty);
    press_at(tx, ty);
    move_to(tx + sign * 2, ty + 14);
    move_to(tx + sign * 6, ty + 14);
    move_to(tx + sign * 10, ty + 14);
    release();
    pump(600);
    e2 = extraction_of(app, id);
    check("a drag with a vertical wobble still pulls", e2 > e1 && e2 < TIMBER_SLIP_AT);

    /* A thumb pad is taller than the 64 px track and its contact drifts as it
     * starts to drag, so the finger is off the track within the first reads.
     * The track keeps the press (LVGL's default press lock on objects) and
     * the pull goes on from the finger's x. */
    memset(&seen, 0, sizeof(seen));
    lv_obj_add_event_cb(track, track_probe, LV_EVENT_ALL, app);
    centre_of(track, &tx, &ty);
    press_at(tx, ty);
    move_to(tx + sign * 2, ty + 40);   /* off the band at once */
    move_to(tx + sign * 6, ty + 44);
    move_to(tx + sign * 10, ty + 44);
    move_to(tx + sign * 14, ty + 40);
    release();
    lv_obj_remove_event_cb(track, track_probe);
    printf("note: thumb drift off the track: PRESSED %d, PRESSING %d, PRESS_LOST %d, RELEASED %d, extraction %d -> %d\n",
           seen.pressed, seen.pressing, seen.lost, seen.released, e2, extraction_of(app, id));
    check("a thumb that drifts off the track keeps the press (LVGL locks it by default)", seen.lost == 0 && seen.released == 1);
    check("a thumb that drifts off the track keeps pulling", extraction_of(app, id) > e2 && extraction_of(app, id) < TIMBER_SLIP_AT);
    e1 = extraction_of(app, id);
    /* And a finger that wanders far off and comes back: the x travel counts
     * the whole way, nothing is handed to the piece card or the buttons. */
    x = drag_track(track, sign, 4, 1, 0);
    move_to(x, ty - 140);
    move_to(x + sign * 8, ty - 140);
    move_to(x + sign * 8, ty);
    move_to(x + sign * 12, ty);
    release();
    check("a finger that wanders far off the track and back keeps pulling", extraction_of(app, id) > e1 && extraction_of(app, id) < TIMBER_SLIP_AT);
    check("nothing else answered the wandering finger", app->run.turn == TIMBER_TURN_PULLING && timber_run_selected(&app->run) == id);
    e1 = extraction_of(app, id);
    drag_track(track, sign, 4, 2, 1);
    check("after the wander a fresh drag pulls again", extraction_of(app, id) > e1 && extraction_of(app, id) < TIMBER_SLIP_AT);

    /* Leave mid-press: the finger is still on the track when the app goes. */
    drag_track(track, sign, 4, 1, 0);
    close_app(app);
    app = open_app();
    release();
    fresh_instance(app, "after leaving mid-press");
    id = begin_and_select(app, &track, &table, &test);
    check("after leaving mid-press, BEGIN and selection answer", id >= 0);
    if (id >= 0) {
        test_answers(app, test, id, 1, "after leaving mid-press");
        sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
        drag_track(track, sign, 4, 3, 1);
        check("after leaving mid-press, the track pulls", extraction_of(app, id) > 0);
    }
    close_app(app);
}

/* 3. Does a tap with a vertical wobble reach BEGIN under the scrollable body?
 *    Measured with the screen as built, with the body's paddings taken away,
 *    and with elastic scrolling off. Reported, not judged; a clean tap must
 *    always answer. */
static void gesture_stealing(void)
{
    static const struct {
        int overflow;
        int elastic;
        const char *name;
    } variants[] = {
        { 1, 1, "as built, elastic scroll" },
        { 0, 1, "paddings removed, elastic scroll" },
        { 1, 0, "as built, elastic off" },
        { 0, 0, "paddings removed, elastic off" },
    };
    size_t v;

    for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        struct timber_app *app = open_app();
        lv_obj_t *begin;
        int started_by_wobble;
        int32_t bottom;

        if (!app) {
            return;
        }
        if (!variants[v].overflow) {
            lv_obj_set_style_pad_top(body, 0, 0);
            lv_obj_set_style_pad_bottom(body, 0, 0);
        }
        if (!variants[v].elastic) {
            lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);
        }
        pump(60);
        lv_obj_update_layout(lv_screen_active());
        bottom = lv_obj_get_scroll_bottom(body);
        begin = find_button(body, "BEGIN");
        wobbly_tap(begin, 12);
        started_by_wobble = app->run.state == TIMBER_RUN_ACTIVE;
        printf("note: %s: body can scroll %d px; a tap rolling 12 px %s BEGIN\n", variants[v].name, (int)bottom,
               started_by_wobble ? "reaches" : "is taken from");
        if (!started_by_wobble) {
            tap(begin);
            check("a clean tap reaches BEGIN under every body variant", app->run.state == TIMBER_RUN_ACTIVE);
        }
        close_app(app);
    }
}

/* 4. The K230's own input grammar through LVGL's real evdev parser. The
 *    GT9895 driver (goodix_berlin, protocol B, no pointer emulation) reports
 *    a touch as: tracking id, tool type, MT position x, y, touch major,
 *    BTN_TOUCH, SYN_REPORT; a move as the changed MT positions and
 *    SYN_REPORT; a release as tracking id -1, BTN_TOUCH 0, SYN_REPORT. Those
 *    frames go down a pipe into lv_evdev_create_fd, calibrated the way the
 *    device is (raw 0..1059 x 0..2399 onto 568 x 1232), and the track is
 *    instrumented: it must see the press, see motion with non-zero banked
 *    travel, and the engine must move the block. */
#if LV_USE_EVDEV
#define GT_MAX_X 1059
#define GT_MAX_Y 2399

static int gt_fd = -1;
static int gt_last_x;
static int gt_last_y;
static int gt_id;

static void gt_event(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    if (write(gt_fd, &e, sizeof(e)) != (ssize_t)sizeof(e)) {
        printf("FAIL cannot write an input event\n");
        failed++;
    }
}

/* Display pixel to the controller's raw coordinate, inverting LVGL's
 * calibration (v * (out_max - out_min) / (in_max - in_min)). */
static int gt_raw_x(int px)
{
    return (px * GT_MAX_X + (PANEL_W - 1) / 2) / (PANEL_W - 1);
}

static int gt_raw_y(int px)
{
    return (px * GT_MAX_Y + (PANEL_H - 1) / 2) / (PANEL_H - 1);
}

static void gt_down(int px, int py)
{
    gt_last_x = gt_raw_x(px);
    gt_last_y = gt_raw_y(py);
    gt_event(EV_ABS, ABS_MT_TRACKING_ID, ++gt_id);
    gt_event(EV_ABS, ABS_MT_TOOL_TYPE, 0);
    gt_event(EV_ABS, ABS_MT_POSITION_X, gt_last_x);
    gt_event(EV_ABS, ABS_MT_POSITION_Y, gt_last_y);
    gt_event(EV_ABS, ABS_MT_TOUCH_MAJOR, 20);
    gt_event(EV_KEY, BTN_TOUCH, 1);
    gt_event(EV_SYN, SYN_REPORT, 0);
    pump(8);
}

static void gt_move(int px, int py)
{
    int rx = gt_raw_x(px);
    int ry = gt_raw_y(py);

    if (rx != gt_last_x) {
        gt_event(EV_ABS, ABS_MT_POSITION_X, rx);
    }
    if (ry != gt_last_y) {
        gt_event(EV_ABS, ABS_MT_POSITION_Y, ry);
    }
    gt_last_x = rx;
    gt_last_y = ry;
    gt_event(EV_SYN, SYN_REPORT, 0);
    pump(8);
}

static void gt_up(void)
{
    gt_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
    gt_event(EV_KEY, BTN_TOUCH, 0);
    gt_event(EV_SYN, SYN_REPORT, 0);
    pump(60);
}

static void gt_tap(lv_obj_t *obj)
{
    int x;
    int y;

    centre_of(obj, &x, &y);
    gt_down(x, y);
    pump(50);
    gt_up();
}

static void k230_grammar(void)
{
    int pipefd[2];
    lv_indev_t *gt;
    struct timber_app *app;
    lv_obj_t *track = NULL;
    lv_obj_t *table = NULL;
    lv_obj_t *test = NULL;
    lv_obj_t *begin;
    int id;
    int sign;
    int x;
    int y;
    int i;
    int e0;

    if (pipe(pipefd) != 0) {
        check("a pipe for the controller's events", 0);
        return;
    }
    gt_fd = pipefd[1];
    gt = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, pipefd[0]);
    check("LVGL's evdev driver accepts the pipe", gt != NULL);
    if (!gt) {
        return;
    }
    lv_evdev_set_calibration(gt, 0, 0, GT_MAX_X, GT_MAX_Y);

    app = open_app();
    begin = find_button(body, "BEGIN");
    gt_tap(begin);
    check("through evdev, a tap on BEGIN starts the run", app && app->run.state == TIMBER_RUN_ACTIVE);
    track = find_track(body);
    table = find_table(body);
    test = find_button(body, "TEST");
    id = choose_block(app, -1);
    if (!track || !table || !test || id < 0) {
        check("through evdev, the widgets and a loose block are found", 0);
        close_app(app);
        return;
    }
    {
        struct timber_shape s;
        lv_area_t a;

        timber_view_block(&app->view, &app->run, id, &s);
        lv_obj_update_layout(lv_screen_active());
        lv_obj_get_coords(table, &a);
        gt_down(a.x1 + s.end_x, a.y1 + s.end_y);
        pump(50);
        gt_up();
    }
    check("through evdev, a tap on a block end selects it", timber_run_selected(&app->run) == id);
    gt_tap(test);
    check("through evdev, TEST answers", app->run.tower.blocks[id].tested && app->run.tests_left == 1);

    /* The drag, one frame every 8 ms like the controller, 2 px per frame. */
    memset(&seen, 0, sizeof(seen));
    lv_obj_add_event_cb(track, track_probe, LV_EVENT_ALL, app);
    sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
    e0 = extraction_of(app, id);
    centre_of(track, &x, &y);
    gt_down(x, y);
    for (i = 1; i <= 12; i++) {
        gt_move(x + sign * 2 * i, y);
    }
    gt_up();
    printf("note: evdev drag on the track: PRESSED %d, PRESSING %d, RELEASED %d, PRESS_LOST %d, x seen %d..%d, banked travel up to %d px, extraction %d -> %d\n",
           seen.pressed, seen.pressing, seen.released, seen.lost, seen.min_x, seen.max_x, seen.max_pending, e0,
           extraction_of(app, id));
    check("the track receives the press", seen.pressed == 1);
    check("the track receives continuous motion", seen.pressing >= 4 && seen.max_x - seen.min_x >= 16);
    check("the banked travel is non-zero", seen.max_pending > 0);
    check("the engine moved the block (move_block ran)", extraction_of(app, id) > e0 && app->run.turn == TIMBER_TURN_PULLING);
    check("the track receives the release and never lost the press", seen.released == 1 && seen.lost == 0);

    /* The same through evdev with the thumb's drift: off the band by the
     * second frame, still pulling. */
    memset(&seen, 0, sizeof(seen));
    e0 = extraction_of(app, id);
    centre_of(track, &x, &y);
    gt_down(x, y);
    for (i = 1; i <= 12; i++) {
        gt_move(x + sign * 2 * i, y + (i < 4 ? 14 * i : 42));
    }
    gt_up();
    printf("note: evdev drag with a drifting thumb: PRESSED %d, PRESSING %d, PRESS_LOST %d, extraction %d -> %d\n",
           seen.pressed, seen.pressing, seen.lost, e0, extraction_of(app, id));
    check("through evdev, a drifting thumb keeps the press", seen.pressed == 1 && seen.lost == 0);
    check("through evdev, a drifting thumb keeps pulling", extraction_of(app, id) > e0 && extraction_of(app, id) < TIMBER_SLIP_AT);
    lv_obj_remove_event_cb(track, track_probe);
    close_app(app);
    lv_indev_delete(gt);
    close(pipefd[1]);
}
#else
static void k230_grammar(void)
{
    printf("note: LV_USE_EVDEV is off in this build; the K230 input grammar is not exercised\n");
}
#endif

int main(void)
{
    static uint8_t draw_buf[PANEL_W * 40 * 2];
    lv_display_t *disp;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pocketui_init();

    /* The shell's content area: everything below the status bar the shell
     * gives this app, which for fullscreen Timber is none. */
    content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), PANEL_H - STATUS_H);
    lv_obj_set_pos(content, 0, STATUS_H);
    pump(60);

    bench_sequence();
    wander_wobble_and_mid_press();
    gesture_stealing();
    k230_grammar();

    printf("timber_input_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
