/*
 * PocketFleet Battle screen: the target grid, the turn loop and your waters.
 *
 * A whole normal turn is on the screen at once and nothing a turn needs is
 * ever reached by scrolling - in either shape, at any type size, with any
 * corner (DS 28.6). Across the page that is a deliberate arrangement rather
 * than a happy fit: the board takes the height, which is the scarce
 * dimension, and what is left over is spent in the order the turn is played -
 * what you are aiming at, then the button that commits it, then a glance at
 * your own waters.
 *
 * Aim-then-confirm (approved deviation for dense grids): aiming at the target
 * grid only moves the crosshair, which is harmless and correctable. The shot
 * is committed by the 64 px FIRE button and nothing else. That is true at
 * every board size.
 *
 * Across the page a row is 34 px, and no layout can make it more: ten rows in
 * a 386 px body is the whole of the arithmetic. So the player is never asked
 * to hit a row. There are two ways to aim and neither needs a small target:
 *
 *   - land anywhere on the board and slide. The square under the finger is
 *     reported the whole way and named in the readout beside the board, so
 *     the aim is corrected by watching rather than by hitting;
 *   - the four STEP buttons, each a finger's size, move the crosshair one
 *     square. With no crosshair the first one starts it in the middle.
 *
 * Between them, every square on the board is reachable exactly without a
 * single precise touch, which is what deviation D1 assumes when it calls a
 * mis-aim correctable.
 *
 * The objects are built once. Changing shape moves and resizes them and
 * changes the board's cell size; it never creates or deletes anything, so a
 * turn in flight - the crosshair, the paced reply, the log line - survives a
 * relayout untouched.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "../fleet_app.h"

#include "fleet_grid.h"
#include "fleet_view.h"
#include "fleet_view_mp.h"
#include "fleet_widgets.h"

#include "../link/fleet_session.h"

#include <stdio.h>
#include <stdlib.h>

/* Your own waters are read, never aimed at, so the board is small and its
 * size is fixed rather than derived: 20 px in both shapes. Across the page it
 * was briefly larger, which made the panel holding it as tall as the target
 * board and the screen read as two boards of equal standing. It is the lesser
 * of the two by a long way - 218 px against 382 - and it is the last thing
 * the turn needs, so it is the one that gives up room. */
#define OWN_CELL 20
/* The one-square nudges: four of them, and the gap between them. */
#define STEP_COUNT 4
#define STEP_GAP 8
#define PIP_H 14
#define PIP_UNIT 7
#define PIP_GAP 6
/* Explicit, because LV_SIZE_CONTENT on a flex row inside a space-between row
 * measured short and clipped the leading bars. */
#define PIP_BAR_W (FLEET_HULL_CELLS * PIP_UNIT + (FLEET_SHIP_COUNT - 1) * PIP_GAP)

struct fleet_battle_ui {
    struct fleet_app *app;
    lv_obj_t *target;
    lv_obj_t *own;
    lv_obj_t *cell_value;
    lv_obj_t *pip[FLEET_SHIP_COUNT];
    lv_obj_t *fire;
    lv_obj_t *step;                 /* the four one-square nudges */
    lv_obj_t *note;
    lv_obj_t *log;
    /* Everything beside the board in the wide shape, stacked under it in the
     * tall one. side holds all of it; cols holds the two columns; act carries
     * the readout, waters your own board.
     *
     * Two things move between shapes rather than being built twice, the way
     * Command moves RESUME: FIRE, which is at the foot of the readout column
     * down the page and spans the whole region across it; and the log line,
     * which reports the exchange just played and so belongs with the readout
     * across the page, where the room for it is. */
    lv_obj_t *side;
    lv_obj_t *cols;
    lv_obj_t *act;
    lv_obj_t *waters;
    lv_obj_t *target_panel;
    lv_obj_t *waters_panel;
    uint8_t exchanged;   /* an exchange has been reported in the log line */
    /* The opponent's reply is paced a moment after the player's shot so the
     * two are readable apart. It is never a blocking wait: input stays live
     * and the reply is settled at once if the screen is left. */
    lv_timer_t *reply;
    uint8_t awaiting;
    char own_text[40];
    /* Multiplayer: the plies already shown, so each new one is flashed once,
     * on the board it landed on. */
    int mp_resolved;
};

