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
