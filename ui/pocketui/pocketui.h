/*
 * PocketUI: the shared widgets the shell and in-process apps use, built on
 * the Design System v0.1 role styles (pos_styles.h) and tokens (pos_theme.h).
 * Components never name colours or fonts; they add role styles.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETUI_H
#define POCKETUI_H

#include "lvgl.h"
#include "pos_display.h"
#include "pos_input.h"
#include "pos_styles.h"

#include <stdbool.h>

/* Reference panel: 568x1232 portrait. Layout constants in pixels (DS §7).
 * There is no status bar height: the full-width bar is gone and the status
 * cluster lies over the header row (DS §36, ui/shell/chrome.h). */
#define POCKETUI_HEADER_H 72
#define POCKETUI_TOUCH_MIN 64     /* C1: 64 until glove testing says otherwise */
#define POCKETUI_PAD 20
#define POCKETUI_BODY_PAD_TOP 24  /* DS §7: body top padding */
#define POCKETUI_RADIUS 6
#define POCKETUI_ROW_H 64
#define POCKETUI_TILE_H 150

/* Initialise styles from the current theme. Call once after lv_init(). */
void pocketui_init(void);
/* Apply the screen role to a screen object. */
void pocketui_style_screen(lv_obj_t *screen);

/* ---- Display geometry and safe area (pos_display.h) --------------------- */

/* The shell sets the effective geometry once, before building anything; the
 * default is the 568x1232 reference panel, upright, with no unsafe area. */
void pocketui_set_display_geometry(const struct pos_display_geometry *g);
const struct pos_display_geometry *pocketui_display_geometry(void);
/* For a bar lying along a screen edge (a status bar along the top, a sheet
 * along the bottom): raise its padding to the safe-area insets of that edge
 * where they are larger, so nothing in it enters a rounded corner. Padding
 * that already clears the corners is left as it is. */
void pocketui_apply_bar_insets(lv_obj_t *bar, enum pos_edge edge);

/* ---- Responsive layout guard (DS §21.3, §22.2) ------------------------ */

/* What a responsive app's layout was last chosen from: the frame's box, and
 * how far the panel's unsafe area reaches into it. Both, because the same box
 * on a panel with different corners leaves a different amount of room, so a
 * box alone is not enough to say the answer is unchanged.
 *
 * This is PocketUI layout state, not display geometry: pos_display.h stays
 * pure C and knows nothing of LVGL or of who last laid anything out. The
 * guard owns change detection and nothing else - which shape an app takes
 * from the room it is given, and what it does with the insets, stays in the
 * app. */
struct pocketui_layout_guard {
    lv_area_t area;
    struct pos_insets insets;
    bool valid;
};

/* Open a layout pass on frame. Returns false, and the caller lays nothing
 * out, when there is nothing to do:
 *
 *   - no guard, no frame or nowhere to write the insets;
 *   - the frame has no area yet (a fresh object, or one the parent has not
 *     sized), which is nothing to lay out in and nothing worth remembering:
 *     the guard is left as it was, so the next pass is still the first one;
 *   - the box and the insets are both exactly what the last pass was chosen
 *     from, so a pass that ran anyway would repeat the whole cost of the
 *     app's layout for no change at all.
 *
 * Otherwise it records the box and the insets as what this pass is chosen
 * from, writes the insets to *insets, and returns true. The insets are
 * pos_display_rect_insets() over the current display geometry, so every app
 * takes the corner clearance from the one platform rule rather than working
 * it out for itself. The box the pass was chosen from is guard->area.
 *
 * A guard must be zeroed before its first use: an app struct cleared when the
 * app starts, or pocketui_layout_guard_reset(). */
bool pocketui_layout_begin(struct pocketui_layout_guard *guard, lv_obj_t *frame,
                           struct pos_insets *insets);

/* Forget what was laid out, so the next pocketui_layout_begin() behaves as
 * the first one on a fresh guard. */
void pocketui_layout_guard_reset(struct pocketui_layout_guard *guard);

/* Panel: hairline-bordered container with 20 px padding, vertical flex. */
lv_obj_t *pocketui_card(lv_obj_t *parent);
/* Launcher tile (C7): slab, symbol icon top-left, row-title label bottom-left. */
lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data);
/* The same tile with an icon mask (DS §20): an A8 image in the icon's place,
 * drawn in accent_primary through POS_STYLE_APP_ICON, so it follows theme
 * and display mode. Tile, label and click area are pocketui_tile()'s. A NULL
 * mask gives exactly pocketui_tile() with the text icon. */
lv_obj_t *pocketui_tile_mask(lv_obj_t *parent, const lv_image_dsc_t *mask, const char *icon,
                             const char *label, lv_event_cb_t on_click, void *user_data);
/* Key/value row: secondary label left, value right; returns the value label. */
lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value);
/* Primary button, full width of parent, 64 px tall. */
lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data);
/* Plain label with a role style (text, caption, value, status colours). */
lv_obj_t *pocketui_label(lv_obj_t *parent, const char *text, enum pos_style_role role);

/* ---- Text field (DS §17.1) --------------------------------------------- */

/* The text-entry primitive. Single-line is POCKETUI_ROW_H tall and never
 * wraps; multi-line wraps, scrolls vertically and starts at three body lines.
 * A multi-line field grows into its wrapper, so one that should fill a body
 * grows the wrapper (lv_obj_set_flex_grow on the returned object's parent);
 * an error caption then takes its room from the field, not from the body.
 * placeholder may be NULL.
 *
 * Returns the LVGL text area, which is what an app reads and writes. It is
 * wrapped in a transparent full-width container so an error caption has
 * somewhere to go (pocketui_text_field_set_error); the container is the
 * returned object's parent, so a field drops into a column flex like any
 * other component.
 *
 * The field joins the focus group of pos_input.h, so a tap and a key focus it
 * the same way (DS §17.2), and it takes its characters from that one stream
 * whatever produced them (DS §17.4). It never talks to a keyboard. */
lv_obj_t *pocketui_text_field(lv_obj_t *parent, const char *placeholder, bool single_line);

/* Show or clear the error state; message NULL clears it. A message is
 * required when showing, because DS §2 forbids colour from carrying meaning
 * alone: the caption below the field is not optional. */
void pocketui_text_field_set_error(lv_obj_t *field, const char *message);

/* A disabled field takes no focus, shows no caret and cannot be edited. */
void pocketui_text_field_set_enabled(lv_obj_t *field, bool enabled);

/* Reduced motion (DS §12): the caret is drawn solid instead of blinking.
 * The shell sets this from the "reduced_motion" setting before it builds
 * anything; fields created afterwards follow it. */
void pocketui_set_reduced_motion(bool on);

#endif