/* The match in hand, when this is a multiplayer battle; NULL otherwise. */
static struct fleet_match *mp_match(struct fleet_battle_ui *ui)
{
    struct fleet_app *app = ui->app;

    return app->mode == FLEET_MODE_MULTI && app->mp && app->mp->ready ? &app->mp->m : NULL;
}

/* Long enough to read your own result, short enough not to feel like a
 * delay. Skipped entirely under reduced motion. */
#define REPLY_PACE_MS 420

static struct fleet_game *game_of(struct fleet_battle_ui *ui)
{
    return &ui->app->game;
}

void fleet_screen_battle_aim(struct fleet_app *app, int row, int col)
{
    if (!app || !app->battle) {
        return;
    }
    fleet_grid_set_cursor(app->battle->target, row, col);
    fleet_screen_battle_refresh(app);
}

void fleet_screen_battle_nudge(struct fleet_app *app, int drow, int dcol)
{
    struct fleet_battle_ui *ui = app ? app->battle : NULL;
    int row = 0;
    int col = 0;

    if (!ui) {
        return;
    }
    if (fleet_grid_get_cursor(ui->target, &row, &col) != 0) {
        /* Nothing aimed yet: start in the middle rather than in a corner, so
         * the first press is never more than five of them from anywhere. */
        row = FLEET_GRID / 2;
        col = FLEET_GRID / 2;
    } else {
        row += drow;
        col += dcol;
        if (row < 0) {
            row = 0;
        }
        if (col < 0) {
            col = 0;
        }
        if (row >= FLEET_GRID) {
            row = FLEET_GRID - 1;
        }
        if (col >= FLEET_GRID) {
            col = FLEET_GRID - 1;
        }
    }
    fleet_screen_battle_aim(app, row, col);
}

static void on_step(lv_event_t *e)
{
    struct fleet_battle_ui *ui = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int i = (int)lv_obj_get_index(btn);
    static const int drow[STEP_COUNT] = { 0, -1, 1, 0 };
    static const int dcol[STEP_COUNT] = { -1, 0, 0, 1 };

    if (!ui || i < 0 || i >= STEP_COUNT) {
        return;
    }
    fleet_screen_battle_nudge(ui->app, drow[i], dcol[i]);
}

/* Four buttons in a row: left, up, down, right. A row rather than a pad
 * because height is what this shape is short of, and each of them still has
 * to be a finger's size. */
static lv_obj_t *fleet_steps(lv_obj_t *parent, struct fleet_battle_ui *ui)
{
    static const char *const label[STEP_COUNT] = {
        LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT
    };
    lv_obj_t *row = fleet_hbox(parent, POCKETUI_TOUCH_MIN, STEP_GAP);
    int i;

    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (i = 0; i < STEP_COUNT; i++) {
        lv_obj_t *btn = fleet_button_secondary(row, label[i], on_step, ui);
        lv_obj_t *glyph = lv_obj_get_child(btn, 0);

        /* The arrows live in the symbol font, not the text one (DS 11). */
        if (glyph) {
            pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
        }
        lv_obj_set_height(btn, POCKETUI_TOUCH_MIN);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(btn, 1);
    }
    return row;
}

/* A tap moves the crosshair and nothing else (aim-then-confirm). */
static void on_target_cell(void *user, int row, int col)
{
    struct fleet_battle_ui *ui = user;

    fleet_screen_battle_aim(ui->app, row, col);
}

/* Play the opponent's half of the exchange and finish the log line. Does no
 * navigation, so it is safe to call while the screen is being left. */
