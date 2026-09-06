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
 * Reading order on the scan screen follows what the player needs: the scope,
 * then the contact being worked, then the score, then ENGAGE.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../radar_app.h"

#include "pocketui.h"
#include "radar_scope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shell body is the panel width less its padding on both sides. */
#define SCOPE_SIZE 520

struct radar_scan_ui {
    lv_obj_t *scope;
    lv_obj_t *target_caption;
    lv_obj_t *target_value;
    lv_obj_t *target_detail;
    lv_obj_t *score_caption;
    lv_obj_t *score_value;
    lv_obj_t *score_detail;
    lv_obj_t *action;
    /* Last values written, so a 20 Hz clock does not rewrite six labels that
     * have not changed. */
    int32_t seen_score;
    uint32_t seen_target;
    int seen_lock;
    int seen_state;
    int seen_armed;
};

struct radar_result_ui {
    lv_obj_t *score;
    lv_obj_t *best;
    lv_obj_t *engaged;
    lv_obj_t *mistakes;
    lv_obj_t *missed;
    lv_obj_t *streak;
    lv_obj_t *level;
};

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

/* A card with a caption, a value line and a detail line: the only panel
 * shape either screen needs. */
static lv_obj_t *readout(lv_obj_t *parent, const char *caption, enum pos_style_role value,
                         lv_obj_t **caption_out, lv_obj_t **value_out,
                         lv_obj_t **detail_out)
{
    lv_obj_t *card = pocketui_card(parent);

    *caption_out = pocketui_label(card, caption, POS_STYLE_CAPTION);
    *value_out = pocketui_label(card, "--", value);
    *detail_out = pocketui_label(card, " ", POS_STYLE_CAPTION);
    lv_obj_set_style_pad_row(card, 6, 0);
    return card;
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

void radar_screen_scan_refresh(struct radar_app *app)
{
    struct radar_scan_ui *ui;
    const struct radar_contact *target;
    char detail[64];
    char bearing[RADAR_BEARING_NAME_MAX];
    int armed;
    int lock;
    int state;
    uint32_t id;

    if (!app || !app->scan) {
        return;
    }
    ui = app->scan;
    target = radar_run_selected(&app->run);
    id = target ? target->id : RADAR_NO_CONTACT;
    lock = target ? radar_contact_lock_permille(target) : -1;
    state = app->run.state;

    /* The target readout. It changes only when the contact, its lock or the
     * run state does, which at 20 Hz is most of the time worth checking. */
    if (id != ui->seen_target || lock != ui->seen_lock || state != ui->seen_state) {
        ui->seen_target = id;
        ui->seen_lock = lock;
        if (!target) {
            lv_label_set_text(ui->target_value,
                              state == RADAR_RUN_ACTIVE ? "NO CONTACT" : "STANDBY");
            lv_label_set_text(ui->target_detail,
                              state == RADAR_RUN_ACTIVE ? "TAP A RETURN TO SELECT"
                                                        : "SECTOR CLEAR");
        } else if (target->state == RADAR_CONTACT_ACQUIRED) {
            radar_bearing_name(target->bearing, bearing, sizeof(bearing));
            lv_label_set_text(ui->target_value, radar_class_name((enum radar_class)target->cls));
            if (radar_class_is_target((enum radar_class)target->cls)) {
                snprintf(detail, sizeof(detail), "BRG %s   RNG %03d   WORTH %d",
                         bearing, target->range,
                         (int)radar_run_engage_value(&app->run));
            } else {
                snprintf(detail, sizeof(detail), "BRG %s   RNG %03d   DO NOT ENGAGE",
                         bearing, target->range);
            }
            lv_label_set_text(ui->target_detail, detail);
        } else {
            radar_bearing_name(target->bearing, bearing, sizeof(bearing));
            /* The class stays hidden until the lock completes: that is the
             * whole of what makes a decoy cost anything. */
            lv_label_set_text(ui->target_value,
                              target->classified
                                  ? radar_class_name((enum radar_class)target->cls)
                                  : "UNKNOWN");
            snprintf(detail, sizeof(detail), "BRG %s   RNG %03d   LOCK %d%%", bearing,
                     target->range, lock / 10);
            lv_label_set_text(ui->target_detail, detail);
        }
    }

    /* The score readout, which stands in for the best score while standby. */
    if (app->run.score.points != ui->seen_score || state != ui->seen_state) {
        ui->seen_score = app->run.score.points;
        if (state == RADAR_RUN_ACTIVE) {
            lv_label_set_text(ui->score_caption, "SCORE");
            lv_label_set_text_fmt(ui->score_value, "%d", (int)app->run.score.points);
            snprintf(detail, sizeof(detail), "STREAK x%d.%d   LVL %d   SECTOR %d%%",
                     (10 + 2 * (app->run.score.streak > RADAR_SCORE_STREAK_CAP
                                    ? RADAR_SCORE_STREAK_CAP
                                    : app->run.score.streak)) / 10,
                     (10 + 2 * (app->run.score.streak > RADAR_SCORE_STREAK_CAP
                                    ? RADAR_SCORE_STREAK_CAP
                                    : app->run.score.streak)) % 10,
                     radar_run_level(&app->run), app->run.score.integrity);
        } else {
            lv_label_set_text(ui->score_caption, "BEST SCORE");
            lv_label_set_text_fmt(ui->score_value, "%u",
                                  (unsigned)app->record.best_score);
            if (app->record.runs == 0) {
                snprintf(detail, sizeof(detail), "NO RUNS YET");
            } else {
                snprintf(detail, sizeof(detail), "%u RUNS   %u ENGAGED   LVL %u",
                         (unsigned)app->record.runs, (unsigned)app->record.engaged,
                         (unsigned)app->record.best_level);
            }
        }
        lv_label_set_text(ui->score_detail, detail);
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

    ui->scope = radar_scope_create(screen, SCOPE_SIZE);
    radar_scope_bind(ui->scope, &app->run);
    radar_scope_set_motion(ui->scope, !app->reduced_motion);
    radar_scope_set_tap(ui->scope, scan_tap_cb, app);

    readout(screen, "TARGET", POS_STYLE_VALUE, &ui->target_caption, &ui->target_value,
            &ui->target_detail);
    readout(screen, "SCORE", POS_STYLE_HERO_40, &ui->score_caption, &ui->score_value,
            &ui->score_detail);

    ui->action = pocketui_button(screen, "BEGIN SCAN", scan_action_cb, app);
    return screen;
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

    if (!app || !app->result) {
        return;
    }
    ui = app->result;
    s = &app->run.score;
    lv_label_set_text_fmt(ui->score, "%d", (int)s->points);
    if (app->new_best) {
        lv_label_set_text(ui->best, "NEW BEST SCORE");
    } else if (!app->storage_ok) {
        lv_label_set_text_fmt(ui->best, "BEST %u THIS SESSION",
                              (unsigned)app->record.best_score);
    } else {
        lv_label_set_text_fmt(ui->best, "BEST %u", (unsigned)app->record.best_score);
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
    lv_obj_t *caption;
    lv_obj_t *detail;

    if (!ui) {
        return screen;
    }
    app->result = ui;

    pocketui_label(screen, "RUN COMPLETE", POS_STYLE_TITLE);
    readout(screen, "FINAL SCORE", POS_STYLE_HERO_48, &caption, &ui->score, &ui->best);

    card = pocketui_card(screen);
    ui->engaged = pocketui_kv_row(card, "Targets engaged", "0");
    ui->mistakes = pocketui_kv_row(card, "Decoys engaged", "0");
    ui->missed = pocketui_kv_row(card, "Tracks lost", "0");
    ui->streak = pocketui_kv_row(card, "Best streak", "0");
    ui->level = pocketui_kv_row(card, "Level reached", "0");
    /* The last row sits on the card's own bottom padding, so its divider
     * would draw a line to nowhere. */
    lv_obj_remove_style(lv_obj_get_parent(ui->level), pos_style(POS_STYLE_DIVIDER), 0);
    (void)detail;

    pocketui_button(screen, "NEW RUN", result_again_cb, app);
    return screen;
}
