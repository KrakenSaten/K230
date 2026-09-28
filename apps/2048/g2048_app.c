/*
 * PG 2048: the app. A HUD with the score and the best, the board, and a row
 * of controls that changes with the state of the game.
 *
 * This file is the screen and nothing else. What a move does belongs to the
 * engine (engine/g2048_rules.c), what a key or a swipe means and where
 * everything goes to the view model (ui/g2048_view.c), how the board is drawn
 * to the board widget (ui/g2048_board.c), and the file to g2048_store.c.
 *
 * LAYOUT. One root fills the body the shell gives the app. Its three blocks
 * are placed from g2048_view_layout() whenever the root's size changes, so
 * the arrangement follows the area rather than the panel it is on. Nothing
 * here knows the panel is 568 x 1232.
 *
 * INPUT. The root is the one object in the shell's focus group, and every
 * key the stream delivers to it - simulator keyboard, touch keyboard, physical
 * keyboard - goes through g2048_view_command_for_key() (DS 17.4). A finger
 * that goes down anywhere on the root outside the buttons and comes up far
 * enough away in one direction is a swipe (g2048_view_swipe). The buttons do
 * not take focus when tapped, so taps and keys mix freely. Keys and swipes
 * reach the same commands the buttons do.
 *
 * PERSISTENCE. The game is saved at most once a second from the shell's tick
 * when something changed, at once when a game ends or reaches the goal (so
 * the best score is never left behind), and when the app closes. A move
 * never waits for the card. A failed save is noted on the controls row and
 * play continues.
 *
 * MOTION. A move slides for 100 ms and settles for 90 (g2048_view.h); a key
 * or swipe during that finishes it first. Reduced motion draws none of it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "g2048_app.h"
#include "g2048_board.h"
#include "g2048_store.h"
#include "g2048_view.h"

#include "app.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The view names LVGL's keys by number so it can be tested without LVGL. If
 * LVGL ever renumbers one, this stops the build rather than the key. */
_Static_assert(G2048_KEY_UP == LV_KEY_UP, "g2048_view.h Up is not LVGL's");
_Static_assert(G2048_KEY_DOWN == LV_KEY_DOWN, "g2048_view.h Down is not LVGL's");
_Static_assert(G2048_KEY_LEFT == LV_KEY_LEFT, "g2048_view.h Left is not LVGL's");
_Static_assert(G2048_KEY_RIGHT == LV_KEY_RIGHT, "g2048_view.h Right is not LVGL's");
_Static_assert(G2048_KEY_ENTER == LV_KEY_ENTER, "g2048_view.h Enter is not LVGL's");
_Static_assert(G2048_KEY_ESC == LV_KEY_ESC, "g2048_view.h Esc is not LVGL's");
_Static_assert(G2048_KEY_BACKSPACE == LV_KEY_BACKSPACE, "g2048_view.h Backspace is not LVGL's");

#define BUTTON_H 64
#define BUTTON_GAP 8
_Static_assert(BUTTON_H >= POCKETUI_TOUCH_MIN, "a button must meet the DS 64 px touch minimum");

/* Development aid: $PG2048_SCREEN opens the app in a state from a fixed seed,
 * without reading or writing the save file, so the simulator can render each
 * state for review. Nothing happens unless the variable is set. */
#define G2048_DEBUG_SEED 20260913u

struct g2048_app {
    struct g2048_game game;
    lv_obj_t *root;
    lv_obj_t *hud;
    lv_obj_t *score_panel;
    lv_obj_t *best_panel;
    lv_obj_t *score;
    lv_obj_t *best;
    lv_obj_t *board;
    lv_obj_t *controls;
    lv_obj_t *caption;
    lv_obj_t *buttons;
    lv_obj_t *primary;   /* the accent button, when the panel has one */
    lv_obj_t *secondary; /* the other */
    enum g2048_panel shown;
    int confirming;
    int dirty;
    int save_failed;
    int unreadable; /* the save file was refused at start */
    int session_only;
    lv_point_t press;
    int pressed;
};

/* ---- persistence ------------------------------------------------------------ */

static void save_now(struct g2048_app *a)
{
    if (a->session_only) {
        a->dirty = 0;
        return;
    }
    a->save_failed = g2048_store_save(&a->game) != 0;
    a->dirty = a->save_failed;
}

