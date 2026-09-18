/*
 * PocketRadar screens: the scan screen, which is both standby and the run,
 * and the result screen.
 *
 * Everything here composes PocketUI role styles onto plain LVGL objects.
 * Nothing names a colour or a font, so the app follows the current PocketOS
 * theme and mode exactly as the shell does and tests/style_lint.sh stays
 * green. The one thing drawn by hand is the scope, which reaches the tokens
 * through pos_theme_color() in its own draw callback (radar_scope.c).
 *
 * Layout follows the PocketRadar visual kit: the persistent numbers sit in a
 * strip above the scope, and the contact being worked sits directly below it
 * and directly above ENGAGE. The first pass had the score between the two,
 * which put a number the player only glances at between the thing they are
 * looking at and the thing they are about to press.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../radar_app.h"

#include "pocketui.h"
#include "radar_scope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One segment per leaker the sector can absorb, so the bar counts the lives
 * the player has left rather than an abstract percentage. */
#define INTEGRITY_SEGMENTS (RADAR_INTEGRITY_MAX / RADAR_INTEGRITY_MISS)
#define METER_HEIGHT 10

struct radar_scan_ui {
    lv_obj_t *scope;
    /* Everything but the scope, and the boxes that hold it. Down the page the
     * boxes are transparent and stack their children at the panel gap, so the
     * screen reads exactly as the single column it was; across the page side
     * carries the two cards abreast with ENGAGE under them, and the scope
     * moves out of cards to stand first in the row. */
    lv_obj_t *side;
    lv_obj_t *cards;
    lv_obj_t *hud;
    lv_obj_t *target_card;
    /* HUD strip */
    lv_obj_t *score_caption;
    lv_obj_t *score_value;
    lv_obj_t *streak_value;
    lv_obj_t *level_value;
    lv_obj_t *segment[INTEGRITY_SEGMENTS];
    /* target card */
    lv_obj_t *target_value;
    lv_obj_t *target_chip;
    lv_obj_t *bearing_value;
    lv_obj_t *range_value;
    lv_obj_t *third_caption;
    lv_obj_t *third_value;
    lv_obj_t *action;
    /* Last values written, so a 20 Hz clock does not rewrite the strip when
     * nothing behind it has changed. */
    int32_t seen_score;
    uint32_t seen_target;
    int seen_lock;
    int seen_state;
    int seen_armed;
    int seen_integrity;
    int seen_streak;
    int seen_level;
};

struct radar_result_ui {
    /* The outcome stays at the top in both shapes; across the page the two
     * cards stand side by side and the one way on sits under them. */
    lv_obj_t *cards;
    lv_obj_t *score_card;
    lv_obj_t *stats_card;
    /* The five figures, in two boxes so the card can hold them in one column
     * down the page and two across it. Five rows of a Design System row are
     * taller than a landscape body, and a results screen is the last place
     * anything should have to be scrolled to. */
    lv_obj_t *stats_a;
    lv_obj_t *stats_b;
    lv_obj_t *foot;
    lv_obj_t *again;
    lv_obj_t *score;
    lv_obj_t *best;
    lv_obj_t *engaged;
    lv_obj_t *mistakes;
    lv_obj_t *missed;
    lv_obj_t *streak;
    lv_obj_t *level;
};

/* ---- small compositions ------------------------------------------------ */

/* Enable or disable a button made by pocketui_button(): swaps the primary
 * role for the Design System's disabled role and takes away the click. */
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

/* Horizontal band inside a card. */
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

/* A caption above a value, which is how every number on both screens is
 * presented. Returns the value label. */
static lv_obj_t *stat(lv_obj_t *parent, const char *caption, enum pos_style_role role)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_t *value;

    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(box, caption, POS_STYLE_CAPTION);
    value = pocketui_label(box, "--", role);
    return value;
}

/* Chip states share the geometry style and differ only in the state style,
 * the way the shell's radio chip does. */
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

/* 12,480 rather than 12480. Five figures is where a good run ends up, and
 * the grouping is the difference between reading a score and counting it. */
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

