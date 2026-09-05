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

    panel = fleet_panel(screen, "OPPONENT");
    bar = fleet_segments(panel, levels, FLEET_DIFFICULTY_COUNT);
    for (i = 0; i < FLEET_DIFFICULTY_COUNT; i++) {
        lv_obj_add_event_cb(lv_obj_get_child(bar, i), on_difficulty, LV_EVENT_CLICKED, app);
    }
    ui->segments = bar;
    ui->brief = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->brief, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->brief, LV_PCT(100));

    panel = fleet_list_panel(screen, "YOUR FLEET");
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_obj_t *row = fleet_row(panel, POCKETUI_ROW_H, i < FLEET_SHIP_COUNT - 1);

        pocketui_label(row, fleet_ship_name((enum fleet_ship)i), POS_STYLE_ROW_TITLE);
        ship_pips(row, (enum fleet_ship)i);
    }

    panel = fleet_list_panel(screen, "ENGAGEMENT");
    pocketui_kv_row(panel, "Waters", "10 \xc3\x97 10");
    pocketui_kv_row(panel, "Shots per turn", "1");
    pocketui_kv_row(panel, "Hulls to sink", "17");

    panel = fleet_panel(screen, "SAVED ENGAGEMENT");
    ui->saved_state = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->saved_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->saved_state, LV_PCT(100));
    ui->resume = fleet_button_secondary(panel, "RESUME", on_resume, app);

    pocketui_button(screen, "DEPLOY FLEET", on_deploy, app);
    return screen;
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
