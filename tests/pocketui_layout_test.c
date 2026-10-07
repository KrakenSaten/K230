/*
 * The shared responsive-layout guard (pocketui_layout_begin, DS §21.3, §22.2).
 *
 * Eight apps open every layout pass with it, so what it decides is the
 * difference between a relayout and a repaint in all of them. It is checked
 * here two ways:
 *
 *   1. through a real LVGL frame, moved, resized and emptied the way the
 *      shell's body moves one: the first pass, the repeat that must do
 *      nothing, a frame with no area yet, and that a rejected pass leaves the
 *      stored state exactly as it was;
 *   2. one field at a time, with the frame's coordinates written directly and
 *      the display geometry chosen so that exactly one inset moves. LVGL ties
 *      x1 to x2 through the object's size, so isolating a single coordinate is
 *      the only way to prove all four of them - and all four insets - are
 *      really compared. A guard that forgot any one of the eight numbers would
 *      pass every other check here.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/display_geometry_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketui.h"

/* Part 2 writes the frame's coordinates rather than asking LVGL for a size,
 * because a width below zero is something lv_obj_get_coords() can hand the
 * guard and no LVGL layout can be talked into producing. */
#include "src/core/lv_obj_private.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define CORNER 30

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

/* ---- a display that draws nowhere -------------------------------------- */

static uint8_t draw_buf[PANEL_W * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

/* ---- the geometry under the guard -------------------------------------- */

/* Built here rather than through pos_display_geometry_init() so that one edge
 * or one corner can be moved on its own; the rotation mapping that fills a
 * geometry in has its own test (tests/display_geometry_test.c). */
static void set_geometry(struct pos_insets edges, struct pos_corners corners)
{
    struct pos_display_geometry g;

    memset(&g, 0, sizeof(g));
    g.rotation = POS_ROTATION_0;
    g.native_width = PANEL_W;
    g.native_height = PANEL_H;
    g.width = PANEL_W;
    g.height = PANEL_H;
    g.edges = edges;
    g.corners = corners;
    pocketui_set_display_geometry(&g);
}

static const struct pos_insets no_edges;
static const struct pos_corners no_corners;
static const struct pos_corners round_corners = {CORNER, CORNER, CORNER, CORNER};

static int insets_are(struct pos_insets in, int32_t l, int32_t t, int32_t r, int32_t b)
{
    return in.left == l && in.top == t && in.right == r && in.bottom == b;
}

static int area_is(const lv_area_t *a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    return a->x1 == x1 && a->y1 == y1 && a->x2 == x2 && a->y2 == y2;
}

static int guard_still(const struct pocketui_layout_guard *g,
                       const struct pocketui_layout_guard *want)
{
    return g->valid == want->valid &&
           area_is(&g->area, want->area.x1, want->area.y1, want->area.x2, want->area.y2) &&
           insets_are(g->insets, want->insets.left, want->insets.top, want->insets.right,
                      want->insets.bottom);
}

/* ---- part 2: a frame whose coordinates are written, not laid out -------- */

static void force_area(lv_obj_t *frame, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    frame->coords.x1 = x1;
    frame->coords.y1 = y1;
    frame->coords.x2 = x2;
    frame->coords.y2 = y2;
}

/* The base box of part 2: clear of every edge and corner used there, so the
 * insets are whatever the geometry alone says. */
#define BASE_X1 100
#define BASE_Y1 200
#define BASE_X2 299
#define BASE_Y2 499

/* One field of the box changed, from a guard that holds the base: the guard
 * must say a layout is needed. */
static void one_coord_changes(lv_obj_t *frame, const char *what, int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2)
{
    struct pocketui_layout_guard guard;
    struct pos_insets in;
    char label[96];

    memset(&guard, 0, sizeof(guard));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y2);
    snprintf(label, sizeof(label), "%s: the base box is laid out first", what);
    check(label, pocketui_layout_begin(&guard, frame, &in));
    force_area(frame, x1, y1, x2, y2);
    snprintf(label, sizeof(label), "%s alone is a change the guard sees", what);
    check(label, pocketui_layout_begin(&guard, frame, &in));
}

/* The box does not move, but the geometry does, and exactly one inset with it.
 * Both the value and the change have to come out right. */
static void one_inset_changes(lv_obj_t *frame, const char *what, struct pos_insets edges,
                              int32_t l, int32_t t, int32_t r, int32_t b)
{
    struct pocketui_layout_guard guard;
    struct pos_insets in;
    char label[96];

    memset(&guard, 0, sizeof(guard));
    set_geometry(no_edges, no_corners);
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y2);
    snprintf(label, sizeof(label), "%s: the base geometry is laid out first", what);
    check(label, pocketui_layout_begin(&guard, frame, &in) && insets_are(in, 0, 0, 0, 0));
    set_geometry(edges, no_corners);
    snprintf(label, sizeof(label), "%s alone is a change the guard sees", what);
    check(label, pocketui_layout_begin(&guard, frame, &in));
    snprintf(label, sizeof(label), "%s: and what it hands back is the platform rule's", what);
    check(label, insets_are(in, l, t, r, b));
}

