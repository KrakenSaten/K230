/*
 * The top-left way back drawn and touched as the screen's corner (DS §48,
 * pocketui_back_corner()), under a real LVGL pointer device.
 *
 * The app header is built as the shell builds it (shell.c app_open): a 72 px
 * flex row from the top edge, its left padding the side margin or the
 * corner inset, the 72 x 56 back slab, the title 16 px after it, and the
 * app's body below with a 64 px row at its top-left. Then the launcher's
 * folder page and Controls, which place the same slab absolutely (home.c,
 * controls.c). In each, at every left padding the panel can give it (20, 30,
 * 50 and 72), in portrait and landscape:
 *
 *   - the slab keeps its place in the row, so the title does not move, and
 *     draws nothing itself;
 *   - the corner box is drawn in the look asked for, from past the screen's
 *     top and left edges (its own rounded corner off the screen) to the
 *     slab's right edge and the header row's foot, 16 px short of the
 *     title; the chevron is centred in what is seen of it, clear of the
 *     corner's inset;
 *   - a tap anywhere in the box goes back once: the slab's middle, the
 *     top-left pixel of the screen, the strip left of the slab, the row
 *     above it, the foot of the header under it, and its last column;
 *   - a tap one pixel further right or down, on the title, on the rest of
 *     the header row, or on the body's first row does not, and the body's
 *     row gets its own tap;
 *   - the box shows pressed while a finger holds it, and while a key
 *     presses the slab; a finger that slides off and lifts elsewhere does
 *     what it did; a drag that scrolls the page lets it go;
 *   - Enter on the focused slab still goes back.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/back_corner_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketui.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define TITLE_GAP 16 /* shell.c: the header's pad_column */

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

static uint8_t draw_buf[PANEL_H * 40 * 2];
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

static int backs;
static int rows;

static void on_back(lv_event_t *e)
{
    (void)e;
    backs++;
}

static void on_row(lv_event_t *e)
{
    (void)e;
    rows++;
}

static int tap_goes_back(int x, int y)
{
    backs = rows = 0;
    press_at(x, y);
    release();
    return backs == 1 && rows == 0;
}

static int tap_stays(int x, int y)
{
    backs = 0;
    press_at(x, y);
    release();
    return backs == 0;
}

/* ---- the three top-left slabs ------------------------------------------ */

enum kind { APP_HEADER, PAGE };
static const char *kind_name[] = { "app header", "folder page / Controls" };

struct screen {
    lv_obj_t *root;
    lv_obj_t *back;
    lv_obj_t *title;
    lv_obj_t *row; /* the body's first row (app header only) */
    lv_obj_t *chevron;
    lv_obj_t *face; /* what pocketui_back_corner() drew */
};

