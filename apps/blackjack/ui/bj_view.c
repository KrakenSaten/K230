/*
 * PG Blackjack view model. See bj_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bj_view.h"

#include <stdio.h>
#include <string.h>

#define MINUS "\xE2\x88\x92" /* U+2212, which the product fonts carry */
#define DOT "\xC2\xB7"

_Static_assert(BJ_BET_STEP == 10u, "the bet buttons say 10");

static int max_i(int a, int b)
{
    return a > b ? a : b;
}

static int min_i(int a, int b)
{
    return a < b ? a : b;
}

static struct bj_rect rect(int x, int y, int w, int h)
{
    struct bj_rect r = { x, y, w, h };

    return r;
}

/* ---- blocks --------------------------------------------------------------------- */

void bj_view_screen(int w, int h, struct bj_screen *out)
{
    if (!out) {
        return;
    }
    w = max_i(w, 1);
    h = max_i(h, 1);
    if (w > h) {
        int table_w = max_i(w - BJ_SIDE_W - BJ_GAP, 1);
        int side_x = table_w + BJ_GAP;

        out->side_by_side = 1;
        out->table = rect(0, 0, table_w, h);
        /* BANK above BET: a side column is too narrow for two large numbers
         * next to each other. */
        out->hud = rect(side_x, 0, max_i(w - side_x, 0), min_i(2 * BJ_HUD_H + BJ_HUD_GAP, h));
        /* The rest of the column under the HUD: the column is too narrow for
         * three buttons in a row, so they take two rows there (the accent one
         * alone on the first), and a caption that wraps grows upwards into
         * the column rather than over the HUD. */
        out->buttons_rows = 2;
        out->controls = rect(side_x, min_i(out->hud.h + BJ_GAP, h), max_i(w - side_x, 0),
                             max_i(h - out->hud.h - BJ_GAP, 0));
    } else {
        out->side_by_side = 0;
        out->buttons_rows = 1;
        out->hud = rect(0, 0, w, min_i(BJ_HUD_H, h));
        out->table = rect(0, min_i(BJ_HUD_H + BJ_GAP, h - 1), w,
                          max_i(h - BJ_HUD_H - BJ_CONTROLS_H - 2 * BJ_GAP, 1));
        out->controls = rect(0, max_i(h - BJ_CONTROLS_H, 0), w, min_i(BJ_CONTROLS_H, h));
    }
}

/* ---- the HUD ---------------------------------------------------------------------- */

const char *bj_view_hud_label(int panel)
{
    return panel == 0 ? "BANK" : panel == 1 ? "BET" : "";
}

void bj_view_hud_value(const struct bj_game *g, int panel, char *out, size_t len)
{
    if (!out || !len) {
        return;
    }
    if (!g || (panel != 0 && panel != 1)) {
        out[0] = '\0';
        return;
    }
    if (panel == 0) {
        snprintf(out, len, "%u", (unsigned)g->bankroll);
    } else {
        snprintf(out, len, "%u", (unsigned)(g->phase == BJ_PLAYER ? g->stake : g->bet));
    }
}

/* ---- the table ----------------------------------------------------------------------- */

void bj_view_table(int w, int h, struct bj_table *out)
{
    int by_width;
    int by_height;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->w = max_i(w, 1);
    out->h = max_i(h, 1);
    by_width = (out->w - 2 * BJ_TABLE_PAD) * 26 / 100;
    by_height = ((out->h - 2 * BJ_TABLE_PAD - 2 * BJ_LABEL_H - BJ_TABLE_PAD) / 2) * 5 / 7;
    out->card_w = max_i(min_i(by_width, by_height), 10);
    out->card_h = out->card_w * 7 / 5;
    out->label_y[BJ_ROW_DEALER] = BJ_TABLE_PAD;
    out->cards_y[BJ_ROW_DEALER] = BJ_TABLE_PAD + BJ_LABEL_H;
    out->cards_y[BJ_ROW_PLAYER] = out->h - BJ_TABLE_PAD - out->card_h;
    out->label_y[BJ_ROW_PLAYER] = out->cards_y[BJ_ROW_PLAYER] - BJ_LABEL_H;
}

