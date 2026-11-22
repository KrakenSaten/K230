/*
 * Card games: the Lamplight deck. See cg_deck.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_deck.h"

#include "cg_atlas.h"
#include "pocketui.h"

#include <string.h>

/* s percent of size, rounded. */
#define P(size, pct) (((size) * (pct) + 50) / 100)

/* ---- primitives ----------------------------------------------------------------- */

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

static void frame(lv_layer_t *layer, const lv_area_t *area, int radius, lv_color_t fill, lv_opa_t fill_opa,
                  lv_color_t line, int line_w, lv_opa_t line_opa)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = fill;
    dsc.bg_opa = fill_opa;
    dsc.border_color = line;
    dsc.border_width = line_w;
    dsc.border_opa = line_w ? line_opa : LV_OPA_TRANSP;
    lv_draw_rect(layer, &dsc, area);
}

static void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t c, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = c;
    dsc.opa = opa;
    dsc.width = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void cg_deck_suit(lv_layer_t *layer, int cx, int cy, int s, enum cg_suit suit, lv_color_t c)
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

/* ---- the face ------------------------------------------------------------------- */

void cg_deck_paper(lv_layer_t *layer, const lv_area_t *area, int radius, const struct cg_deck_inks *inks)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = inks->paper;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    dsc.bg_grad.stops_count = 2;
    dsc.bg_grad.stops[0].color = inks->paper;
    dsc.bg_grad.stops[0].opa = LV_OPA_COVER;
    dsc.bg_grad.stops[0].frac = 96;
    dsc.bg_grad.stops[1].color = lv_color_mix(inks->edge, inks->paper, 72);
    dsc.bg_grad.stops[1].opa = LV_OPA_COVER;
    dsc.bg_grad.stops[1].frac = 255;
    dsc.border_color = inks->edge;
    dsc.border_width = 1;
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, area);
}

/* Where the pips of a rank go: column 0 left, 1 centre, 2 right; row in
 * twelfths of the way down the pip field. */
struct pip {
    uint8_t col, row;
};

static int pips_of(int rank, const struct pip **out)
{
    static const struct pip p2[] = { { 1, 0 }, { 1, 12 } };
    static const struct pip p3[] = { { 1, 0 }, { 1, 6 }, { 1, 12 } };
    static const struct pip p4[] = { { 0, 0 }, { 2, 0 }, { 0, 12 }, { 2, 12 } };
    static const struct pip p5[] = { { 0, 0 }, { 2, 0 }, { 1, 6 }, { 0, 12 }, { 2, 12 } };
    static const struct pip p6[] = { { 0, 0 }, { 2, 0 }, { 0, 6 }, { 2, 6 }, { 0, 12 }, { 2, 12 } };
    static const struct pip p7[] = { { 0, 0 }, { 2, 0 }, { 1, 3 }, { 0, 6 }, { 2, 6 }, { 0, 12 }, { 2, 12 } };
    static const struct pip p8[] = { { 0, 0 }, { 2, 0 }, { 1, 3 }, { 0, 6 }, { 2, 6 }, { 1, 9 }, { 0, 12 }, { 2, 12 } };
    static const struct pip p9[] = { { 0, 0 }, { 2, 0 }, { 0, 4 }, { 2, 4 }, { 1, 6 }, { 0, 8 }, { 2, 8 }, { 0, 12 },
                                     { 2, 12 } };
    static const struct pip p10[] = { { 0, 0 }, { 2, 0 }, { 1, 2 }, { 0, 4 }, { 2, 4 }, { 0, 8 }, { 2, 8 }, { 1, 10 },
                                      { 0, 12 }, { 2, 12 } };
    static const struct pip *const all[] = { p2, p3, p4, p5, p6, p7, p8, p9, p10 };

    if (rank < 2 || rank > 10) {
        return 0;
    }
    *out = all[rank - 2];
    return rank;
}

/* ---- the court ------------------------------------------------------------------ */

/* A court figure is drawn in thousandths of its box: u across, v down one
 * half, from the box's outer edge (v 0) to its centre line (v 1000). The
 * lower half of a double-ended card is the same figure turned half round. */
struct figure {
    lv_layer_t *layer;
    int x1, y1, w, h; /* the whole box */
    int half;         /* the height one figure stands in */
    int turned;       /* drawn turned half round, from the box's foot */
};