int main(void)
{
    struct pocketui_layout_guard guard;
    struct pocketui_layout_guard before;
    struct pos_insets in;
    struct pos_insets edges;
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *frame;
    lv_obj_t *fresh;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    pocketui_init();

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, PANEL_W, PANEL_H);
    lv_screen_load(screen);

    /* ---- 1. a real frame, on a panel with rounded corners --------------- */
    set_geometry(no_edges, round_corners);

    /* A frame LVGL has never laid out has no area, which is nothing to lay
     * out in and nothing worth remembering. */
    fresh = lv_obj_create(screen);
    lv_obj_remove_style_all(fresh);
    memset(&guard, 0, sizeof(guard));
    before = guard;
    check("a frame LVGL has not laid out yet is refused",
          !pocketui_layout_begin(&guard, fresh, &in));
    check("and it leaves the guard as it found it: still unused", guard_still(&guard, &before));

    frame = lv_obj_create(screen);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, 0, 0);
    lv_obj_set_size(frame, PANEL_W, PANEL_H);
    lv_obj_update_layout(frame);

    /* The whole panel: the rounded corners reach into the top and the foot,
     * and into neither side (DS §22.2). */
    check("the first pass on a real frame is needed", pocketui_layout_begin(&guard, frame, &in));
    check("and it hands back the panel's corner clearance", insets_are(in, 0, CORNER, 0, CORNER));
    check("the guard now holds the box that was laid out",
          guard.valid && area_is(&guard.area, 0, 0, PANEL_W - 1, PANEL_H - 1));
    check("and the insets it was chosen with", insets_are(guard.insets, 0, CORNER, 0, CORNER));

    check("a repeat with nothing changed is refused", !pocketui_layout_begin(&guard, frame, &in));
    check("and a second repeat too", !pocketui_layout_begin(&guard, frame, &in));

    /* The body losing height is what the keyboard coming up looks like. */
    lv_obj_set_size(frame, PANEL_W, PANEL_H - 400);
    lv_obj_update_layout(frame);
    check("a frame that has been resized is laid out again",
          pocketui_layout_begin(&guard, frame, &in));
    check("and the repeat after it is refused", !pocketui_layout_begin(&guard, frame, &in));
    lv_obj_set_pos(frame, 20, 40);
    lv_obj_update_layout(frame);
    check("a frame that has been moved is laid out again",
          pocketui_layout_begin(&guard, frame, &in));
    check("and the repeat after that is refused", !pocketui_layout_begin(&guard, frame, &in));

    /* A frame emptied to nothing, both ways round. Neither may disturb what
     * the last real pass was chosen from: the app has that layout on the
     * glass, and the next pass has to be compared with it. */
    before = guard;
    lv_obj_set_size(frame, 0, PANEL_H - 400);
    lv_obj_update_layout(frame);
    check("a frame with no width is refused", !pocketui_layout_begin(&guard, frame, &in));
    check("and the stored state is untouched by it", guard_still(&guard, &before));
    lv_obj_set_size(frame, PANEL_W, 0);
    lv_obj_update_layout(frame);
    check("a frame with no height is refused", !pocketui_layout_begin(&guard, frame, &in));
    check("and the stored state is untouched by that too", guard_still(&guard, &before));

    /* Back to the box the last real pass was chosen from: still nothing to
     * do, which is only true if neither refusal above wrote anything. */
    lv_obj_set_size(frame, PANEL_W, PANEL_H - 400);
    lv_obj_update_layout(frame);
    check("and the box that was already laid out is still refused",
          !pocketui_layout_begin(&guard, frame, &in));

    /* Off the top-left of the display: negative coordinates are a box like
     * any other, and the guard neither refuses nor mangles them. */
    lv_obj_set_pos(frame, -50, -60);
    lv_obj_set_size(frame, 100, 200);
    lv_obj_update_layout(frame);
    check("a frame at negative coordinates is laid out",
          pocketui_layout_begin(&guard, frame, &in));
    check("and that is the box the guard stored", area_is(&guard.area, -50, -60, 49, 139));
    check("the repeat after it is refused", !pocketui_layout_begin(&guard, frame, &in));

    /* Reset is the app starting again with the same frame in front of it. */
    pocketui_layout_guard_reset(&guard);
    memset(&before, 0, sizeof(before));
    check("a reset guard holds exactly what a zeroed one holds", guard_still(&guard, &before));
    check("so the very same box is laid out again, as if for the first time",
          pocketui_layout_begin(&guard, frame, &in));
    check("and the repeat after that is refused again", !pocketui_layout_begin(&guard, frame, &in));

    /* A one-pixel frame at the origin has the box a zeroed guard holds -
     * {0, 0, 0, 0} is a box one pixel wide and one high, not an empty one -
     * and is still a frame that has never been laid out. On a panel with
     * rounded corners the insets give it away; on a square-cornered one they
     * are zero too, and then the valid flag is the only thing between a first
     * pass and a repeat. Both are checked, square corners first. */
    lv_obj_set_pos(frame, 0, 0);
    lv_obj_set_size(frame, 1, 1);
    lv_obj_update_layout(frame);
    set_geometry(no_edges, no_corners);
    memset(&guard, 0, sizeof(guard));
    check("a first pass on the box and the insets a zeroed guard holds is still a first pass",
          pocketui_layout_begin(&guard, frame, &in) && insets_are(in, 0, 0, 0, 0));
    check("and the repeat after it is refused", !pocketui_layout_begin(&guard, frame, &in));
    set_geometry(no_edges, round_corners);
    memset(&guard, 0, sizeof(guard));
    check("the same box with corners under it is a first pass too",
          pocketui_layout_begin(&guard, frame, &in));
    check("and the repeat after that is refused", !pocketui_layout_begin(&guard, frame, &in));

    /* Nothing to work with is refused rather than dereferenced. */
    memset(&guard, 0, sizeof(guard));
    check("no guard is refused", !pocketui_layout_begin(NULL, frame, &in));
    check("no frame is refused", !pocketui_layout_begin(&guard, NULL, &in));
    check("nowhere to put the insets is refused", !pocketui_layout_begin(&guard, frame, NULL));
    check("and none of the three started a layout", !guard.valid);
    pocketui_layout_guard_reset(NULL);
    check("resetting nothing is harmless", 1);

    /* ---- 2. one number at a time ---------------------------------------- */
    set_geometry(no_edges, no_corners);

    memset(&guard, 0, sizeof(guard));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y2);
    check("the base box of the field checks is laid out",
          pocketui_layout_begin(&guard, frame, &in) && insets_are(in, 0, 0, 0, 0));
    check("and repeating it is refused", !pocketui_layout_begin(&guard, frame, &in));

    one_coord_changes(frame, "x1", BASE_X1 + 1, BASE_Y1, BASE_X2, BASE_Y2);
    one_coord_changes(frame, "y1", BASE_X1, BASE_Y1 + 1, BASE_X2, BASE_Y2);
    one_coord_changes(frame, "x2", BASE_X1, BASE_Y1, BASE_X2 - 1, BASE_Y2);
    one_coord_changes(frame, "y2", BASE_X1, BASE_Y1, BASE_X2, BASE_Y2 - 1);

    /* in.left   = edges.left - x1                  = 150 - 100
     * in.top    = edges.top - y1                   = 250 - 200
     * in.right  = x2 - (width - 1 - edges.right)   = 299 - (568 - 1 - 300)
     * in.bottom = y2 - (height - 1 - edges.bottom) = 499 - (1232 - 1 - 800) */
    edges = no_edges;
    edges.left = 150;
    one_inset_changes(frame, "the left inset", edges, 50, 0, 0, 0);
    edges = no_edges;
    edges.top = 250;
    one_inset_changes(frame, "the top inset", edges, 0, 50, 0, 0);
    edges = no_edges;
    edges.right = 300;
    one_inset_changes(frame, "the right inset", edges, 0, 0, 32, 0);
    edges = no_edges;
    edges.bottom = 800;
    one_inset_changes(frame, "the bottom inset", edges, 0, 0, 0, 68);

    /* The same, driven by a corner rather than an edge: the case the panel
     * actually has. The box is the whole display, so the top-left and
     * bottom-left corner squares both reach into it. */
    memset(&guard, 0, sizeof(guard));
    set_geometry(no_edges, no_corners);
    force_area(frame, 0, 0, PANEL_W - 1, PANEL_H - 1);
    check("a square-cornered panel is laid out with no clearance at all",
          pocketui_layout_begin(&guard, frame, &in) && insets_are(in, 0, 0, 0, 0));
    set_geometry(no_edges, round_corners);
    check("rounding the corners under the very same box needs a layout",
          pocketui_layout_begin(&guard, frame, &in));
    check("and the clearance handed back is the corner squares'",
          insets_are(in, 0, CORNER, 0, CORNER));
    check("with nothing changed after that, the repeat is refused",
          !pocketui_layout_begin(&guard, frame, &in));

    /* ---- 3. an area LVGL cannot produce, which is still refused --------- */
    set_geometry(no_edges, no_corners);
    memset(&guard, 0, sizeof(guard));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y2);
    check("a good box is laid out", pocketui_layout_begin(&guard, frame, &in));
    before = guard;

    force_area(frame, BASE_X1, BASE_Y1, BASE_X1 - 1, BASE_Y2);
    check("a box of exactly zero width is refused", !pocketui_layout_begin(&guard, frame, &in));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y1 - 1);
    check("a box of exactly zero height is refused", !pocketui_layout_begin(&guard, frame, &in));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X1 - 6, BASE_Y2);
    check("a box of negative width is refused", !pocketui_layout_begin(&guard, frame, &in));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y1 - 6);
    check("a box of negative height is refused", !pocketui_layout_begin(&guard, frame, &in));
    check("and not one of the four disturbed what was laid out", guard_still(&guard, &before));
    force_area(frame, BASE_X1, BASE_Y1, BASE_X2, BASE_Y2);
    check("so the box that was laid out is still refused",
          !pocketui_layout_begin(&guard, frame, &in));

    printf("pocketui_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
