/*
 * PG Solitaire: the app. A HUD with the move count and the cards home, the
 * felt table, and a row with a caption and the buttons.
 *
 * This file is the screen and nothing else. The rules are
 * engine/sol_rules.c; what a key or a tap does to the cursor, the selection
 * and the game, where every pile and card is, and every word on the caption
 * are ui/sol_view.c; drawing the table is ui/sol_table_widget.c.
 *
 * LAYOUT. One root fills the body the shell gives the app and places its
 * three blocks from sol_view_screen() whenever its size changes. The table
 * sizes its own cards from the size it is given.
 *
 * INPUT. Keyboard first: the root is the one object in the shell's focus
 * group and every key goes through sol_view_key() (DS 17.4). Touch beside it:
 * a tap on the table becomes a hit and goes through sol_view_tap(); there is
 * no drag. Neither the table nor the buttons take the focus when tapped.
 *
 * PERSISTENCE. The deal is kept in sol_store.c's file and resumed when the
 * app opens again, with the cursor and any selection starting fresh. It is
 * saved at most once a second from the shell's tick when a move, a draw or a
 * new deal changed it, at once when the game is won, and when the app
 * closes. Opening a fresh deal and leaving without touching it writes
 * nothing. A won game is not resumed; a new deal is dealt. A save that
 * cannot be read is refused whole, left in place until the next save
 * replaces it, and said on the caption; a save that fails is said too, and
 * play goes on.
 *
 * No motion: every change is immediate, so reduced motion has nothing to
 * switch off.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_app.h"
#include "sol_store.h"
#include "sol_table_widget.h"

#include "app.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(SOL_KEY_UP == LV_KEY_UP, "sol_view.h Up is not LVGL's");
_Static_assert(SOL_KEY_DOWN == LV_KEY_DOWN, "sol_view.h Down is not LVGL's");
_Static_assert(SOL_KEY_LEFT == LV_KEY_LEFT, "sol_view.h Left is not LVGL's");
_Static_assert(SOL_KEY_RIGHT == LV_KEY_RIGHT, "sol_view.h Right is not LVGL's");
_Static_assert(SOL_KEY_ENTER == LV_KEY_ENTER, "sol_view.h Enter is not LVGL's");
_Static_assert(SOL_KEY_ESC == LV_KEY_ESC, "sol_view.h Esc is not LVGL's");
_Static_assert(SOL_KEY_BACKSPACE == LV_KEY_BACKSPACE, "sol_view.h Backspace is not LVGL's");

#define BUTTON_H 64
#define BUTTON_GAP 8
_Static_assert(BUTTON_H >= POCKETUI_TOUCH_MIN, "a button must meet the DS 64 px touch minimum");

/* Development aid: $PGSOLITAIRE_SCREEN opens a fixed deal in a given state
 * so the simulator can render it for review, without reading or writing the
 * save file. Nothing happens unless set. */
#define SOL_DEBUG_SEED 20260913u

enum sol_button {
    SOL_BUTTON_NONE = 0,
    SOL_BUTTON_NEW_GAME,
    SOL_BUTTON_KEEP_PLAYING
};

struct sol_app {
    struct sol_game game;
    struct sol_ui ui;
    lv_obj_t *root;
    lv_obj_t *hud;
    lv_obj_t *moves;
    lv_obj_t *home;
    lv_obj_t *table;
    lv_obj_t *controls;
    lv_obj_t *caption;
    lv_obj_t *buttons;
    lv_obj_t *primary;
    lv_obj_t *secondary;
    int dirty;        /* the game changed since it was last saved */
    int save_failed;  /* the last save did not reach the file */
    int unreadable;   /* the save found at open was refused */
    int session_only; /* a review state: never read or write the file */
};

/* ---- persistence --------------------------------------------------------------------- */

static void save_now(struct sol_app *a)
{
    if (a->session_only) {
        a->dirty = 0;
        return;
    }
    a->save_failed = sol_store_save(&a->game) != 0;
    a->dirty = a->save_failed;
}

