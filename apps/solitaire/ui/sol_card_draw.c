/*
 * PG Solitaire card rendering. See sol_card_draw.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sol_card_draw.h"

#include "cg_deck.h"

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

void sol_draw_suit(lv_layer_t *layer, int cx, int cy, int s, enum cg_suit suit, lv_color_t c)
{
    if (s < 4) {
        return;
    }
    switch (suit) {
    case CG_DIAMONDS:
        triangle(layer, cx, cy - P(s, 50), cx + P(s, 38), cy, cx - P(s, 38), cy, c);
        triangle(layer, cx - P(s, 38), cy, cx + P(s, 38), cy, cx, cy + P(s, 50), c);
        break;
    case CG_HEARTS: {
        int r = P(s, 26);

        disc(layer, cx - P(s, 24), cy - P(s, 20), r, c);
        disc(layer, cx + P(s, 24), cy - P(s, 20), r, c);
        triangle(layer, cx - P(s, 49), cy - P(s, 12), cx + P(s, 49), cy - P(s, 12), cx, cy + P(s, 48), c);
        break;
    }
    case CG_SPADES: {
        int r = P(s, 24);

        triangle(layer, cx, cy - P(s, 50), cx + P(s, 47), cy + P(s, 8), cx - P(s, 47), cy + P(s, 8), c);
        disc(layer, cx - P(s, 23), cy + P(s, 10), r, c);
        disc(layer, cx + P(s, 23), cy + P(s, 10), r, c);
        triangle(layer, cx, cy + P(s, 6), cx + P(s, 18), cy + P(s, 50), cx - P(s, 18), cy + P(s, 50), c);
        break;
    }
    case CG_CLUBS:
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

/* The corner: rank in app-title type on a card wide enough for it, row title
 * on a smaller one; the pip beside it, the same height as the digits. Both
 * sit in the top strip, which is what a fanned card shows. */
struct corner {
    enum pos_style_role role;
    const lv_font_t *font;
    int pad;    /* the inset from the card's sides, and twice its top one */
    int line;   /* the rank's line height */
    int text_y; /* the top of the rank's line, from the card's top edge */
    int pip;
    int pip_y;  /* the pip's centre, from the card's top edge */
};

static struct corner corner_of(int w)
{
    struct corner c;

    c.pad = w / 14 < 3 ? 3 : w / 14;
    c.role = w >= 56 ? POS_STYLE_TITLE : POS_STYLE_ROW_TITLE;
    c.font = role_font(c.role, NULL);
    c.line = c.font ? lv_font_get_line_height(c.font) : 20;
    c.text_y = c.pad / 2 - c.line / 12;
    c.pip = w / 4;
    c.pip_y = c.pad / 2 + c.line / 2;
    return c;
}

int sol_card_index_h(int card_w)
{
    struct corner c = corner_of(card_w);
    /* The lowest ink of any rank, from the glyphs the label draws: LVGL puts
     * a glyph's foot ofs_y above the baseline, base_line above the line's
     * foot. Without a font, the whole line. */
    int text_foot = c.line;
    int pip_foot = c.pip_y + (c.pip * 50 + 50) / 100;
    int foot;

    if (c.font) {
        static const char ranks[] = "A0123456789JQK";
        int i;

        text_foot = 0;
        for (i = 0; ranks[i]; i++) {
            lv_font_glyph_dsc_t g;

            if (lv_font_get_glyph_dsc(c.font, &g, (uint32_t)ranks[i], 0)) {
                int f = c.line - (int)c.font->base_line - (int)g.ofs_y;

                text_foot = f > text_foot ? f : text_foot;
            }
        }
        text_foot = text_foot > c.line ? c.line : text_foot;
    }
    foot = c.text_y + text_foot > pip_foot ? c.text_y + text_foot : pip_foot;
    /* Two pixels more: the next card's edge, and paper between. */
    return foot + 2;
}

static struct cg_deck_inks deck_inks(void)
{
    struct cg_deck_inks in = { sol_palette[SOL_INK_PAPER], sol_palette[SOL_INK_EDGE],  sol_palette[SOL_INK_BLACK],
                               sol_palette[SOL_INK_RED],   sol_palette[SOL_INK_BACK], sol_palette[SOL_INK_BACK_LINE] };

    return in;
}

/* The card itself, drawn with shapes: what the deck cache composes once per
 * size, and what a first frame shows before it has (cg_deck.h). */
static void compose(lv_layer_t *layer, const lv_area_t *area, int id)
{
    int w = lv_area_get_width(area);
    struct cg_deck_inks in = deck_inks();
    struct corner c = corner_of(w);
    cg_card_t card = (cg_card_t)id;
    lv_color_t ink = cg_card_is_red(card) ? in.red : in.black;

    if (id == CG_DECK_ID_BACK) {
        cg_deck_back(layer, area, sol_card_radius(w), &in);
        return;
    }
    cg_deck_paper(layer, area, sol_card_radius(w), &in);
    text(layer, cg_rank_text(cg_card_rank(card)), c.role, ink, area->x1 + c.pad, area->y1 + c.text_y,
         area->x2 - c.pad, LV_TEXT_ALIGN_LEFT);
    cg_deck_suit(layer, area->x2 - c.pad - c.pip / 2, area->y1 + c.pip_y, c.pip, cg_card_suit(card), ink);
    /* Below the strip a fanned card shows. */
    cg_deck_body(layer, area, sol_card_index_h(w) + 2, 31, 31, card,
                 role_font(w >= 96 ? POS_STYLE_HERO_40 : POS_STYLE_TITLE, NULL), &in);
}

/* One set; its look changes with the fonts of the text size. */
#define SOL_DECK_SET 20

static uintptr_t deck_stamp(void)
{
    return (uintptr_t)role_font(POS_STYLE_TITLE, NULL) ^ ((uintptr_t)role_font(POS_STYLE_HERO_40, NULL) << 1) ^
           ((uintptr_t)role_font(POS_STYLE_ROW_TITLE, NULL) << 2);
}

void sol_draw_face(lv_layer_t *layer, const lv_area_t *area, cg_card_t card)
{
    if (cg_card_valid(card)) {
        cg_deck_draw(layer, area, SOL_DECK_SET, deck_stamp(), card, compose);
    }
}

void sol_draw_shadow(lv_layer_t *layer, const lv_area_t *area)
{
    cg_deck_shadow(layer, area, SOL_DECK_SET, deck_stamp(), compose);
}

void sol_draw_back(lv_layer_t *layer, const lv_area_t *area)
{
    cg_deck_draw(layer, area, SOL_DECK_SET, deck_stamp(), CG_DECK_ID_BACK, compose);
}

void sol_draw_slot(lv_layer_t *layer, const lv_area_t *area, enum sol_slot_mark mark, enum cg_suit suit)
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
