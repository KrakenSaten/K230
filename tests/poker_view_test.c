/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker_view.h"
#include <stdio.h>
#include <string.h>
static int checks, failed;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            ++failed;                                                                                        \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                                      \
        }                                                                                                    \
    } while (0)
int main(void)
{
    for (int w = 480; w <= 700; w += 20)
        for (int h = 1000; h <= 1400; h += 20) {
            struct poker_layout l;
            poker_view_layout(w, h, &l);
            struct poker_rect r[] = {l.table, l.caption, l.stepper, l.actions, l.secondary};
            for (int i = 0; i < 5; ++i) {
                CHECK(r[i].x >= 0 && r[i].y >= 0 && r[i].w > 0 && r[i].h >= 64);
                CHECK(r[i].x + r[i].w <= w && r[i].y + r[i].h <= h);
                if (i)
                    CHECK(r[i].y >= r[i - 1].y + r[i - 1].h);
            }
        }
    {
        /* The table's cards. The panel's portrait body (568 x 1160 under the
         * header): exactly the width-bound sizes, as before, and clear. An
         * external display's portrait column at 1080p (568 x 1008): the
         * player's seat clear of the board, the cards smaller. */
        struct poker_layout l;
        struct poker_cards c;
        poker_view_layout(568, 1160, &l);
        poker_view_cards(l.table.w, l.table.h, &c);
        int mini = l.table.w / 9 > 58 ? 58 : l.table.w / 9;
        int board = (l.table.w - 56) / 5 > 82 ? 82 : (l.table.w - 56) / 5;
        int hero = l.table.w / 6 > 86 ? 86 : l.table.w / 6;
        CHECK(c.mini == mini && c.board == board && c.hero == hero);
        CHECK(c.side_y == 48 + c.mini * 7 / 5 + 100 && c.hero_y == l.table.h - c.hero * 7 / 5 - 92);
        CHECK(c.hero_y - 6 >= c.board_y + c.board * 7 / 5);
        poker_view_layout(568, 1008, &l);
        poker_view_cards(l.table.w, l.table.h, &c);
        CHECK(c.hero_y - 6 >= c.board_y + c.board * 7 / 5);
        CHECK(c.mini < mini && c.board < board && c.hero < hero && c.mini >= 24);
        CHECK(c.board_y - 46 >= c.side_y + c.mini * 7 / 5 + 60); /* the pot line below the side seats */
        int all_positive = 1;
        for (int w = 1; w <= 120; ++w) /* narrower than any board: no division by zero, no negative card */
            for (int h = 1; h <= 400; h += 13) {
                poker_view_cards(w, h, &c);
                all_positive &= c.mini >= 1 && c.board >= 1 && c.hero >= 1;
            }
        CHECK(all_positive);
        for (int h = 300; h <= 900; h += 10) { /* every height: clear, or at the smallest cards */
            poker_view_cards(488, h, &c);
            CHECK(c.hero_y - 6 >= c.board_y + c.board * 7 / 5 || c.mini <= 12 || c.board <= 12 || c.hero <= 12);
            CHECK(c.mini > 0 && c.board > 0 && c.hero > 0);
        }
    }
    CHECK(poker_view_key('f', false) == POKER_UI_FOLD);
    CHECK(poker_view_key(10, false) == POKER_UI_CALL);
    CHECK(poker_view_key(10, true) == POKER_UI_NEXT);
    CHECK(poker_view_key(19, false) == POKER_UI_PLUS);
    CHECK(poker_view_key(20, false) == POKER_UI_MINUS);
    CHECK(poker_view_key('R', false) == POKER_UI_RAISE);
    CHECK(poker_view_key('A', false) == POKER_UI_ALL_IN);
    CHECK(poker_view_key('G', true) == POKER_UI_RESTART);
    CHECK(poker_view_key(27, false) == POKER_UI_NONE); /* shell owns Escape/Back */
    CHECK(!strcmp(poker_seat_name(0), "YOU") && !strcmp(poker_seat_name(3), "GRACE"));
    printf("poker_view_test: %d checks, %d failures\n", checks, failed);
    return failed != 0;
}
