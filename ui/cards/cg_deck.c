/*
 * Card games: the Lamplight deck. See cg_deck.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_deck.h"

#include "cg_atlas.h"

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
        /* A small card: one pip, or the letter. */
        int cy = (y1 + y2) / 2;

        if (rank >= CG_JACK && court_font) {
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
        /* The court: a tinted panel inside a gold line, the letter large,
         * the suit at two of its corners. */
        lv_area_t in = { area->x1 + P(w, left_pct - 11), y1, area->x2 - P(w, right_pct - 11), y2 };
        int s = P(w, 15);

        frame(layer, &in, 3, lv_color_mix(ink, inks->paper, 18), LV_OPA_COVER, inks->back_line, 2, LV_OPA_COVER);
        cg_deck_suit(layer, in.x1 + 4 + s / 2, in.y1 + 5 + s / 2, s, suit, ink);
        cg_deck_suit(layer, in.x2 - 4 - s / 2, in.y2 - 5 - s / 2, s, suit, ink);
        if (court_font) {
            lv_draw_label_dsc_t dsc;
            int lh = lv_font_get_line_height(court_font);
            int cy = (in.y1 + in.y2) / 2;
            lv_area_t a = { in.x1, cy - lh / 2, in.x2, cy - lh / 2 + lh - 1 };

            lv_draw_label_dsc_init(&dsc);
            dsc.font = court_font;
            dsc.color = ink;
            dsc.align = LV_TEXT_ALIGN_CENTER;
            dsc.text = cg_rank_text(rank);
            lv_draw_label(layer, &dsc, &a);
        }
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
    if (cg_atlas_build(&e->atlas, lv_layer_sys(), e->w, e->h, CG_DECK_IDS, compose_one, e) != 0) {
        /* No memory for it: the direct path keeps drawing. */
        e->atlas.count = 0;
        return;
    }
    e->margin = e->w / 9 < 4 ? 4 : e->w / 9;
    e->drop = e->w / 22 < 2 ? 2 : e->w / 22;
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

static void blit(lv_layer_t *layer, const lv_image_dsc_t *src, int x, int y, int black)
{
    lv_draw_image_dsc_t dsc;
    lv_area_t area = { x, y, x + (int)src->header.w - 1, y + (int)src->header.h - 1 };

    lv_draw_image_dsc_init(&dsc);
    dsc.src = src;
    if (black) {
        dsc.recolor = lv_color_black();
        dsc.recolor_opa = LV_OPA_COVER;
    }
    lv_draw_image(layer, &dsc, &area);
}

void cg_deck_draw(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, int id,
                  cg_deck_compose_cb compose)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    struct entry *e;

    if (!layer || !compose || id < 0 || id >= CG_DECK_IDS || w < 8 || h < 8) {
        return;
    }
    e = find(set, stamp, w, h, compose);
    if (!e || !e->atlas.count) {
        compose(layer, area, id);
        return;
    }
    blit(layer, cg_atlas_image(&e->atlas, id), area->x1, area->y1, 0);
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
        blit(layer, &e->shadow, area->x1 - e->margin, area->y1 - e->margin + e->drop, 1);
    }
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