static int fu(const struct figure *f, int u)
{
    return f->turned ? f->x1 + f->w - 1 - u * f->w / 1000 : f->x1 + u * f->w / 1000;
}

static int fv(const struct figure *f, int v)
{
    return f->turned ? f->y1 + f->h - 1 - v * f->half / 1000 : f->y1 + v * f->half / 1000;
}

static void f_tri(const struct figure *f, int u0, int v0, int u1, int v1, int u2, int v2, lv_color_t c)
{
    triangle(f->layer, fu(f, u0), fv(f, v0), fu(f, u1), fv(f, v1), fu(f, u2), fv(f, v2), c);
}

static void f_box(const struct figure *f, int u0, int v0, int u1, int v1, int radius, lv_color_t fill,
                  lv_color_t edge, int edge_w)
{
    int ax = fu(f, u0), bx = fu(f, u1), ay = fv(f, v0), by = fv(f, v1);
    lv_area_t a = { LV_MIN(ax, bx), LV_MIN(ay, by), LV_MAX(ax, bx), LV_MAX(ay, by) };

    frame(f->layer, &a, radius, fill, LV_OPA_COVER, edge, edge_w, LV_OPA_COVER);
}

/* A disc of radius r thousandths of the box's width. */
static void f_disc(const struct figure *f, int u, int v, int r, lv_color_t fill, lv_color_t edge, int edge_w)
{
    int cx = fu(f, u), cy = fv(f, v), rr = r * f->w / 1000;
    lv_area_t a = { cx - rr, cy - rr, cx + rr, cy + rr };

    if (rr < 1) {
        return;
    }
    frame(f->layer, &a, LV_RADIUS_CIRCLE, fill, LV_OPA_COVER, edge, edge_w, LV_OPA_COVER);
}

static void f_line(const struct figure *f, int u0, int v0, int u1, int v1, int width, lv_color_t c)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = c;
    dsc.width = width < 1 ? 1 : width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = fu(f, u0);
    dsc.p1.y = fv(f, v0);
    dsc.p2.x = fu(f, u1);
    dsc.p2.y = fv(f, v1);
    lv_draw_line(f->layer, &dsc);
}

/* The court's inks: two robes (the suit's colour and its counterpart), gold
 * trim, the face, and ink for the lines. Fixed, as the rest of the deck. */
struct court_inks {
    lv_color_t robe, panel, gold, face, ink, paper, hair;
};

