/*
 * PG Blackjack: the app. A HUD with BANK and BET in large numbers, the felt
 * table with the dealer's hand above the player's and a settled round's
 * result between them, and a caption over three buttons that change with the
 * phase: NEW ROUND, BET -10 and BET +10 between rounds; HIT, STAND and DOUBLE
 * during one; NEW BANKROLL when the chips are gone.
 *
 * This file is the screen and nothing else: the rules are engine/bj_rules.c,
 * every key, button, label, caption and position is ui/bj_view.c, the table
 * is drawn by ui/bj_table_widget.c, and the file is bj_store.c.
 *
 * LAYOUT. One root fills the body and places HUD, table and controls from
 * bj_view_screen() on every size change; the table sizes its cards from its
 * own size.
 *
 * INPUT. Keyboard first: the root is the one object in the shell's focus group
 * and keys go through bj_view_command_for_key() (DS 17.4). The buttons run the
 * same commands and never take the focus. The table takes no taps.
 *
 * PERSISTENCE. Leaving and reopening resumes the session exactly, a hand in
 * play included (bj_store.h). A command that changed the game marks it; the
 * shell's 1 Hz tick saves it, a settled round is saved at once, and closing
 * the app saves. A new session nobody has touched writes nothing. A saved
 * game that cannot be used is left in place and a new bankroll starts, saying
 * so; the review states never read or write the file.
 *
 * No motion: the dealer's cards appear at once, so reduced motion has nothing
 * to switch off.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "bj_app.h"
#include "bj_store.h"
#include "bj_table_widget.h"

#include "app.h"
#include "pocketui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(BJ_KEY_UP == LV_KEY_UP, "bj_view.h Up is not LVGL's");
_Static_assert(BJ_KEY_DOWN == LV_KEY_DOWN, "bj_view.h Down is not LVGL's");
_Static_assert(BJ_KEY_LEFT == LV_KEY_LEFT, "bj_view.h Left is not LVGL's");
_Static_assert(BJ_KEY_RIGHT == LV_KEY_RIGHT, "bj_view.h Right is not LVGL's");
_Static_assert(BJ_KEY_ENTER == LV_KEY_ENTER, "bj_view.h Enter is not LVGL's");
_Static_assert(BJ_KEY_ESC == LV_KEY_ESC, "bj_view.h Esc is not LVGL's");
_Static_assert(BJ_KEY_BACKSPACE == LV_KEY_BACKSPACE, "bj_view.h Backspace is not LVGL's");

#define BUTTON_H 64
#define BUTTON_GAP 8
_Static_assert(BUTTON_H >= POCKETUI_TOUCH_MIN, "a button must meet the DS 64 px touch minimum");

/* Development aid: $PGBLACKJACK_SCREEN opens a fixed session in a given state
 * for review renders. Nothing happens unless it is set. */
#define BJ_DEBUG_SEED 20260913u

struct bj_app {
    struct bj_game game;
    enum bj_cmd last_cmd;
    enum bj_result last;
    int dirty;        /* the game changed since it was last saved */
    int save_failed;  /* the last save did not reach the file */
    int unreadable;   /* the save found at open was refused */
    int session_only; /* a review state: never read or write the file */
    lv_obj_t *root;
    lv_obj_t *hud;
    lv_obj_t *panel[2];
    lv_obj_t *label[2];
    lv_obj_t *value[2];
    lv_obj_t *table;
    lv_obj_t *controls;
    lv_obj_t *caption;
    lv_obj_t *buttons;
    lv_obj_t *button[3];
};

/* ---- persistence --------------------------------------------------------------------- */

static void save_now(struct bj_app *a)
{
    if (a->session_only) {
        a->dirty = 0;
        return;
    }
    a->save_failed = bj_store_save(&a->game) != 0;
    a->dirty = a->save_failed;
}

