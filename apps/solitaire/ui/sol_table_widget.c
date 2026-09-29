/*
 * PG Solitaire table widget. See sol_table_widget.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_table_widget.h"

#include "pocketui.h"
#include "sol_card_draw.h"
#include "sol_felt.h"
#include "sol_palette.h"

struct sol_table_state {
    const struct sol_game *game;
    const struct sol_ui *ui;
    void (*tap)(void *user, struct sol_hit hit);
    void *user;
};

static struct sol_table_state *state_of(lv_obj_t *obj)
{
    return obj ? lv_obj_get_user_data(obj) : NULL;
}

static void geometry(lv_obj_t *obj, struct sol_table *t)
{
    sol_view_table(lv_obj_get_width(obj), lv_obj_get_height(obj), t);
}

static lv_area_t area_of(const lv_area_t *origin, struct sol_rect r)
{
    lv_area_t a = { origin->x1 + r.x, origin->y1 + r.y, origin->x1 + r.x + r.w - 1, origin->y1 + r.y + r.h - 1 };

    return a;
}

/* ---- drawing ------------------------------------------------------------------ */

static void draw_felt(lv_layer_t *layer, const lv_area_t *coords)
{
    const lv_image_dsc_t *felt = sol_felt_image();

    if (felt) {
        lv_draw_image_dsc_t dsc;

        lv_draw_image_dsc_init(&dsc);
        dsc.src = felt;
        dsc.tile = 1;
        lv_draw_image(layer, &dsc, coords);
    } else {
        lv_draw_rect_dsc_t dsc;

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = sol_palette[SOL_INK_FELT];
        dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &dsc, coords);
    }
}

static void draw_pile(lv_layer_t *layer, const lv_area_t *origin, const struct sol_table *t,
                      const struct sol_game *g, int pile)
{
    const struct sol_stack *s = &g->pile[pile];
    int i;

    if (s->n == 0 || sol_is_column(pile)) {
        enum sol_slot_mark mark = SOL_SLOT_PLAIN;
        lv_area_t a = area_of(origin, sol_view_slot(t, pile));

        if (sol_is_foundation(pile)) {
            mark = SOL_SLOT_SUIT;
        } else if (sol_is_column(pile)) {
            mark = SOL_SLOT_KING;
        } else if (pile == SOL_STOCK && g->pile[SOL_WASTE].n > 0) {
            mark = SOL_SLOT_TURN;
        }
        if (s->n == 0) {
            sol_draw_slot(layer, &a, mark, (enum sol_suit)(pile - SOL_F0));
            return;
        }
    }
    /* A pile off the tableau shows only its top card; drawing the rest,
     * exactly underneath, would cost time and show nothing. */
    for (i = sol_is_column(pile) ? 0 : s->n - 1; i < s->n; i++) {
        lv_area_t a = area_of(origin, sol_view_card(t, g, pile, i));

        if (i < s->down) {
            sol_draw_back(layer, &a);
        } else {
            sol_draw_face(layer, &a, s->card[i]);
        }
    }
}

/* The area a run from `index` to the top of `pile` covers, or the slot. */
static lv_area_t run_area(const lv_area_t *origin, const struct sol_table *t, const struct sol_game *g, int pile,
                          int index)
{
    const struct sol_stack *s = &g->pile[pile];
    lv_area_t a;

    if (s->n == 0 || index < 0) {
        return area_of(origin, sol_view_slot(t, pile));
    }
    a = area_of(origin, sol_view_card(t, g, pile, index));
    if (sol_is_column(pile)) {
        lv_area_t last = area_of(origin, sol_view_card(t, g, pile, s->n - 1));

        a.y2 = last.y2;
    }
    return a;
}

static void draw_selection(lv_layer_t *layer, const lv_area_t *a, int radius)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.radius = radius;
    dsc.outline_color = pos_theme_color(POS_COLOR_FOCUS);
    dsc.outline_width = 3;
    dsc.outline_pad = 1;
    dsc.outline_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, a);
}

/* The keyboard cursor: four corner brackets just outside the target, so it
 * reads as a different thing from the selection's full outline even where
 * both are on the same cards (DS 2: never colour alone). */