struct bj_rect bj_view_card(const struct bj_table *t, enum bj_row row, int n, int index)
{
    int room;
    int step;
    int span;
    int x0;

    if (!t || n <= 0 || index < 0 || index >= n || (row != BJ_ROW_DEALER && row != BJ_ROW_PLAYER)) {
        return rect(0, 0, 0, 0);
    }
    room = t->w - 2 * BJ_TABLE_PAD - t->card_w;
    step = t->card_w * 56 / 100;
    if (n > 1 && step * (n - 1) > room) {
        step = max_i(room / (n - 1), 1);
    }
    span = t->card_w + step * (n - 1);
    x0 = max_i((t->w - span) / 2, BJ_TABLE_PAD);
    return rect(x0 + step * index, t->cards_y[row], t->card_w, t->card_h);
}

int bj_view_face_down(const struct bj_game *g, int index)
{
    return g && g->phase == BJ_PLAYER && index == 1;
}

struct bj_rect bj_view_result_area(const struct bj_table *t)
{
    int top;

    if (!t) {
        return rect(0, 0, 0, 0);
    }
    top = t->cards_y[BJ_ROW_DEALER] + t->card_h;
    return rect(BJ_TABLE_PAD, top, max_i(t->w - 2 * BJ_TABLE_PAD, 0), max_i(t->label_y[BJ_ROW_PLAYER] - top, 0));
}

/* ---- phases, buttons, keys -------------------------------------------------------------- */

enum bj_panel bj_view_panel(const struct bj_game *g)
{
    if (!g) {
        return BJ_PANEL_BET;
    }
    if (g->phase == BJ_PLAYER) {
        return BJ_PANEL_PLAY;
    }
    return bj_can_deal(g) ? BJ_PANEL_BET : BJ_PANEL_BROKE;
}

enum bj_cmd bj_view_command_for_key(enum bj_panel panel, uint32_t key)
{
    switch (panel) {
    case BJ_PANEL_BET:
        switch (key) {
        case BJ_KEY_ENTER:
        case ' ':
        case 'n':
        case 'N':
            return BJ_CMD_DEAL;
        case BJ_KEY_LEFT:
        case BJ_KEY_DOWN:
        case '-':
            return BJ_CMD_BET_DOWN;
        case BJ_KEY_RIGHT:
        case BJ_KEY_UP:
        case '+':
        case '=':
            return BJ_CMD_BET_UP;
        default:
            return BJ_CMD_NONE;
        }
    case BJ_PANEL_PLAY:
        switch (key) {
        case 'h':
        case 'H':
        case BJ_KEY_ENTER:
            return BJ_CMD_HIT;
        case 's':
        case 'S':
        case ' ':
            return BJ_CMD_STAND;
        case 'd':
        case 'D':
            return BJ_CMD_DOUBLE;
        default:
            return BJ_CMD_NONE;
        }
    case BJ_PANEL_BROKE:
        return (key == BJ_KEY_ENTER || key == ' ' || key == 'n' || key == 'N') ? BJ_CMD_NEW_BANKROLL : BJ_CMD_NONE;
    default:
        return BJ_CMD_NONE;
    }
}

