/*
 * PocketTimber screens: the table, which is standby, the run and the
 * collapse, and the result.
 *
 * Everything here composes PocketUI role styles onto plain LVGL objects;
 * nothing names a colour or a font (tests/style_lint.sh). The one thing
 * drawn by hand is the table (timber_table.c).
 *
 * Layout, top to bottom, the reading order the design review set: the
 * numbers that persist, the tower, the piece being worked, the controls.
 * The tower is looked at, not dragged: a tap picks a block, and the pull
 * happens on a track in the thumb zone below, so the finger never covers
 * the block it is moving (PocketFleet's aim-then-confirm, approved D1).
 * Placement is aim-then-confirm too: a side button moves the ghost, PLACE
 * commits, so a mis-tap can never be what fells the tower.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../timber_app.h"

#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "timber_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shell body is the panel width less its padding on both sides. */
#define TABLE_WIDTH 528
/* The viewport: what the body has left once the HUD, the piece card and
 * the controls have their rows, each a single row so the tower gets the
 * height (docs/apps/POCKETTIMBER_ART.md, composition). */
#define TABLE_HEIGHT 672
#define METER_WIDTH 196
#define METER_SEGMENTS 10
#define METER_HEIGHT 10
#define TRACK_HEIGHT 64
#define BUTTON_HEIGHT 56

struct timber_table_ui {
    lv_obj_t *table;
    /* HUD */
    lv_obj_t *score_caption;
    lv_obj_t *score_value;
    lv_obj_t *height_value;
    lv_obj_t *segment[METER_SEGMENTS];
    /* piece card */
    lv_obj_t *piece_value;
    lv_obj_t *piece_chip;
    lv_obj_t *where_value;
    lv_obj_t *worth_value;
    lv_obj_t *tests_value;
    /* controls */
    lv_obj_t *track;
    lv_obj_t *track_label;
    lv_obj_t *sides;
    lv_obj_t *side_button[3];
    lv_obj_t *test;
    lv_obj_t *action;
    /* last values written */
    int32_t seen_score;
    int seen_state;
    int seen_turn;
    int seen_selected;
    int seen_tests;
    int seen_height;
    int seen_meter;
    int seen_ghost;
    int seen_class;
};

struct timber_result_ui {
    lv_obj_t *title;
    lv_obj_t *score;
    lv_obj_t *best;
    lv_obj_t *cause;
    lv_obj_t *height;
    lv_obj_t *pulls;
    lv_obj_t *clean;
    lv_obj_t *streak;
};

/* ---- small compositions ------------------------------------------------ */

static void button_set_enabled(lv_obj_t *button, int enabled)
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

static void button_set_text(lv_obj_t *button, const char *text)
{
    lv_obj_t *label = button ? lv_obj_get_child(button, 0) : NULL;

    if (label) {
        lv_label_set_text(label, text);
    }
}

/* A secondary button: the primary's geometry with the secondary role. */
static lv_obj_t *secondary_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *button = pocketui_button(parent, text, cb, user);

    if (button) {
        lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        pos_style_add(button, POS_STYLE_BUTTON_SECONDARY, 0);
        lv_obj_set_height(button, BUTTON_HEIGHT);
    }
    return button;
}

static lv_obj_t *band(lv_obj_t *parent, int height, lv_flex_align_t main)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *stat(lv_obj_t *parent, const char *caption, enum pos_style_role role)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(box, caption, POS_STYLE_CAPTION);
    return pocketui_label(box, "--", role);
}

static void chip_set(lv_obj_t *chip, enum pos_style_role state, const char *text)
{
    static const enum pos_style_role states[] = {
        POS_STYLE_CHIP_RX, POS_STYLE_CHIP_TX, POS_STYLE_CHIP_OFF, POS_STYLE_CHIP_NA
    };
    size_t i;

    if (!chip) {
        return;
    }
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        lv_obj_remove_style(chip, pos_style(states[i]), 0);
    }
    pos_style_add(chip, state, 0);
    lv_label_set_text(chip, text);
}

static void grouped(char *buf, size_t n, int32_t value)
{
    char plain[16];
    int len;
    int i;
    int j = 0;

    snprintf(plain, sizeof(plain), "%d", (int)(value < 0 ? -value : value));
    len = (int)strlen(plain);
    if (value < 0 && j + 1 < (int)n) {
        buf[j++] = '-';
    }
    for (i = 0; i < len && j + 1 < (int)n; i++) {
        if (i > 0 && (len - i) % 3 == 0 && j + 1 < (int)n) {
            buf[j++] = ',';
        }
        if (j + 1 < (int)n) {
            buf[j++] = plain[i];
        }
    }
    buf[j] = '\0';
}

