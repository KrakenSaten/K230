/*
 * PocketFleet Command screen: choose an opponent and start an engagement.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_view.h"
#include "fleet_widgets.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PIP_SIZE 14
#define PIP_GAP 4

struct fleet_command_ui {
    struct fleet_app *app;
    lv_obj_t *segments;
    lv_obj_t *brief;
    lv_obj_t *saved_state;
    lv_obj_t *resume;
    /* Four panels and one action. Across the page they stand in three
     * columns - the choice, the fleet, the terms - which is the split that
     * lets all four be read at once without scrolling anything, and DEPLOY
     * FLEET sits on a foot row below them, one column wide. */
    lv_obj_t *cols;
    lv_obj_t *left;
    lv_obj_t *mid;
    lv_obj_t *right;
    lv_obj_t *saved;
    lv_obj_t *foot;
    lv_obj_t *deploy;
};

static void on_difficulty(lv_event_t *e)
{
    struct fleet_app *app = lv_event_get_user_data(e);
    lv_obj_t *seg = lv_event_get_target_obj(e);
    lv_obj_t *bar = lv_obj_get_parent(seg);
    uint32_t count = lv_obj_get_child_count(bar);
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (lv_obj_get_child(bar, (int32_t)i) == seg) {
            app->difficulty = (uint8_t)i;
            break;
        }
    }
    fleet_screen_command_refresh(app);
}

static void on_deploy(lv_event_t *e)
{
    fleet_app_new_match(lv_event_get_user_data(e));
}

static void on_resume(lv_event_t *e)
{
    fleet_app_resume(lv_event_get_user_data(e));
}

/* Hull length drawn as blocks, in the spirit of the segmented meter (DS §9). */
static void ship_pips(lv_obj_t *parent, enum fleet_ship ship)
{
    lv_obj_t *box = lv_obj_create(parent);
    int n;

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, PIP_SIZE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, PIP_GAP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    for (n = 0; n < fleet_ship_length(ship); n++) {
        lv_obj_t *pip = lv_obj_create(box);

        lv_obj_remove_style_all(pip);
        pos_style_add(pip, POS_STYLE_SLAB, 0);
        lv_obj_set_size(pip, PIP_SIZE, PIP_SIZE);
        lv_obj_set_style_radius(pip, 2, 0);
        lv_obj_remove_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
    }
}

lv_obj_t *fleet_screen_command_create(struct fleet_app *app, lv_obj_t *parent)
{
    static const char *const levels[FLEET_DIFFICULTY_COUNT] = {
        "RECRUIT", "OFFICER", "COMMANDER", "ADMIRAL"
    };
    struct fleet_command_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *panel;
    lv_obj_t *bar;
    int i;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->command = ui;
    screen = fleet_app_screen_container(parent);
    ui->cols = fleet_app_box(screen);
    ui->left = fleet_app_box(ui->cols);
    ui->mid = fleet_app_box(ui->cols);
    ui->right = fleet_app_box(ui->cols);
    ui->foot = fleet_app_box(screen);

    panel = fleet_panel(ui->left, "OPPONENT");
    bar = fleet_segments(panel, levels, FLEET_DIFFICULTY_COUNT);
    for (i = 0; i < FLEET_DIFFICULTY_COUNT; i++) {
        lv_obj_add_event_cb(lv_obj_get_child(bar, i), on_difficulty, LV_EVENT_CLICKED, app);
    }
    ui->segments = bar;
    ui->brief = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->brief, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->brief, LV_PCT(100));

    panel = fleet_list_panel(ui->mid, "YOUR FLEET");
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_obj_t *row = fleet_row(panel, POCKETUI_ROW_H, i < FLEET_SHIP_COUNT - 1);

        pocketui_label(row, fleet_ship_name((enum fleet_ship)i), POS_STYLE_ROW_TITLE);
        ship_pips(row, (enum fleet_ship)i);
    }

    panel = fleet_list_panel(ui->right, "ENGAGEMENT");
    pocketui_kv_row(panel, "Waters", "10 \xc3\x97 10");
    pocketui_kv_row(panel, "Shots per turn", "1");
    pocketui_kv_row(panel, "Hulls to sink", "17");

    panel = fleet_panel(ui->right, "SAVED ENGAGEMENT");
    ui->saved = panel;
    ui->saved_state = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->saved_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->saved_state, LV_PCT(100));
    ui->resume = fleet_button_secondary(panel, "RESUME", on_resume, app);

    ui->deploy = pocketui_button(ui->foot, "DEPLOY FLEET", on_deploy, app);
    return screen;
}