void bj_view_buttons(const struct bj_game *g, struct bj_button out[3])
{
    int i;

    for (i = 0; i < 3; i++) {
        out[i].text = NULL;
        out[i].cmd = BJ_CMD_NONE;
        out[i].enabled = 0;
    }
    if (!g) {
        return;
    }
    switch (bj_view_panel(g)) {
    case BJ_PANEL_PLAY:
        out[0] = (struct bj_button){ "HIT", BJ_CMD_HIT, 1 };
        out[1] = (struct bj_button){ "STAND", BJ_CMD_STAND, 1 };
        out[2] = (struct bj_button){ "DOUBLE", BJ_CMD_DOUBLE, bj_can_double(g) };
        break;
    case BJ_PANEL_BROKE:
        out[0] = (struct bj_button){ "NEW BANKROLL", BJ_CMD_NEW_BANKROLL, 1 };
        break;
    case BJ_PANEL_BET:
    default:
        out[0] = (struct bj_button){ g->rounds ? "NEW ROUND" : "DEAL", BJ_CMD_DEAL, 1 };
        out[1] = (struct bj_button){ "BET " MINUS "10", BJ_CMD_BET_DOWN, g->bet > BJ_BET_MIN };
        out[2] = (struct bj_button){ "BET +10", BJ_CMD_BET_UP, g->bet + BJ_BET_STEP <= bj_bet_ceiling(g) };
        break;
    }
}

enum bj_result bj_view_run(struct bj_game *g, enum bj_cmd cmd)
{
    if (!g) {
        return BJ_ERR_PHASE;
    }
    switch (cmd) {
    case BJ_CMD_DEAL:
        return bj_deal(g);
    case BJ_CMD_BET_DOWN:
        return bj_bet_down(g);
    case BJ_CMD_BET_UP:
        return bj_bet_up(g);
    case BJ_CMD_HIT:
        return bj_hit(g);
    case BJ_CMD_STAND:
        return bj_stand(g);
    case BJ_CMD_DOUBLE:
        return bj_double(g);
    case BJ_CMD_NEW_BANKROLL:
        return bj_view_panel(g) == BJ_PANEL_BROKE ? bj_new_bankroll(g) : BJ_ERR_PHASE;
    case BJ_CMD_NONE:
    default:
        return BJ_ERR_PHASE;
    }
}

/* ---- words ----------------------------------------------------------------------------- */

void bj_view_dealer_label(const struct bj_game *g, char *out, size_t len)
{
    if (!out || !len) {
        return;
    }
    if (!g || g->dealer.n == 0) {
        snprintf(out, len, "DEALER");
    } else if (g->phase == BJ_PLAYER) {
        snprintf(out, len, "DEALER SHOWS %s", bj_rank_text(bj_card_rank(g->dealer.card[0])));
    } else if (bj_is_blackjack(&g->dealer)) {
        snprintf(out, len, "DEALER BLACKJACK");
    } else if (bj_is_bust(&g->dealer)) {
        snprintf(out, len, "DEALER %d BUST", bj_hand_total(&g->dealer, NULL));
    } else {
        snprintf(out, len, "DEALER %d", bj_hand_total(&g->dealer, NULL));
    }
}

void bj_view_player_label(const struct bj_game *g, char *out, size_t len)
{
    int soft;
    int total;

    if (!out || !len) {
        return;
    }
    if (!g || g->player.n == 0) {
        snprintf(out, len, "YOU");
        return;
    }
    total = bj_hand_total(&g->player, &soft);
    if (bj_is_blackjack(&g->player)) {
        snprintf(out, len, "YOU BLACKJACK");
    } else if (total > 21) {
        snprintf(out, len, "YOU %d BUST", total);
    } else if (soft && total < 21 && g->phase == BJ_PLAYER) {
        snprintf(out, len, "YOU SOFT %d", total);
    } else {
        snprintf(out, len, "YOU %d", total);
    }
}

int bj_view_player_result(const struct bj_game *g)
{
    if (!g || g->phase != BJ_SETTLED) {
        return 0;
    }
    switch (g->outcome) {
    case BJ_PLAYER_BLACKJACK:
    case BJ_PLAYER_WINS:
    case BJ_DEALER_BUSTS:
        return 1;
    case BJ_DEALER_WINS:
    case BJ_PLAYER_BUSTS:
    case BJ_DEALER_BLACKJACK:
        return -1;
    default:
        return 0;
    }
}

