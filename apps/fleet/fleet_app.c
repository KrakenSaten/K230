/*
 * PocketFleet: a tactical naval game for PocketOS. Application entry point,
 * screen ownership and navigation. See fleet_app.h.
 *
 * Phase 1 is local single player. The app talks to no service and opens no
 * device; it needs the shell's app API and PocketUI, nothing else.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_app.h"

#include "app.h"
#include "pocketui.h"
#include "ui/fleet_view.h"

#include <stdlib.h>
#include <time.h>

/* Gap between panels (DS §7). */
#define FLEET_PANEL_GAP 22
/* The shell's body padding plus this is the DS body top padding of 24, and
 * it keeps the first panel's caption inside the scroll area. */
#define FLEET_CAPTION_ROOM 8

lv_obj_t *fleet_app_screen_container(lv_obj_t *parent)
{
    lv_obj_t *screen = lv_obj_create(parent);

    lv_obj_remove_style_all(screen);
    lv_obj_set_width(screen, LV_PCT(100));
    lv_obj_set_height(screen, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, FLEET_PANEL_GAP, 0);
    /* Room for the first panel's caption, which straddles its top border. */
    lv_obj_set_style_pad_top(screen, FLEET_CAPTION_ROOM, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

void fleet_app_show(struct fleet_app *app, enum fleet_screen screen)
{
    char status[48];
    int i;

    if (!app || (unsigned)screen >= FLEET_SCREEN_COUNT || !app->screen[screen]) {
        return;
    }
    for (i = 0; i < FLEET_SCREEN_COUNT; i++) {
        if (app->screen[i]) {
            lv_obj_add_flag(app->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(app->screen[screen], LV_OBJ_FLAG_HIDDEN);
    app->current = (uint8_t)screen;
    lv_obj_scroll_to_y(app->body, 0, LV_ANIM_OFF);

    switch (screen) {
    case FLEET_SCREEN_COMMAND:
        fleet_screen_command_refresh(app);
        pocketos_shell_set_status_hint("COMMAND");
        return;
    default:
        break;
    }
    if (fleet_view_status(&app->game, status, sizeof(status)) == 0) {
        pocketos_shell_set_status_hint(status);
    }
}

void fleet_app_new_match(struct fleet_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    /* The engine is deterministic given a seed; the seed itself is taken from
     * the clock so successive engagements differ, and it is stored in the
     * match so one can be replayed exactly. */
    seed = (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get();
    fleet_game_new(&app->game, seed, (enum fleet_difficulty)app->difficulty);
    fleet_app_show(app, FLEET_SCREEN_DEPLOY);
}

/* ---- shell app API ---------------------------------------------------- */

static void *fleet_create(lv_obj_t *root)
{
    struct fleet_app *app = calloc(1, sizeof(*app));

    if (!app) {
        return NULL;
    }
    app->body = root;
    app->difficulty = FLEET_OFFICER;
    /* A match exists from the start so every screen has something to read. */
    fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);

    app->screen[FLEET_SCREEN_COMMAND] = fleet_screen_command_create(app, root);
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
    return app;
}

static void fleet_destroy(void *priv)
{
    struct fleet_app *app = priv;

    if (!app) {
        return;
    }
    /* The screen containers are children of the shell's root and are deleted
     * with it; only the private blocks are ours to release. */
    free(app->command);
    free(app);
}

const struct pocketos_app app_fleet = {
    .id = "fleet",
    .name = "Fleet",
    /* Placeholder: the Design System stroke icon set does not exist yet, so
     * the launcher uses the closest LV_SYMBOL glyph (DS §11, step 5+). */
    .icon = LV_SYMBOL_GPS,
    .create = fleet_create,
    .tick = NULL,
    .destroy = fleet_destroy,
};
