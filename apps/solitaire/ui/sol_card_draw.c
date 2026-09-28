/*
 * PG Solitaire card rendering. See sol_card_draw.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_card_draw.h"

#include "pocketui.h"
#include "sol_palette.h"

#include <string.h>

int sol_card_radius(int card_w)
{
    int r = card_w / 12;

    return r < 3 ? 3 : r;
}

/* ---- primitives ------------------------------------------------------------ */

static void triangle(lv_layer_t *layer, int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c)
{
    lv_draw_triangle_dsc_t dsc;

    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = c;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

static void disc(lv_layer_t *layer, int cx, int cy, int r, lv_color_t c)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = c;
    dsc.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, &a);
}

/* s percent of size, rounded. */
#define P(size, pct) (((size) * (pct) + 50) / 100)

void sol_draw_suit(lv_layer_t *layer, int cx, int cy, int s, enum sol_suit suit, lv_color_t c)
{
    if (s < 4) {
        return;
    }
    switch (suit) {
    case SOL_DIAMONDS:
        triangle(layer, cx, cy - P(s, 50), cx + P(s, 38), cy, cx - P(s, 38), cy, c);
        triangle(layer, cx - P(s, 38), cy, cx + P(s, 38), cy, cx, cy + P(s, 50), c);
        break;
    case SOL_HEARTS: {
        int r = P(s, 26);

        disc(layer, cx - P(s, 24), cy - P(s, 20), r, c);
        disc(layer, cx + P(s, 24), cy - P(s, 20), r, c);
        triangle(layer, cx - P(s, 49), cy - P(s, 12), cx + P(s, 49), cy - P(s, 12), cx, cy + P(s, 48), c);
        break;
    }
    case SOL_SPADES: {
        int r = P(s, 24);

        triangle(layer, cx, cy - P(s, 50), cx + P(s, 47), cy + P(s, 8), cx - P(s, 47), cy + P(s, 8), c);
        disc(layer, cx - P(s, 23), cy + P(s, 10), r, c);
        disc(layer, cx + P(s, 23), cy + P(s, 10), r, c);
        triangle(layer, cx, cy + P(s, 6), cx + P(s, 18), cy + P(s, 50), cx - P(s, 18), cy + P(s, 50), c);
        break;
    }
    case SOL_CLUBS:
    default: {
        int r = P(s, 22);

        disc(layer, cx, cy - P(s, 26), r, c);
        disc(layer, cx - P(s, 25), cy + P(s, 6), r, c);
        disc(layer, cx + P(s, 25), cy + P(s, 6), r, c);
        triangle(layer, cx, cy - P(s, 4), cx + P(s, 18), cy + P(s, 50), cx - P(s, 18), cy + P(s, 50), c);
        break;
    }
    }
}

static const lv_font_t *role_font(enum pos_style_role role, int32_t *letter_space)
{
    lv_style_value_t v;

    if (letter_space) {
        *letter_space = 0;
        if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_LETTER_SPACE, &v) == LV_STYLE_RES_FOUND) {
            *letter_space = v.num;
        }
    }
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND) {
        return v.ptr;
    }
    return NULL;
}

static void text(lv_layer_t *layer, const char *str, enum pos_style_role role, lv_color_t c, int x1, int y1,
                 int x2, lv_text_align_t align)
{
    lv_draw_label_dsc_t dsc;
    int32_t ls;
    const lv_font_t *font = role_font(role, &ls);
    lv_area_t a;

    if (!font) {
        return;
    }
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.letter_space = ls;
    dsc.color = c;
    dsc.align = align;
    dsc.text = str;
    dsc.text_local = 1;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y1 + lv_font_get_line_height(font) - 1;
    lv_draw_label(layer, &dsc, &a);
}

static void panel(lv_layer_t *layer, const lv_area_t *area, int radius, lv_color_t fill, lv_opa_t fill_opa,
                  lv_color_t border, int border_w)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = fill;
    dsc.bg_opa = fill_opa;
    dsc.border_color = border;
    dsc.border_width = border_w;
    dsc.border_opa = border_w ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(layer, &dsc, area);
}

/* ---- a card ------------------------------------------------------------------ */