/* ---- painting ------------------------------------------------------------------------ */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void style_button(lv_obj_t *btn, int primary, int enabled)
{
    static const enum pos_style_role roles[] = { POS_STYLE_BUTTON_PRIMARY, POS_STYLE_BUTTON_SECONDARY,
                                                 POS_STYLE_BUTTON_DISABLED };
    size_t i;

    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        lv_obj_remove_style(btn, pos_style(roles[i]), 0);
    }
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (!enabled) {
        pos_style_add(btn, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        return;
    }
    lv_obj_remove_state(btn, LV_STATE_DISABLED);
    if (primary) {
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(btn, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

static void refresh(struct bj_app *a)
{
    struct bj_button b[3];
    const char *note = bj_view_store_note(a->save_failed, a->unreadable);
    char text[96];
    int visible = 0;
    int i;

    for (i = 0; i < 2; i++) {
        bj_view_hud_value(&a->game, i, text, sizeof(text));
        set_text(a->value[i], text);
    }
    bj_view_caption(&a->game, a->last_cmd, a->last, text, sizeof(text));
    /* A refusal is explained first; otherwise trouble saving is said. */
    set_text(a->caption, note && a->last == BJ_OK ? note : text);

    bj_view_buttons(&a->game, b);
    for (i = 0; i < 3; i++) {
        if (!b[i].text) {
            lv_obj_add_flag(a->button[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        visible++;
        lv_obj_remove_flag(a->button[i], LV_OBJ_FLAG_HIDDEN);
        set_text(lv_obj_get_child(a->button[i], 0), b[i].text);
        lv_obj_set_user_data(a->button[i], (void *)(intptr_t)b[i].cmd);
        style_button(a->button[i], i == 0, b[i].enabled);
        /* Between rounds the bet steppers are narrow and NEW ROUND is wide;
         * during a hand the three actions share the row equally. */
        lv_obj_set_flex_grow(a->button[i], bj_view_panel(&a->game) == BJ_PANEL_BET && i == 0 ? 2 : 1);
    }
    /* LVGL's flex row counts the gap beside a hidden button. */
    lv_obj_set_style_pad_column(a->buttons, visible > 1 ? BUTTON_GAP : 0, 0);
    lv_obj_invalidate(a->table);
}

/* ---- commands ------------------------------------------------------------------------ */

static void run(struct bj_app *a, enum bj_cmd cmd)
{
    uint32_t rounds_before = a->game.rounds;

    if (cmd == BJ_CMD_NONE) {
        return;
    }
    a->last_cmd = cmd;
    a->last = bj_view_run(&a->game, cmd);
    /* The rules change nothing when they refuse. */
    if (a->last == BJ_OK) {
        a->dirty = 1;
        a->unreadable = 0;
        if (a->game.rounds != rounds_before) {
            save_now(a);
        }
    }
    refresh(a);
}

static void on_key(lv_event_t *e)
{
    struct bj_app *a = lv_event_get_user_data(e);

    run(a, bj_view_command_for_key(bj_view_panel(&a->game), lv_event_get_key(e)));
}

static void on_button(lv_event_t *e)
{
    struct bj_app *a = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);

    if (!lv_obj_has_state(btn, LV_STATE_DISABLED)) {
        run(a, (enum bj_cmd)(intptr_t)lv_obj_get_user_data(btn));
    }
}

/* ---- building ------------------------------------------------------------------------ */

static void place(lv_obj_t *obj, struct bj_rect r)
{
    lv_obj_set_pos(obj, r.x, r.y);
    lv_obj_set_size(obj, r.w, r.h);
}

static void layout(struct bj_app *a)
{
    struct bj_screen s;
    int i;

    bj_view_screen(lv_obj_get_content_width(a->root), lv_obj_get_content_height(a->root), &s);
    place(a->hud, s.hud);
    place(a->table, s.table);
    place(a->controls, s.controls);
    lv_obj_set_flex_flow(a->hud, s.side_by_side ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    for (i = 0; i < 2; i++) {
        if (s.side_by_side) {
            lv_obj_set_size(a->panel[i], LV_PCT(100), BJ_HUD_H);
        } else {
            lv_obj_set_height(a->panel[i], LV_PCT(100));
        }
        lv_obj_set_flex_grow(a->panel[i], s.side_by_side ? 0 : 1);
    }
}

static void on_resize(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* A HUD panel: the word small above, the number large (DS hero-40). */
static void hud_panel(struct bj_app *a, int i)
{
    lv_obj_t *panel = lv_obj_create(a->hud);

    lv_obj_remove_style_all(panel);
    pos_style_add(panel, POS_STYLE_PANEL, 0);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    a->panel[i] = panel;
    a->label[i] = pocketui_label(panel, bj_view_hud_label(i), POS_STYLE_CAPTION);
    a->value[i] = pocketui_label(panel, "0", POS_STYLE_HERO_40);
}

static lv_obj_t *button(struct bj_app *a, lv_obj_t *parent)
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
    style_button(btn, 0, 1);
    return btn;
}

static void build(struct bj_app *a, lv_obj_t *body)
{
    int i;

    a->root = lv_obj_create(body);
    lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root, LV_PCT(100));
    lv_obj_set_flex_grow(a->root, 1);
    lv_obj_remove_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);

    a->hud = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->hud);
    lv_obj_set_flex_flow(a->hud, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->hud, BJ_HUD_GAP, 0);
    lv_obj_set_style_pad_row(a->hud, BJ_HUD_GAP, 0);
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < 2; i++) {
        hud_panel(a, i);
    }

    a->table = bj_table_create(a->root);
    bj_table_bind(a->table, &a->game);

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
    for (i = 0; i < 3; i++) {
        a->button[i] = button(a, a->buttons);
    }

    lv_obj_add_event_cb(a->root, on_resize, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_add_event_cb(a->root, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->root);
    pos_input_focus(a->root);
}

/* A saved session if there is a usable one, else a new one. A refused file
 * stays where it is until the first command gives something to save. */
static void start(struct bj_app *a)
{
    int rc = bj_store_load(&a->game);

    if (rc == 0) {
        return;
    }
    a->unreadable = rc < 0;
    /* The shoe is shuffled from a seed chosen here, at the app boundary, as
     * PocketRadar does: the wall clock separates sessions and the LVGL tick
     * separates two in one. */
    bj_new_session(&a->game, (uint32_t)time(NULL) ^ (uint32_t)lv_tick_get());
}

/* Review states from a fixed session, never saved. */
static void debug_state(struct bj_app *a, const char *want)
{
    uint32_t tries;

    a->session_only = 1;
    bj_new_session(&a->game, BJ_DEBUG_SEED);
    if (strcmp(want, "bet") == 0) {
        return;
    }
    if (strcmp(want, "broke") == 0) {
        a->game.bankroll = 0;
        a->game.phase = BJ_SETTLED;
        return;
    }
    /* Deal until the wanted kind of hand turns up; the seed is fixed, so
     * this is the same hand every time. */
    for (tries = 0; tries < 400; tries++) {
        bj_new_session(&a->game, BJ_DEBUG_SEED + tries);
        a->game.bet = 50;
        bj_deal(&a->game);
        if (strcmp(want, "play") == 0 && a->game.phase == BJ_PLAYER && bj_hand_total(&a->game.player, NULL) <= 12) {
            bj_hit(&a->game);
            if (a->game.phase == BJ_PLAYER) {
                return;
            }
        } else if (strcmp(want, "win") == 0 && a->game.phase == BJ_PLAYER) {
            bj_stand(&a->game);
            if (a->game.outcome == BJ_DEALER_BUSTS && a->game.dealer.n >= 3) {
                return;
            }
        } else if (strcmp(want, "blackjack") == 0 && a->game.outcome == BJ_PLAYER_BLACKJACK) {
            return;
        } else if (strcmp(want, "bust") == 0 && a->game.phase == BJ_PLAYER) {
            while (a->game.phase == BJ_PLAYER) {
                bj_hit(&a->game);
            }
            if (a->game.outcome == BJ_PLAYER_BUSTS && a->game.player.n >= 4) {
                return;
            }
        }
    }
}

static void *blackjack_create(lv_obj_t *body)
{
    struct bj_app *a = lv_malloc_zeroed(sizeof(*a));
    const char *debug = getenv("PGBLACKJACK_SCREEN");

    if (!a) {
        return NULL;
    }
    build(a, body);
    if (debug && *debug) {
        debug_state(a, debug);
    } else {
        start(a);
    }
    a->last = BJ_OK;
    lv_obj_update_layout(a->root);
    layout(a);
    refresh(a);
    return a;
}

static void blackjack_tick(void *priv)
{
    struct bj_app *a = priv;

    if (a && a->dirty) {
        save_now(a);
        refresh(a);
    }
}

static void blackjack_destroy(void *priv)
{
    struct bj_app *a = priv;

    /* The shell deletes the objects; the game is saved if it changed. */
    if (a && a->dirty) {
        save_now(a);
    }
    lv_free(priv);
}

/* ---- for tests (bj_app.h) ------------------------------------------------------------ */

struct bj_game *bj_app_game(void *priv)
{
    return priv ? &((struct bj_app *)priv)->game : NULL;
}

lv_obj_t *bj_app_table(void *priv)
{
    return priv ? ((struct bj_app *)priv)->table : NULL;
}

lv_obj_t *bj_app_caption(void *priv)
{
    return priv ? ((struct bj_app *)priv)->caption : NULL;
}

lv_obj_t *bj_app_value(void *priv, int index)
{
    return priv && index >= 0 && index < 2 ? ((struct bj_app *)priv)->value[index] : NULL;
}

lv_obj_t *bj_app_hud_label(void *priv, int index)
{
    return priv && index >= 0 && index < 2 ? ((struct bj_app *)priv)->label[index] : NULL;
}

lv_obj_t *bj_app_button(void *priv, int index)
{
    return priv && index >= 0 && index < 3 ? ((struct bj_app *)priv)->button[index] : NULL;
}

void bj_app_refresh(void *priv)
{
    if (priv) {
        refresh(priv);
    }
}

const struct pocketos_app app_blackjack = {
    .id = "blackjack",
    .name = "Blackjack",
    /* A placeholder until the DS section 11 icon set exists: LVGL's symbol
     * font has no card, and a framed rectangle is the nearest shape to one. */
    .icon = LV_SYMBOL_IMAGE,
    .create = blackjack_create,
    .tick = blackjack_tick,
    .destroy = blackjack_destroy,
};
