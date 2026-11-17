/*
 * The Games Hub's drawing. See hub.h; the geometry is hub_layout.c.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hub.h"

#include "art.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_glyphs.h"

#include <string.h>

/* The dashed outline of the two disabled lobby buttons: four straight lines
 * each (LVGL dashes only those), whose points LVGL keeps by reference. One
 * hub is up at a time, so one set. */
static lv_point_precise_t ghost_points[2][4][2];

bool hub_art_load(const struct hub_layout *l, lv_image_dsc_t *art[HUB_TILE_COUNT])
{
    char name[48];
    int t;

    memset(art, 0, sizeof(art[0]) * HUB_TILE_COUNT);
    for (t = 0; t < HUB_TILE_COUNT; t++) {
        if (hub_art_name(t, l->landscape, name, sizeof(name)) < 0) {
            break;
        }
        art[t] = art_load(name);
        if (!art[t]) {
            break;
        }
        if ((int32_t)art[t]->header.w != l->tile[t].w || (int32_t)art[t]->header.h != l->tile[t].h ||
            art[t]->header.cf != LV_COLOR_FORMAT_RGB565) {
            LOG_WARN("hub: %s is %ux%u, format %u; its tile is %dx%d RGB565", name, (unsigned)art[t]->header.w,
                     (unsigned)art[t]->header.h, (unsigned)art[t]->header.cf, (int)l->tile[t].w, (int)l->tile[t].h);
            break;
        }
    }
    if (t < HUB_TILE_COUNT) {
        hub_art_free(art);
        return false;
    }
    return true;
}

