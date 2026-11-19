/* The two inks of the original illustration follow the owner's theme. Expand
 * to RGB888 once per palette change. Animation invalidates the eyes
 * over this still, without recolouring or allocating another frame.
 * Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 */
#include "db_perch.h"
#include "pocketui.h"
#include "src/misc/cache/instance/lv_image_cache.h"

extern const uint8_t db_perch_pixels[DB_PERCH_SIZE * DB_PERCH_SIZE];

void db_perch_tint(struct db_perch *p, enum db_mode mode)
{
    enum pos_color_token coat = mode == DB_MODE_GUARD ? POS_COLOR_ACCENT_SECONDARY : POS_COLOR_ACCENT_PRIMARY;
    enum pos_color_token sky = mode == DB_MODE_GUARD ? POS_COLOR_ACCENT_PRIMARY : POS_COLOR_ACCENT_SECONDARY;
    uint32_t bg_rgb = pos_theme_rgb(POS_COLOR_BG);
    uint32_t sky_rgb = pos_theme_rgb(sky);
    uint32_t coat_rgb = pos_theme_rgb(coat);
    lv_color_t ink[256];
    lv_color_t backdrop = pos_theme_color(POS_COLOR_BG);
    lv_color_t feather = lv_color_mix(pos_identity_hue(mode == DB_MODE_GUARD ? 5 : 1),
                                      pos_theme_color(coat), 96);
    lv_color_t dusk = lv_color_mix(pos_identity_hue(mode == DB_MODE_GUARD ? 4 : 6),
                                   pos_theme_color(sky), 155);

    if (p->mode == mode && p->bg == bg_rgb && p->sky == sky_rgb && p->coat == coat_rgb) {
        return;
    }
    p->mode = mode;
    p->bg = bg_rgb;
    p->sky = sky_rgb;
    p->coat = coat_rgb;
    for (int i = 0; i < 128; i++) {
        int light = mode == DB_MODE_NIGHT ? i : 2 * i;
        ink[i] = lv_color_mix(dusk, backdrop, light);
        ink[i + 128] = lv_color_mix(feather, backdrop, light);
    }
    lv_image_cache_drop(&p->image);
    for (size_t i = 0; i < sizeof(db_perch_pixels); i++) {
        lv_color_t color = ink[db_perch_pixels[i]];
        p->pixels[i * 3] = color.blue;
        p->pixels[i * 3 + 1] = color.green;
        p->pixels[i * 3 + 2] = color.red;
    }
    lv_obj_invalidate(p->view);
}

void db_perch_init(struct db_perch *p, lv_obj_t *parent)
{
    p->image = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB888,
                    .w = DB_PERCH_SIZE, .h = DB_PERCH_SIZE, .stride = DB_PERCH_SIZE * 3 },
        .data = (const uint8_t *)p->pixels, .data_size = sizeof(p->pixels),
    };
    p->view = lv_image_create(parent);
    lv_obj_remove_flag(p->view, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(p->view, &p->image);
    lv_image_set_inner_align(p->view, LV_IMAGE_ALIGN_STRETCH);
    p->mode = DB_MODE_COUNT;
    db_perch_tint(p, DB_MODE_COMPANION);
}

void db_perch_release(struct db_perch *p)
{
    lv_image_cache_drop(&p->image);
}