static uint32_t fresh_seed(const struct g2048_app *a)
{
    /* The engine is deterministic from a seed, and the seed is chosen here at
     * the app boundary, as PocketRadar does: the wall clock separates
     * sessions, the LVGL tick separates games within one, and the move count
     * keeps two games apart on a board whose clock was never set. */
    return (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get() ^ (a->game.moves * 2654435761u);
}

/* ---- painting ------------------------------------------------------------------ */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* A score in hero-40 while it fits its panel, the app title size when it does
 * not: a seven-digit score in a narrow side column must still be whole. */
static void fit_value(lv_obj_t *label, lv_obj_t *panel)
{
    lv_point_t size;
    int32_t room;

    lv_obj_update_layout(panel);
    room = lv_obj_get_content_width(panel);
    lv_obj_remove_style(label, pos_style(POS_STYLE_TITLE), 0);
    lv_obj_add_style(label, pos_style(POS_STYLE_HERO_40), 0);
    lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
                     lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (room > 0 && size.x > room) {
        lv_obj_remove_style(label, pos_style(POS_STYLE_HERO_40), 0);
        lv_obj_add_style(label, pos_style(POS_STYLE_TITLE), 0);
    }
}

static void refresh_hud(struct g2048_app *a)
{
    char buf[16];

    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)a->game.score);
    set_text(a->score, buf);
    fit_value(a->score, a->score_panel);
    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)a->game.best);
    set_text(a->best, buf);
    fit_value(a->best, a->best_panel);
}

static void style_button(lv_obj_t *btn, int primary)
{
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (primary) {
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(btn, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

static void set_button(lv_obj_t *btn, const char *text, enum g2048_cmd cmd, int primary)
{
    if (!text) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    set_text(lv_obj_get_child(btn, 0), text);
    lv_obj_set_user_data(btn, (void *)(intptr_t)cmd);
    style_button(btn, primary);
}

static void refresh_controls(struct g2048_app *a)
{
    enum g2048_panel panel = g2048_view_panel(&a->game, a->confirming);
    const char *hint = "SWIPE OR USE THE ARROW KEYS";

    if (a->save_failed) {
        hint = "NOT SAVED \xC2\xB7 PLAY CONTINUES";
    } else if (a->unreadable && a->game.moves == 0) {
        hint = "SAVED GAME UNREADABLE \xC2\xB7 NEW GAME";
    }
    a->shown = panel;
    /* The first button is the accent one whenever there is an accent
     * action; DS 17.5 puts the accent on the safe choice of a question. */
    switch (panel) {
    case G2048_PANEL_CONFIRM:
        set_text(a->caption, "START A NEW GAME? THIS BOARD WILL BE LOST");
        set_button(a->primary, "KEEP PLAYING", G2048_CMD_CANCEL, 1);
        set_button(a->secondary, "NEW GAME", G2048_CMD_NEW_GAME, 0);
        break;
    case G2048_PANEL_WON:
        set_text(a->caption, "2048 \xC2\xB7 YOU MADE IT");
        set_button(a->primary, "KEEP GOING", G2048_CMD_KEEP_GOING, 1);
        set_button(a->secondary, "NEW GAME", G2048_CMD_NEW_GAME, 0);
        break;
    case G2048_PANEL_OVER:
        set_text(a->caption, "NO MOVES LEFT");
        set_button(a->primary, "NEW GAME", G2048_CMD_NEW_GAME, 1);
        set_button(a->secondary, NULL, G2048_CMD_NONE, 0);
        break;
    case G2048_PANEL_PLAY:
    default:
        set_text(a->caption, hint);
        set_button(a->primary, NULL, G2048_CMD_NONE, 0);
        set_button(a->secondary, "NEW GAME",
                   g2048_view_has_progress(&a->game) ? G2048_CMD_ASK_NEW : G2048_CMD_NEW_GAME, 0);
        break;
    }
    /* LVGL's flex row still counts the gap beside a hidden button, which
     * left a lone button 8 px short of the edge. */
    lv_obj_set_style_pad_column(a->buttons,
                                lv_obj_has_flag(a->primary, LV_OBJ_FLAG_HIDDEN) ||
                                        lv_obj_has_flag(a->secondary, LV_OBJ_FLAG_HIDDEN)
                                    ? 0
                                    : BUTTON_GAP,
                                0);
}

static void refresh(struct g2048_app *a)
{
    refresh_hud(a);
    refresh_controls(a);
}

/* ---- commands ---------------------------------------------------------------- */

static void new_game(struct g2048_app *a)
{
    g2048_new_game(&a->game, fresh_seed(a), a->game.best);
    a->confirming = 0;
    a->unreadable = 0;
    g2048_board_show(a->board, &a->game);
    a->dirty = 1;
}

static void run(struct g2048_app *a, enum g2048_cmd cmd)
{
    enum g2048_dir dir;

    if (g2048_view_cmd_dir(cmd, &dir)) {
        struct g2048_turn turn;

        g2048_board_finish(a->board);
        if (!g2048_move(&a->game, dir, &turn)) {
            return;
        }
        g2048_board_animate(a->board, &a->game, &turn, pocketos_shell_reduced_motion());
        a->unreadable = 0;
        a->dirty = 1;
        if (turn.ended || turn.reached_goal) {
            save_now(a);
        }
        refresh(a);
        return;
    }
    switch (cmd) {
    case G2048_CMD_ASK_NEW:
        a->confirming = 1;
        break;
    case G2048_CMD_CANCEL:
        a->confirming = 0;
        break;
    case G2048_CMD_NEW_GAME:
        new_game(a);
        break;
    case G2048_CMD_KEEP_GOING:
        g2048_keep_going(&a->game);
        a->dirty = 1;
        break;
    default:
        return;
    }
    refresh(a);
}

/* ---- events ------------------------------------------------------------------- */

static void on_key(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);
    enum g2048_panel panel = g2048_view_panel(&a->game, a->confirming);

    run(a, g2048_view_command_for_key(panel, g2048_view_has_progress(&a->game), lv_event_get_key(e)));
}

static void on_press(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();

    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return; /* Enter on the focused root arrives as a press too */
    }
    lv_indev_get_point(indev, &a->press);
    a->pressed = 1;
}

static void on_release(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    enum g2048_dir dir;

    if (!a->pressed || !indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    a->pressed = 0;
    lv_indev_get_point(indev, &p);
    if (g2048_view_panel(&a->game, a->confirming) != G2048_PANEL_PLAY) {
        return;
    }
    if (g2048_view_swipe(p.x - a->press.x, p.y - a->press.y,
                         g2048_view_swipe_threshold(lv_obj_get_width(a->board)), &dir)) {
        run(a, g2048_view_move_cmd(dir));
    }
}

static void on_press_lost(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);

    a->pressed = 0;
}

static void on_button(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);

    run(a, (enum g2048_cmd)(intptr_t)lv_obj_get_user_data(btn));
}

