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
/* The line heights of the table's text - seat line, captions, pot line -
 * at Small, Medium and Large (mono medium 16/19/22, mono 14/17/19, sans
 * 20/24/28 in ui/pocketui/fonts; poker_app_test checks them against the
 * fonts), and the narrowest face card whose "10" index fits on one line
 * at each (poker_app_test measures that too). */
static const struct poker_type type[3] = {{21, 18, 26}, {24, 21, 30}, {28, 24, 35}};
static const int face_min[3] = {30, 34, 40};
/* The app's content box in production (measured in simulator captures):
 * the shell gives a portrait-only app a column 568 px wide on an external
 * display, the body's padding leaves 528; below the 72 px header, the
 * body's 24 px top and 20 px bottom padding leave 604 at 720p, 964 at
 * 1080p, and 1116 on the panel. */
static const struct {
    const char *name;
    int w, h;
} screen[] = {{"720p", 528, 604}, {"1080p", 528, 964}, {"panel", 528, 1116}};
static int inside(struct poker_rect r, int w, int h)
{
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}
static int meet(struct poker_rect a, struct poker_rect b)
{
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static struct poker_rect span(struct poker_rect a, struct poker_rect b)
{
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int r = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, f = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct poker_rect){x, y, r - x, f - y};
}
/* The short table as drawn: everything inside it, nothing on anything
 * else - not a seat's line, cards or caption on another's, nor the ring
 * round the player to act - and every card wide enough to read. */
static void check_short(const struct poker_table_place *p, int w, int h, int size)
{
    struct poker_rect seat[POKER_SEATS], other[8];
    int n = 0;
    other[n++] = p->hand;
    other[n++] = p->blinds;
    other[n++] = p->pot;
    for (int i = 0; i < 5; ++i) {
        other[n++] = p->board[i];
        CHECK(p->board[i].w >= face_min[size] && p->board[i].h == p->board[i].w * 7 / 5);
    }
    for (int i = 0; i < n; ++i) {
        CHECK(inside(other[i], w, h));
        for (int j = i + 1; j < n; ++j)
            CHECK(!meet(other[i], other[j]));
    }
    for (int s = 0; s < POKER_SEATS; ++s) {
        const struct poker_seat_place *q = &p->seat[s];
        struct poker_rect part[] = {q->name, q->card[0], q->card[1], q->status};
        seat[s] = q->name;
        for (int i = 0; i < 4; ++i) {
            CHECK(inside(part[i], w, h));
            for (int j = i + 1; j < 4; ++j)
                CHECK(!meet(part[i], part[j]));
            seat[s] = span(seat[s], part[i]);
        }
        CHECK(q->card[0].w >= face_min[size] && q->card[0].w == q->card[1].w);
        CHECK(inside(q->ring, w, h));
        CHECK(q->ring.x < seat[s].x && q->ring.y < seat[s].y && q->ring.x + q->ring.w > seat[s].x + seat[s].w &&
              q->ring.y + q->ring.h > seat[s].y + seat[s].h);
        for (int i = 0; i < n; ++i)
            CHECK(!meet(q->ring, other[i]));
    }
    for (int s = 0; s < POKER_SEATS; ++s)
        for (int t = 0; t < POKER_SEATS; ++t)
            if (s != t)
                CHECK(!meet(p->seat[s].ring, seat[t]));
}
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
    for (unsigned d = 0; d < sizeof(screen) / sizeof(screen[0]); ++d) {
        /* Regression: at 720p (1280 x 720 external display) the tall layout
         * put the board below the table's foot (y 305 of 260) and the
         * player's seat on the side seats'; it is short there now. The
         * 1080p column and the panel keep the tall layout exactly. */
        struct poker_layout l;
        poker_view_layout(screen[d].w, screen[d].h, &l);
        bool short_table = d == 0;
        struct poker_rect controls[] = {l.caption, l.stepper, l.actions, l.secondary};
        CHECK(l.table.y >= 0 && l.table.y + l.table.h <= l.caption.y);
        for (int i = 0; i < 4; ++i)
            CHECK(controls[i].h == 64 && controls[i].y + 64 <= screen[d].h);
        CHECK(l.caption.y == screen[d].h - 20 - (4 * 64 + 3 * 12)); /* the controls never move */
        if (short_table)
            CHECK(l.table.y == 4 && l.table.h == l.caption.y - 12 && l.table.h == 280);
        else
            CHECK(l.table.y == 20 && l.table.h == l.caption.y - 32);
        CHECK(poker_view_short(l.table.w, l.table.h) == short_table);
        for (int size = 0; size < 3; ++size) {
            struct poker_table_place p;
            poker_view_table(l.table.w, l.table.h, &type[size], &p);
            CHECK(p.compact == short_table);
            if (p.compact) {
                check_short(&p, l.table.w, l.table.h, size);
                continue;
            }
            /* The tall layout, where it is everything it always drew. */
            struct poker_cards c;
            poker_view_cards(l.table.w, l.table.h, &c);
            int sw = l.table.w / 2 - 16, ch = c.board * 7 / 5;
            CHECK(c.hero_y - 6 >= c.board_y + ch);
            CHECK(p.hand.x == 0 && p.hand.y == 12 && p.hand.w == l.table.w && p.blinds.w == 0);
            CHECK(p.pot.y == c.board_y - 46 && p.pot.h == type[size].pot + 6);
            CHECK(p.board[0].x == (l.table.w - 5 * c.board - 32) / 2 && p.board[4].y == c.board_y &&
                  p.board[4].w == c.board && p.board[4].h == ch &&
                  p.board[4].x == p.board[0].x + 4 * (c.board + 8));
            CHECK(p.seat[2].name.x == (l.table.w - sw) / 2 && p.seat[2].name.y == 48 &&
                  p.seat[2].card[0].w == c.mini);
            CHECK(p.seat[1].name.x == 8 && p.seat[3].name.x == l.table.w - sw - 8 && p.seat[1].name.y == c.side_y);
            CHECK(p.seat[0].name.y == c.hero_y && p.seat[0].card[1].w == c.hero &&
                  p.seat[0].card[0].y == c.hero_y + 30 && p.seat[0].ring.y == c.hero_y - 6 &&
                  p.seat[0].ring.h == c.hero * 7 / 5 + 66);
            CHECK(p.seat[0].status.y == c.hero_y + c.hero * 7 / 5 + 34 && p.seat[0].status.w == sw + 8 &&
                  p.seat[0].status.h == 2 * type[size].caption + 6);
            CHECK(p.board[4].y + ch <= l.table.h && p.seat[0].status.y <= l.table.h);
        }
    }
    {
        /* Every short table from the tallest that needs it down to 280:
         * inside and clear at every text size. */
        int tallest = 0;
        for (int h = 200; h <= 900; ++h)
            if (poker_view_short(488, h))
                tallest = h;
        CHECK(tallest >= 280 && tallest < 620);
        for (int h = 280; h <= tallest; ++h)
            for (int size = 0; size < 3; ++size) {
                struct poker_table_place p;
                poker_view_table(488, h, &type[size], &p);
                CHECK(p.compact);
                check_short(&p, 488, h, size);
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