static uint32_t fresh_seed(const struct sol_app *a)
{
    /* The deal is a function of its seed, chosen here at the app boundary as
     * PocketRadar does: the wall clock separates sessions, the LVGL tick
     * separates deals within one, and the move count keeps two deals apart
     * on a board whose clock was never set. */
    return (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get() ^ (a->game.moves * 2654435761u);
}

/* ---- painting -------------------------------------------------------------------- */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
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

static void set_button(lv_obj_t *btn, const char *text, enum sol_button what, int primary)
{
    if (!text) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    set_text(lv_obj_get_child(btn, 0), text);
    lv_obj_set_user_data(btn, (void *)(intptr_t)what);
    style_button(btn, primary);
}

static void refresh(struct sol_app *a)
{
    char buf[96];

    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)a->game.moves);
    set_text(a->moves, buf);
    lv_snprintf(buf, sizeof(buf), "%d/52", sol_cards_home(&a->game));
    set_text(a->home, buf);
    sol_view_caption(&a->ui, &a->game, buf, sizeof(buf));
    /* The file's news takes the caption only when the view has nothing more
     * pressing to say: no question, no refusal, no selection, no win. */
    if (!a->game.won && !a->ui.confirming && a->ui.note == SOL_NOTE_NONE && a->ui.sel_pile < 0) {
        if (a->save_failed) {
            lv_snprintf(buf, sizeof(buf), "NOT SAVED \xC2\xB7 PLAY CONTINUES");
        } else if (a->unreadable) {
            lv_snprintf(buf, sizeof(buf), "SAVED GAME UNREADABLE \xC2\xB7 NEW DEAL");
        }
    }
    set_text(a->caption, buf);
    if (a->game.won) {
        set_button(a->primary, "NEW GAME", SOL_BUTTON_NEW_GAME, 1);
        set_button(a->secondary, NULL, SOL_BUTTON_NONE, 0);
    } else if (a->ui.confirming) {
        /* DS 17.5: the accent on the safe choice. */
        set_button(a->primary, "KEEP PLAYING", SOL_BUTTON_KEEP_PLAYING, 1);
        set_button(a->secondary, "NEW GAME", SOL_BUTTON_NEW_GAME, 0);
    } else {
        set_button(a->primary, NULL, SOL_BUTTON_NONE, 0);
        set_button(a->secondary, "NEW GAME", SOL_BUTTON_NEW_GAME, 0);
    }
    /* LVGL's flex row counts the gap beside a hidden button. */
    lv_obj_set_style_pad_column(a->buttons,
                                lv_obj_has_flag(a->primary, LV_OBJ_FLAG_HIDDEN) ||
                                        lv_obj_has_flag(a->secondary, LV_OBJ_FLAG_HIDDEN)
                                    ? 0
                                    : BUTTON_GAP,
                                0);
    lv_obj_invalidate(a->table);
}

/* ---- commands ----------------------------------------------------------------------- */

static void deal(struct sol_app *a, uint32_t seed)
{
    sol_deal(&a->game, seed);
    sol_view_reset(&a->ui, &a->game);
}

/* After the view has run a command: deal if it asked, and note whether the
 * game itself changed. Every move and draw advances the move count, so a
 * count that did not move means only the cursor, the selection or a caption
 * did, and there is nothing to save. */
static void apply(struct sol_app *a, enum sol_cmd cmd, uint32_t moves_before, uint8_t won_before)
{
    if (cmd == SOL_CMD_NEW_GAME) {
        deal(a, fresh_seed(a));
        a->dirty = 1;
        a->unreadable = 0;
    } else if (a->game.moves != moves_before) {
        a->dirty = 1;
        a->unreadable = 0;
        if (a->game.won && !won_before) {
            save_now(a);
        }
    }
    if (cmd != SOL_CMD_NONE) {
        refresh(a);
    }
}

static void on_key(lv_event_t *e)
{
    struct sol_app *a = lv_event_get_user_data(e);
    uint32_t moves = a->game.moves;
    uint8_t won = a->game.won;

    apply(a, sol_view_key(&a->ui, &a->game, lv_event_get_key(e)), moves, won);
}

static void on_tap(void *user, struct sol_hit hit)
{
    struct sol_app *a = user;
    uint32_t moves = a->game.moves;
    uint8_t won = a->game.won;

    apply(a, sol_view_tap(&a->ui, &a->game, hit), moves, won);
}