/* One court figure, head and shoulders, standing in one half of its box. */
static void court_figure(const struct figure *f, int rank, enum cg_suit suit, const struct court_inks *k)
{
    int bw = f->w >= 60 ? 2 : 1; /* a line's width at this size */
    lv_color_t ink = k->ink;

    /* Behind: the staff, the flower or the sceptre each holds. */
    if (rank == CG_JACK) {
        f_line(f, 150, 1000, 200, 330, bw + 1, k->gold);
        f_tri(f, 200, 150, 260, 360, 140, 360, k->gold);
    } else if (rank == CG_QUEEN) {
        f_line(f, 190, 1000, 170, 540, bw, k->gold);
        f_disc(f, 120, 470, 62, k->panel, ink, 1);
        f_disc(f, 220, 470, 62, k->panel, ink, 1);
        f_disc(f, 170, 400, 62, k->panel, ink, 1);
        f_disc(f, 170, 470, 40, k->gold, k->gold, 0);
    } else {
        f_line(f, 840, 1000, 860, 330, bw + 1, k->gold);
        f_disc(f, 860, 300, 60, k->gold, ink, 1);
        f_line(f, 820, 200, 900, 200, bw, k->gold);
        f_line(f, 860, 150, 860, 250, bw, k->gold);
    }
    /* The shoulders and the robe, square to the centre line, with a panel
     * down the front edged in gold. */
    f_box(f, 300, 600, 700, 1000, 0, k->robe, k->robe, 0);
    f_tri(f, 300, 600, 300, 760, 70, 760, k->robe);
    f_tri(f, 700, 600, 700, 760, 930, 760, k->robe);
    f_box(f, 70, 760, 930, 1000, 0, k->robe, k->robe, 0);
    f_box(f, 410, 640, 590, 1000, 0, k->panel, k->panel, 0);
    f_line(f, 410, 640, 410, 1000, bw, k->gold);
    f_line(f, 590, 640, 590, 1000, bw, k->gold);
    f_line(f, 70, 760, 300, 600, bw, k->gold);
    f_line(f, 930, 760, 700, 600, bw, k->gold);
    /* The hair behind the head: a bob, long hair, or the king's grey. */
    if (rank == CG_JACK) {
        f_box(f, 290, 250, 710, 580, LV_RADIUS_CIRCLE, k->hair, k->hair, 0);
    } else if (rank == CG_QUEEN) {
        f_box(f, 260, 250, 740, 800, LV_RADIUS_CIRCLE, k->hair, k->hair, 0);
    } else {
        f_box(f, 300, 250, 700, 500, LV_RADIUS_CIRCLE, k->hair, k->hair, 0);
    }
    /* The face. */
    f_disc(f, 500, 400, 190, k->face, ink, 1);
    if (f->w >= 40) {
        int eye = f->w >= 80 ? 24 : 32;

        f_disc(f, 430, 380, eye, ink, ink, 0);
        f_disc(f, 570, 380, eye, ink, ink, 0);
        f_line(f, 460, 500, 540, 500, f->w >= 80 ? 2 : 1, k->panel);
    }
    /* The collar, and the king's beard over the chin. */
    if (rank == CG_JACK) {
        f_box(f, 380, 580, 620, 650, 2, k->gold, ink, 1);
    } else if (rank == CG_QUEEN) {
        /* A lace collar falling in a V, a pendant at its point. */
        f_tri(f, 340, 610, 660, 610, 500, 760, k->paper);
        f_line(f, 340, 610, 500, 760, 1, k->gold);
        f_line(f, 660, 610, 500, 760, 1, k->gold);
        f_disc(f, 500, 735, 40, k->robe, k->gold, 1);
    } else {
        f_tri(f, 330, 470, 670, 470, 500, 740, k->hair);
        f_disc(f, 500, 520, 150, k->hair, k->hair, 0);
        f_disc(f, 430, 380, f->w >= 80 ? 24 : 32, ink, ink, 0);
        f_disc(f, 570, 380, f->w >= 80 ? 24 : 32, ink, ink, 0);
        f_box(f, 330, 610, 670, 680, 2, k->paper, ink, 1);
        if (f->w >= 40) {
            f_line(f, 400, 645, 400, 645, 2, ink);
            f_line(f, 500, 645, 500, 645, 2, ink);
            f_line(f, 600, 645, 600, 645, 2, ink);
        }
    }
    /* The headwear: a cap with a feather, a tiara, a crown. */
    if (rank == CG_JACK) {
        f_box(f, 300, 190, 700, 300, LV_RADIUS_CIRCLE, k->panel, ink, 1);
        f_tri(f, 640, 230, 930, 40, 820, 250, k->gold);
        f_line(f, 300, 300, 700, 300, bw, k->gold);
    } else if (rank == CG_QUEEN) {
        f_tri(f, 360, 270, 420, 270, 390, 150, k->gold);
        f_tri(f, 460, 270, 540, 270, 500, 110, k->gold);
        f_tri(f, 580, 270, 640, 270, 610, 150, k->gold);
        f_box(f, 360, 240, 640, 290, 2, k->gold, ink, 1);
        f_disc(f, 500, 265, 34, k->robe, k->robe, 0);
    } else {
        f_tri(f, 330, 230, 430, 230, 360, 40, k->gold);
        f_tri(f, 440, 230, 560, 230, 500, 10, k->gold);
        f_tri(f, 570, 230, 670, 230, 640, 40, k->gold);
        f_box(f, 330, 200, 670, 290, 2, k->gold, ink, 1);
        f_disc(f, 500, 245, 36, k->robe, k->robe, 0);
        if (f->w >= 60) {
            f_disc(f, 360, 50, 26, k->panel, k->panel, 0);
            f_disc(f, 640, 50, 26, k->panel, k->panel, 0);
        }
    }
    /* The suit on the breast, in the paper's colour. */
    {
        int s = 190 * f->w / 1000;

        if (s >= 6) {
            cg_deck_suit(f->layer, fu(f, 500), fv(f, 840), s, suit, k->paper);
        }
    }
}

static struct court_inks court_inks(cg_card_t card, const struct cg_deck_inks *inks)
{
    static const uint32_t blue = 0x2c4370;
    struct court_inks k;
    int red = cg_card_is_red(card);
    int rank = cg_card_rank(card);