/* ---- the table screen: actions ----------------------------------------- */

static void table_tap_cb(void *user, int id)
{
    timber_screen_table_tap(user, id);
}

void timber_screen_table_tap(struct timber_app *app, int id)
{
    if (!app || app->run.state != TIMBER_RUN_ACTIVE) {
        return;
    }
    if (id < 0) {
        timber_run_deselect(&app->run);
    } else {
        timber_run_select(&app->run, id);
    }
    timber_screen_table_refresh(app);
    if (app->table && app->table->table) {
        lv_obj_invalidate(app->table->table);
    }
}

void timber_screen_table_test(struct timber_app *app)
{
    if (!app) {
        return;
    }
    timber_run_test(&app->run);
    timber_screen_table_refresh(app);
    if (app->table && app->table->table) {
        lv_obj_invalidate(app->table->table);
    }
}

void timber_screen_table_choose(struct timber_app *app, int side)
{
    int layer;
    int slot;

    if (!app || app->run.turn != TIMBER_TURN_PLACING) {
        return;
    }
    layer = timber_tower_place_layer(&app->run.tower);
    slot = timber_view_slot_for_side(layer, side);
    if (timber_tower_can_place(&app->run.tower, slot)) {
        app->ghost = (int8_t)slot;
    }
    timber_screen_table_refresh(app);
}

void timber_screen_table_place(struct timber_app *app)
{
    if (!app || app->run.turn != TIMBER_TURN_PLACING || app->ghost < 0) {
        return;
    }
    if (timber_run_place(&app->run, app->ghost) == 0) {
        app->ghost = -1;
    }
    timber_screen_table_refresh(app);
    if (app->table && app->table->table) {
        lv_obj_invalidate(app->table->table);
    }
    if (timber_run_is_over(&app->run)) {
        timber_app_finish(app);
    }
}

static void test_cb(lv_event_t *e)
{
    timber_screen_table_test(lv_event_get_user_data(e));
}

static void action_cb(lv_event_t *e)
{
    struct timber_app *app = lv_event_get_user_data(e);

    if (!app) {
        return;
    }
    if (app->run.state == TIMBER_RUN_READY || app->run.state == TIMBER_RUN_OVER) {
        timber_app_begin(app);
    } else if (app->run.turn == TIMBER_TURN_PLACING) {
        timber_screen_table_place(app);
    }
}

static void side_cb(lv_event_t *e)
{
    lv_obj_t *button = lv_event_get_target_obj(e);
    struct timber_app *app = lv_event_get_user_data(e);
    int i;

    if (!app || !app->table) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (app->table->side_button[i] == button) {
            timber_screen_table_choose(app, i - 1);
        }
    }
}

/* Diagnostic trace of the pull path, on when POCKETTIMBER_TRACE is set in
 * the environment: every track event with the pointer's coordinates and
 * the banked travel, and every tick that pays travel with what the engine
 * did with it. Off, this costs one getenv per process. */
static int timber_trace(void)
{
    static int on = -1;

    if (on < 0) {
        on = getenv("POCKETTIMBER_TRACE") != NULL;
    }
    return on;
}

/* The pull track. The finger's horizontal travel is banked here and paid
 * into the engine once a tick by timber_screen_table_tick(), so the engine
 * sees exactly one travel per tick whatever the panel's event rate is. */
static void track_cb(lv_event_t *e)
{
    struct timber_app *app = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t point = { -1, -1 };
    const char *name = NULL;

    if (!app || !indev) {
        if (app && timber_trace()) {
            LOG_INFO("timber trace: track event %d with no active indev", (int)code);
        }
        return;
    }
    lv_indev_get_point(indev, &point);
    switch (code) {
    case LV_EVENT_PRESSED:
        app->pressing = 1;
        app->last_x = point.x;
        app->pending_px = 0;
        name = "PRESSED";
        break;
    case LV_EVENT_PRESSING:
        app->pending_px += point.x - app->last_x;
        app->last_x = point.x;
        name = "PRESSING";
        break;
    case LV_EVENT_RELEASED:
        app->pressing = 0;
        app->pending_px = 0;
        name = "RELEASED";
        break;
    case LV_EVENT_PRESS_LOST:
        app->pressing = 0;
        app->pending_px = 0;
        name = "PRESS_LOST";
        break;
    default:
        break;
    }
    if (name && timber_trace()) {
        LOG_INFO("timber trace: track %s x=%d y=%d last_x=%d pending_px=%d pressing=%d t=%u",
                 name, (int)point.x, (int)point.y, (int)app->last_x, (int)app->pending_px,
                 (int)app->pressing, (unsigned)lv_tick_get());
    }
}