static void on_button(lv_event_t *e)
{
    struct sol_app *a = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    uint32_t moves = a->game.moves;
    uint8_t won = a->game.won;

    switch ((enum sol_button)(intptr_t)lv_obj_get_user_data(btn)) {
    case SOL_BUTTON_NEW_GAME:
        apply(a, sol_view_new_game_button(&a->ui, &a->game), moves, won);
        break;
    case SOL_BUTTON_KEEP_PLAYING:
        apply(a, sol_view_keep_playing_button(&a->ui), moves, won);
        break;
    default:
        break;
    }
}

/* ---- building ----------------------------------------------------------------------- */

static void place(lv_obj_t *obj, struct sol_rect r)
{
    lv_obj_set_pos(obj, r.x, r.y);
    lv_obj_set_size(obj, r.w, r.h);
}

static void layout(struct sol_app *a)
{
    struct sol_screen s;

    sol_view_screen(lv_obj_get_content_width(a->root), lv_obj_get_content_height(a->root), &s);
    place(a->hud, s.hud);
    place(a->table, s.table);
    place(a->controls, s.controls);
    /* Under a tall table the caption is one line, cut with an ellipsis (a
     * dotted label only cuts at a fixed height; left to size itself it
     * wraps). In a side column it wraps, upwards into the column. */
    if (s.arrangement == SOL_SIDE_BY_SIDE) {
        lv_label_set_long_mode(a->caption, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(a->caption, LV_SIZE_CONTENT);
    } else {
        lv_label_set_long_mode(a->caption, LV_LABEL_LONG_DOT);
        lv_obj_set_height(a->caption, lv_font_get_line_height(lv_obj_get_style_text_font(a->caption, 0)));
    }
}

static void on_resize(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static lv_obj_t *hud_panel(lv_obj_t *parent, const char *caption, lv_obj_t **value)
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
    *value = pocketui_label(panel, "0", POS_STYLE_TITLE);
    return panel;
}

static lv_obj_t *button(struct sol_app *a, lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, BUTTON_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, on_button, LV_EVENT_CLICKED, a);
    style_button(btn, 0);
    return btn;
}