static void place(lv_obj_t *obj, struct g2048_rect r)
{
    lv_obj_set_pos(obj, r.x, r.y);
    lv_obj_set_size(obj, r.w, r.h);
}

static void on_resize(lv_event_t *e)
{
    struct g2048_app *a = lv_event_get_user_data(e);
    struct g2048_layout l;

    g2048_view_layout(lv_obj_get_content_width(a->root), lv_obj_get_content_height(a->root), &l);
    place(a->hud, l.hud);
    place(a->board, l.board);
    place(a->controls, l.controls);
    refresh_hud(a);
}

/* ---- building ------------------------------------------------------------------ */

static lv_obj_t *score_panel(lv_obj_t *parent, const char *caption, lv_obj_t **value)
{
    lv_obj_t *panel = lv_obj_create(parent);

    lv_obj_remove_style_all(panel);
    pos_style_add(panel, POS_STYLE_PANEL, 0);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    pocketui_label(panel, caption, POS_STYLE_CAPTION);
    *value = pocketui_label(panel, "0", POS_STYLE_HERO_40);
    lv_label_set_long_mode(*value, LV_LABEL_LONG_CLIP);
    return panel;
}

static lv_obj_t *button(struct g2048_app *a, lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, BUTTON_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    /* A tap must not move focus off the root, or the next key goes nowhere
     * (DS 17.2). */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, on_button, LV_EVENT_CLICKED, a);
    style_button(btn, 0);
    return btn;
}

