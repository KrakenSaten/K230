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
#include "engine/fleet_store.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "ui/fleet_grid.h"
#include "ui/fleet_view.h"
#include "ui/fleet_widgets.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gap between panels (DS §7). */
#define FLEET_PANEL_GAP 22
/* Gutter between the columns of the wide shape (DS §7). */
#define FLEET_COL_GAP POCKETUI_PAD

/* A panel's caption straddles the panel's top border, so it is drawn a little
 * above the panel (fleet_widgets.c). LVGL clips a child to its parent's box
 * grown by the parent's own extra draw size, so every transparent container a
 * panel is nested in has to allow for that rise or the caption is cut. The
 * containers paint nothing themselves, so this costs no pixels. */
static void box_ext_draw(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, FLEET_CAPTION_RISE + 4);
}

static void allow_caption(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj, box_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(obj);
}

lv_obj_t *fleet_app_screen_container(lv_obj_t *parent)
{
    lv_obj_t *screen = lv_obj_create(parent);

    lv_obj_remove_style_all(screen);
    allow_caption(screen);
    lv_obj_set_width(screen, LV_PCT(100));
    lv_obj_set_height(screen, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    /* Panels fill the width; the grids, which are a few pixels narrower, are
     * centred rather than left-aligned. */
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, FLEET_PANEL_GAP, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

lv_obj_t *fleet_app_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    /* A box paints nothing, so it has nothing to clip to: a panel caption
     * straddling its top border must still be drawn. Clipping is the frame's
     * job, and a column's when it is made to scroll. */
    allow_caption(box);
    lv_obj_set_style_pad_row(box, FLEET_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(box, FLEET_COL_GAP, 0);
    fleet_app_box_column(box, 0);
    return box;
}

void fleet_app_box_split(lv_obj_t *box, int wide)
{
    if (!box) {
        return;
    }
    lv_obj_set_flex_flow(box, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 0);
    lv_obj_set_height(box, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
}

void fleet_app_box_column(lv_obj_t *box, int wide)
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

void fleet_app_screen_flow(lv_obj_t *screen, int wide, int across)
{
    if (!screen) {
        return;
    }
    lv_obj_set_flex_flow(screen, (wide && across) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, FLEET_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(screen, FLEET_COL_GAP, 0);
    if (wide) {
        /* Tops in line: the board and what stands beside it start together. */
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_height(screen, LV_PCT(100));
    } else {
        /* The panels fill the width and the boards, a few pixels narrower,
         * are centred rather than left-aligned - as they always were. */
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_height(screen, LV_SIZE_CONTENT);
    }
}

/* ---- the shape ---------------------------------------------------------- *
 *
 * One rule, in three pure functions, so a test can ask it without building
 * anything.
 *
 * A cell's HEIGHT is whatever the body's height can hold, and that is the
 * binding constraint: ten rows, nine gaps and the caption gutter in a 386 px
 * body come to 34 px a row, and no arrangement of anything else can change
 * it. 48 px rows would need 522 px of body, which the wide shape does not
 * have and cannot be given.
 *
 * A cell's WIDTH is the axis with room to spare - the board uses a third of
 * the width and 810 px are left over - so the board takes what the two
 * columns beside it do not need, and its cells are wider than they are tall.
 * That is the only way this shape has of making a target bigger, and it is
 * worth about half as much again in area.
 */

int fleet_cell_for_height(int32_t h)
{
    int cell = fleet_grid_cell_for_span((int)h, 1);

    /* Never coarser than the tall shape's cell: a taller body does not grow
     * the board past 48 px, it gives the room to what is said about it. */
    return cell > FLEET_CELL_TALL ? FLEET_CELL_TALL : cell;
}

int fleet_cell_across(int32_t w, int cell_down)
{
    int room = (int)w - FLEET_COL_GAP - (2 * FLEET_COL_MIN + FLEET_COL_GAP);
    int cell = fleet_grid_cell_for_span(room, 1);
    /* Half as wide again and no wider. Past that a board of ten by ten stops
     * reading as a board, and the cell is already comfortably over the 48 px
     * the tall shape draws. */
    int widest = cell_down * 3 / 2;

    if (cell > widest) {
        cell = widest;
    }
    if (cell < cell_down) {
        cell = cell_down;       /* never narrower than it is tall */
    }
    return cell;
}

int fleet_shape_is_wide(int32_t w, int32_t h, int *cell_w_out, int *cell_h_out)
{
    int cell = fleet_cell_for_height(h);
    int span;

    if (w <= h) {
        return 0;               /* not a wide body at all */
    }
    if (cell < FLEET_CELL_MIN) {
        return 0;               /* the board would be finer than it may be */
    }
    /* The floor is measured on a square board: what has to fit is the board
     * at its smallest, the gutter to it, and two columns that can each hold a
     * 64 px action and a line of text. Short of that the tall stack is kept
     * whole and scrolled, which is what the body did before this layout
     * existed. Only once the shape is taken does the board spread into the
     * width that is left. */
    span = fleet_grid_span_for(cell, 1);
    if (w < span + FLEET_COL_GAP + 2 * FLEET_COL_MIN + FLEET_COL_GAP) {
        return 0;
    }
    if (cell_h_out) {
        *cell_h_out = cell;
    }
    if (cell_w_out) {
        *cell_w_out = fleet_cell_across(w, cell);
    }
    return 1;
}

/* ---- the layout --------------------------------------------------------- *
 *
 * The frame is the whole of the body the shell gives the app, and the shape is
 * chosen from its size alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel). Every screen is the
 *   single column of v0.0.10, the boards at 48 px cells, and the frame scrolls
 *   what does not fit - which is what the shell's body did before.
 *
 *   WIDE (landscape: 1192 x 386, once the foot has cleared the rounded
 *   corners). The board is as large as the body allows on both axes and
 *   everything said about it stands beside it in two columns. Ten rows in
 *   386 px give 34 px, which no layout can improve on; the width has room to
 *   spare, so a cell is drawn wider than it is tall. Aim-then-confirm does not
 *   change: aiming only moves the crosshair, which is harmless and
 *   correctable, and FIRE is still the only thing that commits a shot.
 *
 * Whatever the shape, the frame pads itself by however far the panel's rounded
 * corner squares reach into it - handed over by the shared layout guard
 * (DS §22.2, §23.4), never worked out here - so nothing at the foot is cut.
 */

static void fleet_app_layout(struct fleet_app *app)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;
    int cell = FLEET_CELL_TALL;
    int cell_w = FLEET_CELL_TALL;
    int wide;

    if (!app) {
        return;
    }
    /* No frame, nothing to lay out in, or nothing that the layout is chosen
     * from has changed - a pass that ran anyway would be the whole cost of
     * this app repeated on every one. PocketUI owns that decision for every
     * responsive app, insets included: the same box on a panel with different
     * corners leaves a different amount of room, so a box alone is not enough
     * to say the answer is unchanged. */
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
    wide = fleet_shape_is_wide(w, h, &cell_w, &cell);
    if (!wide) {
        cell = FLEET_CELL_TALL;
        cell_w = FLEET_CELL_TALL;
    }
    app->shape = (uint8_t)(wide ? FLEET_SHAPE_WIDE : FLEET_SHAPE_TALL);
    app->cell = cell;
    app->cell_across = cell_w;

    /* The wide shape fits by construction, so nothing scrolls at the top
     * level; the tall stack is longer than the body and always has been. The
     * frame is the one thing that always clips, whichever shape is in force:
     * it is the body's content box, and nothing of this app may be drawn in
     * the padding the shell left round it. A screen that puts a captioned
     * panel against the frame's top edge leaves the caption its own room
     * (fleet_screen_command.c, and the wide shapes of Battle and Deploy). */
    if (wide) {
        lv_obj_scroll_to_y(app->frame, 0, LV_ANIM_OFF);
        lv_obj_remove_flag(app->frame, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_add_flag(app->frame, LV_OBJ_FLAG_SCROLLABLE);
    }
    /* Every screen is laid out, not only the visible one: a hidden screen has
     * to be right the moment it is shown, and doing one at a time would leave
     * the others carrying the shape the body no longer has. */
    fleet_screen_command_relayout(app, wide);
    fleet_screen_deploy_relayout(app, wide, cell_w, cell);
    fleet_screen_battle_relayout(app, wide, cell_w, cell);
    fleet_screen_result_relayout(app, wide);
}

/* The frame is the body's content box, so this is the body changing size: the
 * shell's content area was resized, or this is the first layout pass after the
 * app was built. */
static void on_frame_size(lv_event_t *e)
{
    fleet_app_layout(lv_event_get_user_data(e));
}

void fleet_app_show(struct fleet_app *app, enum fleet_screen screen)
{
    char status[48];
    int i;

    if (!app || (unsigned)screen >= FLEET_SCREEN_COUNT || !app->screen[screen]) {
        return;
    }
    if (app->current == FLEET_SCREEN_BATTLE && screen != FLEET_SCREEN_BATTLE) {
        fleet_screen_battle_leave(app);
    }
    for (i = 0; i < FLEET_SCREEN_COUNT; i++) {
        if (app->screen[i]) {
            lv_obj_add_flag(app->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(app->screen[screen], LV_OBJ_FLAG_HIDDEN);
    app->current = (uint8_t)screen;
    /* The frame is the scroller in the tall shape; in the wide one it does not
     * scroll and this is a no-op. */
    lv_obj_scroll_to_y(app->frame ? app->frame : app->body, 0, LV_ANIM_OFF);

    switch (screen) {
    case FLEET_SCREEN_COMMAND:
        fleet_screen_command_refresh(app);
        pocketos_shell_set_status_hint("COMMAND");
        return;
    case FLEET_SCREEN_DEPLOY:
        fleet_screen_deploy_refresh(app);
        break;
    case FLEET_SCREEN_BATTLE:
        fleet_screen_battle_refresh(app);
        break;
    case FLEET_SCREEN_RESULT:
        fleet_screen_result_refresh(app);
        break;
    default:
        break;
    }
    if (fleet_view_status(&app->game, status, sizeof(status)) == 0) {
        pocketos_shell_set_status_hint(status);
    }
}

void fleet_app_autosave(struct fleet_app *app)
{
    if (!app || !app->storage_ok) {
        return;
    }
    if (fleet_game_is_over(&app->game)) {
        /* A finished match is not worth resuming, so the slot is freed
         * rather than filled with something Resume would refuse anyway. */
        fleet_store_clear();
        app->resumable = 0;
        return;
    }
    if (fleet_store_save(&app->game) != 0) {
        /* One failure is enough: retrying every turn would spend the whole
         * match writing to a filesystem that has already said no. */
        app->storage_ok = 0;
        app->resumable = 0;
        LOG_WARN("fleet: cannot write %s, continuing without persistence",
                 fleet_store_path());
        fleet_screen_command_refresh(app);
    }
}

void fleet_app_resume(struct fleet_app *app)
{
    if (!app || !app->resumable) {
        return;
    }
    app->resumable = 0;
    app->difficulty = app->game.difficulty;
    switch (app->game.phase) {
    case FLEET_PHASE_DEPLOY:
        fleet_screen_deploy_enter(app);
        fleet_app_show(app, FLEET_SCREEN_DEPLOY);
        break;
    case FLEET_PHASE_OVER:
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        break;
    default:
        fleet_screen_battle_enter(app);
        fleet_app_show(app, FLEET_SCREEN_BATTLE);
        break;
    }
}

void fleet_app_new_match(struct fleet_app *app)
{
    uint32_t seed;

    if (!app) {
        return;
    }
    if (app->storage_ok) {
        fleet_store_clear();
    }
    app->resumable = 0;
    /* The engine is deterministic given a seed; the seed itself is taken from
     * the clock so successive engagements differ, and it is stored in the
     * match so one can be replayed exactly. */
    seed = (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get();
    fleet_game_new(&app->game, seed, (enum fleet_difficulty)app->difficulty);
    fleet_screen_deploy_enter(app);
    fleet_app_show(app, FLEET_SCREEN_DEPLOY);
}

/* ---- development aid --------------------------------------------------- */

/* $POCKETFLEET_SCREEN opens the app on a given screen with a fixed seed, so
 * the simulator can render every screen for design review the way the shell's
 * own --screenshot does. It does nothing unless the variable is set. */
#define FLEET_DEBUG_SEED 20260905u

/* Fire at a cell for the player and let the opponent answer. */
static void debug_exchange(struct fleet_app *app, int row, int col)
{
    if (fleet_game_fire(&app->game, FLEET_SIDE_PLAYER, row, col, NULL) == FLEET_SHOT_INVALID) {
        return;
    }
    if (!fleet_game_is_over(&app->game)) {
        fleet_game_opponent_turn(&app->game, NULL, NULL, NULL);
    }
    /* Same save point as a turn played by hand, so the persistence path is
     * exercised by the headless tests too. */
    fleet_app_autosave(app);
}

static void debug_start_match(struct fleet_app *app)
{
    struct fleet_rng deploy;

    fleet_game_new(&app->game, FLEET_DEBUG_SEED, (enum fleet_difficulty)app->difficulty);
    fleet_screen_deploy_enter(app);
    fleet_rng_seed(&deploy, FLEET_DEBUG_SEED ^ 0x5A5A5A5Au);
    fleet_board_autoplace(&app->game.board[FLEET_SIDE_PLAYER], &deploy);
}

static void debug_open(struct fleet_app *app)
{
    const char *want = getenv("POCKETFLEET_SCREEN");
    const struct fleet_board *enemy;
    int i;

    if (!want) {
        return;
    }
    if (strcmp(want, "deploy") == 0) {
        debug_start_match(app);
        /* Two ships left waiting, so both roster states are visible. */
        fleet_board_unplace(&app->game.board[FLEET_SIDE_PLAYER], FLEET_SHIP_CRUISER);
        fleet_board_unplace(&app->game.board[FLEET_SIDE_PLAYER], FLEET_SHIP_DESTROYER);
        fleet_app_show(app, FLEET_SCREEN_DEPLOY);
        return;
    }
    if (strcmp(want, "battle") != 0 && strcmp(want, "battle_paced") != 0 &&
        strcmp(want, "result") != 0) {
        return;
    }
    debug_start_match(app);
    fleet_game_start(&app->game);
    fleet_screen_battle_enter(app);
    enemy = &app->game.board[FLEET_SIDE_OPPONENT];
    if (strcmp(want, "result") == 0) {
        for (i = 0; i < FLEET_CELLS && !fleet_game_is_over(&app->game); i++) {
            if (enemy->ship_at[i] != FLEET_NO_SHIP) {
                debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
            }
        }
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        return;
    }
    /* A readable mid-game: one ship sunk, a wounded one, and some water. */
    for (i = 0; i < FLEET_CELLS; i++) {
        if (enemy->ship_at[i] == FLEET_SHIP_DESTROYER) {
            debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
        }
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        if (enemy->ship_at[i] == FLEET_SHIP_CARRIER &&
            app->game.board[FLEET_SIDE_OPPONENT].ships[FLEET_SHIP_CARRIER].hits < 2) {
            debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
        }
    }
    for (i = 3; i < FLEET_CELLS; i += 11) {
        debug_exchange(app, i / FLEET_GRID, i % FLEET_GRID);
    }
    fleet_app_show(app, FLEET_SCREEN_BATTLE);
    /* Aimed but not fired, which is the state the FIRE button acts on. */
    fleet_screen_battle_aim(app, 2, 6);
    if (strcmp(want, "battle_paced") == 0) {
        /* Take the shot through the button's own path, so a headless run
         * exercises the paced reply, its timer and the teardown that has to
         * settle it. */
        fleet_screen_battle_fire(app);
    }
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
    app->storage_ok = 1;
    app->reduced_motion = (uint8_t)(pocketos_shell_reduced_motion() != 0);
    /* A match exists from the start so every screen has something to read. */
    fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);
    /* Appearance of a stored match must never delay or prevent the app from
     * opening: an absent, unreadable, damaged or impossible save simply
     * means there is nothing to resume. */
    {
        int loaded = fleet_store_load(&app->game);

        if (loaded == 0 && !fleet_game_is_over(&app->game)) {
            app->resumable = 1;
            app->difficulty = app->game.difficulty;
            LOG_INFO("fleet: resumable match from %s, %s turn %u", fleet_store_path(),
                     fleet_difficulty_name((enum fleet_difficulty)app->game.difficulty),
                     (unsigned)app->game.turn);
        } else {
            if (loaded < 0) {
                LOG_WARN("fleet: stored match at %s rejected, starting fresh",
                         fleet_store_path());
            }
            fleet_game_new(&app->game, 1u, (enum fleet_difficulty)app->difficulty);
        }
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
    app->shape = FLEET_SHAPE_TALL;
    app->cell = FLEET_CELL_TALL;

    app->screen[FLEET_SCREEN_COMMAND] = fleet_screen_command_create(app, app->frame);
    app->screen[FLEET_SCREEN_DEPLOY] = fleet_screen_deploy_create(app, app->frame);
    app->screen[FLEET_SCREEN_BATTLE] = fleet_screen_battle_create(app, app->frame);
    app->screen[FLEET_SCREEN_RESULT] = fleet_screen_result_create(app, app->frame);
    fleet_screen_deploy_enter(app);
    fleet_app_show(app, FLEET_SCREEN_COMMAND);
    debug_open(app);
    return app;
}

static void fleet_destroy(void *priv)
{
    struct fleet_app *app = priv;

    if (!app) {
        return;
    }
    /* Settle a paced turn and stop every timer before the objects they refer
     * to go away with the shell's root. */
    fleet_screen_battle_leave(app);
    /* Nothing may lay out against a half-freed app: the shell deletes the
     * body's children after this returns, and a layout pass in between would
     * reach the screens through a struct that is already gone. */
    if (app->frame) {
        lv_obj_remove_event_cb_with_user_data(app->frame, on_frame_size, app);
        app->frame = NULL;
    }
    /* The screen containers are children of the shell's root and are deleted
     * with it; only the private blocks are ours to release. */
    free(app->command);
    free(app->deploy);
    free(app->battle);
    free(app->result);
    free(app);
}

LV_IMAGE_DECLARE(pos_app_icon_fleet);

const struct pocketos_app app_fleet = {
    .id = "fleet",
    .name = "Fleet",
    /* The launcher draws the Doors icon (DS §20); the glyph stays as the
     * app's text icon, the closest LV_SYMBOL. */
    .icon = LV_SYMBOL_GPS,
    .icon_mask = &pos_app_icon_fleet,
    .create = fleet_create,
    .tick = NULL,
    .destroy = fleet_destroy,
};
