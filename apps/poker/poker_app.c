/* Offline Texas Hold'em, hosted by the standard Doors app lifecycle.
 * Only the private screen is allocated. AI performs one bounded action on
 * the shell's tick; closing pauses play and leaves no timer or process.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker_app.h"
#include "app.h"
#include "pocketui.h"
#include "poker_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct poker_app {
    struct poker_game game;
    lv_obj_t *root, *table, *caption, *amount, *button[POKER_UI_COUNT];
    uint32_t raise_to;
    bool confirm_restart, review;
};
/* Session-only resume, no retained objects, pointers, workers or timers. */
static struct poker_game session;
static bool session_exists;
static void refresh(struct poker_app *a);

static void place(lv_obj_t *obj, struct poker_rect r)
{
    lv_obj_set_pos(obj, r.x, r.y);
    lv_obj_set_size(obj, r.w, r.h);
}
static void style_button(lv_obj_t *obj, bool enabled, bool primary)
{
    lv_obj_remove_style(obj, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(obj, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(obj, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
    if (enabled)
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    pos_style_add(obj,
                  !enabled  ? POS_STYLE_BUTTON_DISABLED
                  : primary ? POS_STYLE_BUTTON_PRIMARY
                            : POS_STYLE_BUTTON_SECONDARY,
                  0);
}
static void layout(struct poker_app *a)
{
    struct poker_layout l;
    poker_view_layout(lv_obj_get_content_width(a->root), lv_obj_get_content_height(a->root), &l);
    place(a->table, l.table);
    place(a->caption, l.caption);
    int third = (l.actions.w - 16) / 3;
    for (int i = 0; i < 3; ++i) {
        struct poker_rect r = {l.actions.x + i * (third + 8), l.actions.y, third, 64};
        place(a->button[i], r);
        r.y = l.secondary.y;
        place(a->button[i + 3], r);
    }
    place(a->button[POKER_UI_MINUS], (struct poker_rect){l.stepper.x, l.stepper.y, 64, 64});
    place(a->button[POKER_UI_PLUS], (struct poker_rect){l.stepper.x + l.stepper.w - 64, l.stepper.y, 64, 64});
    place(a->amount, (struct poker_rect){l.stepper.x + 72, l.stepper.y + 18, l.stepper.w - 144, 40});
}
static void resize(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}
static bool our_turn(const struct poker_app *a)
{
    return poker_playing(&a->game) && a->game.actor == 0;
}
static void refresh(struct poker_app *a)
{
    struct poker_options o = poker_options(&a->game);
    bool our = our_turn(a), playing = poker_playing(&a->game);
    char caption[200], value[72];
    if (a->raise_to < o.min_raise_to)
        a->raise_to = o.min_raise_to;
    if (a->raise_to > o.max_raise_to)
        a->raise_to = o.max_raise_to;
    snprintf(value, sizeof(value), "To %u%s", a->raise_to,
             our && a->raise_to == o.max_raise_to ? " (all in)" : "");
    lv_label_set_text(a->amount, value);
    if (a->confirm_restart)
        snprintf(caption, sizeof(caption),
                 "Restart with 1,000 chips each?\nPress RESTART again; Back cancels.");
    else if (a->game.phase == POKER_GAME_OVER)
        snprintf(caption, sizeof(caption),
                 a->game.player[0].stack ? "You won the table!\nRESTART to play again."
                                         : "Out of chips. Game over.\nRESTART for a new table.");
    else if (!playing) {
        size_t at = 0;
        unsigned winners = 0;
        for (int i = 0; i < 4; ++i)
            if (a->game.winners & (1u << i)) {
                ++winners;
                int n = snprintf(caption + at, sizeof(caption) - at, "%s%s +%u", at ? ", " : "",
                                 poker_seat_name(i), a->game.player[i].payout - a->game.player[i].refunded);
                if (n > 0 && (size_t)n < sizeof(caption) - at)
                    at += (size_t)n;
            }
        if (winners > 1) {
            snprintf(caption, sizeof(caption), "Multiple winners; payouts at seats.");
            at = strlen(caption);
        }
        snprintf(caption + at, sizeof(caption) - at, "\n%s; NEXT HAND to continue.",
                 a->game.showdown ? "Showdown" : "Everyone else folded");
    } else if (our && o.can_check)
        snprintf(caption, sizeof(caption), "YOUR TURN | Check available\nMinimum raise to %u; chips only.",
                 o.min_raise_to);
    else if (our)
        snprintf(caption, sizeof(caption), "YOUR TURN | Call %u%s\nRaise is the total bet; chips only.",
                 o.call_cost, o.call_cost < o.to_call ? " (all in)" : "");
    else
        snprintf(caption, sizeof(caption), "%s is thinking...\n%s | Current bet %u",
                 poker_seat_name(a->game.actor), poker_phase_name(a->game.phase), a->game.current_bet);
    lv_label_set_text(a->caption, caption);
    lv_label_set_text(lv_obj_get_child(a->button[POKER_UI_CALL], 0), o.can_check ? "CHECK" : "CALL");
    for (int i = 0; i < POKER_UI_COUNT; ++i) {
        bool enabled = false;
        switch (i) {
        case POKER_UI_FOLD:
        case POKER_UI_CALL:
            enabled = our;
            break;
        case POKER_UI_RAISE:
            enabled = our && o.can_raise;
            break;
        case POKER_UI_ALL_IN:
            enabled = our && o.can_all_in;
            break;
        case POKER_UI_NEXT:
            enabled = a->game.phase == POKER_FINISHED;
            break;
        case POKER_UI_RESTART:
            enabled = true;
            break;
        case POKER_UI_MINUS:
            enabled = our && o.can_raise && a->raise_to > o.min_raise_to;
            break;
        case POKER_UI_PLUS:
            enabled = our && o.can_raise && a->raise_to < o.max_raise_to;
            break;
        }
        style_button(a->button[i], enabled,
                     i == POKER_UI_CALL || i == POKER_UI_NEXT ||
                         (i == POKER_UI_RESTART && a->confirm_restart));
    }
    lv_obj_invalidate(a->table);
}
static void command(struct poker_app *a, enum poker_control control)
{
    struct poker_options o = poker_options(&a->game);
    if (control == POKER_UI_NONE)
        return;
    if (control == POKER_UI_RESTART) {
        if (a->confirm_restart) {
            poker_restart(&a->game, (uint32_t)time(NULL) ^ lv_tick_get());
            poker_new_hand(&a->game);
        }
        a->confirm_restart = !a->confirm_restart;
    } else {
        a->confirm_restart = false;
        if (control == POKER_UI_NEXT)
            poker_new_hand(&a->game);
        else if (our_turn(a))
            switch (control) {
            case POKER_UI_FOLD:
                poker_act(&a->game, POKER_FOLD, 0);
                break;
            case POKER_UI_CALL:
                poker_act(&a->game, o.can_check ? POKER_CHECK : POKER_CALL, 0);
                break;
            case POKER_UI_RAISE:
                poker_act(&a->game, POKER_RAISE, a->raise_to);
                break;
            case POKER_UI_ALL_IN:
                poker_act(&a->game, POKER_ALL_IN, 0);
                break;
            case POKER_UI_MINUS:
                a->raise_to = a->raise_to > o.min_raise_to + POKER_BIG_BLIND ? a->raise_to - POKER_BIG_BLIND
                                                                             : o.min_raise_to;
                break;
            case POKER_UI_PLUS:
                a->raise_to = o.max_raise_to - a->raise_to > POKER_BIG_BLIND ? a->raise_to + POKER_BIG_BLIND
                                                                             : o.max_raise_to;
                break;
            default:
                break;
            }
    }
    refresh(a);
}
static void click(lv_event_t *e)
{
    struct poker_app *a = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    if (!lv_obj_has_state(obj, LV_STATE_DISABLED))
        command(a, (enum poker_control)(intptr_t)lv_obj_get_user_data(obj));
}
static void key(lv_event_t *e)
{
    struct poker_app *a = lv_event_get_user_data(e);
    command(a, poker_view_key(lv_event_get_key(e), !poker_playing(&a->game)));
}
static void review(struct poker_app *a, const char *screen)
{
    a->review = true;
    poker_restart(&a->game, 20261008);
    poker_new_hand(&a->game);
    if (strcmp(screen, "split") == 0) {
        for (int i = 0; i < POKER_SEATS; ++i) {
            a->game.player[i].hole[0] = bj_card(2 + i, BJ_SPADES);
            a->game.player[i].hole[1] = bj_card(2 + i, BJ_HEARTS);
        }
        const int index[] = {9, 10, 11, 13, 15};
        const int rank[] = {BJ_ACE, 13, 12, 11, 10};
        for (int i = 0; i < 5; ++i)
            a->game.deck[index[i]] = bj_card(rank[i], BJ_DIAMONDS);
    }
    int steps = 0;
    while (poker_playing(&a->game) && steps++ < 100) {
        if (strcmp(screen, "preflop") == 0 && a->game.actor == 0)
            break;
        if (strcmp(screen, "flop") == 0 && a->game.phase == POKER_FLOP && a->game.actor == 0)
            break;
        struct poker_options o = poker_options(&a->game);
        poker_act(&a->game, o.can_check ? POKER_CHECK : POKER_CALL, 0);
    }
    if (strcmp(screen, "gameover") == 0) {
        a->game.player[0].stack = 0;
        a->game.phase = POKER_GAME_OVER;
    }
}
static void *create(lv_obj_t *body)
{
    struct poker_app *a = lv_malloc_zeroed(sizeof(*a));
    if (!a)
        return NULL;
    const char *screen = getenv("DOORS_POKER_SCREEN");
    if (screen && *screen)
        review(a, screen);
    else if (session_exists)
        a->game = session;
    else {
        poker_restart(&a->game, (uint32_t)time(NULL) ^ lv_tick_get());
        poker_new_hand(&a->game);
    }
    a->root = lv_obj_create(body);
    lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root, LV_PCT(100));
    lv_obj_set_flex_grow(a->root, 1);
    lv_obj_remove_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);
    a->table = poker_table_create(a->root, &a->game);
    a->caption = pocketui_label(a->root, "", POS_STYLE_TEXT_PRIMARY);
    lv_obj_set_style_text_align(a->caption, LV_TEXT_ALIGN_CENTER, 0);
    a->amount = pocketui_label(a->root, "", POS_STYLE_VALUE);
    lv_obj_set_style_text_align(a->amount, LV_TEXT_ALIGN_CENTER, 0);
    static const char *const labels[] = {"FOLD", "CALL", "RAISE", "ALL IN", "NEXT HAND", "RESTART", "-", "+"};
    for (int i = 0; i < POKER_UI_COUNT; ++i) {
        a->button[i] = lv_button_create(a->root);
        lv_obj_remove_style_all(a->button[i]);
        lv_obj_remove_flag(a->button[i], LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->button[i], LV_OBJ_FLAG_CLICKABLE);
        pos_style_add(a->button[i], POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
        lv_obj_t *label = pocketui_label(a->button[i], labels[i], POS_STYLE_BUTTON_LABEL);
        lv_obj_center(label);
        lv_obj_set_user_data(a->button[i], (void *)(intptr_t)i);
        lv_obj_add_event_cb(a->button[i], click, LV_EVENT_CLICKED, a);
    }
    lv_obj_add_event_cb(a->root, resize, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_add_event_cb(a->root, key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->root);
    pos_input_focus(a->root);
    lv_obj_update_layout(a->root);
    layout(a);
    refresh(a);
    return a;
}
static void tick(void *priv)
{
    struct poker_app *a = priv;
    if (!a || a->confirm_restart || a->review || !poker_playing(&a->game) || a->game.actor == 0)
        return;
    struct poker_decision decision = poker_ai(&a->game);
    poker_act(&a->game, decision.action, decision.raise_to);
    refresh(a);
}
static void destroy(void *priv)
{
    struct poker_app *a = priv;
    if (a && !a->review) {
        session = a->game;
        session_exists = true;
    }
    /* The shell owns/deletes the objects; no timer or worker survives. */
    lv_free(a);
}
static int back(void *priv)
{
    struct poker_app *a = priv;
    if (a && a->confirm_restart) {
        a->confirm_restart = false;
        refresh(a);
        return 1;
    }
    return 0;
}
struct poker_game *poker_app_game(void *priv)
{
    return priv ? &((struct poker_app *)priv)->game : NULL;
}
lv_obj_t *poker_app_root(void *priv)
{
    return priv ? ((struct poker_app *)priv)->root : NULL;
}
lv_obj_t *poker_app_button(void *priv, enum poker_control c)
{
    return priv && (unsigned)c < POKER_UI_COUNT ? ((struct poker_app *)priv)->button[c] : NULL;
}
lv_obj_t *poker_app_caption(void *priv)
{
    return priv ? ((struct poker_app *)priv)->caption : NULL;
}
void poker_app_refresh(void *priv)
{
    if (priv)
        refresh(priv);
}

LV_IMAGE_DECLARE(pos_app_icon_blackjack); /* shared card glyph, no new asset */
const struct pocketos_app app_poker = {
    .id = "poker",
    .name = "Poker",
    .icon = LV_SYMBOL_IMAGE,
    .create = create,
    .tick = tick,
    .destroy = destroy,
    .back = back,
    .back_slab_in_app = true,
    .icon_mask = &pos_app_icon_blackjack,
    .chrome = POCKETOS_CHROME_NONE,
    .orientation = POCKETOS_APP_ORIENTATION_PORTRAIT,
};
