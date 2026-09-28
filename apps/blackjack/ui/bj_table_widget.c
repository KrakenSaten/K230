/*
 * PG Blackjack table widget. See bj_table_widget.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "bj_table_widget.h"

#include "bj_card_draw.h"
#include "bj_felt.h"
#include "bj_palette.h"
#include "pocketui.h"

struct bj_table_state {
    const struct bj_game *game;
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

static void draw_felt(lv_layer_t *layer, const lv_area_t *coords)
{
    const lv_image_dsc_t *felt = bj_felt_image();

    if (felt) {
        lv_draw_image_dsc_t dsc;

        lv_draw_image_dsc_init(&dsc);
        dsc.src = felt;
        dsc.tile = 1;
        lv_draw_image(layer, &dsc, coords);
    } else {
        lv_draw_rect_dsc_t dsc;

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = bj_palette[BJ_INK_FELT];
        dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &dsc, coords);
    }
}

static void draw_hand(lv_layer_t *layer, const lv_area_t *origin, const struct bj_table *t, const struct bj_game *g,
                      enum bj_row row)
{
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

            bj_draw_slot(layer, &a, BJ_SLOT_PLAIN, BJ_SPADES);
        }
        return;
    }
    for (i = 0; i < h->n; i++) {
        lv_area_t a = area_of(origin, bj_view_card(t, row, h->n, i));

        if (row == BJ_ROW_DEALER && bj_view_face_down(g, i)) {
            bj_draw_back(layer, &a);
        } else {
            bj_draw_face(layer, &a, h->card[i], BJ_INDEX_BELOW);
        }
    }
}

/* Text in a DS role's font and tracking, centred across `r` and vertically
 * in it. Left out when the role has no font or the text is taller than r. */
static void draw_text(lv_layer_t *layer, const lv_area_t *origin, struct bj_rect r, enum pos_style_role role,
                      const char *text, enum pos_color_token color)
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
    dsc.color = pos_theme_color(color);
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
                       const char *text, enum pos_color_token color)
{
    struct bj_rect r = { BJ_TABLE_PAD, t->label_y[row], t->w - 2 * BJ_TABLE_PAD, BJ_LABEL_H };

    draw_text(layer, origin, r, POS_STYLE_BUTTON_LABEL, text, color);
}

static void table_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct bj_table_state *st = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    struct bj_table t;
    lv_area_t coords;
    char text[48];
    int result;

    if (!st || !st->game || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    bj_view_table(lv_obj_get_width(obj), lv_obj_get_height(obj), &t);
    draw_felt(layer, &coords);
    draw_hand(layer, &coords, &t, st->game, BJ_ROW_DEALER);
    draw_hand(layer, &coords, &t, st->game, BJ_ROW_PLAYER);
    /* Night dims the world, as in PocketTimber's art review; the labels are
     * interface and draw after it, in the night tokens. */
    if (pos_theme_current_mode() == POS_MODE_NIGHT) {
        lv_draw_rect_dsc_t dim;

        lv_draw_rect_dsc_init(&dim);
        dim.bg_color = pos_theme_color(POS_COLOR_BG);
        dim.bg_opa = LV_OPA_50;
        lv_draw_rect(layer, &dim, &coords);
    }
    bj_view_dealer_label(st->game, text, sizeof(text));
    draw_label(layer, &coords, &t, BJ_ROW_DEALER, text, POS_COLOR_TEXT_PRIMARY);
    bj_view_player_label(st->game, text, sizeof(text));
    /* A won hand's label in the success colour; the caption says it in words
     * too, so colour is never the only carrier (DS 2). */
    result = bj_view_player_result(st->game);
    draw_label(layer, &coords, &t, BJ_ROW_PLAYER, text,
               result > 0 ? POS_COLOR_STATUS_OK : result < 0 ? POS_COLOR_TEXT_SECONDARY : POS_COLOR_TEXT_PRIMARY);
    /* What the round won or lost, large, in the open felt between the hands:
     * the sign says it, the colour only repeats it. */
    bj_view_result(st->game, text, sizeof(text));
    draw_text(layer, &coords, bj_view_result_area(&t), POS_STYLE_HERO_40, text,
              result > 0 ? POS_COLOR_STATUS_OK : result < 0 ? POS_COLOR_TEXT_SECONDARY : POS_COLOR_TEXT_PRIMARY);
}

static void table_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);

    lv_free(state_of(obj));
    lv_obj_set_user_data(obj, NULL);
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
    /* The felt inside a hairline DS panel, as PocketTimber's viewport. */
    pos_style_add(obj, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_clip_corner(obj, 1, 0);
    lv_obj_set_user_data(obj, st);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, table_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, table_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, table_theme_changed, (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    return obj;
}

void bj_table_bind(lv_obj_t *table, const struct bj_game *g)
{
    struct bj_table_state *st = state_of(table);

    if (st) {
        st->game = g;
        lv_obj_invalidate(table);
    }
}

void bj_table_geometry(lv_obj_t *table, struct bj_table *out)
{
    if (table && out) {
        bj_view_table(lv_obj_get_width(table), lv_obj_get_height(table), out);
    }
}