static void delta_text(int32_t delta, char *out, size_t len)
{
    if (delta > 0) {
        snprintf(out, len, "+%d", (int)delta);
    } else if (delta < 0) {
        snprintf(out, len, MINUS "%d", (int)-delta);
    } else {
        snprintf(out, len, "0");
    }
}

void bj_view_result(const struct bj_game *g, char *out, size_t len)
{
    if (!out || !len) {
        return;
    }
    if (!g || g->phase != BJ_SETTLED || g->outcome == BJ_NO_OUTCOME) {
        out[0] = '\0';
    } else if (g->outcome == BJ_PUSH) {
        snprintf(out, len, "PUSH");
    } else {
        delta_text(g->last_delta, out, len);
    }
}

const char *bj_view_store_note(int save_failed, int unreadable)
{
    if (save_failed) {
        return "NOT SAVED " DOT " PLAY CONTINUES";
    }
    if (unreadable) {
        return "SAVED GAME UNREADABLE " DOT " NEW BANKROLL";
    }
    return NULL;
}

void bj_view_caption(const struct bj_game *g, enum bj_cmd last_cmd, enum bj_result last, char *out, size_t len)
{
    char d[16];

    if (!out || !len) {
        return;
    }
    if (!g) {
        out[0] = '\0';
        return;
    }
    if (last == BJ_ERR_FUNDS) {
        snprintf(out, len, "NOT ENOUGH CHIPS TO DOUBLE");
        return;
    }
    if (last == BJ_ERR_NOT_FIRST) {
        snprintf(out, len, "DOUBLE ONLY ON YOUR FIRST TWO CARDS");
        return;
    }
    if (last == BJ_ERR_LIMIT) {
        if (last_cmd == BJ_CMD_BET_DOWN) {
            snprintf(out, len, "THE MINIMUM BET IS %u", (unsigned)BJ_BET_MIN);
        } else if (bj_bet_ceiling(g) < BJ_BET_MAX) {
            snprintf(out, len, "A BET CANNOT BE MORE THAN YOUR CHIPS");
        } else {
            snprintf(out, len, "THE MAXIMUM BET IS %u", (unsigned)BJ_BET_MAX);
        }
        return;
    }
    switch (bj_view_panel(g)) {
    case BJ_PANEL_BROKE:
        snprintf(out, len, "OUT OF CHIPS");
        return;
    case BJ_PANEL_PLAY: {
        int soft;
        int total = bj_hand_total(&g->player, &soft);

        snprintf(out, len, "%sYOU HAVE %s%d " DOT " HIT OR STAND", g->reshuffled && g->player.n == 2 ? "NEW SHOE " DOT " " : "",
                 soft ? "SOFT " : "", total);
        return;
    }
    case BJ_PANEL_BET:
    default:
        break;
    }
    if (g->phase != BJ_SETTLED) {
        snprintf(out, len, "SET YOUR BET, THEN DEAL");
        return;
    }
    delta_text(g->last_delta, d, sizeof(d));
    switch (g->outcome) {
    case BJ_PLAYER_BLACKJACK:
        snprintf(out, len, "BLACKJACK " DOT " %s", d);
        break;
    case BJ_PLAYER_WINS:
        snprintf(out, len, "YOU WIN " DOT " %s", d);
        break;
    case BJ_DEALER_BUSTS:
        snprintf(out, len, "DEALER BUSTS " DOT " %s", d);
        break;
    case BJ_PUSH:
        snprintf(out, len, "PUSH " DOT " BET RETURNED");
        break;
    case BJ_DEALER_WINS:
        snprintf(out, len, "DEALER WINS " DOT " %s", d);
        break;
    case BJ_PLAYER_BUSTS:
        snprintf(out, len, "BUST " DOT " %s", d);
        break;
    case BJ_DEALER_BLACKJACK:
        snprintf(out, len, "DEALER BLACKJACK " DOT " %s", d);
        break;
    default:
        snprintf(out, len, "SET YOUR BET, THEN DEAL");
        break;
    }
}