    k.robe = red ? inks->red : lv_color_hex(blue);
    k.panel = red ? lv_color_hex(blue) : inks->red;
    k.gold = lv_color_hex(0xc9a24e);
    k.face = lv_color_hex(0xf3e2c2);
    k.ink = inks->black;
    k.paper = inks->paper;
    k.hair = rank == CG_KING ? lv_color_hex(0xcfc7b6) : rank == CG_QUEEN ? lv_color_hex(0x8a4a22)
                                                                          : lv_color_hex(0x3b2a1c);
    return k;
}

void cg_deck_court(lv_layer_t *layer, const lv_area_t *box, cg_card_t card, int double_ended,
                   const struct cg_deck_inks *inks)
{
    struct court_inks k = court_inks(card, inks);
    struct figure f;
    int rank = cg_card_rank(card);

    if (!layer || !box || rank < CG_JACK || lv_area_get_width(box) < 16 || lv_area_get_height(box) < 16) {
        return;
    }
    f.layer = layer;
    f.x1 = box->x1;
    f.y1 = box->y1;
    f.w = lv_area_get_width(box);
    f.h = lv_area_get_height(box);
    f.half = double_ended ? f.h / 2 : f.h;
    f.turned = 0;
    court_figure(&f, rank, cg_card_suit(card), &k);
    if (double_ended) {
        f.turned = 1;
        court_figure(&f, rank, cg_card_suit(card), &k);
        line(layer, f.x1 + 2, f.y1 + f.half, f.x1 + f.w - 3, f.y1 + f.half, k.gold, LV_OPA_COVER);
    }
}

void cg_deck_body(lv_layer_t *layer, const lv_area_t *area, int top, int left_pct, int right_pct, cg_card_t card,
                  const lv_font_t *court_font, const struct cg_deck_inks *inks)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    int rank = cg_card_rank(card);
    enum cg_suit suit = cg_card_suit(card);
    lv_color_t ink = cg_card_is_red(card) ? inks->red : inks->black;
    int foot = P(h, 7);
    int y1 = area->y1 + top;
    int y2 = area->y2 - foot;
    int cx = area->x1 + w / 2;
    int corner = w / 7;
    const struct pip *pip = NULL;
    int n = pips_of(rank, &pip);
    int i;

    if (!cg_card_valid(card) || y2 - y1 < 12) {
        return;
    }
    if (w < 96) {
        /* A small card: one pip, or the court's head and shoulders, or the
         * letter where even that has no room. */
        int cy = (y1 + y2) / 2;

        if (rank >= CG_JACK && y2 - y1 >= 24 && w >= 32) {
            int fw = P(w, 74);
            int fh = y2 - y1;
            lv_area_t box;

            if (fh > fw * 13 / 10) {
                fh = fw * 13 / 10;
            }
            box.x1 = cx - fw / 2;
            box.x2 = box.x1 + fw - 1;
            box.y1 = cy - fh / 2;
            box.y2 = box.y1 + fh - 1;
            cg_deck_court(layer, &box, card, 0, inks);
        } else if (rank >= CG_JACK && court_font) {
            lv_draw_label_dsc_t dsc;
            int lh = lv_font_get_line_height(court_font);
            lv_area_t a = { area->x1, cy - lh / 2, area->x2, cy - lh / 2 + lh - 1 };

            lv_draw_label_dsc_init(&dsc);
            dsc.font = court_font;
            dsc.color = ink;
            dsc.align = LV_TEXT_ALIGN_CENTER;
            dsc.text = cg_rank_text(rank);
            lv_draw_label(layer, &dsc, &a);
        } else {
            int s = rank == CG_ACE ? P(w, 58) : P(w, 44);

            cg_deck_suit(layer, cx, cy, s > y2 - y1 ? y2 - y1 : s, suit, ink);
        }
        return;
    }
    if (rank >= CG_JACK) {
        /* The court: a double-ended figure in a ruled frame on a warm
         * ground; the suit is on each figure's breast. */
        lv_area_t in = { area->x1 + P(w, left_pct - 11), y1, area->x2 - P(w, right_pct - 11), y2 };
        lv_area_t fig = { in.x1 + 3, in.y1 + 3, in.x2 - 3, in.y2 - 3 };

        frame(layer, &in, 3, lv_color_mix(lv_color_hex(0xe9d9b0), inks->paper, 110), LV_OPA_COVER,
              inks->back_line, 2, LV_OPA_COVER);
        cg_deck_court(layer, &fig, card, 1, inks);
        frame(layer, &in, 3, inks->paper, LV_OPA_TRANSP, inks->black, 1, LV_OPA_40);
        (void)court_font;
        return;
    }
    if (rank == CG_ACE) {
        int s = P(w, 54);

        cg_deck_suit(layer, cx, (y1 + y2) / 2, s > y2 - y1 ? y2 - y1 : s, suit, ink);
    } else {
        /* Rows are twelfths of the field; the closest two of a rank are two
         * twelfths apart (the ten), three for seven and eight, four for
         * nine, six otherwise. The pip is as large as that leaves room for. */
        int step = rank == 10 ? 2 : rank >= 9 ? 4 : rank >= 7 ? 3 : 6;
        int field = y2 - y1;
        int size = P(w, 21);
        int room = field * step / (12 + step);
        int left = area->x1 + P(w, left_pct);
        int right = area->x2 - P(w, right_pct);

        if (size > room - 1) {
            size = room - 1;
        }
        for (i = 0; i < n; i++) {
            int x = pip[i].col == 0 ? left : pip[i].col == 2 ? right : (left + right) / 2;
            int y = y1 + size / 2 + (field - size) * pip[i].row / 12;

            cg_deck_suit(layer, x, y, size, suit, ink);
        }
    }
    if (rank <= 3) {
        cg_deck_suit(layer, area->x2 - P(w, 7) - corner / 2, area->y2 - P(w, 7) - corner / 2, corner, suit, ink);
    }
}