void timber_screen_table_tick(struct timber_app *app)
{
    struct timber_event ev;
    int moving;

    if (!app || !app->table) {
        return;
    }
    if (app->run.state == TIMBER_RUN_ACTIVE && app->pressing && app->run.turn != TIMBER_TURN_PLACING &&
        timber_run_selected(&app->run) >= 0) {
        int id = app->run.selected;
        int layer = app->run.tower.blocks[id].layer;
        int32_t travel = timber_view_travel(&app->view, layer, app->pending_px);
        int before = app->run.tower.blocks[id].extraction;
        int pulled = timber_run_pull(&app->run, travel);

        if (timber_trace()) {
            LOG_INFO("timber trace: tick pending_px=%d block=%d layer=%d axis=%c sign=%d scale=%d travel=%d "
                     "pull=%d extraction %d -> %d turn=%d t=%u",
                     (int)app->pending_px, id, layer,
                     timber_layer_axis(layer) == TIMBER_AXIS_X ? 'x' : 'y',
                     timber_view_track_sign(layer), app->view.scale, (int)travel, pulled, before,
                     (int)app->run.tower.blocks[id].extraction, (int)app->run.turn, (unsigned)lv_tick_get());
        }
        app->pending_px = 0;
    } else if (app->pressing && timber_trace()) {
        LOG_INFO("timber trace: tick pressing but not paying: state=%d turn=%d selected=%d pending_px=%d",
                 (int)app->run.state, (int)app->run.turn, timber_run_selected(&app->run),
                 (int)app->pending_px);
    }
    timber_run_tick(&app->run);
    while (timber_run_take_event(&app->run, &ev)) {
        /* P7 shows nothing for an event that the state does not already
         * show; the shake and the sounds attach here later. */
    }
    timber_screen_table_refresh(app);
    moving = app->run.state == TIMBER_RUN_COLLAPSING || app->pressing || app->run.disturb > 0 ||
             app->run.turn == TIMBER_TURN_PULLING;
    if (moving && app->table->table) {
        lv_obj_invalidate(app->table->table);
    }
}

/* ---- the table screen: refresh ----------------------------------------- */

static void refresh_piece(struct timber_app *app)
{
    struct timber_table_ui *ui = app->table;
    const struct timber_run *run = &app->run;
    int id = timber_run_selected(run);
    const struct timber_block *b = id >= 0 ? timber_tower_block(&run->tower, id) : NULL;
    char text[24];

    if (run->state != TIMBER_RUN_ACTIVE) {
        lv_label_set_text(ui->piece_value, run->state == TIMBER_RUN_COLLAPSING ? "TIMBER" : "STANDBY");
        chip_set(ui->piece_chip, POS_STYLE_CHIP_NA, run->state == TIMBER_RUN_COLLAPSING ? "FALLING" : "CLEAR");
        lv_label_set_text(ui->where_value, "---");
        lv_label_set_text(ui->worth_value, "--");
        lv_label_set_text(ui->tests_value, "--");
        return;
    }
    if (!b) {
        lv_label_set_text(ui->piece_value, "NO PIECE");
        chip_set(ui->piece_chip, POS_STYLE_CHIP_NA, "SELECT");
        lv_label_set_text(ui->where_value, "---");
        lv_label_set_text(ui->worth_value, "--");
        lv_label_set_text_fmt(ui->tests_value, "%d", run->tests_left);
        return;
    }
    if (run->turn == TIMBER_TURN_PLACING) {
        lv_label_set_text(ui->piece_value, "IN HAND");
        chip_set(ui->piece_chip, POS_STYLE_CHIP_TX, "PLACE");
        lv_label_set_text_fmt(ui->where_value, "TOP %d", timber_tower_place_layer(&run->tower) + 1);
        lv_label_set_text(ui->worth_value, "--");
        lv_label_set_text(ui->tests_value, "--");
        return;
    }
    if (b->tested) {
        int cls = timber_run_class(run, id);

        lv_label_set_text(ui->piece_value, timber_class_name((enum timber_class)cls));
        chip_set(ui->piece_chip, cls >= TIMBER_CLASS_FIRM ? POS_STYLE_CHIP_TX : POS_STYLE_CHIP_RX,
                 run->turn == TIMBER_TURN_PULLING ? "PULLING" : "KNOWN");
    } else {
        lv_label_set_text(ui->piece_value, "UNKNOWN");
        chip_set(ui->piece_chip, POS_STYLE_CHIP_OFF,
                 run->turn == TIMBER_TURN_PULLING ? "PULLING" : (b->tell ? "LOOSE?" : "UNTESTED"));
    }
    {
        static const char *const sides[3] = { "LEFT", "CENTRE", "RIGHT" };

        lv_label_set_text_fmt(ui->where_value, "L%d %s", b->layer + 1,
                              sides[timber_view_slot_side(b->layer, b->slot) + 1]);
    }
    grouped(text, sizeof(text), timber_run_worth(run, id));
    lv_label_set_text(ui->worth_value, text);
    lv_label_set_text_fmt(ui->tests_value, "%d", run->tests_left);
}