/* ---- scan screen ------------------------------------------------------- */

static void scan_tap_cb(void *user, int bearing, int range)
{
    radar_screen_scan_tap(user, bearing, range);
}

static void scan_action_cb(lv_event_t *e)
{
    struct radar_app *app = lv_event_get_user_data(e);

    if (!app) {
        return;
    }
    if (app->run.state == RADAR_RUN_ACTIVE) {
        radar_screen_scan_engage(app);
    } else {
        radar_app_begin(app);
    }
}

void radar_screen_scan_tap(struct radar_app *app, int bearing, int range)
{
    uint32_t id;

    if (!app || app->run.state != RADAR_RUN_ACTIVE) {
        return;
    }
    id = radar_run_pick(&app->run, bearing, range);
    if (id == RADAR_NO_CONTACT) {
        /* Open water clears the selection. Tapping away from everything is
         * the only way to abandon a lock deliberately. */
        radar_run_deselect(&app->run);
    } else {
        radar_run_select(&app->run, id);
    }
    radar_screen_scan_refresh(app);
    if (app->scan && app->scan->scope) {
        lv_obj_invalidate(app->scan->scope);
    }
}

void radar_screen_scan_engage(struct radar_app *app)
{
    struct radar_scan_ui *ui;
    const struct radar_contact *target;
    int bearing;
    int range;
    enum radar_engage result;

    if (!app || !app->scan) {
        return;
    }
    ui = app->scan;
    target = radar_run_selected(&app->run);
    if (!target) {
        return;
    }
    bearing = target->bearing;
    range = target->range;
    result = radar_run_engage(&app->run);
    if (result == RADAR_ENGAGE_INVALID) {
        return;
    }
    radar_scope_flash(ui->scope,
                      result == RADAR_ENGAGE_HIT ? RADAR_FLASH_HIT : RADAR_FLASH_FOUL,
                      bearing, range);
    radar_screen_scan_refresh(app);
    lv_obj_invalidate(ui->scope);
    if (radar_run_is_over(&app->run)) {
        radar_app_finish(app);
    }
}

void radar_screen_scan_tick(struct radar_app *app)
{
    struct radar_event ev;

    if (!app || !app->scan) {
        return;
    }
    radar_run_tick(&app->run);
    while (radar_run_take_event(&app->run, &ev)) {
        /* A lost track is the only event the tick itself has to show; the
         * rest are either already visible on the scope or reported by the
         * action that caused them. */
        if (ev.type == RADAR_EVENT_FADED && ev.value < 0) {
            radar_scope_flash(app->scan->scope, RADAR_FLASH_FADED, ev.bearing,
                              ev.range);
        }
    }
    radar_screen_scan_refresh(app);
    lv_obj_invalidate(app->scan->scope);
}

/* The three columns of the target card, and the chip that says what to do
 * about the contact. */
