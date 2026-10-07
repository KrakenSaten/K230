/*
 * PG Blackjack view model: the layout in tall and wide areas, hands laid out
 * across the table and squeezed when long, the hole card, the key map and the
 * buttons for every phase, and every label and caption the table can show.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bj_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

#define MINUS "\xE2\x88\x92"
#define DOT "\xC2\xB7"

/* Ranks dealt in order: player, dealer, player, dealer, then draws. */
static void stack(struct bj_game *g, ...)
{
    int ranks[40];
    int k = 0;
    int i;
    va_list ap;

    va_start(ap, g);
    for (;;) {
        int r = va_arg(ap, int);

        if (r == 0) {
            break;
        }
        ranks[k++] = r;
    }
    va_end(ap);
    g->shoe.n = (uint8_t)(40 + k);
    for (i = 0; i < 40; i++) {
        g->shoe.card[i] = bj_card(2 + i % 3, (enum bj_suit)(i % 4));
    }
    for (i = 0; i < k; i++) {
        g->shoe.card[g->shoe.n - 1 - i] = bj_card(ranks[i], (enum bj_suit)((i + 1) % 4));
    }
}

static int inside(struct bj_rect r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= w && r.y + r.h <= h;
}

static int overlap(struct bj_rect a, struct bj_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_layout(void)
{
    struct bj_screen s;
    struct bj_table t;
    struct bj_rect a;
    struct bj_rect b;
    int bad = 0;
    int n;
    int i;

    bj_view_screen(528, 1060, &s);
    check("a tall body's buttons share one row", s.buttons_rows == 1);
    check("a tall body stacks HUD, table and controls", !s.side_by_side && s.hud.y == 0 &&
                                                            s.table.y == BJ_HUD_H + BJ_GAP &&
                                                            s.controls.y + s.controls.h == 1060 &&
                                                            !overlap(s.table, s.controls));
    bj_view_table(s.table.w, s.table.h, &t);
    /* (528 - 2 * 16) * 26 / 100 = 128, and 7:5 makes it 179 tall. */
    check("portrait cards are about a quarter of the width: 128 x 179", t.card_w == 128 && t.card_h == 179);
    check("the dealer's row is at the top", t.cards_y[BJ_ROW_DEALER] == BJ_TABLE_PAD + BJ_LABEL_H);
    check("the player's at the foot", t.cards_y[BJ_ROW_PLAYER] + t.card_h == t.h - BJ_TABLE_PAD);
    check("the rows and their labels do not meet",
          t.cards_y[BJ_ROW_DEALER] + t.card_h < t.label_y[BJ_ROW_PLAYER]);
    check("cards meet the touch minimum, though nothing on them is tapped", t.card_w >= 64);

    a = bj_view_result_area(&t);
    check("the result goes in the open felt between the dealer's cards and the player's label",
          a.y == t.cards_y[BJ_ROW_DEALER] + t.card_h && a.y + a.h == t.label_y[BJ_ROW_PLAYER] && a.x == BJ_TABLE_PAD &&
              a.x + a.w == t.w - BJ_TABLE_PAD);
    check("which in portrait has room for a 40 px number several times over", a.h >= 3 * 56);

    bj_view_screen(1172, 470, &s);
    check("a wide body puts the chrome beside the table", s.side_by_side && s.hud.x > s.table.w);
    check("with BANK above BET, both full height, clear of the controls",
          s.hud.h == 2 * BJ_HUD_H + BJ_HUD_GAP && s.hud.y + s.hud.h < s.controls.y);
    check("the controls take the column under the HUD, to the foot",
          s.controls.x == s.hud.x && s.controls.w == s.hud.w && s.controls.y == s.hud.h + BJ_GAP &&
              s.controls.y + s.controls.h == 470);
    check("where the buttons take two rows; stacked they take one", s.buttons_rows == 2);
    bj_view_table(s.table.w, s.table.h, &t);
    check("a short table binds the cards by height", t.card_h <= (470 - 32 - 72 - 16) / 2);
    check("and the rows still do not meet", t.cards_y[BJ_ROW_DEALER] + t.card_h <= t.label_y[BJ_ROW_PLAYER]);
    check("the result area there is what is left, never negative", bj_view_result_area(&t).h >= 0 &&
                                                                         bj_view_result_area(NULL).w == 0);

    bj_view_table(528, 820, &t);
    a = bj_view_card(&t, BJ_ROW_PLAYER, 2, 0);
    b = bj_view_card(&t, BJ_ROW_PLAYER, 2, 1);
    check("a two-card hand is centred, to the pixel an odd margin allows",
          (528 - (b.x + b.w)) - a.x >= 0 && (528 - (b.x + b.w)) - a.x <= 1);
    check("each card steps a little over half a card", b.x - a.x == t.card_w * 56 / 100);
    check("both in the player's row", a.y == t.cards_y[BJ_ROW_PLAYER] && b.w == t.card_w);
    for (n = 1; n <= BJ_HAND_MAX; n++) {
        for (i = 0; i < n; i++) {
            struct bj_rect r = bj_view_card(&t, BJ_ROW_DEALER, n, i);

            bad += !inside(r, t.w, t.h) || r.x < BJ_TABLE_PAD || r.x + r.w > t.w - BJ_TABLE_PAD;
            if (i > 0) {
                bad += r.x <= bj_view_card(&t, BJ_ROW_DEALER, n, i - 1).x;
            }
        }
    }
    check("every hand from 1 to 16 cards fits across, in order", bad == 0);
    bj_view_table(890, 470, &t);
    bad = 0;
    for (i = 0; i < BJ_HAND_MAX; i++) {
        bad += !inside(bj_view_card(&t, BJ_ROW_PLAYER, BJ_HAND_MAX, i), t.w, t.h);
    }
    check("so does the longest hand on a wide table", bad == 0);
    check("an index outside the hand is an empty rect", bj_view_card(&t, BJ_ROW_PLAYER, 3, 3).w == 0 &&
                                                            bj_view_card(&t, BJ_ROW_PLAYER, 0, 0).w == 0);
}

static void test_panels_and_keys(void)
{
    struct bj_game g;
    struct bj_button btn[3];
    static const uint32_t junk[] = { 'q', 'x', '7', 0xE6, 9u, 11u, 127u, 0u };
    int p;
    size_t i;

    bj_new_session(&g, 1);
    check("a session opens on the bet", bj_view_panel(&g) == BJ_PANEL_BET);
    bj_view_buttons(&g, btn);
    check("DEAL first, then the bet steps, which say what they do",
          strcmp(btn[0].text, "DEAL") == 0 && btn[0].cmd == BJ_CMD_DEAL && strcmp(btn[1].text, "BET " MINUS "10") == 0 &&
              btn[1].cmd == BJ_CMD_BET_DOWN && strcmp(btn[2].text, "BET +10") == 0 && btn[2].cmd == BJ_CMD_BET_UP);
    check("minus is off at the minimum bet, plus is on", !btn[1].enabled && btn[2].enabled);
    check("BET: Enter, Space and n deal", bj_view_command_for_key(BJ_PANEL_BET, BJ_KEY_ENTER) == BJ_CMD_DEAL &&
                                              bj_view_command_for_key(BJ_PANEL_BET, ' ') == BJ_CMD_DEAL &&
                                              bj_view_command_for_key(BJ_PANEL_BET, 'N') == BJ_CMD_DEAL);
    check("BET: Right, Up, + and = raise the bet",
          bj_view_command_for_key(BJ_PANEL_BET, BJ_KEY_RIGHT) == BJ_CMD_BET_UP &&
              bj_view_command_for_key(BJ_PANEL_BET, BJ_KEY_UP) == BJ_CMD_BET_UP &&
              bj_view_command_for_key(BJ_PANEL_BET, '+') == BJ_CMD_BET_UP &&
              bj_view_command_for_key(BJ_PANEL_BET, '=') == BJ_CMD_BET_UP);
    check("BET: Left, Down and - lower it", bj_view_command_for_key(BJ_PANEL_BET, BJ_KEY_LEFT) == BJ_CMD_BET_DOWN &&
                                               bj_view_command_for_key(BJ_PANEL_BET, BJ_KEY_DOWN) == BJ_CMD_BET_DOWN &&
                                               bj_view_command_for_key(BJ_PANEL_BET, '-') == BJ_CMD_BET_DOWN);
    check("BET: h, s and d are not bets", bj_view_command_for_key(BJ_PANEL_BET, 'h') == BJ_CMD_NONE &&
                                              bj_view_command_for_key(BJ_PANEL_BET, 's') == BJ_CMD_NONE &&
                                              bj_view_command_for_key(BJ_PANEL_BET, 'd') == BJ_CMD_NONE);
    check("PLAY: h and Enter hit", bj_view_command_for_key(BJ_PANEL_PLAY, 'h') == BJ_CMD_HIT &&
                                       bj_view_command_for_key(BJ_PANEL_PLAY, 'H') == BJ_CMD_HIT &&
                                       bj_view_command_for_key(BJ_PANEL_PLAY, BJ_KEY_ENTER) == BJ_CMD_HIT);
    check("PLAY: s and Space stand", bj_view_command_for_key(BJ_PANEL_PLAY, 's') == BJ_CMD_STAND &&
                                         bj_view_command_for_key(BJ_PANEL_PLAY, 'S') == BJ_CMD_STAND &&
                                         bj_view_command_for_key(BJ_PANEL_PLAY, ' ') == BJ_CMD_STAND);
    check("PLAY: d doubles", bj_view_command_for_key(BJ_PANEL_PLAY, 'd') == BJ_CMD_DOUBLE &&
                                 bj_view_command_for_key(BJ_PANEL_PLAY, 'D') == BJ_CMD_DOUBLE);
    check("PLAY: arrows and n do nothing mid-hand", bj_view_command_for_key(BJ_PANEL_PLAY, BJ_KEY_UP) == BJ_CMD_NONE &&
                                                        bj_view_command_for_key(BJ_PANEL_PLAY, 'n') == BJ_CMD_NONE &&
                                                        bj_view_command_for_key(BJ_PANEL_PLAY, '+') == BJ_CMD_NONE);
    check("BROKE: Enter, Space and n start a new bankroll",
          bj_view_command_for_key(BJ_PANEL_BROKE, BJ_KEY_ENTER) == BJ_CMD_NEW_BANKROLL &&
              bj_view_command_for_key(BJ_PANEL_BROKE, ' ') == BJ_CMD_NEW_BANKROLL &&
              bj_view_command_for_key(BJ_PANEL_BROKE, 'n') == BJ_CMD_NEW_BANKROLL);
    check("BROKE: nothing else", bj_view_command_for_key(BJ_PANEL_BROKE, 'h') == BJ_CMD_NONE &&
                                     bj_view_command_for_key(BJ_PANEL_BROKE, BJ_KEY_RIGHT) == BJ_CMD_NONE);
    {
        int acts = 0;

        for (p = 0; p < 3; p++) {
            for (i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
                acts += bj_view_command_for_key((enum bj_panel)p, junk[i]) != BJ_CMD_NONE;
            }
        }
        check("keys outside the map do nothing in any phase", acts == 0);
    }

    stack(&g, 6, 10, 5, 7, 0);
    check("running DEAL deals", bj_view_run(&g, BJ_CMD_DEAL) == BJ_OK && bj_view_panel(&g) == BJ_PANEL_PLAY);
    bj_view_buttons(&g, btn);
    check("HIT first, then STAND and DOUBLE", strcmp(btn[0].text, "HIT") == 0 && strcmp(btn[1].text, "STAND") == 0 &&
                                                  strcmp(btn[2].text, "DOUBLE") == 0 && btn[2].enabled);
    check("the dealer's second card is face down while the player acts",
          bj_view_face_down(&g, 1) && !bj_view_face_down(&g, 0));
    check("a bet command mid-hand is refused", bj_view_run(&g, BJ_CMD_BET_UP) == BJ_ERR_PHASE && g.bet == 10);
    check("and so is a new bankroll", bj_view_run(&g, BJ_CMD_NEW_BANKROLL) == BJ_ERR_PHASE);
    bj_view_run(&g, BJ_CMD_HIT);
    bj_view_buttons(&g, btn);
    check("after a hit DOUBLE is shown but off", g.phase != BJ_PLAYER || !btn[2].enabled);
    if (g.phase == BJ_PLAYER) {
        bj_view_run(&g, BJ_CMD_STAND);
    }
    check("the hole card is face up once the round settles", !bj_view_face_down(&g, 1));
    bj_view_buttons(&g, btn);
    check("after a round DEAL reads NEW ROUND", strcmp(btn[0].text, "NEW ROUND") == 0);

    g.bankroll = 0;
    bj_view_buttons(&g, btn);
    check("with no chips the only button is NEW BANKROLL", bj_view_panel(&g) == BJ_PANEL_BROKE &&
                                                               strcmp(btn[0].text, "NEW BANKROLL") == 0 &&
                                                               !btn[1].text && !btn[2].text);
    check("which runs", bj_view_run(&g, BJ_CMD_NEW_BANKROLL) == BJ_OK && g.bankroll == BJ_BANKROLL_START);
    check("but not when there are chips", bj_view_run(&g, BJ_CMD_NEW_BANKROLL) == BJ_ERR_PHASE);
    for (p = 0; p < 30; p++) {
        bj_view_run(&g, BJ_CMD_BET_UP);
    }
    bj_view_buttons(&g, btn);
    check("plus is off at the bet limit", g.bet == BJ_BET_MAX && !btn[2].enabled && btn[1].enabled);
    check("NONE runs nothing", bj_view_run(&g, BJ_CMD_NONE) == BJ_ERR_PHASE);
}

static void test_words(void)
{
    struct bj_game g;
    char s[96];

    bj_new_session(&g, 2);
    check_str("the HUD's left panel is BANK", bj_view_hud_label(0), "BANK");
    check_str("its right panel BET", bj_view_hud_label(1), "BET");
    bj_view_hud_value(&g, 0, s, sizeof(s));
    check_str("BANK shows the chips", s, "1000");
    bj_bet_up(&g);
    bj_bet_up(&g);
    bj_view_hud_value(&g, 1, s, sizeof(s));
    check_str("BET between rounds shows the bet being set", s, "30");
    bj_view_result(&g, s, sizeof(s));
    check_str("no round, no result on the felt", s, "");
    bj_bet_down(&g);
    bj_bet_down(&g);
    bj_view_dealer_label(&g, s, sizeof(s));
    check_str("no cards: DEALER", s, "DEALER");
    bj_view_player_label(&g, s, sizeof(s));
    check_str("no cards: YOU", s, "YOU");
    bj_view_caption(&g, BJ_CMD_NONE, BJ_OK, s, sizeof(s));
    check_str("before the first round", s, "SET YOUR BET, THEN DEAL");

    stack(&g, 1, 10, 6, 7, 0); /* player A 6, dealer 10 7 */
    bj_deal(&g);
    bj_view_dealer_label(&g, s, sizeof(s));
    check_str("the dealer shows only the upcard", s, "DEALER SHOWS 10");
    bj_view_player_label(&g, s, sizeof(s));
    check_str("a soft hand says so", s, "YOU SOFT 17");
    bj_view_caption(&g, BJ_CMD_DEAL, BJ_OK, s, sizeof(s));
    check_str("the play caption", s, "YOU HAVE SOFT 17 " DOT " HIT OR STAND");
    bj_view_hud_value(&g, 0, s, sizeof(s));
    check_str("BANK during a hand is what is not on the table", s, "990");
    bj_view_hud_value(&g, 1, s, sizeof(s));
    check_str("BET during a hand is what is on it", s, "10");
    bj_view_result(&g, s, sizeof(s));
    check_str("a hand in play has no result yet", s, "");
    bj_stand(&g);
    bj_view_dealer_label(&g, s, sizeof(s));
    check_str("the dealer's total once revealed", s, "DEALER 17");
    bj_view_player_label(&g, s, sizeof(s));
    check_str("the player's total, no longer marked soft", s, "YOU 17");
    bj_view_caption(&g, BJ_CMD_STAND, BJ_OK, s, sizeof(s));
    check_str("a push", s, "PUSH " DOT " BET RETURNED");
    check("a push is neither win nor loss", bj_view_player_result(&g) == 0);
    bj_view_result(&g, s, sizeof(s));
    check_str("the felt says PUSH", s, "PUSH");

    stack(&g, 1, 9, 13, 7, 0);
    bj_deal(&g);
    bj_view_player_label(&g, s, sizeof(s));
    check_str("a player blackjack", s, "YOU BLACKJACK");
    bj_view_caption(&g, BJ_CMD_DEAL, BJ_OK, s, sizeof(s));
    check_str("pays in words", s, "BLACKJACK " DOT " +15");
    check("and is a win", bj_view_player_result(&g) == 1);
    bj_view_result(&g, s, sizeof(s));
    check_str("the felt shows what it paid", s, "+15");

    stack(&g, 10, 1, 9, 12, 0);
    bj_deal(&g);
    bj_view_dealer_label(&g, s, sizeof(s));
    check_str("a dealer blackjack", s, "DEALER BLACKJACK");
    bj_view_caption(&g, BJ_CMD_DEAL, BJ_OK, s, sizeof(s));
    check_str("costs in words, with a real minus sign", s, "DEALER BLACKJACK " DOT " " MINUS "10");
    check("and is a loss", bj_view_player_result(&g) == -1);
    bj_view_result(&g, s, sizeof(s));
    check_str("the felt shows what it cost, with the same minus", s, MINUS "10");
    bj_view_hud_value(&g, 1, s, sizeof(s));
    check_str("and BET after the round is the next bet again", s, "10");

    stack(&g, 10, 10, 6, 7, 9, 0);
    bj_deal(&g);
    bj_hit(&g);
    bj_view_player_label(&g, s, sizeof(s));
    check_str("a bust", s, "YOU 25 BUST");
    bj_view_caption(&g, BJ_CMD_HIT, BJ_OK, s, sizeof(s));
    check_str("bust in words", s, "BUST " DOT " " MINUS "10");

    stack(&g, 10, 10, 7, 6, 6, 0);
    bj_deal(&g);
    bj_stand(&g);
    bj_view_dealer_label(&g, s, sizeof(s));
    check_str("a dealer bust", s, "DEALER 22 BUST");
    bj_view_caption(&g, BJ_CMD_STAND, BJ_OK, s, sizeof(s));
    check_str("dealer bust in words", s, "DEALER BUSTS " DOT " +10");

    stack(&g, 10, 10, 9, 7, 0);
    bj_deal(&g);
    bj_stand(&g);
    bj_view_caption(&g, BJ_CMD_STAND, BJ_OK, s, sizeof(s));
    check_str("a win in words", s, "YOU WIN " DOT " +10");

    stack(&g, 10, 10, 6, 9, 0);
    bj_deal(&g);
    bj_stand(&g);
    bj_view_caption(&g, BJ_CMD_STAND, BJ_OK, s, sizeof(s));
    check_str("a loss in words", s, "DEALER WINS " DOT " " MINUS "10");

    /* Refusals. */
    bj_view_caption(&g, BJ_CMD_BET_DOWN, BJ_ERR_LIMIT, s, sizeof(s));
    check_str("the minimum bet", s, "THE MINIMUM BET IS 10");
    bj_view_caption(&g, BJ_CMD_BET_UP, BJ_ERR_LIMIT, s, sizeof(s));
    check_str("the maximum bet", s, "THE MAXIMUM BET IS 200");
    g.bankroll = 70;
    bj_view_caption(&g, BJ_CMD_BET_UP, BJ_ERR_LIMIT, s, sizeof(s));
    check_str("a bet above the chips", s, "A BET CANNOT BE MORE THAN YOUR CHIPS");
    bj_view_caption(&g, BJ_CMD_DOUBLE, BJ_ERR_FUNDS, s, sizeof(s));
    check_str("a double the chips cannot cover", s, "NOT ENOUGH CHIPS TO DOUBLE");
    bj_view_caption(&g, BJ_CMD_DOUBLE, BJ_ERR_NOT_FIRST, s, sizeof(s));
    check_str("a late double", s, "DOUBLE ONLY ON YOUR FIRST TWO CARDS");
    g.bankroll = 0;
    bj_view_caption(&g, BJ_CMD_NONE, BJ_OK, s, sizeof(s));
    check_str("no chips", s, "OUT OF CHIPS");

    /* Saving. */
    check("no note while saving is fine", bj_view_store_note(0, 0) == NULL);
    check_str("a failed save", bj_view_store_note(1, 0), "NOT SAVED " DOT " PLAY CONTINUES");
    check_str("a refused saved game", bj_view_store_note(0, 1), "SAVED GAME UNREADABLE " DOT " NEW BANKROLL");
    check_str("a failed save is the more urgent", bj_view_store_note(1, 1), "NOT SAVED " DOT " PLAY CONTINUES");
    bj_view_hud_value(NULL, 0, s, sizeof(s));
    check_str("no game, no HUD value", s, "");

    bj_new_session(&g, 3);
    g.shoe.n = 10;
    bj_deal(&g);
    if (g.phase == BJ_PLAYER) {
        int soft;
        int total = bj_hand_total(&g.player, &soft);
        char want[96];

        bj_view_caption(&g, BJ_CMD_DEAL, BJ_OK, s, sizeof(s));
        snprintf(want, sizeof(want), "NEW SHOE " DOT " YOU HAVE %s%d " DOT " HIT OR STAND", soft ? "SOFT " : "", total);
        check_str("a fresh shoe is mentioned on the deal it happened", s, want);
    }
}

int main(void)
{
    test_layout();
    test_panels_and_keys();
    test_words();
    printf("bj_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