/* Three columns of panels with the ways on under them. The roster is taller
 * than a 386 px body whatever the split, so a column scrolls - which is what a
 * landscape column does elsewhere (DS §24.1, §25.1, §26.1). Three columns
 * rather than two is what leaves only the roster needing it. DEPLOY FLEET is
 * kept off the columns entirely, on a foot row at one column's width: the
 * primary action must never be the thing that has been scrolled out of
 * sight, and it has no business stretching across the whole body either. */
void fleet_screen_command_relayout(struct fleet_app *app, int wide)
{
    struct fleet_command_ui *ui = app ? app->command : NULL;
    lv_obj_t *col[3];
    int i;

    if (!ui) {
        return;
    }
    /* The screen keeps its column in both shapes: the panels turn across the
     * page, the action stays under them. */
    fleet_app_screen_flow(app->screen[FLEET_SCREEN_COMMAND], wide, 0);
    fleet_app_box_split(ui->cols, wide);
    fleet_app_box_column(ui->left, wide);
    fleet_app_box_column(ui->mid, wide);
    fleet_app_box_column(ui->right, wide);
    /* The foot is a row in the wide shape so its one button can take a third
     * of it and sit at the end, rather than stretch across the whole body. */
    fleet_app_box_split(ui->foot, wide);
    lv_obj_set_flex_grow(ui->foot, 0);
    lv_obj_set_height(ui->foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(ui->foot, wide ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(ui->deploy, wide ? LV_PCT(33) : LV_PCT(100));

    /* RESUME is the other way to start an engagement, and across the page it
     * belongs beside DEPLOY FLEET rather than at the end of a column that
     * scrolls - a saved match is exactly when it is the button you want, and
     * it must not be the thing below the fold. Down the page it goes back
     * inside the panel that describes the saved match, where v0.0.10 has it.
     * The button is moved, never rebuilt, so its handler and its hidden state
     * come with it. */
    if (lv_obj_get_parent(ui->resume) != (wide ? ui->foot : ui->saved)) {
        lv_obj_set_parent(ui->resume, wide ? ui->foot : ui->saved);
        if (wide) {
            lv_obj_move_to_index(ui->resume, 0);
        }
    }
    lv_obj_set_width(ui->resume, wide ? LV_PCT(33) : LV_PCT(100));
    /* cols takes the height the foot leaves; a column shows what it can of
     * its panels and scrolls the rest. */
    lv_obj_set_flex_grow(ui->cols, wide ? 1 : 0);
    lv_obj_set_height(ui->cols, wide ? LV_PCT(100) : LV_SIZE_CONTENT);

    /* This is the one screen whose first object is a captioned panel, and a
     * caption straddles the panel's top border. Whatever clips above that
     * panel is what has to leave room for it: down the page that is the
     * frame, so the room goes above the columns; across the page each column
     * is its own scroller, so the room goes inside each of them. */
    lv_obj_set_style_pad_top(ui->cols, wide ? 0 : FLEET_CAPTION_RISE, 0);
    col[0] = ui->left;
    col[1] = ui->mid;
    col[2] = ui->right;
    for (i = 0; i < 3; i++) {
        lv_obj_set_style_pad_top(col[i], wide ? FLEET_CAPTION_RISE : 0, 0);
        if (wide) {
            lv_obj_add_flag(col[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(col[i], LV_DIR_VER);
            /* A scroller clips: that is what makes a scroll look like one. */
            lv_obj_remove_flag(col[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        } else {
            /* Down the page there is one scroller, the frame. A column that
             * kept a scroll position would hold its panels off the top of a
             * stack that is already where it should be. */
            lv_obj_scroll_to_y(col[i], 0, LV_ANIM_OFF);
            lv_obj_remove_flag(col[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(col[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        }
    }
}

void fleet_screen_command_refresh(struct fleet_app *app)
{
    struct fleet_command_ui *ui;

    if (!app || !app->command) {
        return;
    }
    ui = app->command;
    fleet_segments_select(ui->segments, app->difficulty);
    lv_label_set_text(ui->brief,
                      fleet_view_difficulty_brief((enum fleet_difficulty)app->difficulty));

    lv_obj_remove_style(ui->saved_state, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    if (app->resumable) {
        char status[48];

        fleet_view_status(&app->game, status, sizeof(status));
        lv_label_set_text_fmt(ui->saved_state, "An engagement is waiting: %s.", status);
        lv_obj_remove_flag(ui->resume, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (app->storage_ok) {
            lv_label_set_text(ui->saved_state, "Nothing stored. A new engagement is "
                                               "saved after every turn.");
        } else {
            /* A warning, not an error: the game plays perfectly well without
             * it, only Resume is gone. */
            pos_style_add(ui->saved_state, POS_STYLE_STATUS_WARN_TEXT, 0);
            lv_label_set_text(ui->saved_state, "Storage is unavailable. This engagement "
                                               "lasts for the session only.");
        }
        lv_obj_add_flag(ui->resume, LV_OBJ_FLAG_HIDDEN);
    }
}
