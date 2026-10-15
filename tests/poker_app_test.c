/* Real LVGL objects, pointer/keyboard actions and shell lifecycle.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "app.h"
#include "pocketui.h"
#include "poker_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct pocketos_app app_poker;
static int checks, failed;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            ++failed;                                                                                        \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                                      \
        }                                                                                                    \
    } while (0)
void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
}
void pocketos_shell_go_home(void)
{
}
int pocketos_shell_reduced_motion(void)
{
    return 1;
}
int64_t pocketos_shell_system_day(void)
{
    return -1;
}
const char *pocketos_shell_radio_state(void)
{
    return NULL;
}
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*done)(void *), void *user)
{
    (void)ret;
    (void)done;
    (void)user;
}
void pocketos_shell_keyboard_hide(void)
{
}
int pocketos_shell_keyboard_visible(void)
{
    return 0;
}

static uint8_t buffer[568 * 40 * 4];
static lv_point_t point;
static lv_indev_state_t finger = LV_INDEV_STATE_RELEASED;
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = point;
    data->state = finger;
}
static void pump(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}
static void tap(void *priv, enum poker_control control)
{
    lv_obj_t *button = poker_app_button(priv, control);
    lv_area_t area;
    lv_obj_update_layout(button);
    lv_obj_get_coords(button, &area);
    point.x = (area.x1 + area.x2) / 2;
    point.y = (area.y1 + area.y2) / 2;
    finger = LV_INDEV_STATE_PRESSED;
    pump(50);
    finger = LV_INDEV_STATE_RELEASED;
    pump(50);
}
static const lv_font_t *style_font(enum pos_style_role role)
{
    lv_style_value_t v;
    return lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND ? v.ptr : NULL;
}
static int timers(void)
{
    int n = 0;
    lv_timer_t *t = NULL;
    while ((t = lv_timer_get_next(t)))
        ++n;
    return n;
}
static void human_turn(struct poker_game *g)
{
    int steps = 0;
    while (poker_playing(g) && g->actor != 0 && steps++ < 100) {
        struct poker_options o = poker_options(g);
        CHECK(poker_act(g, o.can_check ? POKER_CHECK : POKER_CALL, 0));
    }
}
int main(void)
{
    unsetenv("DOORS_POKER_SCREEN");
    lv_init();
    lv_display_t *display = lv_display_create(568, 1232);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    lv_obj_t *body = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 28, 72);
    lv_obj_set_size(body, 512, 1132);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    pump(50);
    int baseline = timers();
    void *priv = app_poker.create(body);
    CHECK(priv != NULL);
    pump(60);
    struct poker_game *g = poker_app_game(priv);
    CHECK(poker_playing(g) && g->hand_number == 1);
    CHECK(app_poker.orientation == POCKETOS_APP_ORIENTATION_PORTRAIT);
    CHECK(app_poker.chrome == POCKETOS_CHROME_NONE);
    CHECK(timers() == baseline);
    for (int i = 0; i < POKER_UI_COUNT; ++i) {
        lv_obj_t *b = poker_app_button(priv, (enum poker_control)i);
        lv_area_t a, root;
        lv_obj_get_coords(b, &a);
        lv_obj_get_coords(poker_app_root(priv), &root);
        CHECK(lv_obj_get_height(b) >= 64 && lv_obj_get_width(b) >= 64);
        CHECK(a.x1 >= root.x1 && a.x2 <= root.x2 && a.y1 >= root.y1 && a.y2 <= root.y2);
        CHECK(!lv_obj_has_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE));
    }
    poker_restart(g, 30);
    poker_new_hand(g);
    human_turn(g);
    poker_app_refresh(priv);
    pump(40);
    CHECK(g->actor == 0 && !lv_obj_has_state(poker_app_button(priv, POKER_UI_CALL), LV_STATE_DISABLED));
    uint32_t before = g->current_bet;
    tap(priv, POKER_UI_PLUS);
    tap(priv, POKER_UI_RAISE);
    CHECK(g->current_bet > before && g->actor != 0);
    struct poker_game saved = *g;
    pos_input_push_key('f');
    pump(50);
    CHECK(memcmp(&saved, g, sizeof(saved)) == 0); /* opponent turn */
    app_poker.tick(priv);
    CHECK(memcmp(&saved, g, sizeof(saved)) != 0);
    tap(priv, POKER_UI_RESTART);
    CHECK(strstr(lv_label_get_text(poker_app_caption(priv)), "again") != NULL);
    CHECK(app_poker.back(priv) == 1);
    CHECK(app_poker.back(priv) == 0);
    tap(priv, POKER_UI_RESTART);
    tap(priv, POKER_UI_RESTART);
    CHECK(g->hand_number == 1 && g->player[0].stack == 1000);
    human_turn(g);
    poker_app_refresh(priv);
    pump(40);
    pos_input_push_key('c');
    pump(60);
    CHECK(g->actor != 0);
    int steps = 0;
    while (poker_playing(g) && steps++ < 300) {
        if (g->actor == 0) {
            human_turn(g);
            struct poker_options o = poker_options(g);
            poker_act(g, o.can_check ? POKER_CHECK : POKER_CALL, 0);
            poker_app_refresh(priv);
        } else
            app_poker.tick(priv);
    }
    CHECK(!poker_playing(g));
    CHECK(!g->showdown || g->board_count == 5);
    poker_app_refresh(priv);
    pump(40);
    if (g->phase == POKER_FINISHED) {
        tap(priv, POKER_UI_NEXT);
        CHECK(g->hand_number == 2);
    }
    poker_restart(g, 31);
    poker_new_hand(g);
    human_turn(g);
    poker_app_refresh(priv);
    pump(40);
    tap(priv, POKER_UI_FOLD);
    CHECK(g->player[0].folded && g->actor != 0);
    poker_restart(g, 32);
    poker_new_hand(g);
    human_turn(g);
    poker_app_refresh(priv);
    pump(40);
    tap(priv, POKER_UI_ALL_IN);
    CHECK(g->player[0].stack == 0 && g->player[0].committed == 1000);
    CHECK(!lv_obj_has_state(poker_app_button(priv, POKER_UI_RESTART), LV_STATE_DISABLED));
    g->phase = POKER_GAME_OVER;
    poker_app_refresh(priv);
    CHECK(strstr(lv_label_get_text(poker_app_caption(priv)), "Game over") != NULL);
    CHECK(lv_obj_has_state(poker_app_button(priv, POKER_UI_NEXT), LV_STATE_DISABLED));
    saved = *g;
    app_poker.destroy(priv);
    lv_obj_clean(body);
    pump(60);
    CHECK(timers() == baseline);
    priv = app_poker.create(body);
    g = poker_app_game(priv);
    CHECK(memcmp(&saved, g, sizeof(saved)) == 0);
    app_poker.destroy(priv);
    lv_obj_clean(body);
    pump(60);
    for (int i = 0; i < 40; ++i) {
        priv = app_poker.create(body);
        pump(10);
        app_poker.destroy(priv);
        lv_obj_clean(body);
        pump(10);
        CHECK(timers() == baseline);
        CHECK(lv_obj_get_child_count(body) == 0);
    }
    {
        /* The short table (a 1280 x 720 external display's portrait column,
         * 528 x 604 of content) with the real fonts at every text size:
         * the line heights poker_view_test assumes, every seat line, caption
         * and pot line the table can write inside its place, a "10" on one
         * line on every card (there and on every taller short table), and
         * the app's own table that size with the
         * controls below it - drawn on the flop, turn and river. */
        static const char *const names[] = {">GRACE D/SB 4000", ">LINUS D/SB 4000", ">ADA D/SB 4000",
                                            ">YOU D/SB 4000"};
        static const char *const captions[] = {
            "Straight flush | +4000", "Three of a kind | +4000", "Four of a kind | +4000", "Full house | +4000",
            "High card | +4000",      "ALL IN | 4000",           "Hand complete",          "+4000 chips",
            "FOLDED",                 "BET 4000"};
        static const char *const pots[] = {"Hand complete | POT 4000", "Game over | POT 4000",
                                           "New table | POT 4000", "Preflop | POT 4000"};
        static const int lines[3][3] = {{21, 18, 26}, {24, 21, 30}, {28, 24, 35}};
        static const char *const screens[] = {"flop", "turn", "river"};
        enum pos_text_size was = pos_theme_current_text_size();
        lv_obj_t *column = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(column);
        lv_obj_set_pos(column, 20, 96);
        lv_obj_set_size(column, 528, 604);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        for (int size = 0; size < 3; ++size) {
            pos_theme_select_text_size((enum pos_text_size)size);
            const lv_font_t *name = style_font(POS_STYLE_BUTTON_LABEL), *caption = style_font(POS_STYLE_CAPTION);
            const lv_font_t *pot = style_font(POS_STYLE_ROW_TITLE); /* also the cards' corner */
            const struct poker_type type = {lv_font_get_line_height(name), lv_font_get_line_height(caption),
                                            lv_font_get_line_height(pot)};
            CHECK(type.name == lines[size][0] && type.caption == lines[size][1] && type.pot == lines[size][2]);
            struct poker_layout l;
            poker_view_layout(528, 604, &l);
            CHECK(poker_view_short(l.table.w, l.table.h));
            lv_point_t sz, ten;
            lv_text_get_size(&ten, "10", pot, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            /* 720p's table, and every taller one still short. */
            for (int h = l.table.h; h <= 900; ++h) {
                if (!poker_view_short(l.table.w, h))
                    continue;
                struct poker_table_place p;
                poker_view_table(l.table.w, h, &type, &p);
                CHECK(p.compact);
                struct poker_rect cards[POKER_SEATS + 1];
                for (int s = 0; s < POKER_SEATS; ++s) {
                    const struct poker_seat_place *q = &p.seat[s];
                    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
                        lv_text_get_size(&sz, names[i], name, 0, 0, q->name.w, LV_TEXT_FLAG_NONE);
                        CHECK(sz.y <= q->name.h);
                    }
                    for (unsigned i = 0; i < sizeof(captions) / sizeof(captions[0]); ++i) {
                        lv_text_get_size(&sz, captions[i], caption, 0, 0, q->status.w, LV_TEXT_FLAG_NONE);
                        CHECK(sz.x <= q->status.w && sz.y <= q->status.h);
                    }
                    cards[s] = q->card[0];
                }
                cards[POKER_SEATS] = p.board[0];
                for (int s = 0; s <= POKER_SEATS; ++s) { /* bj_draw_face's corner, inset max(3, w / 14) */
                    int pad = cards[s].w / 14 < 3 ? 3 : cards[s].w / 14;
                    CHECK(ten.x <= cards[s].w - 2 * pad);
                }
                for (unsigned i = 0; i < sizeof(pots) / sizeof(pots[0]); ++i) {
                    lv_text_get_size(&sz, pots[i], pot, 0, 0, p.pot.w, LV_TEXT_FLAG_NONE);
                    CHECK(sz.y <= p.pot.h);
                }
                lv_text_get_size(&sz, "HAND 999", caption, 0, 0, p.hand.w, LV_TEXT_FLAG_NONE);
                CHECK(sz.x <= p.hand.w && sz.y <= p.hand.h);
                lv_text_get_size(&sz, "BLINDS\n10 / 20", caption, 0, 0, p.blinds.w, LV_TEXT_FLAG_NONE);
                CHECK(sz.x <= p.blinds.w && sz.y <= p.blinds.h);
            }
            for (int k = 0; k < 3; ++k) {
                setenv("DOORS_POKER_SCREEN", screens[k], 1);
                void *shown = app_poker.create(column);
                pump(40);
                struct poker_game *sg = poker_app_game(shown);
                CHECK(sg->board_count == 3 + k && sg->actor == 0 && poker_playing(sg));
                lv_obj_t *table = lv_obj_get_child(poker_app_root(shown), 0);
                lv_area_t t, b;
                lv_obj_get_coords(table, &t);
                CHECK(lv_obj_get_width(table) == l.table.w && lv_obj_get_height(table) == l.table.h);
                for (int i = 0; i < POKER_UI_COUNT; ++i) {
                    lv_obj_get_coords(poker_app_button(shown, (enum poker_control)i), &b);
                    CHECK(lv_area_get_width(&b) >= 64 && lv_area_get_height(&b) >= 64 && b.y1 > t.y2);
                }
                lv_obj_invalidate(table);
                pump(20);
                app_poker.destroy(shown);
                lv_obj_clean(column);
                pump(10);
            }
        }
        unsetenv("DOORS_POKER_SCREEN");
        pos_theme_select_text_size(was);
        lv_obj_delete(column);
    }
    CHECK(poker_app_game(NULL) == NULL);
    CHECK(poker_app_button(NULL, POKER_UI_CALL) == NULL);
    lv_obj_delete(body);
    lv_indev_delete(pointer);
    lv_display_delete(display);
    printf("poker_app_test: %d checks, %d failures\n", checks, failed);
    return failed != 0;
}