void hub_art_free(lv_image_dsc_t *art[HUB_TILE_COUNT])
{
    int t;

    for (t = 0; t < HUB_TILE_COUNT; t++) {
        art_free(art[t]);
        art[t] = NULL;
    }
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void place(lv_obj_t *obj, const struct hub_rect *r, const struct hub_rect *origin)
{
    lv_obj_set_pos(obj, r->x - (origin ? origin->x : 0), r->y - (origin ? origin->y : 0));
    lv_obj_set_size(obj, r->w, r->h);
}

/* One line of text across a rectangle, centred on its height. */
static lv_obj_t *line_of(lv_obj_t *parent, const struct hub_rect *r, const struct hub_rect *origin,
                         enum pos_style_role role, const char *text, lv_text_align_t align)
{
    lv_obj_t *o = lv_label_create(parent);
    int32_t lh;

    pos_style_add(o, role, 0);
    lv_label_set_text(o, text);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lh = lv_font_get_line_height(lv_obj_get_style_text_font(o, LV_PART_MAIN));
    lv_obj_set_size(o, r->w, lh);
    lv_obj_set_style_text_align(o, align, 0);
    lv_obj_set_pos(o, r->x - (origin ? origin->x : 0), r->y - (origin ? origin->y : 0) + (r->h - lh) / 2);
    return o;
}

/* A filled pill or slab with its words in the middle; the words take the
 * role's colour and face from it. */
static lv_obj_t *plate(lv_obj_t *parent, const struct hub_rect *r, const struct hub_rect *origin,
                       enum pos_style_role role, int over, const char *symbol, const char *text)
{
    lv_obj_t *p = plain(parent);
    lv_obj_t *o;

    pos_style_add(p, role, 0);
    if (over >= 0) {
        pos_style_add(p, (enum pos_style_role)over, 0);
    }
    place(p, r, origin);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(p, 10, 0);
    if (symbol) {
        /* An LV_SYMBOL_* is not in the text faces. */
        o = lv_label_create(p);
        pos_style_add(o, POS_STYLE_SYMBOL, 0);
        lv_label_set_text(o, symbol);
    }
    o = lv_label_create(p);
    lv_label_set_text(o, text);
    return p;
}

static void art_image(lv_obj_t *parent, const lv_image_dsc_t *img)
{
    lv_obj_t *o = lv_image_create(parent);

    pos_style_add(o, POS_STYLE_HUB_ART, 0);
    lv_image_set_src(o, img);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* A lobby button that is not one: a dashed outline, a padlock and its
 * words, in the disabled state, with nothing to click. */
static lv_obj_t *ghost(lv_obj_t *parent, const struct hub_rect *r, const struct hub_rect *origin, int which,
                       const char *text)
{
    lv_obj_t *g = plain(parent);
    lv_point_precise_t (*pt)[2] = ghost_points[which];
    lv_obj_t *o;
    int k;

    place(g, r, origin);
    lv_obj_add_state(g, LV_STATE_DISABLED);
    pos_style_add(g, POS_STYLE_HUB_GHOST, 0);
    /* top, bottom, left, right: each 2 px line lies wholly inside the box */
    pt[0][0] = (lv_point_precise_t){ 0, 1 };
    pt[0][1] = (lv_point_precise_t){ r->w, 1 };
    pt[1][0] = (lv_point_precise_t){ 0, r->h - 1 };
    pt[1][1] = (lv_point_precise_t){ r->w, r->h - 1 };
    pt[2][0] = (lv_point_precise_t){ 1, 0 };
    pt[2][1] = (lv_point_precise_t){ 1, r->h };
    pt[3][0] = (lv_point_precise_t){ r->w - 1, 0 };
    pt[3][1] = (lv_point_precise_t){ r->w - 1, r->h };
    for (k = 0; k < 4; k++) {
        o = lv_line_create(g);
        lv_obj_remove_style_all(o);
        pos_style_add(o, POS_STYLE_HUB_GHOST, 0);
        lv_line_set_points(o, pt[k], 2);
        lv_obj_set_pos(o, 0, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    /* The 32 px lock glyph at half size, in the outline's colour. */
    o = lv_image_create(g);
    lv_obj_remove_style_all(o);
    pos_style_add(o, POS_STYLE_HUB_GHOST_GLYPH, 0);
    lv_image_set_src(o, &pos_glyph_lock);
    lv_image_set_scale(o, 128);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(o, LV_ALIGN_LEFT_MID, 2, 0);
    o = lv_label_create(g);
    lv_label_set_text(o, text);
    lv_obj_align(o, LV_ALIGN_CENTER, 11, 0);
    return g;
}

static void lobby_build(lv_obj_t *root, const struct hub_layout *l, const lv_image_dsc_t *img,
                        struct hub_view *view)
{
    const struct hub_rect *tile = &l->tile[HUB_TILE_LOBBY];
    lv_obj_t *lobby = plain(root);

    view->lobby = lobby;
    place(lobby, tile, NULL);
    lv_obj_add_state(lobby, LV_STATE_DISABLED);
    art_image(lobby, img);
    plate(lobby, &l->lobby_chip, tile, POS_STYLE_HUB_CHIP, POS_STYLE_HUB_CHIP_LOBBY, NULL, HUB_LOBBY_SOON_TEXT);
    if (l->lobby_text.w > 0) {
        line_of(lobby, &l->lobby_text, tile, POS_STYLE_HUB_LOBBY_TEXT, HUB_LOBBY_SENTENCE, LV_TEXT_ALIGN_LEFT);
    }
    view->lobby_host = ghost(lobby, &l->lobby_host, tile, 0, HUB_LOBBY_HOST_TEXT);
    view->lobby_join = ghost(lobby, &l->lobby_join, tile, 1, HUB_LOBBY_JOIN_TEXT);
}

void hub_build(lv_obj_t *parent, const struct hub_layout *l, lv_image_dsc_t *const art[HUB_TILE_COUNT],
               struct hub_view *view)
{
    lv_obj_t *root = plain(parent);
    lv_obj_t *o;
    int t;

    memset(view, 0, sizeof(*view));
    view->root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    pos_style_add(root, POS_STYLE_HUB_BG, 0);
    /* The wall is opaque and whole: a touch between two posters is the
     * hub's, and does nothing. */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    /* The header: the way back as the screen's corner (DS §48), the name,
     * the count. */
    view->back = plain(root);
    place(view->back, &l->back, NULL);
    lv_obj_add_flag(view->back, LV_OBJ_FLAG_CLICKABLE);
    o = lv_label_create(view->back);
    lv_label_set_text(o, LV_SYMBOL_LEFT);
    pos_style_add(o, POS_STYLE_HUB_TEXT, 0);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_center(o);
    pocketui_back_corner(view->back, l->back.x, l->back.y, POS_STYLE_HUB_BACK, POS_STYLE_HUB_BACK_PRESSED);
    line_of(root, &l->title, NULL, l->landscape ? POS_STYLE_HUB_TITLE_COMPACT : POS_STYLE_HUB_TITLE, HUB_TITLE_TEXT,
            LV_TEXT_ALIGN_LEFT);
    line_of(root, &l->meta, NULL, POS_STYLE_HUB_META, HUB_META_TEXT, LV_TEXT_ALIGN_RIGHT);

    for (t = 0; t < HUB_GAMES; t++) {
        const struct hub_rect *tile = &l->tile[t];
        lv_obj_t *p = plain(root);

        view->poster[t] = p;
        place(p, tile, NULL);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        /* The same frame for the keys' focus and a held finger. */
        pos_style_add(p, POS_STYLE_HUB_FOCUS, LV_STATE_FOCUSED);
        pos_style_add(p, POS_STYLE_HUB_FOCUS, LV_STATE_PRESSED);
        art_image(p, art[t]);
        if (t < HUB_FEATURED) {
            plate(p, &l->chip[t], tile, POS_STYLE_HUB_CHIP, t == HUB_TILE_RELAY_NINE ? POS_STYLE_HUB_CHIP_AMBER : -1,
                  NULL, HUB_FEATURED_TEXT);
            plate(p, &l->play[t], tile, POS_STYLE_HUB_PLAY, t == HUB_TILE_RELAY_NINE ? POS_STYLE_HUB_PLAY_AMBER : -1,
                  LV_SYMBOL_PLAY, HUB_PLAY_TEXT);
        }
        if (hub_game(t)->portrait_only && l->badge.w > 0) {
            plate(p, &l->badge, tile, POS_STYLE_HUB_BADGE, -1, NULL, HUB_PORTRAIT_TEXT);
        }
    }
    lobby_build(root, l, art[HUB_TILE_LOBBY], view);
    if (l->hint.w > 0) {
        line_of(root, &l->hint, NULL, POS_STYLE_HUB_META, HUB_HINT_TEXT, LV_TEXT_ALIGN_CENTER);
    }
}
