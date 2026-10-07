/*
 * PG 2048 board widget. See g2048_board.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_board.h"

#include "g2048_view.h"
#include "pocketui.h"

#include <string.h>

struct g2048_board {
    uint8_t cell[G2048_CELLS];
    struct g2048_turn turn;
    int animating;
    int elapsed_ms;
};

/* The type roles a tile's number may use, largest first. The first whose
 * text fits the tile wins, so a 2 is hero-48 and 131072 steps down until it
 * fits, at whatever size the layout gave the board. */
static const enum pos_style_role label_roles[] = {
    POS_STYLE_HERO_48, POS_STYLE_HERO_40, POS_STYLE_TITLE, POS_STYLE_ROW_TITLE,
};

static struct g2048_board *state_of(lv_obj_t *obj)
{
    return obj ? lv_obj_get_user_data(obj) : NULL;
}

/* ---- drawing ------------------------------------------------------------- */

static const lv_font_t *role_font(enum pos_style_role role, int32_t *letter_space)
{
    lv_style_value_t v;

    *letter_space = 0;
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_LETTER_SPACE, &v) == LV_STYLE_RES_FOUND) {
        *letter_space = v.num;
    }
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND) {
        return v.ptr;
    }
    return NULL;
}

static void draw_tile(lv_layer_t *layer, const lv_area_t *origin, struct g2048_rect r, uint8_t exp)
{
    const struct pos_theme_tokens *tokens = pos_theme_current();
    struct g2048_tile_look look;
    lv_draw_rect_dsc_t rect;
    lv_draw_label_dsc_t label;
    lv_area_t a;
    char text[12];
    size_t i;

    if (r.w <= 0 || r.h <= 0 || exp == 0) {
        return;
    }
    g2048_view_tile_look(exp, tokens, &look);
    a.x1 = origin->x1 + r.x;
    a.y1 = origin->y1 + r.y;
    a.x2 = a.x1 + r.w - 1;
    a.y2 = a.y1 + r.h - 1;

    lv_draw_rect_dsc_init(&rect);
    rect.radius = POCKETUI_RADIUS;
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = lv_color_mix(pos_theme_color(look.fill_toward), pos_theme_color(look.fill_base),
                                 look.fill_mix);
    if (look.border_px) {
        /* The goal's outline sits outside the tile, in the gap, the way the
         * DS draws its 2 px focus outline. */
        rect.outline_width = look.border_px;
        rect.outline_pad = 0;
        rect.outline_color = pos_theme_color(look.border);
        rect.outline_opa = LV_OPA_COVER;
    }
    lv_draw_rect(layer, &rect, &a);

    g2048_view_label(exp, text, sizeof(text));
    lv_draw_label_dsc_init(&label);
    for (i = 0; i < sizeof(label_roles) / sizeof(label_roles[0]); i++) {
        int32_t ls;
        const lv_font_t *font = role_font(label_roles[i], &ls);
        lv_point_t size;

        if (!font) {
            continue;
        }
        label.font = font;
        label.letter_space = ls;
        lv_text_get_size(&size, text, font, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= r.w - r.w / 6) {
            break;
        }
    }
    if (!label.font) {
        return;
    }
    label.color = pos_theme_color(look.text);
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = text;
    label.text_local = 1;
    {
        int32_t lh = lv_font_get_line_height(label.font);
        lv_area_t t = { a.x1, a.y1 + (r.h - lh) / 2, a.x2, a.y1 + (r.h - lh) / 2 + lh - 1 };

        lv_draw_label(layer, &label, &t);
    }
}

static void draw_empty(lv_layer_t *layer, const lv_area_t *origin, struct g2048_rect r)
{
    lv_draw_rect_dsc_t rect;
    lv_area_t a = { origin->x1 + r.x, origin->y1 + r.y, origin->x1 + r.x + r.w - 1,
                    origin->y1 + r.y + r.h - 1 };

    lv_draw_rect_dsc_init(&rect);
    rect.radius = POCKETUI_RADIUS;
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = pos_theme_color(POS_COLOR_SURFACE);
    lv_draw_rect(layer, &rect, &a);
}

static struct g2048_rect grow(struct g2048_rect r, int px)
{
    r.x -= px;
    r.y -= px;
    r.w += 2 * px;
    r.h += 2 * px;
    return r;
}

/* The cell scaled about its centre by q8/256. */
static struct g2048_rect scaled(struct g2048_rect r, int q8)
{
    int w = r.w * q8 / 256;
    int h = r.h * q8 / 256;