static void build(struct sol_app *a, lv_obj_t *body)
{
    a->root = lv_obj_create(body);
    lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root, LV_PCT(100));
    lv_obj_set_flex_grow(a->root, 1);
    lv_obj_remove_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);

    a->hud = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->hud);
    lv_obj_set_flex_flow(a->hud, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->hud, BUTTON_GAP, 0);
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_SCROLLABLE);
    hud_panel(a->hud, "MOVES", &a->moves);
    hud_panel(a->hud, "HOME", &a->home);

    a->table = sol_table_create(a->root);
    sol_table_bind(a->table, &a->game, &a->ui);
    sol_table_on_tap(a->table, on_tap, a);

    a->controls = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->controls);
    lv_obj_set_flex_flow(a->controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->controls, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(a->controls, 16, 0);
    lv_obj_remove_flag(a->controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->controls, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->caption = pocketui_label(a->controls, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->caption, LV_PCT(100));
    lv_label_set_long_mode(a->caption, LV_LABEL_LONG_DOT);
    a->buttons = lv_obj_create(a->controls);
    lv_obj_remove_style_all(a->buttons);
    lv_obj_set_size(a->buttons, LV_PCT(100), BUTTON_H);
    lv_obj_set_flex_flow(a->buttons, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(a->buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->buttons, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->primary = button(a, a->buttons);
    a->secondary = button(a, a->buttons);

    lv_obj_add_event_cb(a->root, on_resize, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_add_event_cb(a->root, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->root);
    pos_input_focus(a->root);
}

/* The saved deal, or a new one. A won game is not resumed; its deal has
 * nothing left to play. An untouched new deal is not written until it is
 * played; one that replaces a won game is, so the win is not offered again. */
static void start(struct sol_app *a)
{
    int rc = sol_store_load(&a->game);

    if (rc == 0 && !a->game.won) {
        sol_view_reset(&a->ui, &a->game);
        return;
    }
    a->unreadable = rc < 0;
    deal(a, fresh_seed(a));
    a->dirty = rc == 0;
}

/* Review states from a fixed deal. */
static void debug_state(struct sol_app *a, const char *want)
{
    a->session_only = 1;
    deal(a, SOL_DEBUG_SEED);
    if (strcmp(want, "deal") == 0) {
        return;
    }
    {
        /* Play the obvious moves a while so the table looks lived in. */
        int round;

        for (round = 0; round < 60; round++) {
            int p;
            int moved = 0;

            for (p = SOL_WASTE; p < SOL_PILES && !moved; p++) {
                int n = a->game.pile[p].n;
                int to;

                if (n == 0 || sol_is_foundation(p)) {
                    continue;
                }
                if (sol_foundation_target(&a->game, p, n - 1) >= 0) {
                    sol_move(&a->game, p, n - 1, sol_foundation_target(&a->game, p, n - 1), NULL);
                    moved = 1;
                    break;
                }
                for (to = SOL_T0; to < SOL_PILES && !moved; to++) {
                    int from_index = sol_is_column(p) ? a->game.pile[p].down : n - 1;

                    if (to != p && a->game.pile[to].n > 0 && sol_move(&a->game, p, from_index, to, NULL) == SOL_OK) {
                        moved = 1;
                    }
                }
            }
            if (!moved) {
                sol_draw(&a->game);
            }
        }
    }
    sol_view_reset(&a->ui, &a->game);
    if (strcmp(want, "selected") == 0) {
        int c;

        for (c = SOL_COLUMNS - 1; c >= 0; c--) {
            const struct sol_stack *s = &a->game.pile[SOL_T0 + c];

            if (s->n > s->down + 1) {
                a->ui.sel_pile = (int8_t)(SOL_T0 + c);
                a->ui.sel_index = (int8_t)s->down;
                break;
            }
        }
    } else if (strcmp(want, "keyboard") == 0) {
        sol_view_key(&a->ui, &a->game, '4');
        sol_view_key(&a->ui, &a->game, SOL_KEY_UP);
    } else if (strcmp(want, "confirm") == 0) {
        a->ui.confirming = 1;
    } else if (strcmp(want, "won") == 0) {
        int s;
        int r;

        memset(&a->game.pile, 0, sizeof(a->game.pile));
        for (s = 0; s < SOL_SUITS; s++) {
            for (r = 1; r <= SOL_KING; r++) {
                a->game.pile[SOL_F0 + s].card[a->game.pile[SOL_F0 + s].n++] = sol_card(r, (enum sol_suit)s);
            }
        }
        a->game.won = 1;
        a->game.moves = 212;
    }
}

static void *solitaire_create(lv_obj_t *body)
{
    struct sol_app *a = lv_malloc_zeroed(sizeof(*a));
    const char *debug = getenv("PGSOLITAIRE_SCREEN");

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
    layout(a);
    refresh(a);
    return a;
}

static void solitaire_tick(void *priv)
{
    struct sol_app *a = priv;

    if (a && a->dirty) {
        save_now(a);
        refresh(a);
    }
}

static void solitaire_destroy(void *priv)
{
    struct sol_app *a = priv;

    /* The shell deletes the objects under the body, which also takes the
     * root out of the focus group. */
    if (a && a->dirty) {
        save_now(a);
    }
    lv_free(priv);
}

/* ---- for tests (sol_app.h) -------------------------------------------------------- */

const struct sol_game *sol_app_game(void *priv)
{
    return priv ? &((struct sol_app *)priv)->game : NULL;
}

const struct sol_ui *sol_app_ui(void *priv)
{
    return priv ? &((struct sol_app *)priv)->ui : NULL;
}

lv_obj_t *sol_app_table(void *priv)
{
    return priv ? ((struct sol_app *)priv)->table : NULL;
}

lv_obj_t *sol_app_caption(void *priv)
{
    return priv ? ((struct sol_app *)priv)->caption : NULL;
}

lv_obj_t *sol_app_button(void *priv, int index)
{
    struct sol_app *a = priv;

    return a ? (index == 0 ? a->primary : a->secondary) : NULL;
}

/* The first-party launcher mask (docs/design/doors-app-icons); .icon stays
 * the text fallback for a shell without the mask. */
LV_IMAGE_DECLARE(pos_app_icon_solitaire);

const struct pocketos_app app_solitaire = {
    .id = "solitaire",
    .name = "Solitaire",
    .icon = LV_SYMBOL_COPY,
    .icon_mask = &pos_app_icon_solitaire,
    .create = solitaire_create,
    .tick = solitaire_tick,
    .destroy = solitaire_destroy,
    /* Fullscreen, like Fleet, Radar and Timber (DS §36): no status
     * cluster over the game; the shell's header keeps the way back. */
    .chrome = POCKETOS_CHROME_NONE,
};
