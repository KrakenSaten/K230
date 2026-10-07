/*
 * RIFT colour emoji on a label (rift_emoji_style.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_emoji_style.h"

#include "rift_emoji_font.h"

#include <stdbool.h>

static lv_style_t styles[POS_STYLE_COUNT];
static bool made[POS_STYLE_COUNT];

/* Puts role's current font, with the colour emoji behind it, in its style;
 * whether that changed the font. */
static bool fill(enum pos_style_role role)
{
    lv_style_value_t v;
    lv_style_value_t now;
    const lv_font_t *font;

    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) != LV_STYLE_RES_FOUND || !v.ptr) {
        return false;
    }
    font = rift_emoji_font(v.ptr);
    if (lv_style_get_prop(&styles[role], LV_STYLE_TEXT_FONT, &now) == LV_STYLE_RES_FOUND && now.ptr == font) {
        return false;
    }
    lv_style_set_text_font(&styles[role], font);
    return true;
}

lv_style_t *rift_emoji_style(enum pos_style_role role)
{
    if (role < 0 || role >= POS_STYLE_COUNT) {
        role = POS_STYLE_TEXT_PRIMARY;
    }
    if (!made[role]) {
        lv_style_init(&styles[role]);
        made[role] = true;
    }
    fill(role);
    return &styles[role];
}

void rift_emoji_style_add(lv_obj_t *label, enum pos_style_role role)
{
    if (label) {
        lv_obj_add_style(label, rift_emoji_style(role), 0);
    }
}

void rift_emoji_style_refresh(void)
{
    int r;

    for (r = 0; r < POS_STYLE_COUNT; r++) {
        if (made[r] && fill((enum pos_style_role)r)) {
            lv_obj_report_style_change(&styles[r]);
        }
    }
}