/* shell.c app_open: the header row, the slab in it, the title, the body. */
static void build_app(struct screen *s, int32_t pad_left)
{
    lv_obj_t *header;
    lv_obj_t *body;
    lv_obj_t *o;

    s->root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_COLUMN);

    header = lv_obj_create(s->root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, TITLE_GAP, 0);
    lv_obj_set_style_pad_hor(header, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_left(header, pad_left, 0);

    s->back = lv_button_create(header);
    lv_obj_remove_style_all(s->back);
    lv_obj_set_size(s->back, 72, 56);
    lv_obj_add_flag(s->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->back, on_back, LV_EVENT_CLICKED, NULL);
    o = lv_label_create(s->back);
    lv_label_set_text(o, LV_SYMBOL_LEFT);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_center(o);
    s->chevron = o;
    s->face = pocketui_back_corner(s->back, pad_left, (POCKETUI_HEADER_H - 56) / 2, POS_STYLE_BUTTON_SECONDARY,
                                   POS_STYLE_SLAB_PRESSED);
    s->title = pocketui_label(header, "Settings", POS_STYLE_TITLE);

    body = lv_obj_create(s->root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    s->row = lv_button_create(body);
    lv_obj_remove_style_all(s->row);
    lv_obj_set_size(s->row, LV_PCT(100), POCKETUI_ROW_H);
    lv_obj_add_event_cb(s->row, on_row, LV_EVENT_CLICKED, NULL);
}

/* home.c folder_header / controls.c: the slab placed at (margin, 8) on a
 * page as large as the screen, the title 16 px after it, and below them a
 * panel taller than the screen, so the page scrolls as a folder's does in
 * landscape when it is full. */
static void build_page(struct screen *s, int32_t margin)
{
    lv_obj_t *panel;

    s->root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(s->root, LV_DIR_VER);
    s->back = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->back);
    lv_obj_remove_flag(s->back, LV_OBJ_FLAG_SCROLLABLE); /* home.c plain() */
    panel = lv_obj_create(s->root);
    lv_obj_remove_style_all(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(panel, 0, 84);
    lv_obj_set_size(panel, LV_PCT(100), 2 * PANEL_H);
    lv_obj_set_pos(s->back, margin, 8);
    lv_obj_set_size(s->back, 72, 56);
    lv_obj_add_flag(s->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->back, on_back, LV_EVENT_CLICKED, NULL);
    s->chevron = lv_label_create(s->back);
    lv_label_set_text(s->chevron, LV_SYMBOL_LEFT);
    pos_style_add(s->chevron, POS_STYLE_SYMBOL, 0);
    lv_obj_center(s->chevron);
    s->face = pocketui_back_corner(s->back, margin, 8, POS_STYLE_ENV_PANEL, POS_STYLE_ENV_PANEL_PRESSED);
    s->title = lv_label_create(s->root);
    lv_label_set_text(s->title, "Utilities");
    lv_obj_set_pos(s->title, margin + 72 + TITLE_GAP, 8);
    s->row = NULL;
}

static void run(enum kind k, bool landscape, int32_t pad)
{
    struct screen s;
    lv_area_t b;
    lv_area_t t;
    char what[200];
    char tag[80];
    int32_t reach_x2;
    int32_t reach_y2;
    int32_t w = landscape ? PANEL_H : PANEL_W;
    int ok;

    snprintf(tag, sizeof(tag), "%s, %s, left %d", kind_name[k], landscape ? "landscape" : "portrait", (int)pad);
    lv_display_set_resolution(lv_display_get_default(), w, landscape ? PANEL_W : PANEL_H);
    if (k == APP_HEADER) {
        build_app(&s, pad);
    } else {
        build_page(&s, pad);
    }
    pump(30);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(s.back, &b);
    lv_obj_get_coords(s.title, &t);
    reach_x2 = b.x2;
    reach_y2 = b.y2 + POCKETUI_BACK_FOOT;

    snprintf(what, sizeof(what), "%s: the slab keeps its 72 x 56 place at (%d, 8) (got %d,%d %dx%d)", tag,
             (int)pad, (int)b.x1, (int)b.y1, (int)lv_area_get_width(&b), (int)lv_area_get_height(&b));
    check(what, b.x1 == pad && b.y1 == 8 && lv_area_get_width(&b) == 72 && lv_area_get_height(&b) == 56);
    snprintf(what, sizeof(what), "%s: the title is where it was, 16 px after the slab (%d)", tag, (int)t.x1);
    check(what, t.x1 == pad + 72 + TITLE_GAP);
    snprintf(what, sizeof(what), "%s: the slab draws nothing itself", tag);
    check(what, lv_obj_get_style_bg_opa(s.back, LV_PART_MAIN) == LV_OPA_TRANSP &&
                    lv_obj_get_style_border_width(s.back, LV_PART_MAIN) == 0);
    {
        lv_area_t f;
        lv_area_t c;
        int32_t cx;

        lv_obj_get_coords(s.face, &f);
        lv_obj_get_coords(s.chevron, &c);
        cx = (c.x1 + c.x2) / 2;
        snprintf(what, sizeof(what), "%s: the corner is drawn from past the edges to (%d, %d) (got %d,%d - %d,%d)",
                 tag, (int)reach_x2, (int)reach_y2, (int)f.x1, (int)f.y1, (int)f.x2, (int)f.y2);
        check(what, f.x1 == -POCKETUI_BACK_BLEED && f.y1 == -POCKETUI_BACK_BLEED && f.x2 == reach_x2 &&
                        f.y2 == reach_y2);
        snprintf(what, sizeof(what), "%s: its own rounded corner is off the screen (radius %d, bleed %d)", tag,
                 (int)lv_obj_get_style_radius(s.face, LV_PART_MAIN), POCKETUI_BACK_BLEED);
        check(what, lv_obj_get_style_radius(s.face, LV_PART_MAIN) < POCKETUI_BACK_BLEED);
        snprintf(what, sizeof(what), "%s: 16 px short of the title, at the header row's foot", tag);
        check(what, t.x1 - f.x2 - 1 == TITLE_GAP && f.y2 == POCKETUI_HEADER_H - 1);
        snprintf(what, sizeof(what), "%s: it is drawn (a fill and a hairline edge) and takes no focus", tag);
        check(what, lv_obj_get_style_bg_opa(s.face, LV_PART_MAIN) > LV_OPA_TRANSP &&
                        lv_obj_get_style_border_width(s.face, LV_PART_MAIN) >= 1 && !lv_obj_get_group(s.face));
        snprintf(what, sizeof(what), "%s: under the chevron", tag);
        check(what, lv_obj_get_index(s.face) < lv_obj_get_index(s.chevron));
        /* The middle of 0..reach_x2, unless that would put it nearer the
         * edge than 14 px inside the slab (only past a 44 px inset). */
        {
            int32_t want = (reach_x2 + 1) / 2 < pad + 14 ? pad + 14 : (reach_x2 + 1) / 2;
            int32_t cy2 = c.y1 + c.y2;

            snprintf(what, sizeof(what),
                     "%s: the chevron centred in what is seen (x %d, want %d; y %d.%d, want 35.5), clear of the inset "
                     "(%d >= %d)",
                     tag, (int)cx, (int)want, (int)(cy2 / 2), (int)(cy2 % 2 ? 5 : 0), (int)c.x1, (int)pad);
            check(what, cx - want <= 1 && want - cx <= 1 && cy2 >= 70 && cy2 <= 72 && c.x1 >= pad);
        }
    }

    /* In the corner: back, once. */
    snprintf(what, sizeof(what), "%s: the slab's middle goes back", tag);
    check(what, tap_goes_back(b.x1 + 36, b.y1 + 28));
    snprintf(what, sizeof(what), "%s: the screen's top-left pixel goes back", tag);
    check(what, tap_goes_back(0, 0));
    snprintf(what, sizeof(what), "%s: the strip left of the slab goes back", tag);
    check(what, tap_goes_back(pad / 2, b.y1 + 28));
    snprintf(what, sizeof(what), "%s: the row above the slab goes back", tag);
    check(what, tap_goes_back(b.x1 + 36, 2));
    snprintf(what, sizeof(what), "%s: the header's foot under the slab goes back", tag);
    check(what, tap_goes_back(b.x1 + 36, reach_y2));
    snprintf(what, sizeof(what), "%s: the bottom-left of the reach goes back", tag);
    check(what, tap_goes_back(0, reach_y2));
    snprintf(what, sizeof(what), "%s: the last column of the reach right of the slab goes back", tag);
    check(what, tap_goes_back(reach_x2, b.y1 + 28));
    snprintf(what, sizeof(what), "%s: the reach's top-right pixel goes back", tag);
    check(what, tap_goes_back(reach_x2, 0));

    /* Outside it: not back. */
    snprintf(what, sizeof(what), "%s: one pixel right of the reach does not go back", tag);
    check(what, tap_stays(reach_x2 + 1, b.y1 + 28));
    snprintf(what, sizeof(what), "%s: the title does not go back", tag);
    check(what, tap_stays(t.x1 + 10, b.y1 + 28));
    snprintf(what, sizeof(what), "%s: the far end of the header row does not go back", tag);
    check(what, tap_stays(w - 30, 20));
    snprintf(what, sizeof(what), "%s: one pixel below the reach does not go back", tag);
    check(what, tap_stays(b.x1 + 36, reach_y2 + 1));
    snprintf(what, sizeof(what), "%s: below the reach at the left edge does not go back", tag);
    check(what, tap_stays(0, reach_y2 + 1));
    if (s.row) {
        lv_area_t r;

        lv_obj_get_coords(s.row, &r);
        backs = rows = 0;
        press_at(r.x1 + 10, r.y1 + 10);
        release();
        snprintf(what, sizeof(what), "%s: the body's first row takes its own tap, not back", tag);
        check(what, rows == 1 && backs == 0);
    }

    /* Held in the corner: pressed, as a tap on the slab is. */
    backs = 0;
    press_at(2, 2);
    snprintf(what, sizeof(what), "%s: a finger on the corner shows it pressed", tag);
    check(what, lv_obj_has_state(s.back, LV_STATE_PRESSED) && lv_obj_has_state(s.face, LV_STATE_PRESSED));
    release();
    snprintf(what, sizeof(what), "%s: and lifting it goes back once, the slab no longer pressed", tag);
    check(what, backs == 1 && !lv_obj_has_state(s.back, LV_STATE_PRESSED) &&
                    !lv_obj_has_state(s.face, LV_STATE_PRESSED));

    /* A slide from the corner onto the title ends as one from the slab's
     * middle does (LVGL's press lock keeps the press on the slab). */
    {
        int from_middle;

        backs = 0;
        press_at(b.x1 + 36, b.y1 + 28);
        press_at(t.x1 + 10, b.y1 + 28);
        release();
        from_middle = backs;
        backs = 0;
        press_at(2, 2);
        press_at(t.x1 + 10, b.y1 + 28);
        release();
        snprintf(what, sizeof(what), "%s: a slide from the corner onto the title ends as one from the slab (%d, %d)",
                 tag, from_middle, backs);
        check(what, backs == from_middle);
    }

    if (k == PAGE) {
        /* A drag that starts in the corner and becomes a scroll of the page:
         * the corner lets go, and nothing goes back. */
        bool scrolled;
        bool held;

        backs = 0;
        press_at(10, reach_y2);
        press_at(10, reach_y2 - 30);
        press_at(10, reach_y2 - 60);
        scrolled = lv_obj_get_scroll_y(s.root) > 0;
        held = lv_obj_has_state(s.back, LV_STATE_PRESSED) || lv_obj_has_state(s.face, LV_STATE_PRESSED);
        release();
        snprintf(what, sizeof(what), "%s: a drag up from the corner scrolls the page", tag);
        check(what, scrolled);
        snprintf(what, sizeof(what), "%s: and the slab is not shown pressed while it scrolls", tag);
        check(what, !held);
        snprintf(what, sizeof(what), "%s: and lifting it does not go back", tag);
        check(what, backs == 0 && !lv_obj_has_state(s.back, LV_STATE_PRESSED));
        lv_obj_scroll_to_y(s.root, 0, LV_ANIM_OFF);
        pump(400);
    }

    /* A key presses the slab, not the box: the box shows it too. */
    lv_obj_add_state(s.back, LV_STATE_PRESSED);
    snprintf(what, sizeof(what), "%s: the slab pressed by a key shows the corner pressed", tag);
    check(what, lv_obj_has_state(s.face, LV_STATE_PRESSED));
    lv_obj_remove_state(s.back, LV_STATE_PRESSED);
    snprintf(what, sizeof(what), "%s: and released, released", tag);
    check(what, !lv_obj_has_state(s.face, LV_STATE_PRESSED));

    /* Keys: Enter on the focused slab. */
    pos_input_add_obj(s.back);
    pos_input_focus(s.back);
    pump(30);
    backs = 0;
    ok = pos_input_push_key(LV_KEY_ENTER);
    pump(120);
    snprintf(what, sizeof(what), "%s: Enter on the focused slab goes back once", tag);
    check(what, ok && backs == 1);
    lv_group_remove_obj(s.back);

    lv_obj_delete(s.root);
    pump(30);
}

int main(void)
{
    static const int32_t pads[] = { POCKETUI_PAD, 30, 50, 72 };
    lv_display_t *disp;
    size_t p;
    int k;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_set_read_cb(lv_indev_create(), read_cb);
    lv_indev_set_type(lv_indev_get_next(NULL), LV_INDEV_TYPE_POINTER);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    for (k = APP_HEADER; k <= PAGE; k++) {
        for (p = 0; p < sizeof(pads) / sizeof(pads[0]); p++) {
            run((enum kind)k, false, pads[p]);
            run((enum kind)k, true, pads[p]);
        }
    }

    printf("pocketui_back_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
