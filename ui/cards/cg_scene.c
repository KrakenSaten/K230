/*
 * Card games: the full-screen frame. See cg_scene.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_scene.h"

#include "app.h"
#include "cg_deck.h"
#include "pocketui.h"

#include <string.h>

static void frame_draw(lv_event_t *e)
{
    struct cg_scene *s = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(s->frame, &coords);
    cg_table_set_night(pos_theme_current_mode() == POS_MODE_NIGHT);
    cg_table_cloth(layer, &coords, s->cloth);
}

static void frame_theme(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

static void back_later(void *user)
{
    struct cg_scene *s = user;

    if (s->on_back && s->on_back(s->user)) {
        return;
    }
    pocketos_shell_go_home();
}

static void back_clicked(lv_event_t *e)
{
    struct cg_scene *s = lv_event_get_user_data(e);

    /* Leaving deletes this button: not from inside its own event. */
    lv_async_call_cancel(back_later, s);
    lv_async_call(back_later, s);
}

/* The chevron clear of the top-left corner square (30 px in portrait, 50 in
 * landscape, DS 21.1) and its touch target running out to the corner. */
static void place_back(struct cg_scene *s)
{
    struct pos_insets in = pos_display_bar_insets(pocketui_display_geometry(), POS_EDGE_TOP);
    int x = in.left - 24 > 8 ? (int)in.left - 24 : 8;

    lv_obj_set_pos(s->back, x, 0);
    lv_obj_set_ext_click_area(s->back, x);
}

static void frame_resized(lv_event_t *e)
{
    place_back(lv_event_get_user_data(e));
}

lv_obj_t *cg_scene_build(struct cg_scene *s, lv_obj_t *body, enum cg_cloth_kind cloth, const char *title,
                         int (*on_back)(void *user), void *user)
{
    lv_obj_t *chevron;

    if (!s || !body) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->cloth = cloth;
    s->on_back = on_back;
    s->user = user;
    pocketos_shell_set_fullscreen(true);

    s->frame = lv_obj_create(body);
    lv_obj_remove_style_all(s->frame);
    lv_obj_set_width(s->frame, LV_PCT(100));
    lv_obj_set_flex_grow(s->frame, 1);
    lv_obj_set_flex_flow(s->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s->frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s->frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->frame, frame_draw, LV_EVENT_DRAW_MAIN, s);
    lv_obj_add_event_cb(s->frame, frame_resized, LV_EVENT_SIZE_CHANGED, s);
    if (pos_theme_watch(s->frame) != 0) {
        lv_obj_add_event_cb(s->frame, frame_theme, (lv_event_code_t)pos_event_theme_changed(), NULL);
    }

    s->bar = lv_obj_create(s->frame);
    lv_obj_remove_style_all(s->bar);
    lv_obj_set_size(s->bar, LV_PCT(100), CG_SCENE_BAR_H);
    lv_obj_remove_flag(s->bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s->bar, LV_OBJ_FLAG_CLICKABLE);

    s->title = pocketui_label(s->bar, title ? title : "", POS_STYLE_CAPTION);
    cg_table_style_text(s->title, CG_INK_GOLD);
    lv_obj_set_style_text_letter_space(s->title, 4, 0);
    lv_obj_align(s->title, LV_ALIGN_CENTER, 0, 0);

    s->back = lv_button_create(s->bar);
    lv_obj_remove_style_all(s->back);
    lv_obj_set_size(s->back, 72, CG_SCENE_BAR_H);
    lv_obj_add_flag(s->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s->back, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(s->back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s->back, 16, 0);
    lv_obj_set_style_bg_color(s->back, cg_table_ink(CG_INK_IVORY), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s->back, LV_OPA_20, LV_STATE_PRESSED);
    chevron = pocketui_label(s->back, LV_SYMBOL_LEFT, POS_STYLE_SYMBOL);
    cg_table_style_text(chevron, CG_INK_IVORY);
    lv_obj_center(chevron);
    lv_obj_add_event_cb(s->back, back_clicked, LV_EVENT_CLICKED, s);
    place_back(s);

    s->content = lv_obj_create(s->frame);
    lv_obj_remove_style_all(s->content);
    lv_obj_set_width(s->content, LV_PCT(100));
    lv_obj_set_flex_grow(s->content, 1);
    lv_obj_set_flex_flow(s->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s->content, CG_SCENE_SIDE, 0);
    lv_obj_set_style_pad_bottom(s->content, CG_SCENE_FOOT, 0);
    lv_obj_remove_flag(s->content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s->content, LV_OBJ_FLAG_CLICKABLE);
    return s->content;
}

void cg_scene_close(struct cg_scene *s)
{
    if (s) {
        lv_async_call_cancel(back_later, s);
    }
    cg_deck_purge();
    cg_table_purge();
}