    r.x += (r.w - w) / 2;
    r.y += (r.h - h) / 2;
    r.w = w;
    r.h = h;
    return r;
}

static void board_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct g2048_board *b = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    int size;
    int i;

    if (!b || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    size = lv_area_get_width(&coords) < lv_area_get_height(&coords) ? lv_area_get_width(&coords)
                                                                     : lv_area_get_height(&coords);
    for (i = 0; i < G2048_CELLS; i++) {
        draw_empty(layer, &coords, g2048_view_cell(size, i));
    }
    if (b->animating && b->elapsed_ms < G2048_SLIDE_MS) {
        int q8 = g2048_view_slide_q8(b->elapsed_ms);

        for (i = 0; i < b->turn.steps; i++) {
            const struct g2048_step *s = &b->turn.step[i];

            draw_tile(layer, &coords,
                      g2048_view_lerp(g2048_view_cell(size, s->from), g2048_view_cell(size, s->to), q8),
                      s->exp);
        }
        return;
    }
    for (i = 0; i < G2048_CELLS; i++) {
        struct g2048_rect r = g2048_view_cell(size, i);

        if (b->cell[i] == 0) {
            continue;
        }
        if (b->animating) {
            if (i == b->turn.spawn) {
                r = scaled(r, g2048_view_spawn_q8(b->elapsed_ms));
            } else {
                int k;

                for (k = 0; k < b->turn.steps; k++) {
                    if (b->turn.step[k].merged && b->turn.step[k].to == i) {
                        r = grow(r, g2048_view_pop_px(b->elapsed_ms));
                        break;
                    }
                }
            }
        }
        draw_tile(layer, &coords, r, b->cell[i]);
    }
}

/* ---- motion ---------------------------------------------------------------- */

static void anim_exec(void *var, int32_t v)
{
    lv_obj_t *obj = var;
    struct g2048_board *b = state_of(obj);

    if (b && b->animating) {
        b->elapsed_ms = v;
        lv_obj_invalidate(obj);
    }
}

static void anim_done(lv_anim_t *a)
{
    lv_obj_t *obj = a->var;
    struct g2048_board *b = state_of(obj);

    if (b) {
        b->animating = 0;
        lv_obj_invalidate(obj);
    }
}

void g2048_board_finish(lv_obj_t *board)
{
    struct g2048_board *b = state_of(board);

    if (!b || !b->animating) {
        return;
    }
    b->animating = 0;
    lv_anim_delete(board, anim_exec);
    lv_obj_invalidate(board);
}

int g2048_board_animating(lv_obj_t *board)
{
    struct g2048_board *b = state_of(board);

    return b ? b->animating : 0;
}

void g2048_board_show(lv_obj_t *board, const struct g2048_game *g)
{
    struct g2048_board *b = state_of(board);

    if (!b || !g) {
        return;
    }
    g2048_board_finish(board);
    memcpy(b->cell, g->cell, sizeof(b->cell));
    lv_obj_invalidate(board);
}

void g2048_board_animate(lv_obj_t *board, const struct g2048_game *g, const struct g2048_turn *turn,
                         int reduced_motion)
{
    struct g2048_board *b = state_of(board);
    lv_anim_t a;

    if (!b || !g || !turn) {
        return;
    }
    g2048_board_show(board, g);
    if (reduced_motion || !turn->moved) {
        return;
    }
    b->turn = *turn;
    b->animating = 1;
    b->elapsed_ms = 0;
    lv_anim_init(&a);
    lv_anim_set_var(&a, board);
    lv_anim_set_exec_cb(&a, anim_exec);
    lv_anim_set_values(&a, 0, G2048_MOTION_MS);
    lv_anim_set_duration(&a, G2048_MOTION_MS);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

/* ---- the object -------------------------------------------------------------- */

static void board_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);

    lv_anim_delete(obj, anim_exec);
    lv_free(state_of(obj));
    lv_obj_set_user_data(obj, NULL);
}

static void board_theme_changed(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

lv_obj_t *g2048_board_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct g2048_board *b = lv_malloc_zeroed(sizeof(*b));

    if (!b) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_user_data(obj, b);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* Not clickable: a press on the board belongs to the app's root, which
     * reads swipes from anywhere between the HUD and the buttons. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, board_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, board_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, board_theme_changed, (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    return obj;
}