static void draw_cursor(lv_layer_t *layer, const lv_area_t *target, int card_w)
{
    lv_draw_rect_dsc_t dsc;
    int len = card_w / 4 < 8 ? 8 : card_w / 4;
    int th = 3;
    int o = 5;
    int x1 = target->x1 - o;
    int y1 = target->y1 - o;
    int x2 = target->x2 + o;
    int y2 = target->y2 + o;
    lv_area_t bars[8] = {
        { x1, y1, x1 + len, y1 + th - 1 }, { x1, y1, x1 + th - 1, y1 + len },
        { x2 - len, y1, x2, y1 + th - 1 }, { x2 - th + 1, y1, x2, y1 + len },
        { x1, y2 - th + 1, x1 + len, y2 }, { x1, y2 - len, x1 + th - 1, y2 },
        { x2 - len, y2 - th + 1, x2, y2 }, { x2 - th + 1, y2 - len, x2, y2 },
    };
    int i;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = pos_theme_color(POS_COLOR_FOCUS);
    dsc.bg_opa = LV_OPA_COVER;
    for (i = 0; i < 8; i++) {
        lv_draw_rect(layer, &dsc, &bars[i]);
    }
}

static void table_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct sol_table_state *st = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    struct sol_table t;
    lv_area_t coords;
    int p;

    if (!st || !layer || !st->game) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    geometry(obj, &t);
    draw_felt(layer, &coords);
    for (p = 0; p < SOL_PILES; p++) {
        draw_pile(layer, &coords, &t, st->game, p);
    }
    /* Night mode dims the world, as PocketTimber's art review designed it
     * (docs/apps/POCKETTIMBER_ART.md 11): half of bg over the scene. The
     * interface marks below are drawn after it, in the night tokens. */
    if (pos_theme_current_mode() == POS_MODE_NIGHT) {
        lv_draw_rect_dsc_t dim;

        lv_draw_rect_dsc_init(&dim);
        dim.bg_color = pos_theme_color(POS_COLOR_BG);
        dim.bg_opa = LV_OPA_50;
        lv_draw_rect(layer, &dim, &coords);
    }
    if (st->ui && st->ui->sel_pile >= 0) {
        lv_area_t a = run_area(&coords, &t, st->game, st->ui->sel_pile, st->ui->sel_index);

        draw_selection(layer, &a, sol_card_radius(t.card_w));
    }
    if (st->ui && st->ui->keyboard && !st->ui->confirming && !st->game->won && st->ui->cursor_pile >= 0) {
        int index = sol_is_column(st->ui->cursor_pile) ? st->ui->cursor_index : st->game->pile[st->ui->cursor_pile].n - 1;
        lv_area_t a = run_area(&coords, &t, st->game, st->ui->cursor_pile, index);

        if (!sol_is_column(st->ui->cursor_pile) || st->game->pile[st->ui->cursor_pile].n == 0) {
            a = area_of(&coords, sol_view_slot(&t, st->ui->cursor_pile));
        }
        draw_cursor(layer, &a, t.card_w);
    }
}

/* ---- input and lifetime --------------------------------------------------------- */

static void table_click(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct sol_table_state *st = state_of(obj);
    lv_indev_t *indev = lv_indev_active();
    struct sol_table t;
    lv_point_t p;
    lv_area_t coords;

    if (!st || !st->tap || !st->game || !indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(obj, &coords);
    geometry(obj, &t);
    st->tap(st->user, sol_view_hit(&t, st->game, p.x - coords.x1, p.y - coords.y1));
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

lv_obj_t *sol_table_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct sol_table_state *st = lv_malloc_zeroed(sizeof(*st));

    if (!st) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    /* The felt is confined to a hairline-framed panel, as PocketTimber's
     * viewport is: the scene is content, the frame is chrome. */
    pos_style_add(obj, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_clip_corner(obj, 1, 0);
    lv_obj_set_user_data(obj, st);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    /* A tap on the table must not take the focus from the app's key sink. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(obj, table_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, table_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, table_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, table_theme_changed, (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    return obj;
}

void sol_table_bind(lv_obj_t *table, const struct sol_game *g, const struct sol_ui *ui)
{
    struct sol_table_state *st = state_of(table);

    if (st) {
        st->game = g;
        st->ui = ui;
        lv_obj_invalidate(table);
    }
}

void sol_table_on_tap(lv_obj_t *table, void (*cb)(void *user, struct sol_hit hit), void *user)
{
    struct sol_table_state *st = state_of(table);

    if (st) {
        st->tap = cb;
        st->user = user;
    }
}

void sol_table_geometry(lv_obj_t *table, struct sol_table *out)
{
    if (table && out) {
        geometry(table, out);
    }
}
