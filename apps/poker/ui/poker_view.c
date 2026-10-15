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
    /* A short table takes the room the margins above and below it had:
     * the short layout needs it at the largest text size. */
    int top = margin, below = gap;
    if (poker_view_short(w, y - margin - gap)) {
        top = 4;
        below = 8;
    }
    out->table = (struct poker_rect){margin, top, w, y - top - below};
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
/* A table narrower than any board still gets cards a pixel wide, never an
 * area of no or negative size. */
static void cards_at_least_one(struct poker_cards *c)
{
    if (c->mini < 1)
        c->mini = 1;
    if (c->board < 1)
        c->board = 1;
    if (c->hero < 1)
        c->hero = 1;
}
void poker_view_cards(int width, int height, struct poker_cards *out)
{
    struct poker_cards c;
    c.mini = width / 9 > 58 ? 58 : width / 9;
    c.board = (width - 56) / 5 > 82 ? 82 : (width - 56) / 5;
    c.hero = width / 6 > 86 ? 86 : width / 6;
    cards_at_least_one(&c);
    cards_place(height, &c);
    /* The height the cards take: positive, every card being at least 1. */
    int need = (2 * c.mini + c.board + c.hero) * 7 / 5;
    if (cards_gap(&c) < 0 && need > 0) {
        /* Every card by the same share of the height the cards take, then
         * a pixel at a time for the rounding. */
        int room = need + cards_gap(&c);
        if (room < need / 4)
            room = need / 4;
        c.mini = c.mini * room / need;
        c.board = c.board * room / need;
        c.hero = c.hero * room / need;
        cards_at_least_one(&c);
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
bool poker_view_short(int width, int height)
{
    struct poker_cards c;
    poker_view_cards(width, height, &c);
    /* Below 30 px a card's index no longer reads at any text size. */
    return cards_gap(&c) < 0 || c.mini < 30 || c.board < 30 || c.hero < 30;
}
/* A seat of the tall layout, as the table has always drawn it: its line,
 * its cards 30 below, its caption (two lines) under them, and the ring
 * round the player to act. */
static void seat_tall(struct poker_seat_place *s, const struct poker_type *t, int x, int y, int width, int cw)
{
    int ch = cw * 7 / 5, left = x + (width - 2 * cw - 8) / 2;
    s->ring = (struct poker_rect){x, y - 6, width, ch + 66};
    s->name = (struct poker_rect){x, y, width, t->name + 6};
    for (int k = 0; k < 2; ++k)
        s->card[k] = (struct poker_rect){left + k * (cw + 8), y + 30, cw, ch};
    s->status = (struct poker_rect){x - 4, y + ch + 34, width + 8, 2 * t->caption + 6};
}
static void table_tall(int w, int h, const struct poker_type *t, struct poker_table_place *p)
{
    struct poker_cards c;
    poker_view_cards(w, h, &c);
    int sw = w / 2 - 16;
    p->hand = (struct poker_rect){0, 12, w, 2 * t->caption + 6};
    seat_tall(&p->seat[2], t, (w - sw) / 2, 48, sw, c.mini);
    seat_tall(&p->seat[1], t, 8, c.side_y, sw, c.mini);
    seat_tall(&p->seat[3], t, w - sw - 8, c.side_y, sw, c.mini);
    p->pot = (struct poker_rect){0, c.board_y - 46, w, t->pot + 6};
    int cw = c.board, start = (w - 5 * cw - 4 * 8) / 2;
    for (int i = 0; i < 5; ++i)
        p->board[i] = (struct poker_rect){start + i * (cw + 8), c.board_y, cw, cw * 7 / 5};
    seat_tall(&p->seat[0], t, (w - sw) / 2, c.hero_y, sw, c.hero);
}
/* A seat of the short layout, in a cell of the given width: its line on
 * top, its cards below at the left and its caption (two lines) beside
 * them, the ring 2 px outside all of it. */
static void seat_short(struct poker_seat_place *s, const struct poker_type *t, int x, int y, int cell, int cw)
{
    int ch = cw * 7 / 5, top = y + t->name + 2, status_h = 2 * t->caption;
    s->name = (struct poker_rect){x, y, cell, t->name};
    for (int k = 0; k < 2; ++k)
        s->card[k] = (struct poker_rect){x + k * (cw + 4), top, cw, ch};
    int sx = x + 2 * cw + 8;
    s->status = (struct poker_rect){sx, top + (ch > status_h ? (ch - status_h) / 2 : 0), x + cell - sx, status_h};
    int foot = top + (ch > status_h ? ch : status_h);
    s->ring = (struct poker_rect){x - 2, y - 2, cell + 4, foot - y + 4};
}
static int at_least_one(int v)
{
    return v < 1 ? 1 : v;
}
static void table_short(int w, int h, const struct poker_type *t, struct poker_table_place *p)
{
    const int pad = 4, col = 8, gap = 4;
    int cell = (w - 2 * pad - col) / 2, status_h = 2 * t->caption;
    /* About 13 caption characters: every caption the table writes fits
     * two lines that wide (tests/poker_app_test.c measures them). */
    int status_w = t->caption * 31 / 5;
    /* The seats' cards: a third of the height the cards share, as wide as
     * the caption beside them leaves. */
    int room = h - 4 * gap - t->pot - 2 * (t->name + 2);
    int cw = room / 3 * 5 / 7;
    if (cw > (cell - 8 - status_w) / 2)
        cw = (cell - 8 - status_w) / 2;
    if (cw > 58)
        cw = 58;
    cw = at_least_one(cw);
    int row = t->name + 2 + (cw * 7 / 5 > status_h ? cw * 7 / 5 : status_h);
    /* The board: the rest of the height, leaving room at its sides for the
     * hand number and the blinds (two caption lines each). */
    int side = status_w * 2 / 3;
    int bw = (h - 4 * gap - t->pot - 2 * row) * 5 / 7;
    if (bw > (w - 2 * pad - 4 * 6 - 2 * side) / 5)
        bw = (w - 2 * pad - 4 * 6 - 2 * side) / 5;
    if (bw > 82)
        bw = 82;
    bw = at_least_one(bw);
    int bh = bw * 7 / 5;
    int slack = h - (4 * gap + t->pot + 2 * row + bh);
    int g = gap + (slack > 0 ? slack / 4 : 0);
    int y = g;
    seat_short(&p->seat[1], t, pad, y, cell, cw);
    seat_short(&p->seat[2], t, pad + cell + col, y, cell, cw);
    y += row + g;
    p->pot = (struct poker_rect){pad, y, w - 2 * pad, t->pot};
    y += t->pot;
    int start = (w - 5 * bw - 4 * 6) / 2;
    for (int i = 0; i < 5; ++i)
        p->board[i] = (struct poker_rect){start + i * (bw + 6), y, bw, bh};
    int ty = y + (bh > status_h ? (bh - status_h) / 2 : 0), right = start + 5 * bw + 4 * 6 + 6;
    p->hand = (struct poker_rect){pad, ty, start - 6 - pad, status_h};
    p->blinds = (struct poker_rect){right, ty, w - pad - right, status_h};
    y += bh + g;
    /* The player at the lower left, so the seats run clockwise from it:
     * YOU, ADA above, LINUS across, GRACE below. */
    seat_short(&p->seat[0], t, pad, y, cell, cw);
    seat_short(&p->seat[3], t, pad + cell + col, y, cell, cw);
}
void poker_view_table(int width, int height, const struct poker_type *type, struct poker_table_place *out)
{
    *out = (struct poker_table_place){0};
    out->compact = poker_view_short(width, height);
    if (out->compact)
        table_short(width, height, type, out);
    else
        table_tall(width, height, type, out);
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
