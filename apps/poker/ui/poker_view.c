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
