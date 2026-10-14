/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker_view.h"

void poker_view_layout(int width, int height, struct poker_layout *out)
{
    const int margin = 20, gap = 12, button = 64;
    int w = width > 2 * margin ? width - 2 * margin : 1;
    int y = height - margin - (4 * button + 3 * gap);
    if (y < margin + 180)
        y = margin + 180;
    out->table = (struct poker_rect){margin, margin, w, y - margin - gap};
    out->caption = (struct poker_rect){margin, y, w, button};
    out->stepper = (struct poker_rect){margin, y + button + gap, w, button};
    out->actions = (struct poker_rect){margin, y + 2 * (button + gap), w, button};
    out->secondary = (struct poker_rect){margin, y + 3 * (button + gap), w, button};
}
/* The places for the cards' widths, as the table draws them: the top seat
 * at 48, the side seats 100 below its cards, the board 125 below theirs,
 * the player's seat 92 above the foot. */
static void cards_place(int height, struct poker_cards *c)
{
    c->side_y = 48 + c->mini * 7 / 5 + 100;
    c->board_y = c->side_y + c->mini * 7 / 5 + 125;
    c->hero_y = height - c->hero * 7 / 5 - 92;
}
/* Room between the board's foot and the player's seat box (drawn 6 px
 * above the seat); negative when they overlap. */
static int cards_gap(const struct poker_cards *c)
{
    return (c->hero_y - 6) - (c->board_y + c->board * 7 / 5);
}
void poker_view_cards(int width, int height, struct poker_cards *out)
{
    struct poker_cards c;
    c.mini = width / 9 > 58 ? 58 : width / 9;
    c.board = (width - 56) / 5 > 82 ? 82 : (width - 56) / 5;
    c.hero = width / 6 > 86 ? 86 : width / 6;
    cards_place(height, &c);
    if (cards_gap(&c) < 0) {
        /* Every card by the same share of the height the cards take, then
         * a pixel at a time for the rounding. */
        int need = (2 * c.mini + c.board + c.hero) * 7 / 5;
        int room = need + cards_gap(&c);
        if (room < need / 4)
            room = need / 4;
        c.mini = c.mini * room / need;
        c.board = c.board * room / need;
        c.hero = c.hero * room / need;
        cards_place(height, &c);
        while (cards_gap(&c) < 0 && c.mini > 12 && c.board > 12 && c.hero > 12) {
            c.mini--;
            c.board--;
            c.hero--;
            cards_place(height, &c);
        }
    }
    *out = c;
}
const char *poker_seat_name(int seat)
{
    static const char *const names[] = {"YOU", "ADA", "LINUS", "GRACE"};
    return (unsigned)seat < 4 ? names[seat] : "";
}
enum poker_control poker_view_key(uint32_t key, bool settled)
{
    switch (key) {
    case 'f':
    case 'F':
        return POKER_UI_FOLD;
    case 'c':
    case 'C':
    case 10:
        return settled ? POKER_UI_NEXT : POKER_UI_CALL;
    case 'r':
    case 'R':
        return POKER_UI_RAISE;
    case 'a':
    case 'A':
        return POKER_UI_ALL_IN;
    case 'n':
    case 'N':
        return POKER_UI_NEXT;
    case 'g':
    case 'G':
        return POKER_UI_RESTART;
    case '-':
    case 20:
    case 18:
        return POKER_UI_MINUS;
    case '+':
    case '=':
    case 19:
    case 17:
        return POKER_UI_PLUS;
    default:
        return POKER_UI_NONE;
    }
}
