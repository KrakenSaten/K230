/*
 * PG Blackjack table widget. See bj_table_widget.h.
 *
 * The cloth is the scene's (cg_scene.h), under the whole screen; this
 * widget draws what lies on it: the hands, the stake as chips, the labels
 * and a settled round's result, and their motion (cg_motion.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bj_table_widget.h"

#include "bj_card_draw.h"
#include "cg_deck.h"
#include "cg_motion.h"
#include "cg_table.h"
#include "pocketui.h"

/* Keys for the objects that move: a card is its epoch, row, place and byte;
 * the chips and the result have keys of their own. */
#define KEY_CHIPS 0xffffff01u
#define KEY_RESULT 0xffffff02u

struct bj_table_state {
    const struct bj_game *game;
    struct cg_motion motion;
    struct cg_spots before;
    int before_phase;
    uint32_t before_stake;
    uint8_t epoch; /* a new round's cards are new cards, whatever their bytes */
};

static struct bj_table_state *state_of(lv_obj_t *obj)
{
    return obj ? lv_obj_get_user_data(obj) : NULL;
}

static lv_area_t area_of(const lv_area_t *origin, struct bj_rect r)
{
    lv_area_t a = { origin->x1 + r.x, origin->y1 + r.y, origin->x1 + r.x + r.w - 1, origin->y1 + r.y + r.h - 1 };

    return a;
}

static lv_area_t moved(const lv_area_t *origin, const lv_area_t *rel)
{
    lv_area_t a = { origin->x1 + rel->x1, origin->y1 + rel->y1, origin->x1 + rel->x2, origin->y1 + rel->y2 };

    return a;
}

/* The table is a Lamplight scene with a palette of its own in every theme
 * (cg_table.h): the roles the view asks for, in its inks. */
static lv_color_t table_ink(enum pos_color_token color)
{
    switch (color) {
    case POS_COLOR_STATUS_OK:
    case POS_COLOR_ACCENT_PRIMARY:
        return cg_table_ink(CG_INK_GOLD);
    case POS_COLOR_TEXT_SECONDARY:
        return cg_table_ink(CG_INK_DIM);
    default:
        return cg_table_ink(CG_INK_IVORY);
    }
}

static uint32_t card_key(const struct bj_table_state *st, enum bj_row row, int i, cg_card_t card)
{
    return ((uint32_t)st->epoch << 24) | ((uint32_t)row << 16) | ((uint32_t)i << 8) | card;
}

static void draw_hand(lv_layer_t *layer, const lv_area_t *origin, const struct bj_table *t,
                      const struct bj_table_state *st, enum bj_row row)
{
    const struct bj_game *g = st->game;
    const struct bj_hand *h = row == BJ_ROW_DEALER ? &g->dealer : &g->player;
    int i;

    if (h->n == 0) {
        /* Before the first round: two outlines where the first two cards will
         * go, side by side rather than overlapping. */
        int gap = 12;
        int x0 = (t->w - 2 * t->card_w - gap) / 2;

        for (i = 0; i < 2; i++) {
            struct bj_rect r = { x0 + i * (t->card_w + gap), t->cards_y[row], t->card_w, t->card_h };
            lv_area_t a = area_of(origin, r);

            bj_draw_slot(layer, &a, BJ_SLOT_PLAIN, CG_SPADES);
        }
        return;
    }
    /* The cards at rest; a card in motion is drawn last, over everything. */
    for (i = 0; i < h->n; i++) {
        lv_area_t a = area_of(origin, bj_view_card(t, row, h->n, i));

        if (cg_motion_pose(&st->motion, card_key(st, row, i, h->card[i]), NULL) < 0) {
            bj_draw_shadow(layer, &a);
        }
    }
    for (i = 0; i < h->n; i++) {
        lv_area_t a = area_of(origin, bj_view_card(t, row, h->n, i));

        if (cg_motion_pose(&st->motion, card_key(st, row, i, h->card[i]), NULL) >= 0) {
            continue;
        }
        if (row == BJ_ROW_DEALER && bj_view_face_down(g, i)) {
            bj_draw_back(layer, &a);
        } else {
            bj_draw_face(layer, &a, h->card[i], BJ_INDEX_BELOW);
        }
    }
}