void sol_draw_face(lv_layer_t *layer, const lv_area_t *area, sol_card_t card)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    int rank = sol_card_rank(card);
    enum sol_suit suit = sol_card_suit(card);
    lv_color_t ink = sol_palette[sol_card_is_red(card) ? SOL_INK_RED : SOL_INK_BLACK];
    int pad = w / 14 < 3 ? 3 : w / 14;
    /* The corner: rank in app-title type on a card wide enough for it, row
     * title on a smaller one; the pip beside it, the same height as the
     * digits. Both sit in the top third, which is what a fanned card shows. */
    enum pos_style_role corner = w >= 56 ? POS_STYLE_TITLE : POS_STYLE_ROW_TITLE;
    const lv_font_t *font = role_font(corner, NULL);
    int line = font ? lv_font_get_line_height(font) : 20;
    int pip = w / 4;

    if (!sol_card_valid(card)) {
        return;
    }
    panel(layer, area, sol_card_radius(w), sol_palette[SOL_INK_PAPER], LV_OPA_COVER, sol_palette[SOL_INK_EDGE], 1);
    text(layer, sol_rank_text(rank), corner, ink, area->x1 + pad, area->y1 + pad / 2 - line / 12,
         area->x2 - pad, LV_TEXT_ALIGN_LEFT);
    sol_draw_suit(layer, area->x2 - pad - pip / 2, area->y1 + pad / 2 + line / 2, pip, suit, ink);

    /* The face below the corner: a large pip, or the court card's letter. */
    {
        int cx = area->x1 + w / 2;
        int cy = area->y1 + h * 62 / 100;

        if (rank >= SOL_JACK) {
            const lv_font_t *big = role_font(POS_STYLE_HERO_40, NULL);
            int bl = big ? lv_font_get_line_height(big) : 40;

            text(layer, sol_rank_text(rank), w >= 56 ? POS_STYLE_HERO_40 : POS_STYLE_TITLE, ink, area->x1,
                 cy - bl / 2, area->x2, LV_TEXT_ALIGN_CENTER);
        } else {
            sol_draw_suit(layer, cx, cy, rank == SOL_ACE ? w * 58 / 100 : w * 44 / 100, suit, ink);
        }
    }
}

void sol_draw_back(lv_layer_t *layer, const lv_area_t *area)
{
    int w = lv_area_get_width(area);
    int r = sol_card_radius(w);
    int inset = w / 12 < 3 ? 3 : w / 12;
    lv_area_t frame = { area->x1 + inset, area->y1 + inset, area->x2 - inset, area->y2 - inset };

    panel(layer, area, r, sol_palette[SOL_INK_BACK], LV_OPA_COVER, sol_palette[SOL_INK_EDGE], 1);
    panel(layer, &frame, r > 2 ? r - 2 : 1, sol_palette[SOL_INK_BACK], LV_OPA_TRANSP,
          sol_palette[SOL_INK_BACK_LINE], 1);
    /* One small diamond in the middle, and nothing else. */
    sol_draw_suit(layer, area->x1 + w / 2, area->y1 + lv_area_get_height(area) / 2, w / 4, SOL_DIAMONDS,
                  sol_palette[SOL_INK_BACK_LINE]);
}

void sol_draw_slot(lv_layer_t *layer, const lv_area_t *area, enum sol_slot_mark mark, enum sol_suit suit)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    lv_color_t c = sol_palette[SOL_INK_SLOT];

    panel(layer, area, sol_card_radius(w), c, LV_OPA_TRANSP, c, 2);
    switch (mark) {
    case SOL_SLOT_SUIT:
        sol_draw_suit(layer, area->x1 + w / 2, area->y1 + h / 2, w * 40 / 100, suit, c);
        break;
    case SOL_SLOT_KING: {
        const lv_font_t *big = role_font(POS_STYLE_HERO_40, NULL);
        int bl = big ? lv_font_get_line_height(big) : 40;

        text(layer, "K", POS_STYLE_HERO_40, c, area->x1, area->y1 + h / 2 - bl / 2, area->x2,
             LV_TEXT_ALIGN_CENTER);
        break;
    }
    case SOL_SLOT_TURN: {
        const lv_font_t *sym = role_font(POS_STYLE_SYMBOL_LARGE, NULL);
        int sl = sym ? lv_font_get_line_height(sym) : 32;

        text(layer, LV_SYMBOL_REFRESH, POS_STYLE_SYMBOL_LARGE, c, area->x1, area->y1 + h / 2 - sl / 2, area->x2,
             LV_TEXT_ALIGN_CENTER);
        break;
    }
    case SOL_SLOT_PLAIN:
    default:
        break;
    }
}
