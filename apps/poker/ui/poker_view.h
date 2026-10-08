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
void poker_view_layout(int width, int height, struct poker_layout *out);
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