void cg_deck_back(lv_layer_t *layer, const lv_area_t *area, int radius, const struct cg_deck_inks *inks)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    int inset = w / 14 < 3 ? 3 : w / 14;
    lv_area_t field = { area->x1 + inset, area->y1 + inset, area->x2 - inset, area->y2 - inset };
    lv_area_t rule = { field.x1 + 3, field.y1 + 3, field.x2 - 3, field.y2 - 3 };
    int step = w / 9 < 6 ? 6 : w / 9;
    int cx = area->x1 + w / 2;
    int cy = area->y1 + h / 2;
    int c;

    /* An ivory border round a dark field. */
    frame(layer, area, radius, inks->paper, LV_OPA_COVER, inks->edge, 1, LV_OPA_COVER);
    frame(layer, &field, radius > 3 ? radius - 3 : 1, inks->back, LV_OPA_COVER, inks->back, 0, LV_OPA_TRANSP);
    if (w >= 40) {
        /* A lattice inside the rule: lines both ways across the field. */
        int x1 = rule.x1 + 2, y1 = rule.y1 + 2, x2 = rule.x2 - 2, y2 = rule.y2 - 2;

        for (c = x1 - y2; c <= x2 - y1; c += step) {
            int ax = c + y1 < x1 ? x1 : c + y1;
            int bx = c + y2 > x2 ? x2 : c + y2;

            line(layer, ax, ax - c, bx, bx - c, inks->back_line, LV_OPA_30);
        }
        for (c = x1 + y1; c <= x2 + y2; c += step) {
            int ax = c - y1 > x2 ? x2 : c - y1;
            int bx = c - y2 < x1 ? x1 : c - y2;

            line(layer, ax, c - ax, bx, c - bx, inks->back_line, LV_OPA_30);
        }
    }
    frame(layer, &rule, 2, inks->back, LV_OPA_TRANSP, inks->back_line, 1, LV_OPA_80);
    {
        /* The medallion: a door under an arch. */
        int r = P(w, 19);
        int dw = P(w, 15);
        int dh = P(w, 22);
        lv_area_t ring = { cx - r, cy - r, cx + r, cy + r };
        lv_area_t door = { cx - dw / 2, cy - dh / 2, cx - dw / 2 + dw - 1, cy - dh / 2 + dh - 1 };
        lv_area_t sill = { door.x1 - 2, door.y2 - 1, door.x2 + 2, door.y2 };

        frame(layer, &ring, LV_RADIUS_CIRCLE, inks->back, LV_OPA_COVER, inks->back_line, w >= 60 ? 2 : 1, LV_OPA_COVER);
        if (w >= 48) {
            frame(layer, &door, dw / 2, inks->back_line, LV_OPA_COVER, inks->back_line, 0, LV_OPA_TRANSP);
            frame(layer, &sill, 0, inks->back_line, LV_OPA_COVER, inks->back_line, 0, LV_OPA_TRANSP);
            disc(layer, door.x2 - dw / 4, cy + dh / 8, w >= 96 ? 2 : 1, inks->back);
        }
    }
}