static void settle_reply(struct fleet_battle_ui *ui)
{
    struct fleet_game *game = game_of(ui);
    char enemy[40] = "";
    char line[96];
    int erow = 0;
    int ecol = 0;
    int esunk = -1;
    enum fleet_shot_result reply;

    ui->awaiting = 0;
    reply = fleet_game_opponent_turn(game, &erow, &ecol, &esunk);
    if (reply != FLEET_SHOT_INVALID) {
        fleet_view_shot(erow, ecol, reply, esunk, enemy, sizeof(enemy));
        fleet_grid_flash(ui->own, erow, ecol);
    }
    fleet_view_exchange(ui->own_text, enemy[0] ? enemy : NULL, line, sizeof(line));
    lv_label_set_text(ui->log, line);
    fleet_screen_battle_refresh(ui->app);
    fleet_app_autosave(ui->app);
}

static void on_reply_due(lv_timer_t *timer)
{
    struct fleet_battle_ui *ui = lv_timer_get_user_data(timer);

    ui->reply = NULL;   /* a one-shot timer deletes itself after this call */
    settle_reply(ui);
    if (fleet_game_is_over(game_of(ui))) {
        fleet_app_show(ui->app, FLEET_SCREEN_RESULT);
    }
}

static void fire_now(struct fleet_battle_ui *ui)
{
    struct fleet_game *game = game_of(ui);
    struct fleet_match *m = mp_match(ui);
    char line[96];
    int row = 0;
    int col = 0;
    int sunk = -1;
    enum fleet_shot_result result;

    if (m) {
        /* Out of reach, the button asks where the opponent stands instead. */
        if (fleet_match_link(m) == FLEET_LINK_LOST) {
            fleet_session_resume(ui->app->mp, fleet_app_now(ui->app));
            fleet_app_mp_changed(ui->app);
            return;
        }
        if (fleet_grid_get_cursor(ui->target, &row, &col) != 0 ||
            fleet_session_fire(ui->app->mp, row, col, fleet_app_now(ui->app)) != 0) {
            return;
        }
        /* The answer comes over the air; until then the square shows the
         * crosshair's ring, and the readout says the shot is on its way. */
        fleet_grid_flash(ui->target, row, col);
        fleet_grid_set_cursor(ui->target, -1, -1);
        ui->exchanged = 1;
        fleet_app_mp_changed(ui->app);
        return;
    }
    if (ui->awaiting || fleet_grid_get_cursor(ui->target, &row, &col) != 0) {
        return;
    }
    result = fleet_game_fire(game, FLEET_SIDE_PLAYER, row, col, &sunk);
    if (result == FLEET_SHOT_INVALID) {
        return;
    }
    fleet_view_shot(row, col, result, sunk, ui->own_text, sizeof(ui->own_text));
    fleet_grid_flash(ui->target, row, col);
    fleet_grid_set_cursor(ui->target, -1, -1);
    ui->exchanged = 1;

    if (fleet_game_is_over(game)) {
        fleet_view_exchange(ui->own_text, NULL, line, sizeof(line));
        lv_label_set_text(ui->log, line);
        fleet_screen_battle_refresh(ui->app);
        fleet_app_autosave(ui->app);
        fleet_app_show(ui->app, FLEET_SCREEN_RESULT);
        return;
    }
    if (ui->app->reduced_motion) {
        settle_reply(ui);
        if (fleet_game_is_over(game)) {
            fleet_app_show(ui->app, FLEET_SCREEN_RESULT);
        }
        return;
    }
    /* Show your own result first, then let the opponent answer. The pause is
     * a timer, not a wait: taps keep moving the crosshair throughout. */
    ui->awaiting = 1;
    fleet_view_exchange(ui->own_text, NULL, line, sizeof(line));
    lv_label_set_text(ui->log, line);
    fleet_screen_battle_refresh(ui->app);
    ui->reply = lv_timer_create(on_reply_due, REPLY_PACE_MS, ui);
    if (ui->reply) {
        lv_timer_set_repeat_count(ui->reply, 1);
    } else {
        settle_reply(ui);
        if (fleet_game_is_over(game)) {
            fleet_app_show(ui->app, FLEET_SCREEN_RESULT);
        }
    }
}

static void on_fire(lv_event_t *e)
{
    fire_now(lv_event_get_user_data(e));
}

void fleet_screen_battle_fire(struct fleet_app *app)
{
    if (app && app->battle) {
        fire_now(app->battle);
    }
}