static void build(struct g2048_app *a, lv_obj_t *body)
{
    a->root = lv_obj_create(body);
    lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root, LV_PCT(100));
    lv_obj_set_flex_grow(a->root, 1);
    lv_obj_remove_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->root, LV_OBJ_FLAG_CLICKABLE);

    a->hud = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->hud);
    lv_obj_set_flex_flow(a->hud, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->hud, BUTTON_GAP, 0);
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_CLICKABLE);
    a->score_panel = score_panel(a->hud, "SCORE", &a->score);
    a->best_panel = score_panel(a->hud, "BEST", &a->best);

    a->board = g2048_board_create(a->root);

    a->controls = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->controls);
    lv_obj_set_flex_flow(a->controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->controls, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(a->controls, 16, 0);
    lv_obj_remove_flag(a->controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->controls, LV_OBJ_FLAG_CLICKABLE);
    /* The outline a pressed button draws lands outside it, and the buttons
     * sit on the block's edges. */
    lv_obj_add_flag(a->controls, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->caption = pocketui_label(a->controls, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->caption, LV_PCT(100));
    lv_label_set_long_mode(a->caption, LV_LABEL_LONG_DOT);
    a->buttons = lv_obj_create(a->controls);
    lv_obj_remove_style_all(a->buttons);
    lv_obj_set_size(a->buttons, LV_PCT(100), BUTTON_H);
    lv_obj_set_flex_flow(a->buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->buttons, BUTTON_GAP, 0);
    lv_obj_remove_flag(a->buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->buttons, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->buttons, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->primary = button(a, a->buttons);
    a->secondary = button(a, a->buttons);

    lv_obj_add_event_cb(a->root, on_resize, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_add_event_cb(a->root, on_key, LV_EVENT_KEY, a);
    lv_obj_add_event_cb(a->root, on_press, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->root, on_release, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->root, on_press_lost, LV_EVENT_PRESS_LOST, a);
    pos_input_add_obj(a->root);
    pos_input_focus(a->root);
}

/* The saved game, or a new one. A finished game is not resumed: its best
 * score is kept and a new board is dealt. */
static void start(struct g2048_app *a)
{
    int rc = g2048_store_load(&a->game);

    if (rc == 0 && !a->game.over) {
        return;
    }
    a->unreadable = rc < 0;
    g2048_new_game(&a->game, fresh_seed(a), rc == 0 ? a->game.best : 0);
    a->dirty = rc == 0;
}

static void debug_state(struct g2048_app *a, const char *want)
{
    static const enum g2048_dir walk[] = { G2048_LEFT, G2048_DOWN, G2048_RIGHT, G2048_DOWN };
    int i;

    a->session_only = 1;
    g2048_new_game(&a->game, G2048_DEBUG_SEED, 0);
    for (i = 0; i < 120 && !a->game.over; i++) {
        g2048_move(&a->game, walk[i % 4], NULL);
    }
    if (strcmp(want, "won") == 0) {
        static const uint8_t won[G2048_CELLS] = { 11, 7, 3, 1, 9, 6, 2, 0, 5, 4, 1, 0, 2, 1, 0, 0 };

        memcpy(a->game.cell, won, sizeof(won));
        a->game.won = 1;
        a->game.over = 0;
        a->game.keep_going = 0;
        a->game.score = a->game.score < 20480 ? 20480 : a->game.score;
        a->game.best = a->game.score;
    } else if (strcmp(want, "over") == 0) {
        for (i = 0; i < 4000 && !a->game.over; i++) {
            g2048_move(&a->game, walk[i % 4], NULL);
            g2048_move(&a->game, G2048_UP, NULL);
        }
    } else if (strcmp(want, "confirm") == 0) {
        a->confirming = 1;
    }
}

static void *g2048_create(lv_obj_t *body)
{
    struct g2048_app *a = lv_malloc_zeroed(sizeof(*a));
    const char *debug = getenv("PG2048_SCREEN");

    if (!a) {
        return NULL;
    }
    build(a, body);
    if (debug && *debug) {
        debug_state(a, debug);
    } else {
        start(a);
    }
    lv_obj_update_layout(a->root);
    {
        struct g2048_layout l;

        g2048_view_layout(lv_obj_get_content_width(a->root), lv_obj_get_content_height(a->root), &l);
        place(a->hud, l.hud);
        place(a->board, l.board);
        place(a->controls, l.controls);
    }
    g2048_board_show(a->board, &a->game);
    refresh(a);
    return a;
}

static void g2048_tick(void *priv)
{
    struct g2048_app *a = priv;

    if (a && a->dirty) {
        save_now(a);
        refresh_controls(a);
    }
}

static void g2048_destroy(void *priv)
{
    struct g2048_app *a = priv;

    if (!a) {
        return;
    }
    /* The shell deletes the objects under the body, which also takes the
     * root out of the focus group and stops the board's animation. */
    if (a->dirty) {
        save_now(a);
    }
    lv_free(a);
}

/* ---- for tests (g2048_app.h) ------------------------------------------------- */

const struct g2048_game *g2048_app_game(void *priv)
{
    return priv ? &((struct g2048_app *)priv)->game : NULL;
}

int g2048_app_confirming(void *priv)
{
    return priv ? ((struct g2048_app *)priv)->confirming : 0;
}

lv_obj_t *g2048_app_board(void *priv)
{
    return priv ? ((struct g2048_app *)priv)->board : NULL;
}

lv_obj_t *g2048_app_caption(void *priv)
{
    return priv ? ((struct g2048_app *)priv)->caption : NULL;
}

lv_obj_t *g2048_app_button(void *priv, int index)
{
    struct g2048_app *a = priv;

    if (!a) {
        return NULL;
    }
    return index == 0 ? a->primary : a->secondary;
}

const struct pocketos_app app_2048 = {
    .id = "2048",
    .name = "2048",
    /* A placeholder until the DS section 11 icon set exists: LVGL's symbol
     * font has no grid, and the crossing arrows say "slide". */
    .icon = LV_SYMBOL_SHUFFLE,
    .create = g2048_create,
    .tick = g2048_tick,
    .destroy = g2048_destroy,
};