/* ---- the type ------------------------------------------------------------------- */

const lv_font_t *cg_deck_font(enum cg_deck_type type)
{
    struct pos_type_spec spec;

    switch (type) {
    case CG_TYPE_INDEX:
        spec = (struct pos_type_spec){ POS_FACE_SANS_SEMIBOLD, 28 };
        break;
    case CG_TYPE_LARGE:
        spec = (struct pos_type_spec){ POS_FACE_SANS_SEMIBOLD, 40 };
        break;
    case CG_TYPE_HEADLINE:
        spec = (struct pos_type_spec){ POS_FACE_SERIF_ITALIC, 48 };
        break;
    case CG_TYPE_HEADLINE_SMALL:
        spec = (struct pos_type_spec){ POS_FACE_SERIF_ITALIC, 32 };
        break;
    case CG_TYPE_INDEX_SMALL:
    default:
        spec = (struct pos_type_spec){ POS_FACE_SANS, 24 };
        break;
    }
    return pos_type_font(spec);
}

/* ---- the cache ------------------------------------------------------------------ */

#define SETS 6

struct entry {
    int used, building;
    int set, w, h;
    uintptr_t stamp;
    cg_deck_compose_cb compose;
    struct cg_atlas atlas;
    lv_image_dsc_t shadow;
    int margin, drop;
    uint32_t last;
};

static struct entry cache[SETS];
static uint32_t clock_now;
static lv_opa_t dim_opa;

/* Everything the composed sets hold now, and the most they may hold
 * together: more than one game's largest decks (Hold'em's three sizes, about
 * 5.1 MB), so a game never evicts its own sets while it is open. Each app
 * purges on close (cg_deck_purge), so nothing stays behind it. */
#define BUDGET_BYTES ((size_t)8 * 1024 * 1024)

static size_t held_bytes(void)
{
    size_t sum = 0;
    int i;

    for (i = 0; i < SETS; i++) {
        sum += cache[i].atlas.bytes;
        if (cache[i].shadow.data) {
            sum += cache[i].shadow.data_size;
        }
    }
    return sum;
}

static void compose_one(lv_layer_t *layer, const lv_area_t *area, int index, void *user)
{
    struct entry *e = user;

    e->compose(layer, area, index);
}

static void drop_entry(struct entry *e)
{
    cg_atlas_free(&e->atlas);
    cg_shadow_free(&e->shadow);
    memset(e, 0, sizeof(*e));
}

static void build(void *user)
{
    struct entry *e = user;
    int radius = e->w / 12 < 3 ? 3 : e->w / 12;

    e->building = 0;
    if (!e->used) {
        return;
    }
    /* Over the budget with this one: the others, least recently drawn first,
     * make room; if that is not enough the set keeps its direct path. */
    while (held_bytes() + (size_t)e->w * e->h * 3 * CG_DECK_IDS > BUDGET_BYTES) {
        struct entry *victim = NULL;
        int i;

        for (i = 0; i < SETS; i++) {
            struct entry *o = &cache[i];

            if (o != e && o->used && !o->building && o->atlas.count && (!victim || o->last < victim->last)) {
                victim = o;
            }
        }
        if (!victim) {
            e->atlas.count = 0;
            return;
        }
        drop_entry(victim);
    }
    if (cg_atlas_build(&e->atlas, lv_layer_sys(), e->w, e->h, CG_DECK_IDS, compose_one, e) != 0) {
        /* No memory for it: the direct path keeps drawing. */
        e->atlas.count = 0;
        return;
    }
    e->margin = e->w / 9 < 4 ? 4 : e->w / 9;
    e->drop = e->w / 22 < 2 ? 2 : e->w / 22;
    /* cg_deck_shadow_reach says the same. */
    if (cg_shadow_build(&e->shadow, e->w, e->h, radius, e->margin, 110) != 0) {
        memset(&e->shadow, 0, sizeof(e->shadow));
    }
    lv_obj_invalidate(lv_screen_active());
}

static struct entry *find(int set, uintptr_t stamp, int w, int h, cg_deck_compose_cb compose)
{
    struct entry *oldest = NULL;
    int i;