static void draw_moving(lv_layer_t *layer, const lv_area_t *origin, const struct bj_table_state *st)
{
    int i;

    for (i = 0; i < cg_motion_count(&st->motion); i++) {
        uint32_t key = cg_motion_key(&st->motion, i);
        struct cg_pose p;
        lv_area_t a;

        if (key >= KEY_CHIPS || cg_motion_pose(&st->motion, key, &p) != 1) {
            continue;
        }
        a = moved(origin, &p.rect);
        if (p.scale_x >= 256) {
            bj_draw_shadow(layer, &a);
        }
        bj_draw_card_scaled(layer, &a, (cg_card_t)(key & 0xff), p.face, p.scale_x);
    }
}

/* Text in a DS role's font and tracking, centred across `r` and vertically
 * in it. Left out when the role has no font or the text is taller than r. */
static void draw_text(lv_layer_t *layer, const lv_area_t *origin, struct bj_rect r, enum pos_style_role role,
                      const char *text, enum pos_color_token color, lv_opa_t opa)
{
    lv_draw_label_dsc_t dsc;
    lv_style_value_t v;
    lv_area_t a;
    int line_h;

    lv_draw_label_dsc_init(&dsc);
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND) {
        dsc.font = v.ptr;
    }
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_LETTER_SPACE, &v) == LV_STYLE_RES_FOUND) {
        dsc.letter_space = v.num;
    }
    if (!dsc.font || !text[0]) {
        return;
    }
    line_h = lv_font_get_line_height(dsc.font);
    if (line_h > r.h) {
        return;
    }
    dsc.color = table_ink(color);
    dsc.opa = opa;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.text = text;
    dsc.text_local = 1;
    a.x1 = origin->x1 + r.x;
    a.x2 = origin->x1 + r.x + r.w - 1;
    a.y1 = origin->y1 + r.y + (r.h - line_h) / 2;
    a.y2 = a.y1 + line_h - 1;
    lv_draw_label(layer, &dsc, &a);
}

static void draw_label(lv_layer_t *layer, const lv_area_t *origin, const struct bj_table *t, enum bj_row row,
                       int n, const char *text, enum pos_color_token color)
{
    struct bj_rect r = { BJ_TABLE_PAD, bj_view_label_y(t, row, n), t->w - 2 * BJ_TABLE_PAD, BJ_LABEL_H };

    draw_text(layer, origin, r, POS_STYLE_BUTTON_LABEL, text, color, LV_OPA_COVER);
}

/* The view's table for the object's size, with the corner index the cards
 * show in their fixed type. */
static void geometry(lv_obj_t *obj, struct bj_table *t)
{
    bj_view_table(lv_obj_get_width(obj), lv_obj_get_height(obj), t);
    bj_card_index_size(t->card_w, &t->index_w, &t->index_h);
}

/* A chip's width for the open felt, 0 when there is no room for chips. */
static int chip_d(struct bj_rect r)
{
    int d = r.h >= 96 ? 44 : r.h >= 64 ? 34 : 0;

    return d && r.w >= 4 * d ? d : 0;
}

/* Where the stake's chips stand: centred in the open felt, as the box the
 * tallest, widest stake could cover, its foot on their base (what the motion
 * moves and repaints). */
static lv_area_t chips_spot(struct bj_rect r, int d)
{
    int cx = r.x + r.w / 2;
    int base = r.y + r.h / 2 + d / 2 + d / 4;
    int half, high;

    cg_table_chips_reach(d, &half, &high);
    return (lv_area_t){ cx - half, base - high, cx + half, base };
}