static void refresh_meter(struct timber_table_ui *ui, int permille)
{
    int i;

    for (i = 0; i < METER_SEGMENTS; i++) {
        lv_obj_t *seg = ui->segment[i];

        lv_obj_remove_style(seg, pos_style(POS_STYLE_CHIP_RX), 0);
        lv_obj_remove_style(seg, pos_style(POS_STYLE_CHIP_OFF), 0);
        pos_style_add(seg, permille > i * (1000 / METER_SEGMENTS) ? POS_STYLE_CHIP_RX : POS_STYLE_CHIP_OFF,
                      0);
        lv_obj_set_height(seg, METER_HEIGHT);
    }
}

static void refresh_controls(struct timber_app *app)
{
    struct timber_table_ui *ui = app->table;
    const struct timber_run *run = &app->run;
    int placing = run->state == TIMBER_RUN_ACTIVE && run->turn == TIMBER_TURN_PLACING;
    int i;

    if (placing) {
        lv_obj_add_flag(ui->track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->sides, LV_OBJ_FLAG_HIDDEN);
        for (i = 0; i < 3; i++) {
            int layer = timber_tower_place_layer(&run->tower);
            int slot = timber_view_slot_for_side(layer, i - 1);
            int open = timber_tower_can_place(&run->tower, slot);

            if (open) {
                lv_obj_add_flag(ui->side_button[i], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_style(ui->side_button[i], pos_style(POS_STYLE_BUTTON_DISABLED), 0);
                pos_style_add(ui->side_button[i], POS_STYLE_BUTTON_SECONDARY, 0);
            } else {
                lv_obj_remove_flag(ui->side_button[i], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_style(ui->side_button[i], pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
                pos_style_add(ui->side_button[i], POS_STYLE_BUTTON_DISABLED, 0);
            }
            lv_obj_invalidate(ui->side_button[i]);
        }
    } else {
        lv_obj_remove_flag(ui->track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->sides, LV_OBJ_FLAG_HIDDEN);
    }
    if (run->state == TIMBER_RUN_ACTIVE) {
        int id = timber_run_selected(run);

        lv_label_set_text(ui->track_label, id >= 0 && run->turn != TIMBER_TURN_PLACING
                                               ? (timber_view_track_sign(run->tower.blocks[id].layer) > 0
                                                      ? "DRAG RIGHT TO PULL"
                                                      : "DRAG LEFT TO PULL")
                                               : "TAP A BLOCK END");
    } else {
        lv_label_set_text(ui->track_label, run->state == TIMBER_RUN_COLLAPSING ? "TIMBER" : "READY");
    }
    /* TEST is a secondary button, so its disabled look is the Design
     * System's disabled role in place of the secondary one. */
    if (run->state == TIMBER_RUN_ACTIVE && run->turn == TIMBER_TURN_SELECT &&
        timber_run_selected(run) >= 0 && run->tests_left > 0) {
        lv_obj_remove_style(ui->test, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
        pos_style_add(ui->test, POS_STYLE_BUTTON_SECONDARY, 0);
        lv_obj_add_flag(ui->test, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_style(ui->test, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
        pos_style_add(ui->test, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_remove_flag(ui->test, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(ui->test);
    if (run->state == TIMBER_RUN_READY || run->state == TIMBER_RUN_OVER) {
        button_set_text(ui->action, "BEGIN");
        button_set_enabled(ui->action, 1);
    } else if (placing) {
        button_set_text(ui->action, "PLACE");
        button_set_enabled(ui->action, app->ghost >= 0);
    } else {
        button_set_text(ui->action, run->state == TIMBER_RUN_COLLAPSING ? "TIMBER" : "PULL");
        button_set_enabled(ui->action, 0);
    }
}

void timber_screen_table_refresh(struct timber_app *app)
{
    struct timber_table_ui *ui;
    const struct timber_run *run;
    char text[24];
    int state;
    int turn;
    int selected;
    int cls;
    int meter;
    int height;

    if (!app || !app->table) {
        return;
    }
    ui = app->table;
    run = &app->run;
    state = run->state;
    turn = run->turn;
    selected = timber_run_selected(run);
    cls = selected >= 0 ? timber_run_class(run, selected) : -1;
    meter = timber_run_stability_permille(run);
    height = timber_tower_layers(&run->tower);

    /* The view follows the tower's height; the ghost follows the hand. */
    timber_view_frame(&app->view, height);
    if (turn == TIMBER_TURN_PLACING && app->ghost < 0) {
        int layer = timber_tower_place_layer(&run->tower);
        int side;

        /* Against the lean by default, the centre when the lean is nil. */
        for (side = 0; side >= -1 && app->ghost < 0; side--) {
            int slot = timber_view_slot_for_side(layer, side);

            if (timber_tower_can_place(&run->tower, slot)) {
                app->ghost = (int8_t)slot;
            }
        }
        if (app->ghost < 0) {
            int slot;

            for (slot = 0; slot < TIMBER_SLOTS && app->ghost < 0; slot++) {
                if (timber_tower_can_place(&run->tower, slot)) {
                    app->ghost = (int8_t)slot;
                }
            }
        }
    }
    if (ui->table) {
        timber_table_set_ghost(ui->table, state == TIMBER_RUN_ACTIVE && turn == TIMBER_TURN_PLACING
                                              ? app->ghost : -1);
    }

    if (run->score.points != ui->seen_score || state != ui->seen_state) {
        ui->seen_score = run->score.points;
        if (state == TIMBER_RUN_READY) {
            lv_label_set_text(ui->score_caption, "BEST");
            grouped(text, sizeof(text), (int32_t)app->records.core.best_score);
        } else {
            lv_label_set_text(ui->score_caption, "SCORE");
            grouped(text, sizeof(text), run->score.points);
        }
        lv_label_set_text(ui->score_value, text);
    }
    if (height != ui->seen_height) {
        ui->seen_height = height;
        lv_label_set_text_fmt(ui->height_value, "%d", height);
    }
    if (meter != ui->seen_meter || state != ui->seen_state) {
        ui->seen_meter = meter;
        refresh_meter(ui, meter);
    }
    /* Both comparisons are made before either block records what it saw:
     * the piece card and the controls watch the same selection and test
     * budget, and a selection alone must reach the controls (it is what
     * makes TEST answer). */
    {
        int piece_changed = selected != ui->seen_selected || turn != ui->seen_turn ||
                            state != ui->seen_state || run->tests_left != ui->seen_tests ||
                            cls != ui->seen_class;
        int controls_changed = turn != ui->seen_turn || state != ui->seen_state ||
                               selected != ui->seen_selected || app->ghost != ui->seen_ghost ||
                               run->tests_left != ui->seen_tests;

        ui->seen_selected = selected;
        ui->seen_tests = run->tests_left;
        ui->seen_class = cls;
        if (piece_changed) {
            refresh_piece(app);
        }
        if (controls_changed) {
            refresh_controls(app);
        }
    }
    ui->seen_turn = turn;
    ui->seen_state = state;
    ui->seen_ghost = app->ghost;
}

/* ---- the table screen: build ------------------------------------------- */

static void build_hud(struct timber_table_ui *ui, lv_obj_t *parent)
{
    lv_obj_t *card = pocketui_card(parent);
    lv_obj_t *row = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_t *box;
    lv_obj_t *meter;
    int i;

    /* One row: the score, the height, and the meter with its caption, so
     * the HUD is as short as a stat and the tower gets the height. */
    ui->score_value = stat(row, "SCORE", POS_STYLE_VALUE);
    ui->score_caption = lv_obj_get_child(lv_obj_get_parent(ui->score_value), 0);
    ui->height_value = stat(row, "LAYERS", POS_STYLE_VALUE);

    box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, METER_WIDTH);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(box, "STABILITY", POS_STYLE_CAPTION);
    meter = band(box, METER_HEIGHT, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(meter, 6, 0);
    for (i = 0; i < METER_SEGMENTS; i++) {
        lv_obj_t *seg = lv_obj_create(meter);

        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        pos_style_add(seg, POS_STYLE_CHIP, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_height(seg, METER_HEIGHT);
        ui->segment[i] = seg;
    }
}

static void build_piece_card(struct timber_table_ui *ui, lv_obj_t *parent)
{
    lv_obj_t *card = pocketui_card(parent);
    lv_obj_t *row = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_SPACE_BETWEEN);

    /* One row too: what the piece is, where it is, what it is worth, the
     * tests left, and the state chip. */
    ui->piece_value = stat(row, "PIECE", POS_STYLE_VALUE);
    ui->where_value = stat(row, "AT", POS_STYLE_VALUE);
    ui->worth_value = stat(row, "WORTH", POS_STYLE_VALUE);
    ui->tests_value = stat(row, "TESTS", POS_STYLE_VALUE);
    ui->piece_chip = pocketui_label(row, "CLEAR", POS_STYLE_CHIP);
    pos_style_add(ui->piece_chip, POS_STYLE_CHIP_NA, 0);
}

static void build_controls(struct timber_app *app, struct timber_table_ui *ui, lv_obj_t *parent)
{
    lv_obj_t *row;
    int i;
    static const char *const names[3] = { "LEFT", "CENTRE", "RIGHT" };

    /* The track: a slab the width of the body, the thumb's zone. */
    ui->track = lv_obj_create(parent);
    lv_obj_remove_style_all(ui->track);
    pos_style_add(ui->track, POS_STYLE_SLAB, 0);
    lv_obj_set_size(ui->track, LV_PCT(100), TRACK_HEIGHT);
    lv_obj_remove_flag(ui->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->track, track_cb, LV_EVENT_PRESSED, app);
    lv_obj_add_event_cb(ui->track, track_cb, LV_EVENT_PRESSING, app);
    lv_obj_add_event_cb(ui->track, track_cb, LV_EVENT_RELEASED, app);
    lv_obj_add_event_cb(ui->track, track_cb, LV_EVENT_PRESS_LOST, app);
    ui->track_label = pocketui_label(ui->track, "READY", POS_STYLE_CAPTION);
    lv_obj_center(ui->track_label);

    /* The sides, shown instead of the track while a block is in hand. */
    ui->sides = band(parent, BUTTON_HEIGHT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_column(ui->sides, 8, 0);
    for (i = 0; i < 3; i++) {
        ui->side_button[i] = secondary_button(ui->sides, names[i], side_cb, app);
        lv_obj_set_flex_grow(ui->side_button[i], 1);
        lv_obj_set_width(ui->side_button[i], 0);
    }
    lv_obj_add_flag(ui->sides, LV_OBJ_FLAG_HIDDEN);

    row = band(parent, BUTTON_HEIGHT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_column(row, 8, 0);
    ui->test = secondary_button(row, "TEST", test_cb, app);
    lv_obj_set_flex_grow(ui->test, 1);
    lv_obj_set_width(ui->test, 0);
    ui->action = pocketui_button(row, "BEGIN", action_cb, app);
    lv_obj_set_height(ui->action, BUTTON_HEIGHT);
    lv_obj_set_flex_grow(ui->action, 2);
    lv_obj_set_width(ui->action, 0);
}

lv_obj_t *timber_screen_table_create(struct timber_app *app, lv_obj_t *parent)
{
    lv_obj_t *screen = timber_app_screen_container(parent);
    struct timber_table_ui *ui = calloc(1, sizeof(*ui));

    if (!ui) {
        return screen;
    }
    app->table = ui;
    ui->seen_score = -1;
    ui->seen_state = -1;
    ui->seen_turn = -1;
    ui->seen_selected = -2;
    ui->seen_tests = -1;
    ui->seen_height = -1;
    ui->seen_meter = -1;
    ui->seen_ghost = -2;
    ui->seen_class = -2;

    build_hud(ui, screen);
    timber_view_init(&app->view, TABLE_WIDTH, TABLE_HEIGHT, !app->reduced_motion);
    ui->table = timber_table_create(screen, TABLE_WIDTH, TABLE_HEIGHT);
    timber_table_bind(ui->table, &app->run, &app->view);
    timber_table_set_tap(ui->table, table_tap_cb, app);
    build_piece_card(ui, screen);
    build_controls(app, ui, screen);
    return screen;
}

/* ---- the result screen --------------------------------------------------- */

static void result_again_cb(lv_event_t *e)
{
    timber_app_begin(lv_event_get_user_data(e));
}

void timber_screen_result_refresh(struct timber_app *app)
{
    struct timber_result_ui *ui;
    const struct timber_score *s;
    char text[24];
    int cause;

    if (!app || !app->result) {
        return;
    }
    ui = app->result;
    s = &app->run.score;
    cause = timber_run_cause(&app->run);
    lv_label_set_text(ui->title, cause == TIMBER_CAUSE_NONE ? "STILL STANDING" : "TIMBER");
    grouped(text, sizeof(text), s->points);
    lv_label_set_text(ui->score, text);
    /* The best is the record file's once the store works; when it does not,
     * the label says so rather than promising a memory the board lacks. */
    if (app->new_best) {
        lv_label_set_text(ui->best, app->store_ok ? "NEW BEST" : "NEW BEST THIS SESSION");
    } else {
        grouped(text, sizeof(text), (int32_t)app->records.core.best_score);
        if (app->store_ok) {
            lv_label_set_text_fmt(ui->best, "BEST %s", text);
        } else {
            lv_label_set_text_fmt(ui->best, "BEST %s THIS SESSION", text);
        }
    }
    switch (cause) {
    case TIMBER_CAUSE_TIP:
        lv_label_set_text_fmt(ui->cause, "Layer %d gave way: its support was pulled out", app->run.hinge + 1);
        break;
    case TIMBER_CAUSE_JOLT:
        lv_label_set_text_fmt(ui->cause, "Layer %d gave way after a jolt", app->run.hinge + 1);
        break;
    case TIMBER_CAUSE_PLACEMENT:
        lv_label_set_text_fmt(ui->cause, "Layer %d gave way under the last block placed", app->run.hinge + 1);
        break;
    case TIMBER_CAUSE_SWAY:
        lv_label_set_text_fmt(ui->cause, "Layer %d gave way as the tower swayed", app->run.hinge + 1);
        break;
    default:
        lv_label_set_text(ui->cause, "Every block is on top and the tower stands");
        break;
    }
    lv_label_set_text_fmt(ui->height, "%u", (unsigned)s->height);
    lv_label_set_text_fmt(ui->pulls, "%u", (unsigned)s->pulls);
    lv_label_set_text_fmt(ui->clean, "%u", (unsigned)s->clean);
    lv_label_set_text_fmt(ui->streak, "%u", (unsigned)s->best_streak);
}

lv_obj_t *timber_screen_result_create(struct timber_app *app, lv_obj_t *parent)
{
    lv_obj_t *screen = timber_app_screen_container(parent);
    struct timber_result_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *card;

    if (!ui) {
        return screen;
    }
    app->result = ui;

    ui->title = pocketui_label(screen, "TIMBER", POS_STYLE_TITLE);

    card = pocketui_card(screen);
    lv_obj_set_style_pad_row(card, 6, 0);
    pocketui_label(card, "FINAL SCORE", POS_STYLE_CAPTION);
    ui->score = pocketui_label(card, "0", POS_STYLE_HERO_48);
    ui->best = pocketui_label(card, " ", POS_STYLE_CAPTION);
    ui->cause = pocketui_label(card, " ", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->cause, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->cause, LV_PCT(100));

    card = pocketui_card(screen);
    ui->height = pocketui_kv_row(card, "Layers reached", "0");
    ui->pulls = pocketui_kv_row(card, "Blocks pulled", "0");
    ui->clean = pocketui_kv_row(card, "Clean pulls", "0");
    ui->streak = pocketui_kv_row(card, "Best streak", "0");
    lv_obj_remove_style(lv_obj_get_parent(ui->streak), pos_style(POS_STYLE_DIVIDER), 0);

    pocketui_button(screen, "AGAIN", result_again_cb, app);
    return screen;
}
