/*
 * PocketRadar: a tactical sensor game for PocketOS. Application entry point,
 * screen ownership and the run clock. See radar_app.h.
 *
 * It is a game. It talks to no service, opens no device and senses nothing;
 * it needs the shell's app API and PocketUI, and nothing else.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_app.h"

#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gap between panels (DS section 7). RADAR_COL_GAP, the gutter across the
 * page, is in radar_app.h because the screens want it too. */
#define RADAR_PANEL_GAP 22

lv_obj_t *radar_app_screen_container(lv_obj_t *parent)
{
    lv_obj_t *screen = lv_obj_create(parent);

    lv_obj_remove_style_all(screen);
    lv_obj_set_width(screen, LV_PCT(100));
    lv_obj_set_height(screen, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    /* Panels fill the width; the scope, which is square, is centred. */
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, RADAR_PANEL_GAP, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

lv_obj_t *radar_app_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(box, RADAR_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(box, RADAR_COL_GAP, 0);
    radar_app_box_column(box, 0);
    return box;
}

void radar_app_box_split(lv_obj_t *box, int wide)
{
    if (!box) {
        return;
    }
    lv_obj_set_flex_flow(box, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 0);
    lv_obj_set_height(box, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
}

void radar_app_box_column(lv_obj_t *box, int wide)
{
    if (!box) {
        return;
    }
    /* A column is a column in both shapes. Down the page the gap between its
     * children is the panel gap, so a column nested in a screen stacks exactly
     * as the screen itself would have and the tall shape is the v0.0.10 one to
     * the pixel. */
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(box, LV_PCT(100));
    if (wide) {
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_height(box, LV_PCT(100));
    } else {
        lv_obj_set_flex_grow(box, 0);
        lv_obj_set_height(box, LV_SIZE_CONTENT);
    }
}

void radar_app_screen_flow(lv_obj_t *screen, int wide, int across)
{
    if (!screen) {
        return;
    }
    lv_obj_set_flex_flow(screen, (wide && across) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, RADAR_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(screen, RADAR_COL_GAP, 0);
    if (wide) {
        /* Tops in line: the scope and what stands beside it start together. */
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_height(screen, LV_PCT(100));
    } else {
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_height(screen, LV_SIZE_CONTENT);
    }
}

/* ---- the shape ---------------------------------------------------------- *
 *
 * One rule, in two pure functions, so a test can ask it without building
 * anything. The scope is square and its side is what the body's height can
 * hold, never more than the tall shape's - so the wide shape can never
 * invalidate more pixels per tick than the tall one already does, which is
 * the whole of this app's frame cost (hardware verification H1).
 */

int radar_scope_for_height(int32_t h)
{
    if (h <= 0) {
        return 0;
    }
    return h > RADAR_SCOPE_TALL ? RADAR_SCOPE_TALL : (int)h;
}

int radar_shape_is_wide(int32_t w, int32_t h, int *scope_out)
{
    int scope = radar_scope_for_height(h);

    if (w <= h) {
        return 0;               /* not a wide body at all */
    }
    if (scope < RADAR_SCOPE_MIN) {
        return 0;               /* the scope would be too small to work */
    }
    if (w < scope + RADAR_COL_GAP + RADAR_SIDE_MIN) {
        return 0;               /* nothing useful would stand beside it */
    }
    if (scope_out) {
        *scope_out = scope;
    }
    return 1;
}

/* ---- the layout --------------------------------------------------------- *
 *
 * The frame is the whole of the body the shell gives the app, and the shape is
 * chosen from its size alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel). Both screens are the
 *   single column of v0.0.10, the scope 520 px, and the frame scrolls what
 *   does not fit - which is what the shell's body did before.
 *
 *   WIDE (landscape: 1192 x 386, once the foot has cleared the rounded
 *   corners). The scope takes the height and the numbers read off it stand
 *   beside it: the two cards abreast and ENGAGE across the foot of them.
 *
 * Whatever the shape, the frame pads its foot by however far the panel's
 * rounded corner squares reach into it - handed over by the shared layout
 * guard (DS 22.2, 23.4), never worked out here.
 *
 * This runs when the body's box changes and at no other time. It never runs
 * on a tick: the run's clock steps the engine and invalidates the scope, and
 * touches no layout at all.
 */

static void radar_app_layout(struct radar_app *app)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;
    int scope = RADAR_SCOPE_TALL;
    int wide;

    if (!app) {
        return;
    }
    /* No frame, nothing to lay out in, or nothing the layout is chosen from
     * has changed, so there is nothing to do. PocketUI owns that decision for
     * every responsive app, insets included: the same box on a panel with
     * different corners leaves a different amount of room. */
    if (!pocketui_layout_begin(&app->layout_guard, app->frame, &in)) {
        return;
    }
    box = &app->layout_guard.area;
    app->layouts++;
    lv_obj_set_style_pad_left(app->frame, in.left, 0);
    lv_obj_set_style_pad_top(app->frame, in.top, 0);
    lv_obj_set_style_pad_right(app->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(app->frame, in.bottom, 0);
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    wide = radar_shape_is_wide(w, h, &scope);
    if (!wide) {
        scope = RADAR_SCOPE_TALL;
    }
    app->shape = (uint8_t)(wide ? RADAR_SHAPE_WIDE : RADAR_SHAPE_TALL);
    app->scope_size = scope;

    if (wide) {
        lv_obj_scroll_to_y(app->frame, 0, LV_ANIM_OFF);
        lv_obj_remove_flag(app->frame, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_add_flag(app->frame, LV_OBJ_FLAG_SCROLLABLE);
    }
    /* Both screens, not only the visible one: a hidden screen has to be right
     * the moment it is shown. */
    radar_screen_scan_relayout(app, wide, scope);
    radar_screen_result_relayout(app, wide);
}

/* The frame is the body's content box, so this is the body changing size. */
static void on_frame_size(lv_event_t *e)
{
    radar_app_layout(lv_event_get_user_data(e));
}

void radar_app_show(struct radar_app *app, enum radar_screen screen)
{
    int i;

    if (!app || (unsigned)screen >= RADAR_SCREEN_COUNT || !app->screen[screen]) {
        return;
    }
    for (i = 0; i < RADAR_SCREEN_COUNT; i++) {
        if (app->screen[i]) {
            lv_obj_add_flag(app->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(app->screen[screen], LV_OBJ_FLAG_HIDDEN);
    app->current = (uint8_t)screen;
    /* The frame is the scroller in the tall shape; in the wide one it does not
     * scroll and this is a no-op. */
    lv_obj_scroll_to_y(app->frame ? app->frame : app->body, 0, LV_ANIM_OFF);

    if (screen == RADAR_SCREEN_RESULT) {
        radar_screen_result_refresh(app);
        pocketos_shell_set_status_hint("RUN COMPLETE");
        return;
    }
    radar_screen_scan_refresh(app);
    pocketos_shell_set_status_hint(app->run.state == RADAR_RUN_ACTIVE ? "SCANNING"
                                                                      : "STANDBY");
}

/* ---- the run ----------------------------------------------------------- */

void radar_app_begin(struct radar_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    /* The engine is deterministic given a seed, and the seed is chosen here
     * at the app boundary rather than inside it. Two existing sources are
     * mixed: the wall clock, which separates sessions, and the LVGL tick,
     * which separates runs within a session. The lifetime run counter is
     * added so successive runs still differ on a board whose clock was never
     * set and which was rebooted to the same tick - the K230 has no verified
     * RTC yet, so that is not a hypothetical. The seed is kept in the run,
     * so any run can be replayed exactly. */
    seed = (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get() ^
           (app->record.runs * 2654435761u);
    radar_run_new(&app->run, seed);
    radar_run_start(&app->run);
    app->new_best = 0;
    if (app->clock) {
        lv_timer_resume(app->clock);
    }
    radar_app_show(app, RADAR_SCREEN_SCAN);
}

void radar_app_finish(struct radar_app *app)
{
    if (!app) {
        return;
    }
    if (app->clock) {
        lv_timer_pause(app->clock);
    }
    app->new_best = (uint8_t)radar_record_note_run(&app->record, &app->run.score,
                                                   radar_run_level(&app->run));
    if (app->storage_ok && radar_store_save(&app->record) != 0) {
        /* One failure is enough: retrying would spend the rest of the
         * session writing to a filesystem that has already said no. */
        app->storage_ok = 0;
        LOG_WARN("radar: cannot write %s, best score is session-only",
                 radar_store_path());
    }
    radar_app_show(app, RADAR_SCREEN_RESULT);
}

static void radar_clock(lv_timer_t *timer)
{
    struct radar_app *app = lv_timer_get_user_data(timer);

    if (!app || app->run.state != RADAR_RUN_ACTIVE) {
        return;
    }
    radar_screen_scan_tick(app);
    if (radar_run_is_over(&app->run)) {
        radar_app_finish(app);
    }
}

/* ---- development aid --------------------------------------------------- */

/* $POCKETRADAR_SCREEN opens the app in a given state from a fixed seed, so
 * the simulator can render each one for design review the way the shell's own
 * --screenshot does. It does nothing unless the variable is set.
 *
 * The clock is deliberately left paused afterwards. A screenshot is a still,
 * and the shell takes it several hundred milliseconds after the app opens; a
 * running clock would advance the state in between, which on the first pass
 * turned a half-finished lock into a nearly complete one. Frozen, every shot
 * is exactly the state named here and is reproducible. */
#define RADAR_DEBUG_SEED 20260906u
#define RADAR_DEBUG_WARMUP 150
/* A person needs about half a second to see a contact, reach and tap it. The
 * result shot models one rather than the machine that beat level 10, so the
 * numbers on it are numbers a player could recognise. */
#define RADAR_DEBUG_REACTION 10

/* Work the scope until a contact of the wanted kind is acquired, holding the
 * sector intact so the search cannot end the run before the shot is set up. */
static uint32_t debug_acquire(struct radar_app *app, int want_target)
{
    int guard;

    for (guard = 0; guard < 8000; guard++) {
        const struct radar_contact *sel = radar_run_selected(&app->run);
        int i;

        app->run.score.integrity = RADAR_INTEGRITY_MAX;
        if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
            if (radar_class_is_target((enum radar_class)sel->cls) == want_target) {
                return sel->id;
            }
            radar_run_deselect(&app->run);
        }
        if (!radar_run_selected(&app->run)) {
            for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                const struct radar_contact *t = radar_run_slot(&app->run, i);

                if (t->active && !t->classified) {
                    radar_run_select(&app->run, t->id);
                    break;
                }
            }
        }
        radar_run_tick(&app->run);
        radar_run_clear_events(&app->run);
    }
    return RADAR_NO_CONTACT;
}

static void debug_warm(struct radar_app *app, int ticks)
{
    int i;

    for (i = 0; i < ticks; i++) {
        app->run.score.integrity = RADAR_INTEGRITY_MAX;
        radar_run_tick(&app->run);
        radar_run_clear_events(&app->run);
    }
}

static void debug_open(struct radar_app *app)
{
    const char *want = getenv("POCKETRADAR_SCREEN");
    int guard;

    if (!want) {
        return;
    }
    if (strcmp(want, "idle") == 0) {
        radar_app_show(app, RADAR_SCREEN_SCAN);
        return;
    }
    radar_run_new(&app->run, RADAR_DEBUG_SEED);
    radar_run_start(&app->run);
    if (strcmp(want, "scan") == 0) {
        debug_warm(app, RADAR_DEBUG_WARMUP);
    } else if (strcmp(want, "selected") == 0) {
        debug_warm(app, RADAR_DEBUG_WARMUP);
        {
            int i;

            for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                const struct radar_contact *t = radar_run_slot(&app->run, i);

                if (t->active) {
                    radar_run_select(&app->run, t->id);
                    break;
                }
            }
        }
        /* Half way through the lock, so the acquisition arc is visibly
         * partial rather than absent or complete. */
        debug_warm(app, RADAR_ACQUIRE_TICKS / 2);
    } else if (strcmp(want, "acquired") == 0) {
        debug_warm(app, RADAR_DEBUG_WARMUP);
        debug_acquire(app, 1);
    } else if (strcmp(want, "decoy") == 0) {
        debug_warm(app, RADAR_DEBUG_WARMUP);
        debug_acquire(app, 0);
    } else if (strcmp(want, "result") == 0) {
        int idle = 0;
        int fumbles = 0;

        for (guard = 0; guard < 60000 && !radar_run_is_over(&app->run); guard++) {
            const struct radar_contact *sel = radar_run_selected(&app->run);
            int i;

            if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
                if (radar_class_is_target((enum radar_class)sel->cls)) {
                    radar_run_engage(&app->run);
                } else if (fumbles++ % 4 == 0) {
                    radar_run_engage(&app->run);  /* the occasional mis-tap */
                } else {
                    radar_run_deselect(&app->run);
                }
                idle = RADAR_DEBUG_REACTION;
            } else if (!sel) {
                if (idle > 0) {
                    idle--;
                } else {
                    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                        const struct radar_contact *t = radar_run_slot(&app->run, i);

                        if (t->active && !t->classified) {
                            radar_run_select(&app->run, t->id);
                            break;
                        }
                    }
                }
            }
            radar_run_tick(&app->run);
            radar_run_clear_events(&app->run);
        }
        radar_app_finish(app);
        return;
    } else {
        return;
    }
    radar_app_show(app, RADAR_SCREEN_SCAN);
}

/* ---- shell app API ----------------------------------------------------- */

static void *radar_create(lv_obj_t *root)
{
    struct radar_app *app = calloc(1, sizeof(*app));
    int loaded;

    if (!app) {
        return NULL;
    }
    app->body = root;
    app->storage_ok = 1;
    app->reduced_motion = (uint8_t)(pocketos_shell_reduced_motion() != 0);

    /* A run exists from the start so the scope has something to paint; it is
     * READY, so nothing moves until the player says so. */
    radar_run_new(&app->run, 1u);

    /* A missing or damaged record must never delay or prevent the app from
     * opening: it only means there is no best score to beat. */
    radar_record_init(&app->record);
    loaded = radar_store_load(&app->record);
    if (loaded < 0) {
        LOG_WARN("radar: stored record at %s rejected, starting from nothing",
                 radar_store_path());
        radar_record_init(&app->record);
    } else if (loaded == 0) {
        LOG_INFO("radar: best score %u over %u run(s) from %s",
                 (unsigned)app->record.best_score, (unsigned)app->record.runs,
                 radar_store_path());
    }

    /* Exactly the body's content box, whatever ends up in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the layout itself put there. */
    app->frame = lv_obj_create(root);
    lv_obj_remove_style_all(app->frame);
    lv_obj_set_size(app->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(app->frame, LV_DIR_VER);
    lv_obj_add_event_cb(app->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, app);
    app->shape = RADAR_SHAPE_TALL;
    app->scope_size = RADAR_SCOPE_TALL;

    app->screen[RADAR_SCREEN_SCAN] = radar_screen_scan_create(app, app->frame);
    app->screen[RADAR_SCREEN_RESULT] = radar_screen_result_create(app, app->frame);

    app->clock = lv_timer_create(radar_clock, RADAR_TICK_MS, app);
    if (app->clock) {
        lv_timer_pause(app->clock);
    }

    radar_app_show(app, RADAR_SCREEN_SCAN);
    debug_open(app);
    return app;
}

static void radar_destroy(void *priv)
{
    struct radar_app *app = priv;

    if (!app) {
        return;
    }
    /* Stop the clock before the objects its callback paints go away with the
     * shell's root. A run left in progress is simply abandoned: PocketRadar
     * has no resume, so there is nothing to settle. */
    if (app->clock) {
        lv_timer_delete(app->clock);
        app->clock = NULL;
    }
    /* Nothing may lay out against a half-freed app: the shell deletes the
     * body's children after this returns, and a layout pass in between would
     * reach the screens through a struct that is already gone. */
    if (app->frame) {
        lv_obj_remove_event_cb_with_user_data(app->frame, on_frame_size, app);
        app->frame = NULL;
    }
    free(app->scan);
    free(app->result);
    free(app);
}

LV_IMAGE_DECLARE(pos_app_icon_radar);

const struct pocketos_app app_radar = {
    .id = "radar",
    .name = "Radar",
    /* The launcher draws the Doors icon (DS section 20); the glyph stays as
     * the app's text icon, the closest LV_SYMBOL. */
    .icon = LV_SYMBOL_WIFI,
    .icon_mask = &pos_app_icon_radar,
    .create = radar_create,
    .tick = NULL,
    .destroy = radar_destroy,
};