static void refresh_target(struct radar_app *app, const struct radar_contact *target)
{
    struct radar_scan_ui *ui = app->scan;
    char bearing[RADAR_BEARING_NAME_MAX];
    char text[16];

    if (!target) {
        lv_label_set_text(ui->target_value,
                          app->run.state == RADAR_RUN_ACTIVE ? "NO CONTACT" : "STANDBY");
        chip_set(ui->target_chip, POS_STYLE_CHIP_NA,
                 app->run.state == RADAR_RUN_ACTIVE ? "SELECT" : "CLEAR");
        lv_label_set_text(ui->bearing_value, "---");
        lv_label_set_text(ui->range_value, "---");
        lv_label_set_text(ui->third_caption, "LOCK");
        lv_label_set_text(ui->third_value, "--");
        return;
    }
    radar_bearing_name(target->bearing, bearing, sizeof(bearing));
    lv_label_set_text(ui->bearing_value, bearing);
    lv_label_set_text_fmt(ui->range_value, "%03d", target->range);

    if (target->state != RADAR_CONTACT_ACQUIRED) {
        /* The class stays hidden until the lock completes: that is the whole
         * of what makes a decoy cost anything. */
        lv_label_set_text(ui->target_value,
                          target->classified
                              ? radar_class_name((enum radar_class)target->cls)
                              : "UNKNOWN");
        chip_set(ui->target_chip, POS_STYLE_CHIP_NA, "ACQUIRING");
        lv_label_set_text(ui->third_caption, "LOCK");
        lv_label_set_text_fmt(ui->third_value, "%d%%",
                              radar_contact_lock_permille(target) / 10);
        return;
    }
    lv_label_set_text(ui->target_value, radar_class_name((enum radar_class)target->cls));
    if (radar_class_is_target((enum radar_class)target->cls)) {
        chip_set(ui->target_chip, POS_STYLE_CHIP_TX, "THREAT");
        lv_label_set_text(ui->third_caption, "WORTH");
        grouped(text, sizeof(text), radar_run_engage_value(&app->run));
        lv_label_set_text(ui->third_value, text);
    } else {
        /* No colour role in the Design System says "warning" on a chip, so
         * the word does the work, backed by the crossed diamond on the scope
         * and by what the third column now reads. */
        chip_set(ui->target_chip, POS_STYLE_CHIP_OFF, "DECOY");
        lv_label_set_text(ui->third_caption, "ACTION");
        lv_label_set_text(ui->third_value, "HOLD");
    }
}

/* One segment per leaker left. A spent segment drops to the neutral chip
 * fill, which is one RGB565 step from the background (DS feasibility H1), so
 * what the player actually sees is the bar getting shorter - which is the
 * right reading. */
static void refresh_integrity(struct radar_scan_ui *ui, int integrity)
{
    int i;

    for (i = 0; i < INTEGRITY_SEGMENTS; i++) {
        lv_obj_t *seg = ui->segment[i];

        lv_obj_remove_style(seg, pos_style(POS_STYLE_CHIP_RX), 0);
        lv_obj_remove_style(seg, pos_style(POS_STYLE_CHIP_OFF), 0);
        pos_style_add(seg, integrity > i * RADAR_INTEGRITY_MISS ? POS_STYLE_CHIP_RX
                                                                : POS_STYLE_CHIP_OFF, 0);
        lv_obj_set_height(seg, METER_HEIGHT);
    }
}

void radar_screen_scan_refresh(struct radar_app *app)
{
    struct radar_scan_ui *ui;
    const struct radar_contact *target;
    char text[24];
    int armed;
    int lock;
    int state;
    int streak;
    int level;
    uint32_t id;

    if (!app || !app->scan) {
        return;
    }
    ui = app->scan;
    target = radar_run_selected(&app->run);
    id = target ? target->id : RADAR_NO_CONTACT;
    lock = target ? radar_contact_lock_permille(target) : -1;
    state = app->run.state;
    streak = app->run.score.streak;
    level = radar_run_level(&app->run);

    if (id != ui->seen_target || lock != ui->seen_lock || state != ui->seen_state) {
        ui->seen_target = id;
        ui->seen_lock = lock;
        refresh_target(app, target);
    }

    if (app->run.score.points != ui->seen_score || state != ui->seen_state) {
        ui->seen_score = app->run.score.points;
        if (state == RADAR_RUN_ACTIVE) {
            lv_label_set_text(ui->score_caption, "SCORE");
            grouped(text, sizeof(text), app->run.score.points);
        } else {
            /* Standby shows what there is to beat, in the same place the
             * live score will appear, so the number never moves. */
            lv_label_set_text(ui->score_caption, "BEST");
            grouped(text, sizeof(text), (int32_t)app->record.best_score);
        }
        lv_label_set_text(ui->score_value, text);
    }

    if (streak != ui->seen_streak || state != ui->seen_state) {
        int steps = streak > RADAR_SCORE_STREAK_CAP ? RADAR_SCORE_STREAK_CAP : streak;
        int tenths = 10 + RADAR_SCORE_STREAK_STEP * steps;

        ui->seen_streak = streak;
        lv_label_set_text_fmt(ui->streak_value, "x%d.%d", tenths / 10, tenths % 10);
    }
    if (level != ui->seen_level || state != ui->seen_state) {
        ui->seen_level = level;
        lv_label_set_text_fmt(ui->level_value, "%d", level);
    }
    if (app->run.score.integrity != ui->seen_integrity || state != ui->seen_state) {
        ui->seen_integrity = app->run.score.integrity;
        refresh_integrity(ui, app->run.score.integrity);
    }
    ui->seen_state = state;

    armed = target && target->state == RADAR_CONTACT_ACQUIRED;
    if (state != RADAR_RUN_ACTIVE) {
        armed = 1;
    }
    if (armed != ui->seen_armed) {
        ui->seen_armed = armed;
        button_set_enabled(ui->action, armed);
    }
    button_set_text(ui->action, state == RADAR_RUN_ACTIVE ? "ENGAGE" : "BEGIN SCAN");
}