    for (i = 0; i < SETS; i++) {
        struct entry *e = &cache[i];

        if (e->used && e->set == set && e->stamp == stamp && e->w == w && e->h == h && e->compose == compose) {
            e->last = ++clock_now;
            return e;
        }
    }
    for (i = 0; i < SETS; i++) {
        struct entry *e = &cache[i];

        if (!e->used) {
            oldest = e;
            break;
        }
        if (e->building) {
            continue;
        }
        /* A set of the same game at an old look or size goes first; among
         * equals, the one unused longest. */
        if (!oldest || ((e->set == set) && (oldest->set != set)) ||
            (((e->set == set) == (oldest->set == set)) && e->last < oldest->last)) {
            oldest = e;
        }
    }
    if (!oldest) {
        return NULL;
    }
    drop_entry(oldest);
    oldest->used = 1;
    oldest->building = 1;
    oldest->set = set;
    oldest->stamp = stamp;
    oldest->w = w;
    oldest->h = h;
    oldest->compose = compose;
    oldest->last = ++clock_now;
    if (lv_async_call(build, oldest) != LV_RESULT_OK) {
        oldest->building = 0;
    }
    return oldest;
}

/* `scale_x` in 256ths: a card turning over is squashed toward its centre
 * line (the device does this in software; it is one card at a time). */
static void blit(lv_layer_t *layer, const lv_image_dsc_t *src, int x, int y, int black, int scale_x)
{
    lv_draw_image_dsc_t dsc;
    lv_area_t area = { x, y, x + (int)src->header.w - 1, y + (int)src->header.h - 1 };

    lv_draw_image_dsc_init(&dsc);
    dsc.src = src;
    if (black) {
        dsc.recolor = lv_color_black();
        dsc.recolor_opa = LV_OPA_COVER;
    } else if (dim_opa) {
        dsc.recolor = lv_color_black();
        dsc.recolor_opa = dim_opa;
    }
    if (scale_x < LV_SCALE_NONE) {
        dsc.scale_x = (uint32_t)scale_x;
        dsc.scale_y = LV_SCALE_NONE;
        dsc.pivot.x = (int32_t)src->header.w / 2;
        dsc.pivot.y = (int32_t)src->header.h / 2;
    }
    lv_draw_image(layer, &dsc, &area);
}

void cg_deck_draw_scaled(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, int id,
                         cg_deck_compose_cb compose, int scale_x)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    struct entry *e;

    if (!layer || !compose || id < 0 || id >= CG_DECK_IDS || w < 8 || h < 8 || scale_x <= 0) {
        return;
    }
    e = find(set, stamp, w, h, compose);
    if (!e || !e->atlas.count) {
        /* Not composed (yet, or no memory for it): drawn directly, full
         * width, and dimmed at night like the blit. */
        compose(layer, area, id);
        if (dim_opa) {
            frame(layer, area, w / 12 < 3 ? 3 : w / 12, lv_color_black(), dim_opa, lv_color_black(), 0,
                  LV_OPA_TRANSP);
        }
        return;
    }
    blit(layer, cg_atlas_image(&e->atlas, id), area->x1, area->y1, 0,
         scale_x > LV_SCALE_NONE ? LV_SCALE_NONE : scale_x);
}

void cg_deck_draw(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, int id,
                  cg_deck_compose_cb compose)
{
    cg_deck_draw_scaled(layer, area, set, stamp, id, compose, LV_SCALE_NONE);
}

void cg_deck_set_dim(lv_opa_t opa)
{
    dim_opa = opa;
}

size_t cg_deck_bytes(void)
{
    return held_bytes();
}

void cg_deck_shadow(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, cg_deck_compose_cb compose)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    struct entry *e;

    if (!layer || !compose || w < 8 || h < 8) {
        return;
    }
    e = find(set, stamp, w, h, compose);
    if (e && e->atlas.count && e->shadow.data) {
        blit(layer, &e->shadow, area->x1 - e->margin, area->y1 - e->margin + e->drop, 1, LV_SCALE_NONE);
    }
}

int cg_deck_shadow_reach(int card_w)
{
    return (card_w / 9 < 4 ? 4 : card_w / 9) + (card_w / 22 < 2 ? 2 : card_w / 22);
}

void cg_deck_purge(void)
{
    int i;

    for (i = 0; i < SETS; i++) {
        if (cache[i].building) {
            lv_async_call_cancel(build, &cache[i]);
        }
        drop_entry(&cache[i]);
    }
}