static void table_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct bj_table_state *st = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    const struct bj_game *g;
    struct bj_table t;
    struct bj_rect open;
    struct cg_pose p;
    lv_area_t coords;
    char text[48];
    int result, d, shown;

    if (!st || !st->game || !layer) {
        return;
    }
    g = st->game;
    lv_obj_get_coords(obj, &coords);
    geometry(obj, &t);
    /* Night dims the world - cloth, cards, chips - as in PocketTimber's art
     * review; the labels are interface and are not dimmed. */
    cg_table_set_night(pos_theme_current_mode() == POS_MODE_NIGHT);
    draw_hand(layer, &coords, &t, st, BJ_ROW_DEALER);
    draw_hand(layer, &coords, &t, st, BJ_ROW_PLAYER);
    bj_view_dealer_label(g, text, sizeof(text));
    draw_label(layer, &coords, &t, BJ_ROW_DEALER, g->dealer.n, text, POS_COLOR_TEXT_PRIMARY);
    bj_view_player_label(g, text, sizeof(text));
    /* A won hand's label in the success colour; the caption says it in words
     * too, so colour is never the only carrier (DS 2). */
    result = bj_view_player_result(g);
    draw_label(layer, &coords, &t, BJ_ROW_PLAYER, g->player.n, text,
               result > 0 ? POS_COLOR_STATUS_OK : result < 0 ? POS_COLOR_TEXT_SECONDARY : POS_COLOR_TEXT_PRIMARY);
    open = bj_view_result_area(&t, g->dealer.n, g->player.n);
    d = chip_d(open);
    bj_view_result(g, text, sizeof(text));
    if (cg_motion_pose(&st->motion, KEY_CHIPS, &p) == 1 && d) {
        /* The stake on its way to whoever won it. */
        cg_table_chips(layer, coords.x1 + (p.rect.x1 + p.rect.x2) / 2, coords.y1 + p.rect.y2, st->before_stake, d);
    } else if (!text[0] && d) {
        /* No result to show there yet: the stake, as chips on the cloth. */
        lv_area_t c = chips_spot(open, d);

        cg_table_chips(layer, coords.x1 + (c.x1 + c.x2) / 2, coords.y1 + c.y2, g->stake ? g->stake : g->bet, d);
    }
    /* What the round won or lost, large, in the open felt between the hands:
     * the sign says it, the colour only repeats it. It arrives once the cards
     * have. */
    shown = cg_motion_pose(&st->motion, KEY_RESULT, &p);
    if (shown == 1 || (shown < 0 && !cg_motion_busy(&st->motion))) {
        draw_text(layer, &coords, open, POS_STYLE_HERO_40, text,
                  result > 0 ? POS_COLOR_STATUS_OK : result < 0 ? POS_COLOR_TEXT_SECONDARY : POS_COLOR_TEXT_PRIMARY,
                  shown == 1 ? p.opa : LV_OPA_COVER);
    }
    draw_moving(layer, &coords, st);
}

static void table_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct bj_table_state *st = state_of(obj);

    if (st) {
        cg_motion_stop(&st->motion);
    }
    lv_free(st);
    lv_obj_set_user_data(obj, NULL);
}

static void table_resized(lv_event_t *e)
{
    struct bj_table_state *st = state_of(lv_event_get_target_obj(e));

    /* The places changed under whatever was moving: it arrives now. */
    if (st) {
        cg_motion_stop(&st->motion);
    }
}

