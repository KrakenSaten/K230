/*
 * PocketTimber: a tabletop balancing game for PocketOS. Application entry
 * point, screen ownership and the run clock. See timber_app.h.
 *
 * It is a game. It talks to no service, opens no device and senses nothing;
 * it needs the shell's app API and PocketUI, and nothing else.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_app.h"

#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gap between panels (DS section 7). */
#define TIMBER_PANEL_GAP 22

lv_obj_t *timber_app_screen_container(lv_obj_t *parent)
{
    lv_obj_t *screen = lv_obj_create(parent);

    lv_obj_remove_style_all(screen);
    lv_obj_set_width(screen, LV_PCT(100));
    lv_obj_set_height(screen, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, TIMBER_PANEL_GAP, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

void timber_app_show(struct timber_app *app, enum timber_screen screen)
{
    int i;

    if (!app || (unsigned)screen >= TIMBER_SCREEN_COUNT || !app->screen[screen]) {
        return;
    }
    for (i = 0; i < TIMBER_SCREEN_COUNT; i++) {
        if (app->screen[i]) {
            lv_obj_add_flag(app->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(app->screen[screen], LV_OBJ_FLAG_HIDDEN);
    app->current = (uint8_t)screen;
    lv_obj_scroll_to_y(app->body, 0, LV_ANIM_OFF);

    if (screen == TIMBER_SCREEN_RESULT) {
        timber_screen_result_refresh(app);
        pocketos_shell_set_status_hint(timber_run_cause(&app->run) == TIMBER_CAUSE_NONE ? "STANDING"
                                                                                       : "TIMBER");
        return;
    }
    timber_screen_table_refresh(app);
    pocketos_shell_set_status_hint(app->run.state == TIMBER_RUN_ACTIVE ? "IN PLAY" : "STANDBY");
}

/* ---- the run ----------------------------------------------------------- */

void timber_app_begin(struct timber_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    /* The seed is chosen at the app boundary, never inside the engine. The
     * wall clock separates sessions, the LVGL tick separates runs within
     * one, and the run counter separates runs on a board whose clock was
     * never set. The seed stays in the run, so any run can be replayed. */
    seed = (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get() ^ (app->records.core.runs * 2654435761u);
    timber_run_new(&app->run, seed);
    timber_run_start(&app->run);
    app->new_best = 0;
    app->ghost = -1;
    app->pressing = 0;
    app->pending_px = 0;
    if (app->clock) {
        lv_timer_resume(app->clock);
    }
    timber_app_show(app, TIMBER_SCREEN_TABLE);
}

void timber_app_finish(struct timber_app *app)
{
    if (!app) {
        return;
    }
    if (app->clock) {
        lv_timer_pause(app->clock);
    }
    app->new_best = (uint8_t)timber_records_note_run(&app->records, &app->run.score,
                                                     timber_run_cause(&app->run));
    if (app->store_ok && timber_store_save(&app->records) != 0) {
        /* One failure is enough: retrying would spend the rest of the
         * session on a filesystem that has already said no. */
        app->store_ok = 0;
        LOG_WARN("timber: cannot store %s, best score is session-only", timber_store_path());
    }
    timber_app_show(app, TIMBER_SCREEN_RESULT);
}

static void timber_clock(lv_timer_t *timer)
{
    struct timber_app *app = lv_timer_get_user_data(timer);

    if (!app) {
        return;
    }
    if (app->run.state != TIMBER_RUN_ACTIVE && app->run.state != TIMBER_RUN_COLLAPSING) {
        return;
    }
    timber_screen_table_tick(app);
    if (timber_run_is_over(&app->run)) {
        timber_app_finish(app);
    }
}

/* ---- development aid --------------------------------------------------- */

/* $POCKETTIMBER_SCREEN opens the app in a given state from a fixed seed, so
 * the simulator can render each one for review the way the shell's own
 * --screenshot does. The clock is left paused afterwards, so a shot is
 * exactly the state named here. */
#define TIMBER_DEBUG_SEED 20260906u

/* Pull the loosest pullable centre block slowly until it is in hand. */
static int debug_pull_one(struct timber_app *app)
{
    int best = -1;
    int best_seat = -1;
    int id;
    int guard;

    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &app->run.tower.blocks[id];

        if (b->slot == 1 && timber_tower_pullable(&app->run.tower, id) && b->seat > best_seat) {
            best_seat = b->seat;
            best = id;
        }
    }
    if (best < 0 || timber_run_select(&app->run, best) != 0) {
        return -1;
    }
    for (guard = 0; guard < 200 && app->run.turn != TIMBER_TURN_PLACING &&
                    app->run.state == TIMBER_RUN_ACTIVE; guard++) {
        timber_run_tick(&app->run);
        timber_run_pull(&app->run, timber_pull_limit(TIMBER_CLASS_STUCK) * 4 / 5);
        timber_run_clear_events(&app->run);
    }
    return app->run.turn == TIMBER_TURN_PLACING ? best : -1;
}

static void debug_settle(struct timber_app *app, int ticks)
{
    int i;

    for (i = 0; i < ticks; i++) {
        timber_run_tick(&app->run);
        timber_run_clear_events(&app->run);
    }
}

static void debug_open(struct timber_app *app)
{
    const char *want = getenv("POCKETTIMBER_SCREEN");
    int i;

    if (!want) {
        return;
    }
    if (strcmp(want, "idle") == 0) {
        timber_app_show(app, TIMBER_SCREEN_TABLE);
        return;
    }
    timber_run_new(&app->run, TIMBER_DEBUG_SEED);
    timber_run_start(&app->run);
    app->ghost = -1;
    /* Three careful turns so the tower has a history: three thinned layers
     * below and a completed layer on top. */
    for (i = 0; i < 3; i++) {
        if (debug_pull_one(app) >= 0) {
            timber_run_place(&app->run, (i + 1) % TIMBER_SLOTS);
        }
        debug_settle(app, 30);
    }
    if (strcmp(want, "run") == 0) {
        /* A block selected and tested, waiting for the pull. */
        for (i = 0; i < TIMBER_BLOCKS; i++) {
            if (app->run.tower.blocks[i].slot == 0 && timber_tower_pullable(&app->run.tower, i) &&
                timber_run_select(&app->run, i) == 0) {
                timber_run_test(&app->run);
                break;
            }
        }
    } else if (strcmp(want, "pulling") == 0) {
        for (i = 0; i < TIMBER_BLOCKS; i++) {
            if (app->run.tower.blocks[i].slot == 2 && timber_tower_pullable(&app->run.tower, i) &&
                timber_run_select(&app->run, i) == 0) {
                break;
            }
        }
        for (i = 0; i < 12 && app->run.turn != TIMBER_TURN_PLACING; i++) {
            timber_run_tick(&app->run);
            timber_run_pull(&app->run, 40);
        }
    } else if (strcmp(want, "placing") == 0) {
        debug_pull_one(app);
    } else if (strcmp(want, "collapse") == 0 || strcmp(want, "result") == 0) {
        /* The collapse the review promised: yank the base's centre block
         * out, wedged, then a side block, which leaves the stack on one
         * side block and it goes as the second block lets go, blamed on
         * the jolt. */
        static const int victims[2] = { 1, 0 };

        for (i = 0; i < 2 && app->run.state == TIMBER_RUN_ACTIVE; i++) {
            int guard;

            app->run.tower.blocks[victims[i]].seat = 0;
            if (timber_run_select(&app->run, victims[i]) != 0) {
                break;
            }
            timber_run_pull(&app->run, 700);
            for (guard = 0; guard < 4 && app->run.state == TIMBER_RUN_ACTIVE &&
                            app->run.turn != TIMBER_TURN_PLACING; guard++) {
                timber_run_tick(&app->run);
                timber_run_pull(&app->run, 700);
            }
            if (app->run.state == TIMBER_RUN_ACTIVE && app->run.turn == TIMBER_TURN_PLACING) {
                timber_run_place(&app->run, i);
                debug_settle(app, 12);
            }
        }
        if (app->run.state == TIMBER_RUN_ACTIVE) {
            /* The seed did not oblige: lean it over by hand. */
            app->run.lean_x = TIMBER_LEAN_MAX;
            timber_run_tick(&app->run);
        }
        if (strcmp(want, "collapse") == 0) {
            debug_settle(app, TIMBER_TIP_TICKS + 6);
        } else {
            debug_settle(app, TIMBER_COLLAPSE_TICKS_MAX + 1);
            timber_app_finish(app);
            return;
        }
    }
    timber_run_clear_events(&app->run);
    timber_app_show(app, TIMBER_SCREEN_TABLE);
}

/* ---- shell app API ----------------------------------------------------- */

static void *timber_create(lv_obj_t *root)
{
    struct timber_app *app = calloc(1, sizeof(*app));
    int loaded;

    if (!app) {
        return NULL;
    }
    app->body = root;
    app->ghost = -1;
    app->reduced_motion = (uint8_t)(pocketos_shell_reduced_motion() != 0);
    /* A run exists from the start so the table has a tower to paint; it is
     * READY, so nothing moves until the player says so. */
    timber_run_new(&app->run, 1u);
    /* A missing or damaged record must never delay or prevent the app from
     * opening: it only means there is no best score to beat (D2). */
    timber_records_init(&app->records);
    app->store_ok = 1;
    loaded = timber_store_load(&app->records);
    if (loaded < 0) {
        LOG_WARN("timber: stored record at %s rejected, starting from nothing", timber_store_path());
        timber_records_init(&app->records);
    } else if (loaded == 0) {
        LOG_INFO("timber: best score %u over %u run(s) from %s",
                 (unsigned)app->records.core.best_score, (unsigned)app->records.core.runs,
                 timber_store_path());
    }

    app->screen[TIMBER_SCREEN_TABLE] = timber_screen_table_create(app, root);
    app->screen[TIMBER_SCREEN_RESULT] = timber_screen_result_create(app, root);

    app->clock = lv_timer_create(timber_clock, TIMBER_TICK_MS, app);
    if (app->clock) {
        lv_timer_pause(app->clock);
    }
    timber_app_show(app, TIMBER_SCREEN_TABLE);
    debug_open(app);
    return app;
}

static void timber_destroy(void *priv)
{
    struct timber_app *app = priv;

    if (!app) {
        return;
    }
    /* Stop the clock before the objects its callback paints go away with
     * the shell's root. A run left in progress is abandoned: there is no
     * resume (v0.1 lifecycle). */
    if (app->clock) {
        lv_timer_delete(app->clock);
        app->clock = NULL;
    }
    free(app->table);
    free(app->result);
    free(app);
}

LV_IMAGE_DECLARE(pos_app_icon_timber);

const struct pocketos_app app_timber = {
    .id = "timber",
    .name = "Timber",
    /* The launcher draws the Doors icon (DS section 20); the glyph stays as
     * the app's text icon, the closest LV_SYMBOL. */
    .icon = LV_SYMBOL_LIST,
    .icon_mask = &pos_app_icon_timber,
    .create = timber_create,
    .tick = NULL,
    .destroy = timber_destroy,
    /* Fullscreen (DS §30.4 stage 2): no status bar in either orientation,
     * the body from the header down. Whatever this app writes to the hint
     * the shell shows in its header instead. */
    .chrome = POCKETOS_CHROME_NONE,
    /* Portrait only (app.h `orientation`): the tower is tall and gains
     * nothing from landscape but a scroll. The shell holds the display in
     * portrait while Timber is open and gives the mode back when it closes. */
    .orientation = POCKETOS_APP_ORIENTATION_PORTRAIT,
};
