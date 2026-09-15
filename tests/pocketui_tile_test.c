/*
 * The launcher tile with an app icon mask (DS §20), against the text-icon
 * tile it replaces, under a real LVGL pointer device.
 *
 * Two tiles are built side by side in the same column, one by pocketui_tile()
 * with a glyph and one by pocketui_tile_mask() with a Doors icon, and every
 * property the launcher's layout and navigation depend on is compared: the
 * tile's box, the label's box, the icon's origin, the click flags, focus
 * group membership, and what a finger on the icon, the label, the empty
 * middle and the gutter does. Then the icon's tint is read back through every
 * theme and display mode and compared with the accent the glyph is drawn in.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/launcher_icons_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui.h"
#include "pos_theme.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232

LV_IMAGE_DECLARE(pos_app_icon_radio);

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- a display that draws nowhere, and one finger ---------------------- */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void press_at(int x, int y)
{
    finger_point.x = (int32_t)x;
    finger_point.y = (int32_t)y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
}

static void release(void)
{
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

/* ---- what a tap reached -------------------------------------------------- */

static void *clicked_with;
static int clicks;

static void on_click(lv_event_t *e)
{
    clicks++;
    clicked_with = lv_event_get_user_data(e);
}

static int tap_opens(int x, int y, void *want)
{
    clicks = 0;
    clicked_with = NULL;
    press_at(x, y);
    release();
    return clicks == 1 && clicked_with == want;
}

static int tap_opens_nothing(int x, int y)
{
    clicks = 0;
    press_at(x, y);
    release();
    return clicks == 0;
}

static void rel_coords(lv_obj_t *obj, lv_obj_t *tile, lv_area_t *out)
{
    lv_area_t t;

    lv_obj_get_coords(tile, &t);
    lv_obj_get_coords(obj, out);
    lv_area_move(out, -t.x1, -t.y1);
}

static int same_area(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

int main(void)
{
    static const char *themes[] = { "ice", "brass", "olive", "slate", "carbon" };
    static const char *modes[] = { "normal", "outdoor", "night" };
    static int user_text = 1;
    static int user_mask = 2;
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *col;
    lv_obj_t *text_tile;
    lv_obj_t *mask_tile;
    lv_obj_t *null_tile;
    lv_obj_t *text_icon;
    lv_obj_t *mask_icon;
    lv_area_t a;
    lv_area_t b;
    lv_group_t *group;
    uint32_t in_group;
    char why[128];
    char what[160];
    size_t t;
    size_t m;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_set_read_cb(lv_indev_create(), read_cb);
    lv_indev_set_type(lv_indev_get_next(NULL), LV_INDEV_TYPE_POINTER);

    pos_input_init();
    pocketui_init();
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    group = pos_input_group();
    in_group = group ? lv_group_get_obj_count(group) : 0;

    /* One column of the launcher: 254 px wide, 20 px gutters. */
    col = lv_obj_create(screen);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 254, PANEL_H);
    lv_obj_set_pos(col, POCKETUI_PAD, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, POCKETUI_PAD, 0);
    text_tile = pocketui_tile(col, LV_SYMBOL_WIFI, "Radio", on_click, &user_text);
    mask_tile = pocketui_tile_mask(col, &pos_app_icon_radio, LV_SYMBOL_WIFI, "Radio", on_click, &user_mask);
    /* No launcher app goes without a mask; the NULL path is the API's own. */
    null_tile = pocketui_tile_mask(col, NULL, LV_SYMBOL_VOLUME_MAX, "No icon", on_click, NULL);
    pump(60);
    lv_obj_update_layout(screen);

    /* ---- 1. the same tile ---------------------------------------------- */

    lv_obj_get_coords(text_tile, &a);
    lv_obj_get_coords(mask_tile, &b);
    check("the tile is as wide with an icon mask as with a glyph",
          lv_area_get_width(&a) == lv_area_get_width(&b));
    check("the tile is 150 tall either way",
          lv_area_get_height(&a) == POCKETUI_TILE_H && lv_area_get_height(&b) == POCKETUI_TILE_H);
    check("the next tile starts where it did: gutter unchanged", b.y1 == a.y2 + 1 + POCKETUI_PAD);
    check("both tiles have two children, icon first",
          lv_obj_get_child_count(text_tile) == 2 && lv_obj_get_child_count(mask_tile) == 2);
    text_icon = lv_obj_get_child(text_tile, 0);
    mask_icon = lv_obj_get_child(mask_tile, 0);
    check("the glyph tile's icon is a label", lv_obj_check_type(text_icon, &lv_label_class));
    check("the mask tile's icon is an image of that mask",
          lv_obj_check_type(mask_icon, &lv_image_class) &&
          lv_image_get_src(mask_icon) == (const void *)&pos_app_icon_radio);

    rel_coords(text_icon, text_tile, &a);
    rel_coords(mask_icon, mask_tile, &b);
    check("the icon starts at the same place, the 12 px inset", a.x1 == 12 && a.y1 == 12 &&
                                                             b.x1 == 12 && b.y1 == 12);
    check("the mask is drawn 32 x 32, not scaled",
          lv_area_get_width(&b) == 32 && lv_area_get_height(&b) == 32);
    rel_coords(lv_obj_get_child(text_tile, 1), text_tile, &a);
    rel_coords(lv_obj_get_child(mask_tile, 1), mask_tile, &b);
    check("the label's box is identical", same_area(&a, &b));
    check("the label text is identical",
          strcmp(lv_label_get_text(lv_obj_get_child(text_tile, 1)),
                 lv_label_get_text(lv_obj_get_child(mask_tile, 1))) == 0);

    check("a NULL mask builds the glyph tile: a label with the glyph",
          lv_obj_get_child_count(null_tile) == 2 &&
          lv_obj_check_type(lv_obj_get_child(null_tile, 0), &lv_label_class) &&
          strcmp(lv_label_get_text(lv_obj_get_child(null_tile, 0)), LV_SYMBOL_VOLUME_MAX) == 0);
    rel_coords(lv_obj_get_child(null_tile, 0), null_tile, &a);
    rel_coords(text_icon, text_tile, &b);
    check("and in the same box as pocketui_tile()'s glyph",
          lv_area_get_height(&a) == lv_area_get_height(&b) && a.x1 == b.x1 && a.y1 == b.y1);

    /* ---- 2. the same navigation ---------------------------------------- */

    check("the tile flags are identical (clickable, not scrollable, ...)",
          lv_obj_has_flag(mask_tile, LV_OBJ_FLAG_CLICKABLE) &&
          !lv_obj_has_flag(mask_tile, LV_OBJ_FLAG_SCROLLABLE) &&
          lv_obj_has_flag(text_tile, LV_OBJ_FLAG_CLICK_FOCUSABLE) ==
              lv_obj_has_flag(mask_tile, LV_OBJ_FLAG_CLICK_FOCUSABLE));
    check("neither icon is clickable, so a tap on it is the tile's",
          !lv_obj_has_flag(text_icon, LV_OBJ_FLAG_CLICKABLE) &&
          !lv_obj_has_flag(mask_icon, LV_OBJ_FLAG_CLICKABLE));
    check("no tile or icon joined a focus group",
          !lv_obj_get_group(text_tile) && !lv_obj_get_group(mask_tile) &&
          !lv_obj_get_group(text_icon) && !lv_obj_get_group(mask_icon));
    check("the focus group holds as many objects as before the tiles",
          (group ? lv_group_get_obj_count(group) : 0) == in_group);

    lv_obj_get_coords(mask_icon, &a);
    check("a tap on the icon opens that tile's app",
          tap_opens((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, &user_mask));
    check("a tap on an icon stroke pixel opens it too (not only the centre)",
          tap_opens(a.x1 + 4, a.y1 + 16, &user_mask));
    lv_obj_get_coords(lv_obj_get_child(mask_tile, 1), &a);
    check("a tap on the label opens it", tap_opens(a.x1 + 5, (a.y1 + a.y2) / 2, &user_mask));
    lv_obj_get_coords(mask_tile, &a);
    check("a tap in the empty middle opens it", tap_opens(a.x2 - 20, (a.y1 + a.y2) / 2, &user_mask));
    check("a tap in the gutter above opens nothing", tap_opens_nothing(a.x1 + 40, a.y1 - 10));
    lv_obj_get_coords(text_icon, &a);
    check("the glyph tile still opens its own app", tap_opens((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, &user_text));

    lv_obj_get_coords(mask_icon, &a);
    press_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    check("a finger on the icon presses the tile (SLAB_PRESSED shows)",
          lv_obj_has_state(mask_tile, LV_STATE_PRESSED));
    release();
    check("and lifting it releases the tile", !lv_obj_has_state(mask_tile, LV_STATE_PRESSED));

    /* ---- 3. the same colour, in every theme and mode, live -------------- */

    for (t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
        for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            lv_color_t want;
            lv_color_t tint;
            lv_color_t glyph;

            if (pos_theme_apply(themes[t], modes[m], why, sizeof(why)) < 0) {
                snprintf(what, sizeof(what), "%s/%s applies (%s)", themes[t], modes[m], why);
                check(what, 0);
                continue;
            }
            pump(20);
            want = lv_color_hex(pos_theme_rgb(POS_COLOR_ACCENT_PRIMARY));
            tint = lv_obj_get_style_image_recolor(mask_icon, LV_PART_MAIN);
            glyph = lv_obj_get_style_text_color(text_icon, LV_PART_MAIN);
            snprintf(what, sizeof(what), "%s/%s: the mask is tinted accent_primary, fully", themes[t], modes[m]);
            check(what, lv_color_eq(tint, want) &&
                            lv_obj_get_style_image_recolor_opa(mask_icon, LV_PART_MAIN) == LV_OPA_COVER);
            snprintf(what, sizeof(what), "%s/%s: the same colour the glyph is drawn in", themes[t], modes[m]);
            check(what, lv_color_eq(tint, glyph));
        }
    }

    printf("pocketui_tile_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
