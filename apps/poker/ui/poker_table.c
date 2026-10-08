/* Texas Hold'em table using the existing Doors card shapes and felt.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker_table.h"
#include "bj_card_draw.h"
#include "bj_felt.h"
#include "bj_palette.h"
#include "pocketui.h"
#include "poker_view.h"
#include <stdio.h>

static lv_area_t area(const lv_area_t *origin, int x, int y, int w, int h)
{
    return (lv_area_t){origin->x1 + x, origin->y1 + y, origin->x1 + x + w - 1, origin->y1 + y + h - 1};
}
static void text(lv_layer_t *layer, const lv_area_t *origin, int x, int y, int w, const char *value,
                 enum pos_style_role role, enum pos_color_token color)
{
    lv_draw_label_dsc_t d;
    lv_style_value_t v;
    lv_draw_label_dsc_init(&d);
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND)
        d.font = v.ptr;
    d.text = value;
    d.text_local = 1;
    d.align = LV_TEXT_ALIGN_CENTER;
    d.color = pos_theme_color(color);
    /* Hand names and payouts may wrap; reserve two caption lines. */
    int lines = role == POS_STYLE_CAPTION ? 2 : 1;
    lv_area_t r = area(origin, x, y, w, lines * lv_font_get_line_height(d.font) + 6);
    lv_draw_label(layer, &d, &r);
}
static void seat(lv_layer_t *layer, const lv_area_t *origin, const struct poker_game *g, int i, int x, int y,
                 int width, int card_width)
{
    const struct poker_player *p = &g->player[i];
    char line[80];
    enum pos_color_token color = g->actor == i ? POS_COLOR_ACCENT_PRIMARY : POS_COLOR_TEXT_PRIMARY;
    const char *badge = g->button == i        ? (g->small_blind == i ? " D/SB" : " D")
                        : g->small_blind == i ? " SB"
                        : g->big_blind == i   ? " BB"
                                              : "";
    if (g->actor == i) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
        d.border_width = 2;
        d.radius = 8;
        lv_area_t r = area(origin, x, y - 6, width, card_width * 7 / 5 + 66);
        lv_draw_rect(layer, &d, &r);
    }
    snprintf(line, sizeof(line), "%s%s%s %u", g->actor == i ? ">" : "", poker_seat_name(i), badge, p->stack);
    text(layer, origin, x, y, width, line, POS_STYLE_BUTTON_LABEL, color);
    int cw = card_width, ch = cw * 7 / 5, left = x + (width - 2 * cw - 8) / 2;
    for (int k = 0; k < 2; ++k) {
        lv_area_t r = area(origin, left + k * (cw + 8), y + 30, cw, ch);
        if (!p->in_hand || p->folded)
            bj_draw_slot(layer, &r, BJ_SLOT_PLAIN, BJ_SPADES);
        else if (i == 0 || (g->showdown && !poker_playing(g)))
            bj_draw_face(layer, &r, p->hole[k], BJ_INDEX_BELOW);
        else
            bj_draw_back(layer, &r);
    }
    if (!p->in_hand)
        snprintf(line, sizeof(line), "OUT");
    else if (p->folded)
        snprintf(line, sizeof(line), "FOLDED");
    else if (!poker_playing(g) && g->showdown)
        snprintf(line, sizeof(line), "%s | +%u", poker_rank_name(g->rank[i]), p->payout - p->refunded);
    else if (!poker_playing(g))
        snprintf(line, sizeof(line), p->payout ? "+%u chips" : "Hand complete", p->payout - p->refunded);
    else if (!p->stack)
        snprintf(line, sizeof(line), "ALL IN | %u", p->bet);
    else if (p->bet)
        snprintf(line, sizeof(line), "BET %u", p->bet);
    else
        snprintf(line, sizeof(line), p->acted ? "CHECK" : "In hand");
    text(layer, origin, x - 4, y + ch + 34, width + 8, line, POS_STYLE_CAPTION,
         !poker_playing(g) && (g->winners & (1u << i)) ? POS_COLOR_STATUS_OK : POS_COLOR_TEXT_SECONDARY);
}
static void draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const struct poker_game *g = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);
    int w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    if (!g || !layer)
        return;
    const lv_image_dsc_t *felt = bj_felt_image();
    if (felt) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = felt;
        d.tile = 1;
        lv_draw_image(layer, &d, &origin);
    } else {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = bj_palette[BJ_INK_FELT];
        d.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &d, &origin);
    }
    if (pos_theme_current_mode() == POS_MODE_NIGHT) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = pos_theme_color(POS_COLOR_BG);
        d.bg_opa = LV_OPA_50;
        lv_draw_rect(layer, &d, &origin);
    }
    char line[80];
    snprintf(line, sizeof(line), "HAND %u | BLINDS 10 / 20", g->hand_number);
    text(layer, &origin, 0, 12, w, line, POS_STYLE_CAPTION, POS_COLOR_TEXT_SECONDARY);
    int mini = w / 9;
    if (mini > 58)
        mini = 58;
    int sw = w / 2 - 16;
    seat(layer, &origin, g, 2, (w - sw) / 2, 48, sw, mini);
    int sy = 48 + mini * 7 / 5 + 100;
    seat(layer, &origin, g, 1, 8, sy, sw, mini);
    seat(layer, &origin, g, 3, w - sw - 8, sy, sw, mini);
    int board_y = sy + mini * 7 / 5 + 125;
    snprintf(line, sizeof(line), "%s | POT %u", poker_phase_name(g->phase),
             poker_playing(g) ? poker_pot_total(g) : g->settled_pot);
    text(layer, &origin, 0, board_y - 46, w, line, POS_STYLE_ROW_TITLE, POS_COLOR_TEXT_PRIMARY);
    int cw = (w - 56) / 5;
    if (cw > 82)
        cw = 82;
    int start = (w - 5 * cw - 4 * 8) / 2;
    for (int i = 0; i < 5; ++i) {
        lv_area_t r = area(&origin, start + i * (cw + 8), board_y, cw, cw * 7 / 5);
        if (i < g->board_count)
            bj_draw_face(layer, &r, g->board[i], BJ_INDEX_BELOW);
        else
            bj_draw_slot(layer, &r, BJ_SLOT_PLAIN, BJ_SPADES);
    }
    int human_cw = w / 6;
    if (human_cw > 86)
        human_cw = 86;
    seat(layer, &origin, g, 0, (w - sw) / 2, h - human_cw * 7 / 5 - 92, sw, human_cw);
}
lv_obj_t *poker_table_create(lv_obj_t *parent, const struct poker_game *game)
{
    lv_obj_t *table = lv_obj_create(parent);
    lv_obj_remove_style_all(table);
    lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(table, (void *)game);
    lv_obj_add_event_cb(table, draw, LV_EVENT_DRAW_MAIN, NULL);
    return table;
}