static void table_theme_changed(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

lv_obj_t *bj_table_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct bj_table_state *st = lv_malloc_zeroed(sizeof(*st));

    if (!st) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_user_data(obj, st);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, table_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, table_resized, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_add_event_cb(obj, table_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, table_theme_changed, (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    /* A card's shadow reaches a ninth of its width past it. */
    cg_motion_init(&st->motion, obj, 20);
    return obj;
}

void bj_table_bind(lv_obj_t *table, const struct bj_game *g)
{
    struct bj_table_state *st = state_of(table);

    if (st) {
        cg_motion_stop(&st->motion);
        st->game = g;
        lv_obj_invalidate(table);
    }
}

void bj_table_geometry(lv_obj_t *table, struct bj_table *out)
{
    if (table && out) {
        geometry(table, out);
    }
}

/* ---- motion ------------------------------------------------------------------------ */

static void picture(lv_obj_t *obj, const struct bj_table_state *st, struct cg_spots *out)
{
    const struct bj_game *g = st->game;
    struct bj_table t;
    int row, i;

    out->n = 0;
    geometry(obj, &t);
    /* In the order the cards are dealt: the player's, then the dealer's,
     * round by round. */
    for (i = 0; i < BJ_HAND_MAX; i++) {
        for (row = BJ_ROW_PLAYER; row >= BJ_ROW_DEALER; row--) {
            const struct bj_hand *h = row == BJ_ROW_DEALER ? &g->dealer : &g->player;
            struct bj_rect r;
            struct cg_spot *s;

            if (i >= h->n || out->n >= CG_SPOTS) {
                continue;
            }
            r = bj_view_card(&t, (enum bj_row)row, h->n, i);
            s = &out->s[out->n++];
            s->key = card_key(st, (enum bj_row)row, i, h->card[i]);
            s->rect = (lv_area_t){ r.x, r.y, r.x + r.w - 1, r.y + r.h - 1 };
            s->face = !(row == BJ_ROW_DEALER && bj_view_face_down(g, i));
        }
    }
}

void bj_table_before(lv_obj_t *table)
{
    struct bj_table_state *st = state_of(table);

    if (!st || !st->game) {
        return;
    }
    cg_motion_stop(&st->motion);
    picture(table, st, &st->before);
    st->before_phase = st->game->phase;
    st->before_stake = st->game->stake ? st->game->stake : st->game->bet;
}

void bj_table_after(lv_obj_t *table, int new_round, int doubled)
{
    struct bj_table_state *st = state_of(table);
    struct cg_spots after;
    struct bj_table t;
    struct bj_rect open;
    lv_area_t shoe;
    uint32_t end = 0;
    int d;

    if (!st || !st->game) {
        return;
    }
    if (new_round) {
        st->epoch++;
    }
    if (doubled) {
        /* The stake that settles is the doubled one. */
        st->before_stake = st->game->bet * 2;
    }
    picture(table, st, &after);
    geometry(table, &t);
    /* What a moving card repaints: itself and its shadow. */
    st->motion.margin = cg_deck_shadow_reach(t.card_w);
    /* The shoe stands off the table's top-right corner. */
    shoe = (lv_area_t){ t.w - t.card_w - BJ_TABLE_PAD, -t.card_h / 2, t.w - BJ_TABLE_PAD - 1, t.card_h / 2 - 1 };
    {
        /* A deal comes out card by card; a hit at once; after a stand or a
         * double the hole card turns first and the dealer draws at a
         * dealer's pace. */
        static const struct cg_plan deal = { 0, 140, 220, 240 };
        static const struct cg_plan hit = { 0, 0, 200, 240 };
        static const struct cg_plan dealer = { 380, 450, 220, 260 };
        int dealer_drew = st->game->dealer.n > 0 && st->before.n > 0 && st->game->phase == BJ_SETTLED &&
                          st->before_phase != BJ_SETTLED;

        cg_motion_diff(&st->motion, new_round ? NULL : &st->before, &after, &shoe,
                       new_round ? &deal : dealer_drew ? &dealer : &hit);
    }
    end = cg_motion_end(&st->motion);
    /* The settlement, once the cards are down: the stake slides to whoever
     * won it (it nudges and stays on a push), then the result appears. */
    if (st->game->phase != BJ_SETTLED || st->before_phase == BJ_SETTLED) {
        return;
    }
    open = bj_view_result_area(&t, st->game->dealer.n, st->game->player.n);
    d = chip_d(open);
    if (d) {
        lv_area_t from = chips_spot(open, d);
        int result = bj_view_player_result(st->game);
        int target = result > 0 ? t.cards_y[BJ_ROW_PLAYER] + t.card_h / 2 : t.cards_y[BJ_ROW_DEALER] + t.card_h / 2;
        lv_area_t to = from;

        if (result != 0) {
            to.y1 = target - (from.y2 - from.y1);
            to.y2 = target;
            cg_motion_add(&st->motion, KEY_CHIPS, CG_MOVE_TRAVEL, &from, &to, 1, 1, end + 80, 420);
        } else {
            cg_motion_add(&st->motion, KEY_CHIPS, CG_MOVE_HOP, &from, &from, 1, 1, end + 80, 260);
        }
        end += 80 + (result != 0 ? 420 : 260);
    }
    cg_motion_add(&st->motion, KEY_RESULT, CG_MOVE_FADE, NULL, &(lv_area_t){ open.x, open.y, open.x + open.w - 1,
                                                                            open.y + open.h - 1 },
                  1, 1, end + 60, 300);
}

int bj_table_moving(lv_obj_t *table)
{
    struct bj_table_state *st = state_of(table);

    return st && cg_motion_busy(&st->motion);
}

void bj_table_finish(lv_obj_t *table)
{
    struct bj_table_state *st = state_of(table);

    if (st) {
        cg_motion_stop(&st->motion);
    }
}

const struct cg_motion *bj_table_motion(lv_obj_t *table)
{
    struct bj_table_state *st = state_of(table);

    return st ? &st->motion : NULL;
}