static lv_obj_t *build_hud(struct radar_scan_ui *ui, lv_obj_t *parent)
{
    lv_obj_t *card = pocketui_card(parent);
    lv_obj_t *row = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_t *meter;
    lv_obj_t *box;
    int i;

    lv_obj_set_style_pad_row(card, 14, 0);
    ui->score_value = stat(row, "SCORE", POS_STYLE_VALUE);
    ui->score_caption = lv_obj_get_child(lv_obj_get_parent(ui->score_value), 0);
    ui->streak_value = stat(row, "STREAK", POS_STYLE_VALUE);
    ui->level_value = stat(row, "LEVEL", POS_STYLE_VALUE);

    /* Sector integrity as a bar rather than a percentage: it is the number
     * that ends the run, and five blocks are read at a glance where "80%"
     * has to be thought about. */
    box = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(box, 6, 0);
    pocketui_label(box, "SECTOR INTEGRITY", POS_STYLE_CAPTION);
    meter = band(box, METER_HEIGHT, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(meter, 6, 0);
    for (i = 0; i < INTEGRITY_SEGMENTS; i++) {
        /* A plain object, not a label: an empty label is content-sized to
         * nothing and the first attempt drew a caption above no bar at all. */
        lv_obj_t *seg = lv_obj_create(meter);

        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        /* The chip base carries the fill opacity and the radius; the state
         * roles carry only colours, so a segment needs both exactly as the
         * shell status chip does. The geometry it also carries is for a
         * 36 px chip, so height and padding are overridden locally. */
        pos_style_add(seg, POS_STYLE_CHIP, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_height(seg, METER_HEIGHT);
        ui->segment[i] = seg;
    }
    return card;
}

static lv_obj_t *build_target_card(struct radar_scan_ui *ui, lv_obj_t *parent)
{
    lv_obj_t *card = pocketui_card(parent);
    lv_obj_t *head = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_t *cols;

    lv_obj_set_style_pad_row(card, 14, 0);
    ui->target_value = pocketui_label(head, "STANDBY", POS_STYLE_VALUE);
    ui->target_chip = pocketui_label(head, "CLEAR", POS_STYLE_CHIP);
    pos_style_add(ui->target_chip, POS_STYLE_CHIP_NA, 0);

    cols = band(card, LV_SIZE_CONTENT, LV_FLEX_ALIGN_SPACE_BETWEEN);
    ui->bearing_value = stat(cols, "BEARING", POS_STYLE_VALUE);
    ui->range_value = stat(cols, "RANGE", POS_STYLE_VALUE);
    ui->third_value = stat(cols, "LOCK", POS_STYLE_VALUE);
    ui->third_caption = lv_obj_get_child(lv_obj_get_parent(ui->third_value), 0);
    return card;
}

lv_obj_t *radar_screen_scan_create(struct radar_app *app, lv_obj_t *parent)
{
    lv_obj_t *screen = radar_app_screen_container(parent);
    struct radar_scan_ui *ui = calloc(1, sizeof(*ui));

    if (!ui) {
        return screen;
    }
    app->scan = ui;
    ui->seen_target = RADAR_NO_CONTACT;
    ui->seen_lock = -1;
    ui->seen_state = -1;
    ui->seen_score = -1;
    ui->seen_armed = -1;
    ui->seen_integrity = -1;
    ui->seen_streak = -1;
    ui->seen_level = -1;

    /* Reading order down the page, as it always was: the numbers that
     * persist, the scope, the contact being worked, the action. The boxes
     * around them are transparent and add nothing to that order. */
    ui->side = radar_app_box(screen);
    ui->cards = radar_app_box(ui->side);
    ui->hud = build_hud(ui, ui->cards);

    ui->scope = radar_scope_create(ui->cards, RADAR_SCOPE_TALL);
    radar_scope_bind(ui->scope, &app->run);
    radar_scope_set_motion(ui->scope, !app->reduced_motion);
    radar_scope_set_tap(ui->scope, scan_tap_cb, app);

    ui->target_card = build_target_card(ui, ui->cards);
    ui->action = pocketui_button(ui->side, "BEGIN SCAN", scan_action_cb, app);
    return screen;
}

/* The scope is what the player watches, so across the page it takes the
 * height and stands first; the two cards read off it go abreast beside it,
 * and ENGAGE takes the whole foot of that region - it is hit often and in a
 * hurry, and a wide target is the point of having the room.
 *
 * The scope is moved between the card column and the screen, never rebuilt:
 * down the page it belongs between the numbers and the contact card, which is
 * the v0.0.10 order, and across the page it belongs before both. */
void radar_screen_scan_relayout(struct radar_app *app, int wide, int scope)
{
    struct radar_scan_ui *ui = app ? app->scan : NULL;

    if (!ui) {
        return;
    }
    radar_app_screen_flow(app->screen[RADAR_SCREEN_SCAN], wide, 1);
    radar_scope_set_size(ui->scope, scope);
    if (lv_obj_get_parent(ui->scope) != (wide ? app->screen[RADAR_SCREEN_SCAN]
                                              : ui->cards)) {
        lv_obj_set_parent(ui->scope, wide ? app->screen[RADAR_SCREEN_SCAN] : ui->cards);
        lv_obj_move_to_index(ui->scope, wide ? 0 : 1);
    }
    radar_app_box_column(ui->side, wide);
    radar_app_box_split(ui->cards, wide);
    lv_obj_set_flex_grow(ui->side, wide ? 1 : 0);
    /* The cards take the height ENGAGE leaves, and share the width. */
    lv_obj_set_flex_grow(ui->cards, wide ? 1 : 0);
    lv_obj_set_height(ui->cards, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->hud, wide ? 1 : 0);
    lv_obj_set_flex_grow(ui->target_card, wide ? 1 : 0);
    lv_obj_set_height(ui->hud, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_height(ui->target_card, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
}

/* ---- result screen ----------------------------------------------------- */

static void result_again_cb(lv_event_t *e)
{
    radar_app_begin(lv_event_get_user_data(e));
}

void radar_screen_result_refresh(struct radar_app *app)
{
    struct radar_result_ui *ui;
    const struct radar_score *s;
    char text[24];

    if (!app || !app->result) {
        return;
    }
    ui = app->result;
    s = &app->run.score;
    grouped(text, sizeof(text), s->points);
    lv_label_set_text(ui->score, text);
    if (app->new_best) {
        lv_label_set_text(ui->best, "NEW BEST SCORE");
    } else {
        grouped(text, sizeof(text), (int32_t)app->record.best_score);
        if (!app->storage_ok) {
            lv_label_set_text_fmt(ui->best, "BEST %s THIS SESSION", text);
        } else {
            lv_label_set_text_fmt(ui->best, "BEST %s", text);
        }
    }
    lv_label_set_text_fmt(ui->engaged, "%u", (unsigned)s->engaged);
    lv_label_set_text_fmt(ui->mistakes, "%u", (unsigned)s->mistakes);
    lv_label_set_text_fmt(ui->missed, "%u", (unsigned)s->missed);
    lv_label_set_text_fmt(ui->streak, "%u", (unsigned)s->best_streak);
    lv_label_set_text_fmt(ui->level, "%d", radar_run_level(&app->run));
}

lv_obj_t *radar_screen_result_create(struct radar_app *app, lv_obj_t *parent)
{
    lv_obj_t *screen = radar_app_screen_container(parent);
    struct radar_result_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *card;

    if (!ui) {
        return screen;
    }
    app->result = ui;

    pocketui_label(screen, "RUN COMPLETE", POS_STYLE_TITLE);

    ui->cards = radar_app_box(screen);
    card = pocketui_card(ui->cards);
    ui->score_card = card;
    lv_obj_set_style_pad_row(card, 6, 0);
    pocketui_label(card, "FINAL SCORE", POS_STYLE_CAPTION);
    ui->score = pocketui_label(card, "0", POS_STYLE_HERO_48);
    ui->best = pocketui_label(card, " ", POS_STYLE_CAPTION);

    card = pocketui_card(ui->cards);
    ui->stats_card = card;
    ui->stats_a = radar_app_box(card);
    ui->stats_b = radar_app_box(card);
    /* Rows inside a card sit against each other, dividers and all. */
    lv_obj_set_style_pad_row(ui->stats_a, 0, 0);
    lv_obj_set_style_pad_row(ui->stats_b, 0, 0);
    ui->engaged = pocketui_kv_row(ui->stats_a, "Targets engaged", "0");
    ui->mistakes = pocketui_kv_row(ui->stats_a, "Decoys engaged", "0");
    ui->missed = pocketui_kv_row(ui->stats_a, "Tracks lost", "0");
    ui->streak = pocketui_kv_row(ui->stats_b, "Best streak", "0");
    ui->level = pocketui_kv_row(ui->stats_b, "Level reached", "0");
    /* The last row sits on the card's own bottom padding, so its divider
     * would draw a line to nowhere. */
    lv_obj_remove_style(lv_obj_get_parent(ui->level), pos_style(POS_STYLE_DIVIDER), 0);

    ui->foot = radar_app_box(screen);
    ui->again = pocketui_button(ui->foot, "NEW RUN", result_again_cb, app);
    return screen;
}

/* The outcome over everything, the two accounts of it side by side, and the
 * one way on under them - the same shape the other landscape screens take. */
void radar_screen_result_relayout(struct radar_app *app, int wide)
{
    struct radar_result_ui *ui = app ? app->result : NULL;

    if (!ui) {
        return;
    }
    /* The screen keeps its column: the heading stays over everything. */
    radar_app_screen_flow(app->screen[RADAR_SCREEN_RESULT], wide, 0);
    radar_app_box_split(ui->cards, wide);
    radar_app_box_split(ui->foot, wide);
    lv_obj_set_flex_grow(ui->cards, wide ? 1 : 0);
    lv_obj_set_height(ui->cards, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->foot, 0);
    lv_obj_set_height(ui->foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->score_card, wide ? 1 : 0);
    lv_obj_set_flex_grow(ui->stats_card, wide ? 1 : 0);
    lv_obj_set_width(ui->score_card, wide ? LV_PCT(50) : LV_PCT(100));
    lv_obj_set_width(ui->stats_card, wide ? LV_PCT(50) : LV_PCT(100));
    lv_obj_set_height(ui->score_card, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_height(ui->stats_card, wide ? LV_PCT(100) : LV_SIZE_CONTENT);

    /* The figures go into two columns inside their card across the page. In
     * one column the last row of the first three would carry a divider into
     * the gutter beside it, a line to nowhere, so it loses it there and has
     * it back down the page. */
    lv_obj_set_flex_flow(ui->stats_card, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(ui->stats_card, RADAR_COL_GAP, 0);
    radar_app_box_column(ui->stats_a, wide);
    radar_app_box_column(ui->stats_b, wide);
    lv_obj_set_height(ui->stats_a, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->stats_b, LV_SIZE_CONTENT);
    if (wide) {
        lv_obj_remove_style(lv_obj_get_parent(ui->missed), pos_style(POS_STYLE_DIVIDER), 0);
    } else {
        pos_style_add(lv_obj_get_parent(ui->missed), POS_STYLE_DIVIDER, 0);
    }
}
