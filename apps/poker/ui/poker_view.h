/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DOORS_POKER_VIEW_H
#define DOORS_POKER_VIEW_H
#include "poker.h"

struct poker_rect {
    int x, y, w, h;
};
struct poker_layout {
    struct poker_rect table, caption, stepper, actions, secondary;
};
/* On a table too short for the tall layout (poker_view_short) the margins
 * above and below the table tighten; the controls keep their 64 px. */
void poker_view_layout(int width, int height, struct poker_layout *out);
/* The table's card widths (heights are 7/5 of them) - the side seats', the
 * board's and the player's - and where they go. Bounded by the table's
 * width, as ever; on a table too short for those (an external display's
 * portrait column is shorter than the panel's) all three shrink together
 * until the player's seat is clear of the board. */
struct poker_cards {
    int mini, board, hero;
    int side_y, board_y, hero_y; /* the side seats', the board's and the player's tops */
};
void poker_view_cards(int width, int height, struct poker_cards *out);
/* True when the tall layout above cannot fit a table this size: the
 * player's seat would not clear the board, or the cards would be too
 * small to read. The short layout is used instead (poker_view_table). */
bool poker_view_short(int width, int height);
/* Line heights of the table's three text roles at the current text size:
 * the seat line (button label), the captions and the pot line (row title). */
struct poker_type {
    int name, caption, pot;
};
/* Everything the table draws, in the table's coordinates. A rect of no
 * width is not drawn. */
struct poker_seat_place {
    struct poker_rect ring, name, card[2], status;
};
struct poker_table_place {
    bool compact;
    struct poker_rect hand, blinds; /* tall: one line in hand; short: two */
    struct poker_rect pot, board[5];
    struct poker_seat_place seat[POKER_SEATS];
};
/* The tall layout - the opponents round the board, the player below it -
 * or, on a short table (an external display's portrait column at 720p),
 * the short one: two seats a row, each its line above its cards with its
 * caption beside them, and between the rows the pot line and the board
 * with the hand number and the blinds at its sides. */
void poker_view_table(int width, int height, const struct poker_type *type, struct poker_table_place *out);
const char *poker_seat_name(int seat);
/* Shared map for physical keyboard and touchscreen buttons. */
enum poker_control {
    POKER_UI_FOLD,
    POKER_UI_CALL,
    POKER_UI_RAISE,
    POKER_UI_ALL_IN,
    POKER_UI_NEXT,
    POKER_UI_RESTART,
    POKER_UI_MINUS,
    POKER_UI_PLUS,
    POKER_UI_COUNT,
    POKER_UI_NONE = -1
};
enum poker_control poker_view_key(uint32_t key, bool settled);
#endif
