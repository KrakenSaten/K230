/*
 * PocketTimber input path in the running app, driven through a real LVGL
 * pointer device: the sequence that failed on the K230 during D3 (candidate 1,
 * 2026-09-09). Begin a run, select a block, TEST it, drag it part way out on
 * the track, let go before it slips, and then everything must still answer:
 * a second drag keeps pulling, a drag that starts with a vertical wobble (the
 * body scrolls under the finger) keeps pulling, a finger that wanders off
 * the track and comes back wedges nothing, and leaving the app (with a block
 * part way out, or even mid-press) and reopening it gives a fresh run whose
 * BEGIN, selection, TEST and track all answer.
 *
 * The defect this caught: after a tap selected a block, TEST stayed
 * unclickable, because the table screen's refresh recorded the selection for
 * the piece card before it asked whether the controls had to follow it.
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
#include "pocketui.h"
#include "timber_app.h"
#include "engine/timber_pull.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define TABLE_W 528
#define TABLE_H 700

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
    lv_obj_t *label = find_label(root, texts, 1);

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

/* A pullable block with no stiction, so travel moves it at once. */
static int choose_block(const struct timber_app *app)
{
    int id;

    for (id = 0; id < TIMBER_BLOCKS; id++) {
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

/* NULL-safe: a missing widget is a failed check, not a halted test. */
static int clickable(lv_obj_t *obj)
{
    return obj != NULL && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
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
    id = choose_block(app);
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
static void test_answers(struct timber_app *app, lv_obj_t *test, int id, const char *when)
{
    char what[96];

    snprintf(what, sizeof(what), "%s, TEST is clickable once a block is selected", when);
    check(what, clickable(test));
    tap(test);
    snprintf(what, sizeof(what), "%s, TEST answers and costs a test", when);
    check(what, app->run.tower.blocks[id].tested && app->run.tests_left == 1);
}

int main(void)
{
    static uint8_t draw_buf[PANEL_W * 40 * 2];
    lv_display_t *disp;
    lv_indev_t *indev;
    struct timber_app *app;
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

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    pocketui_init();

    /* The shell's content area: everything below the status bar. */
    content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), PANEL_H - POCKETUI_STATUS_BAR_H);
    lv_obj_set_pos(content, 0, POCKETUI_STATUS_BAR_H);
    pump(60);

    /* 1. The run as played on the bench: begin, select, test, then a pull
     *    that stops part way. The whole session's travel stays short of the
     *    slip point, so the block is part way out throughout. */
    app = open_app();
    check("the app opens", app != NULL);
    if (!app) {
        printf("timber_input_test: %d checks, %d failure(s)\n", checks, failed);
        return 1;
    }
    check("the app opens in standby", app->run.state == TIMBER_RUN_READY);
    check("TEST is not clickable with nothing selected", !clickable(find_button(body, "TEST")));
    id = begin_and_select(app, &track, &table, &test);
    if (id < 0) {
        printf("timber_input_test: %d checks, %d failure(s)\n", checks, failed);
        return 1;
    }
    test_answers(app, test, id, "in a fresh run");

    sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
    drag_track(track, sign, 4, 3, 1);
    e1 = extraction_of(app, id);
    check("a short drag draws the block part way out", e1 > 0 && e1 < TIMBER_SLIP_AT);
    check("a block part way out locks the turn to it", app->run.turn == TIMBER_TURN_PULLING &&
                                                          timber_run_selected(&app->run) == id);
    check("while the block is part way out TEST is not clickable", !clickable(test));
    pump(300);
    check("the block stays where the finger left it", extraction_of(app, id) == e1);

    /* 2. What was reported dead on the bench: after that, the track. */
    drag_track(track, sign, 4, 2, 1);
    e2 = extraction_of(app, id);
    check("a second drag keeps pulling the same block", e2 > e1 && e2 < TIMBER_SLIP_AT);

    /* 3. A drag that starts with a vertical wobble past LVGL's scroll limit:
     *    the body scrolls under the finger, and the pull must go on. */
    centre_of(track, &tx, &ty);
    press_at(tx, ty);
    move_to(tx + sign * 2, ty + 14);
    move_to(tx + sign * 6, ty + 14);
    move_to(tx + sign * 10, ty + 14);
    release();
    pump(600);
    e1 = extraction_of(app, id);
    check("a drag with a vertical wobble still pulls", e1 > e2 && e1 < TIMBER_SLIP_AT);

    /* 4. A thumb that wanders off the track mid-pull and comes back. */
    x = drag_track(track, sign, 4, 1, 0);
    centre_of(track, &tx, &ty);
    move_to(x, ty - 140);               /* off the track, onto the piece card */
    move_to(x + sign * 30, ty - 140);
    move_to(x + sign * 30, ty);         /* back on it, still pressed */
    move_to(x + sign * 36, ty);
    release();
    e2 = extraction_of(app, id);
    check("a finger that left the track did not push the block anywhere odd", e2 >= e1 && e2 < TIMBER_SLIP_AT);
    drag_track(track, sign, 4, 2, 1);
    check("after the wander a fresh drag pulls again", extraction_of(app, id) > e2 && extraction_of(app, id) < TIMBER_SLIP_AT);
    check("the action button stays disabled while pulling",
          !clickable(find_button(body, "PULL")));

    /* 5. Leave with the block part way out; reopen: a fresh run must answer. */
    close_app(app);
    app = open_app();
    check("the app reopens in standby", app && app->run.state == TIMBER_RUN_READY);
    id = begin_and_select(app, &track, &table, &test);
    check("after reopening, BEGIN and selection answer", id >= 0);
    if (id >= 0) {
        test_answers(app, test, id, "after reopening");
        sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
        drag_track(track, sign, 4, 3, 1);
        check("after reopening, the track pulls", extraction_of(app, id) > 0);
    }

    /* 6. Leave mid-press: the finger is still on the track when the app goes. */
    if (id >= 0) {
        drag_track(track, sign, 4, 1, 0);
    }
    close_app(app);
    app = open_app();
    release();
    id = begin_and_select(app, &track, &table, &test);
    check("after leaving mid-press, BEGIN and selection answer", id >= 0);
    if (id >= 0) {
        test_answers(app, test, id, "after leaving mid-press");
        sign = timber_view_track_sign(app->run.tower.blocks[id].layer);
        drag_track(track, sign, 4, 3, 1);
        check("after leaving mid-press, the track pulls", extraction_of(app, id) > 0);
    }
    close_app(app);

    printf("timber_input_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