/* One bar per enemy ship, as long as its hull: filled once it has been sunk,
 * which is the only thing the player is told about the enemy fleet. */
static lv_obj_t *fleet_pips(lv_obj_t *parent, lv_obj_t **out)
{
    lv_obj_t *box = fleet_hbox(parent, PIP_H, PIP_GAP);
    int i;

    lv_obj_set_width(box, PIP_BAR_W);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_obj_t *pip = lv_obj_create(box);

        lv_obj_remove_style_all(pip);
        /* The secondary button role is the only one that pairs a surface fill
         * with a hairline border; without the border a surviving hull is
         * almost invisible against the panel. The local size beats the 64 px
         * height that role carries. */
        pos_style_add(pip, POS_STYLE_BUTTON_SECONDARY, 0);
        lv_obj_set_size(pip, fleet_ship_length((enum fleet_ship)i) * PIP_UNIT, PIP_H);
        lv_obj_set_style_radius(pip, 2, 0);
        lv_obj_remove_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
        out[i] = pip;
    }
    return box;
}

lv_obj_t *fleet_screen_battle_create(struct fleet_app *app, lv_obj_t *parent)
{
    struct fleet_battle_ui *ui = calloc(1, sizeof(*ui));
    lv_obj_t *screen;
    lv_obj_t *row;

    if (!ui) {
        return NULL;
    }
    ui->app = app;
    app->battle = ui;
    screen = fleet_app_screen_container(parent);

    ui->target = fleet_grid_create(screen, FLEET_GRID_TARGET, FLEET_CELL_TALL, 1);
    fleet_grid_bind(ui->target, &app->game.board[FLEET_SIDE_OPPONENT]);
    fleet_grid_set_tap_cb(ui->target, on_target_cell, ui);

    /* In the tall shape these boxes are transparent and stack their children
     * at the panel gap, so the screen reads exactly as the single column it
     * was; in the wide shape cols becomes the two columns beside the board and
     * side holds them with FIRE under both. */
    ui->side = fleet_app_box(screen);
    ui->cols = fleet_app_box(ui->side);
    ui->act = fleet_app_box(ui->cols);
    ui->waters = fleet_app_box(ui->cols);

    ui->target_panel = fleet_panel(ui->act, "TARGET");
    row = fleet_row(ui->target_panel, 40, 0);
    ui->cell_value = pocketui_label(row, "\xe2\x80\x94", POS_STYLE_VALUE);
    fleet_pips(row, ui->pip);
    ui->note = pocketui_label(ui->target_panel, "Tap a square, then fire.",
                              POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(ui->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->note, LV_PCT(100));
    /* The nudges belong to the wide shape only: down the page a cell is the
     * 48 px deviation D1 was approved at, and portrait stays the screen
     * v0.0.10 shipped, to the pixel. */
    ui->step = fleet_steps(ui->target_panel, ui);

    ui->fire = pocketui_button(ui->act, "FIRE", on_fire, ui);

    ui->waters_panel = fleet_panel(ui->waters, "YOUR WATERS");
    lv_obj_set_flex_align(ui->waters_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui->own = fleet_grid_create(ui->waters_panel, FLEET_GRID_OWN, OWN_CELL, 0);
    fleet_grid_bind(ui->own, &app->game.board[FLEET_SIDE_PLAYER]);
    ui->log = pocketui_label(ui->waters_panel, "", POS_STYLE_CAPTION);
    lv_label_set_long_mode(ui->log, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->log, LV_PCT(100));
    return screen;
}

/* Move an object to a new parent, at the end, only if it is not there
 * already: re-parenting is not free and this runs on every layout pass. */
static void reparent(lv_obj_t *obj, lv_obj_t *parent)
{
    if (obj && parent && lv_obj_get_parent(obj) != parent) {
        lv_obj_set_parent(obj, parent);
    }
}

/*
 * Across the page the screen is three things, in the order a turn is played.
 *
 *   +-----------------+  +---------------------+ +---------------+
 *   |                 |  | TARGET              | | YOUR WATERS   |
 *   | target board    |  |  F6       [ pips ]  | |  [ own board ]|
 *   | 552 x 382       |  |  Ready to fire.     | |               |
 *   | 51 x 34 cells   |  |  YOU F6 . ENEMY C3  | |               |
 *   |                 |  |  [<] [^] [v] [>]    | |               |
 *   |                 |  +---------------------+ +---------------+
 *   |                 |  +-------------------------------------+
 *   |                 |  |                FIRE                 |
 *   +-----------------+  +-------------------------------------+
 *
 * The board takes the body's whole height, and then as much of the width as
 * the two columns beside it can spare, so its cells are wider than they are
 * tall (DS 28.2). What is left across goes, in order: the readout, which
 * holds what is aimed at, what firing would do, what the last exchange did,
 * and the four one-square nudges; your own waters, only as wide as the small
 * board in it; and FIRE across the foot of both, its foot level with the
 * board's.
 *
 * Nothing is stretched to fill its column. A panel is exactly as tall as what
 * it holds, so a panel's bottom border is the end of it and not the edge of a
 * viewport - which is the whole of why this screen can be read at a glance.
 * The room left over goes to FIRE, and so is spent rather than left as a gap.
 */
void fleet_screen_battle_relayout(struct fleet_app *app, int wide, int cell_w, int cell_h)
{
    struct fleet_battle_ui *ui = app ? app->battle : NULL;

    if (!ui) {
        return;
    }
    fleet_app_screen_flow(app->screen[FLEET_SCREEN_BATTLE], wide, 1);
    fleet_grid_set_cell_size(ui->target, cell_w, cell_h);
    fleet_grid_set_cell(ui->own, OWN_CELL);
    /* A hidden child takes no room in a flex layout, so the tall shape is the
     * stack it always was and the nudges cost it nothing. */
    if (wide) {
        lv_obj_remove_flag(ui->step, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->step, LV_OBJ_FLAG_HIDDEN);
    }

    /* FIRE is at the foot of the readout column down the page, where it has
     * always been, and across the whole region over here. The log goes with
     * the readout across the page and stays under your own board down it. */
    reparent(ui->fire, wide ? ui->side : ui->act);
    reparent(ui->log, wide ? ui->target_panel : ui->waters_panel);
    /* Re-parenting appends, so put the nudges back under the reading matter:
     * what is aimed at, what firing would do, what the last exchange did, and
     * then the controls, nearest FIRE. */
    lv_obj_move_to_index(ui->step, (int32_t)lv_obj_get_child_count(ui->target_panel) - 1);

    /* side takes what the board leaves and is a column in both shapes: the
     * two columns, then FIRE under them. */
    fleet_app_box_column(ui->side, wide);
    fleet_app_box_split(ui->cols, wide);
    fleet_app_box_column(ui->act, wide);
    fleet_app_box_column(ui->waters, wide);
    lv_obj_set_flex_grow(ui->side, wide ? 1 : 0);
    /* Across the page the panels start at the frame's top edge, and the
     * frame clips: the captions on their top borders need that much room
     * of their own. Down the page they sit below the board and rise into
     * the gap above them, as they always have. */
    lv_obj_set_style_pad_top(ui->side, wide ? FLEET_CAPTION_RISE : 0, 0);

    /* The two columns are as tall as what they hold and no taller - your own
     * board is the taller of them and so sets the height, and the readout
     * takes the same so the two panels close on the same line. A panel that
     * ends where its neighbour ends reads as a panel; one that ends at the
     * foot of the screen reads as a view that has been cut off. */
    lv_obj_set_flex_align(ui->cols, LV_FLEX_ALIGN_START,
                          wide ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_height(ui->cols, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->cols, 0);
    lv_obj_set_height(ui->waters, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->waters_panel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->act, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_height(ui->target_panel, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->target_panel, 0);
    lv_obj_set_flex_grow(ui->waters_panel, 0);
    /* The readout's slack goes to the log line, so the nudges come to rest at
     * the foot of the panel whatever the text above them does: controls
     * nearest FIRE, and in one place rather than wherever a wrapped line
     * happens to leave them. */
    lv_obj_set_flex_grow(ui->log, wide ? 1 : 0);
    /* Your own waters are only as wide as the board in them - the panel's
     * caption is out of the layout, so it does not widen it - and the readout
     * takes the rest, because it is the one holding a line of text. */
    lv_obj_set_width(ui->waters_panel, wide ? LV_SIZE_CONTENT : LV_PCT(100));
    lv_obj_set_width(ui->waters, wide ? LV_SIZE_CONTENT : LV_PCT(100));
    lv_obj_set_flex_grow(ui->waters, 0);
    lv_obj_set_flex_grow(ui->act, wide ? 1 : 0);

    /* What the panels leave of the region's height is FIRE's, so the room over
     * is spent on the one thing the turn is for rather than left as a gap.
     * The arithmetic keeps it well clear of the 64 px minimum without a floor
     * having to be set: the columns come to 260 px whatever the type size,
     * because your own board is a fixed number of pixels, and the shortest
     * body the wide shape is taken for is 362, so FIRE is never under 71.
     * Down the page it is the 64 px button it has always been. */
    lv_obj_set_width(ui->fire, LV_PCT(100));
    lv_obj_set_height(ui->fire, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_grow(ui->fire, wide ? 1 : 0);
}

/* A multiplayer battle: the same screen, reading the match. */
static void refresh_mp(struct fleet_battle_ui *ui, struct fleet_match *m)
{
    struct fleet_app *app = ui->app;
    char name[FLEET_CELL_NAME_MAX];
    char peer[40];
    char note[160];
    int row = 0;
    int col = 0;
    int aimed = fleet_grid_get_cursor(ui->target, &row, &col) == 0;
    int lost = fleet_match_link(m) == FLEET_LINK_LOST;
    int ready = 0;
    int k;
    int i;

    fleet_app_peer(app, peer, sizeof(peer));
    /* Each ply once, on the board it landed on. */
    for (k = ui->mp_resolved + 1; k <= m->resolved; k++) {
        int cell = m->log_cell[k];

        fleet_grid_flash(fleet_match_shooter(k) == m->role ? ui->target : ui->own,
                         cell / FLEET_GRID, cell % FLEET_GRID);
    }
    if (m->resolved != ui->mp_resolved) {
        ui->exchanged = m->resolved > 0;
    }
    ui->mp_resolved = m->resolved;

    fleet_view_mp_note(m, peer, fleet_app_now(app), note, sizeof(note));
    if (note[0]) {
        lv_label_set_text(ui->cell_value,
                          aimed && fleet_cell_name(row, col, name, sizeof(name)) == 0 ? name
                                                                                     : "\xe2\x80\x94");
        lv_label_set_text(ui->note, note);
    } else if (aimed && fleet_cell_name(row, col, name, sizeof(name)) == 0) {
        lv_label_set_text(ui->cell_value, name);
        if (m->target.shot[fleet_index(row, col)]) {
            snprintf(note, sizeof(note), "%s has already been fired at.", name);
            lv_label_set_text(ui->note, note);
        } else {
            lv_label_set_text(ui->note, "Ready to fire.");
            ready = 1;
        }
    } else {
        lv_label_set_text(ui->cell_value, "\xe2\x80\x94");
        lv_label_set_text(ui->note, "Tap a square, then fire.");
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        lv_obj_remove_style(ui->pip[i], pos_style(POS_STYLE_CHIP_TX), 0);
        if (fleet_board_ship_sunk(&m->target, (enum fleet_ship)i)) {
            pos_style_add(ui->pip[i], POS_STYLE_CHIP_TX, 0);
        }
    }
    if (ui->exchanged) {
        fleet_view_mp_exchange(m, peer, note, sizeof(note));
    } else {
        char afloat[40];

        fleet_view_afloat(&m->own, afloat, sizeof(afloat));
        snprintf(note, sizeof(note), "YOUR FLEET %s", afloat);
    }
    lv_label_set_text(ui->log, note);
    lv_label_set_text(lv_obj_get_child(ui->fire, 0), lost ? "CHECK LINK" : "FIRE");
    fleet_button_set_enabled(ui->fire, lost || (ready && fleet_match_my_turn(m)));
    fleet_grid_refresh(ui->target);
    fleet_grid_refresh(ui->own);
}

void fleet_screen_battle_refresh(struct fleet_app *app)
{
    struct fleet_battle_ui *ui;
    struct fleet_game *game;
    char name[FLEET_CELL_NAME_MAX];
    char note[64];
    int row = 0;
    int col = 0;
    int aimed;
    int ready = 0;
    int i;

    if (!app || !app->battle) {
        return;
    }
    ui = app->battle;
    if (mp_match(ui)) {
        refresh_mp(ui, mp_match(ui));
        return;
    }
    lv_label_set_text(lv_obj_get_child(ui->fire, 0), "FIRE");
    game = &app->game;
    aimed = fleet_grid_get_cursor(ui->target, &row, &col) == 0;
    if (ui->awaiting) {
        lv_label_set_text(ui->cell_value, "\xe2\x80\x94");
        lv_label_set_text(ui->note, "The enemy is firing.");
    } else if (aimed && fleet_cell_name(row, col, name, sizeof(name)) == 0) {
        lv_label_set_text(ui->cell_value, name);
        if (game->board[FLEET_SIDE_OPPONENT].shot[fleet_index(row, col)]) {
            snprintf(note, sizeof(note), "%s has already been fired at.", name);
            lv_label_set_text(ui->note, note);
        } else {
            lv_label_set_text(ui->note, "Ready to fire.");
            ready = 1;
        }
    } else {
        lv_label_set_text(ui->cell_value, "\xe2\x80\x94");
        lv_label_set_text(ui->note, "Tap a square, then fire.");
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        int sunk = fleet_board_ship_sunk(&game->board[FLEET_SIDE_OPPONENT],
                                         (enum fleet_ship)i);

        lv_obj_remove_style(ui->pip[i], pos_style(POS_STYLE_CHIP_TX), 0);
        if (sunk) {
            pos_style_add(ui->pip[i], POS_STYLE_CHIP_TX, 0);
        }
    }
    if (!ui->exchanged) {
        /* Until the first exchange the log line carries the standing report
         * rather than sitting empty. */
        char afloat[40];

        fleet_view_afloat(&game->board[FLEET_SIDE_PLAYER], afloat, sizeof(afloat));
        snprintf(note, sizeof(note), "YOUR FLEET %s", afloat);
        lv_label_set_text(ui->log, note);
    }
    fleet_button_set_enabled(ui->fire, ready);
    fleet_grid_refresh(ui->target);
    fleet_grid_refresh(ui->own);
}

void fleet_screen_battle_enter(struct fleet_app *app)
{
    struct fleet_battle_ui *ui = app ? app->battle : NULL;

    if (!ui) {
        return;
    }
    if (mp_match(ui)) {
        /* Theirs as far as their answers go; ours as it stands. */
        fleet_grid_bind(ui->target, &mp_match(ui)->target);
        fleet_grid_bind(ui->own, &mp_match(ui)->own);
        ui->mp_resolved = mp_match(ui)->resolved;
    } else {
        fleet_grid_bind(ui->target, &app->game.board[FLEET_SIDE_OPPONENT]);
        fleet_grid_bind(ui->own, &app->game.board[FLEET_SIDE_PLAYER]);
    }
    fleet_grid_set_cursor(ui->target, -1, -1);
    ui->exchanged = 0;
    ui->own_text[0] = '\0';
    lv_label_set_text(ui->log, "");
    if (!app->reduced_motion) {
        fleet_grid_set_motion(ui->target, 1);
        fleet_grid_set_motion(ui->own, 1);
    }
}

void fleet_screen_battle_leave(struct fleet_app *app)
{
    struct fleet_battle_ui *ui = app ? app->battle : NULL;

    if (!ui) {
        return;
    }
    if (ui->reply) {
        lv_timer_delete(ui->reply);
        ui->reply = NULL;
    }
    if (ui->awaiting) {
        /* Never leave a turn half played: the opponent answers at once. */
        settle_reply(ui);
    }
    /* Nothing animates on a screen nobody is looking at. */
    fleet_grid_set_motion(ui->target, 0);
    fleet_grid_set_motion(ui->own, 0);
}
