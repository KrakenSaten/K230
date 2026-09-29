/*
 * The three Pocket Games app tests' shared view of the frame: the display
 * the shell would open in either orientation, and one check that an app's
 * screen fits the body it was given there.
 *
 * Header-only and static, included by tests/g2048_app_test.c,
 * sol_app_test.c and bj_app_test.c after their own check() and pump(), so
 * each test stays one translation unit as the other app tests are.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef GAMES_FRAME_H
#define GAMES_FRAME_H

#include "pocketui.h"
#include "pos_display.h"

#include <stdio.h>

#define GAMES_PANEL_W 568
#define GAMES_PANEL_H 1232
#define GAMES_PANEL_CORNER 30

/* The reference panel with 30 px corners turned to `rotation`, handed to
 * PocketUI, the display resized to it and the content area below the app's
 * chrome sized to what is left (shell.c). With an app open it is the body
 * changing size under a running app, which is what the shell's rotation
 * restart looks like to a layout that follows SIZE_CHANGED. */
static void games_use_display(lv_display_t *disp, lv_obj_t *content, enum pos_rotation rotation,
                              int32_t (*status_h)(void))
{
    struct pos_panel panel = {
        .width = GAMES_PANEL_W,
        .height = GAMES_PANEL_H,
        .corners = { GAMES_PANEL_CORNER, GAMES_PANEL_CORNER, GAMES_PANEL_CORNER, GAMES_PANEL_CORNER },
    };
    struct pos_display_geometry g;
    int32_t top;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    lv_display_set_resolution(disp, g.width, g.height);
    top = status_h();
    lv_obj_set_size(content, g.width, g.height - top);
    lv_obj_set_pos(content, 0, top);
}

static int games_inside(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->y1 >= out->y1 && in->x2 <= out->x2 && in->y2 <= out->y2;
}

/* Every shown object of the app lies inside the body; every button is at
 * least the DS 64 px touch minimum tall and its label is whole (the text's
 * natural width fits the button, so nothing is clipped or cut). Returns the
 * number of problems and prints each with `mode` and the object's area. */
static int games_screen_fits(lv_obj_t *body, lv_obj_t *obj, const char *mode)
{
    lv_area_t b;
    lv_area_t a;
    uint32_t i;
    int bad = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    lv_obj_get_coords(body, &b);
    lv_obj_get_coords(obj, &a);
    if (obj != body && !games_inside(&a, &b)) {
        printf("note %s: object at %d,%d..%d,%d outside the body %d,%d..%d,%d\n", mode, (int)a.x1, (int)a.y1,
               (int)a.x2, (int)a.y2, (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2);
        bad++;
    }
    if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_child_count(obj) > 0) {
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        lv_point_t size;

        if (lv_obj_get_height(obj) < POCKETUI_TOUCH_MIN) {
            printf("note %s: a button %d px tall\n", mode, (int)lv_obj_get_height(obj));
            bad++;
        }
        if (lv_obj_check_type(label, &lv_label_class)) {
            lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
                             lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (size.x > lv_obj_get_content_width(obj)) {
                printf("note %s: \"%s\" is %d px in a %d px button\n", mode, lv_label_get_text(label),
                       (int)size.x, (int)lv_obj_get_content_width(obj));
                bad++;
            }
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        bad += games_screen_fits(body, lv_obj_get_child(obj, (int32_t)i), mode);
    }
    return bad;
}

#endif
