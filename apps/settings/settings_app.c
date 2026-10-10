/*
 * Settings: a short list of categories, each opening one page of its own
 * (DS §52):
 *
 *   Display         brightness, rotation, text size
 *   Appearance      theme and display mode
 *   Sound           volume and mute
 *   Keyboard        the keyboard base and its light
 *   Power & Sleep   screen off, lock, lock at start; what sleep is not
 *   Time & Region   the time zone (and its list), whether the clock is set
 *   Network         Wi-Fi, its networks and the join sheet
 *   System          the System app, Settings' page (DS §47)
 *   Developer       the debug overlay
 *
 * One level: a category's page, and from two of them one page further (a
 * network, the time zone list). The header's back slab and Back go one level
 * back each, to the list and from the list out of Settings (app.h
 * `back_slab_in_app`); Home goes home from anywhere. Each page is short and
 * scrolls as one box, if it scrolls at all: nothing inside a page scrolls on
 * its own (the v0.0.10 screen had a column of four panels and, in landscape,
 * two columns scrolling separately).
 *
 * It owns no hardware and reads nothing from the machine: Wi-Fi is netd's
 * (wifi.* over pocketipc, docs/api/network.md), and everything else is the
 * shell's (app.h): brightness, rotation, appearance, text size, volume, the
 * keyboard light, Power & Sleep, the time zone, the overlay. Every IPC call
 * carries the UI deadline. Nothing is stored here: the shell stores what it
 * applies (settings.conf, the same keys as before).
 *
 * LAYOUT. Each page in two shapes, chosen from the body the app is given and
 * chosen again whenever that body changes size (see "the layout" below): the
 * panels one above the other when the body is tall, two to a row when it is
 * wide. The orientation is the system's (DS section 21.2); nothing here asks
 * what it is, only how much room there is.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "app.h"
#include "brightness.h"
#include "pocketui.h"
#include "settings_internal.h"
#include "settings_view.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small builders ---------------------------------------------------------- */

lv_obj_t *settings_hrow(lv_obj_t *parent, int height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    /* A layout box, not a target: a clickable one would swallow a tap that
     * misses its children and do nothing with it. */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* A box that stacks what is put in it, gap px apart, as tall as its content. */
lv_obj_t *settings_stack(lv_obj_t *parent, int gap)
{
    lv_obj_t *s = lv_obj_create(parent);

    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s, gap, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

lv_obj_t *settings_wrap_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* A button that does not take focus from a text field when tapped (the
 * keyboard's lesson from M4), with the DS disabled treatment. */
lv_obj_t *settings_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, int primary)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    pos_style_add(b, POS_STYLE_BUTTON_DISABLED, LV_STATE_DISABLED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_height(b, SETTINGS_BTN_H);
    return b;
}

/* The current choice is the accented one; the words say it too. */
void settings_accent(lv_obj_t *btn, int on)
{
    if (!btn) {
        return;
    }
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    pos_style_add(btn, on ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
}

void settings_set_enabled(lv_obj_t *obj, int on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

void settings_set_tone(lv_obj_t *lb, enum sv_tone tone)
{
    static const enum pos_style_role roles[] = {
        POS_STYLE_TEXT_PRIMARY, POS_STYLE_TEXT_MUTED, POS_STYLE_STATUS_OK_TEXT,
        POS_STYLE_STATUS_WARN_TEXT, POS_STYLE_STATUS_ERROR_TEXT,
    };
    size_t i;

    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        lv_obj_remove_style(lb, pos_style(roles[i]), 0);
    }
    pos_style_add(lb, roles[tone], 0);
}

void settings_set_hidden(lv_obj_t *obj, int hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *settings_panel(struct settings_app *a, const char *caption, enum settings_col col)
{
    lv_obj_t *p = pocketui_card(a->col[col == COL_RIGHT ? 1 : 0]);

    lv_obj_set_style_pad_row(p, 8, 0);
    if (caption) {
        pocketui_label(p, caption, POS_STYLE_CAPTION);
    }
    return p;
}

/* One flat row - title, minus, value, plus - rather than a content-sized box
 * inside it, which LVGL left unlaid-out with the buttons on top of each other
 * (found by tests/settings_app_test.c). */
void settings_stepper(lv_obj_t *panel, const char *title, lv_event_cb_t down, lv_event_cb_t up, void *user,
                      lv_obj_t **dn, lv_obj_t **value, lv_obj_t **upb)
{
    lv_obj_t *r = settings_hrow(panel, SETTINGS_BTN_H);
    lv_obj_t *t;

    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t = pocketui_label(r, title, POS_STYLE_ROW_TITLE);
    lv_obj_set_flex_grow(t, 1);
    /* Two lines at most where the text size needs them, never under the
     * buttons (DS §46.5). */
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    *dn = settings_button(r, "-", down, user, 0);
    lv_obj_set_width(*dn, SETTINGS_STEP_W);
    *value = pocketui_label(r, "", POS_STYLE_VALUE);
    lv_obj_set_width(*value, SETTINGS_VALUE_W);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, 0);
    *upb = settings_button(r, "+", up, user, 0);
    lv_obj_set_width(*upb, SETTINGS_STEP_W);
}

lv_obj_t *settings_switch_row(lv_obj_t *panel, const char *title, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = settings_hrow(panel, SETTINGS_BTN_H);
    lv_obj_t *t = pocketui_label(r, title, POS_STYLE_ROW_TITLE);
    lv_obj_t *b;

    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(t, 1);
    b = settings_button(r, "OFF", cb, user, 0);
    lv_obj_set_width(b, SETTINGS_TOGGLE_W);
    return b;
}

void settings_switch_paint(lv_obj_t *btn, int on)
{
    if (!btn) {
        return;
    }
    lv_label_set_text(lv_obj_get_child(btn, 0), on ? "ON" : "OFF");
    settings_accent(btn, on);
}

/* ---- polling ------------------------------------------------------------------- */

cJSON *settings_netd(const char *method, cJSON *params, char *err, size_t n)
{
    err[0] = '\0';
    return shell_ipc_call_timeout("netd", method, params, SHELL_IPC_UI_TIMEOUT_MS, err, n);
}

void settings_poll_status(struct settings_app *a)
{
    char err[96];
    cJSON *st = settings_netd("wifi.status", NULL, err, sizeof(err));

    sv_wifi_apply_status(&a->wifi, st);
    cJSON_Delete(st);
}

void settings_poll_networks(struct settings_app *a)
{
    char err[96];
    cJSON *res;

    if (!a->wifi.list_visible) {
        return;
    }
    res = settings_netd("wifi.networks", NULL, err, sizeof(err));
    sv_wifi_apply_networks(&a->wifi, res);
    cJSON_Delete(res);
}

void settings_poll_brightness(struct settings_app *a)
{
    sv_brightness_apply(&a->bright, pocketos_shell_brightness_get(), BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT);
}

/* The orientation is the shell's: this only stores a mode through it and
 * shows what the shell says - including that a change takes effect when the
 * Doors shell restarts. Nothing here rotates anything. */
void settings_poll_rotation(struct settings_app *a)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    sv_rotation_apply(&a->rot, (int)o.mode, o.mode_valid, o.landscape, o.next_landscape, o.applying,
                      o.keyboard == POCKETOS_KEYBOARD_PRESENT);
}

/* The battery is sysd's (system.status.power, docs/api/system.md): asked
 * when Power & Sleep opens and every few seconds while it shows. A sysd
 * that does not answer clears the lines rather than leaving the last ones. */
void settings_poll_battery(struct settings_app *a)
{
    char err[96];
    cJSON *st = shell_ipc_call_timeout("sysd", "system.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                       sizeof(err));

    sv_battery_apply(&a->battery, st);
    cJSON_Delete(st);
}

/* ---- the list of categories ------------------------------------------------------ */

/* System (DS §47) is Settings' page: the System app, which the shell shows
 * with Settings as its way back. The shell opens it after this event, and
 * this app is destroyed by it, so nothing here touches the app afterwards. */
static void on_category(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t p = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (p == PAGE_SYSTEM) {
        pocketos_shell_open_app("system");
        return;
    }
    if (p >= SETTINGS_FIRST_CATEGORY && p < SETTINGS_FIRST_CATEGORY + SETTINGS_CATEGORIES) {
        settings_go(a, (enum settings_page)p);
    }
}

/* A row as tall as its words need at the text size in force, never below a
 * touch target: the summary wraps rather than running out of the row (DS
 * §46.5). The whole row is the target. */
static void category_row(struct settings_app *a, lv_obj_t *panel, enum settings_page page)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_t *left;
    lv_obj_t *lb;
    int k = page - SETTINGS_FIRST_CATEGORY;

    lv_obj_remove_style_all(row);
    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, SETTINGS_BTN_H, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)page);
    lv_obj_add_event_cb(row, on_category, LV_EVENT_CLICKED, a);

    left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    pocketui_label(left, settings_page_title(page), POS_STYLE_ROW_TITLE);
    a->w.summary[k] = settings_wrap_label(left, "", POS_STYLE_CAPTION);
    lb = pocketui_label(row, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
}

/* Two panels: the device itself, then what it is connected to and about. One
 * above the other when tall, side by side when wide. */
static void build_root(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, NULL, COL_LEFT);
    int i;

    lv_obj_set_style_pad_row(p, 0, 0);
    for (i = PAGE_DISPLAY; i <= PAGE_POWER; i++) {
        category_row(a, p, (enum settings_page)i);
    }
    /* A list's last row needs no line under it: the panel's edge is there. */
    lv_obj_remove_style(lv_obj_get_child(p, -1), pos_style(POS_STYLE_DIVIDER), 0);
    p = settings_panel(a, NULL, COL_RIGHT);
    lv_obj_set_style_pad_row(p, 0, 0);
    for (i = PAGE_TIME; i <= PAGE_DEVELOPER; i++) {
        category_row(a, p, (enum settings_page)i);
    }
    lv_obj_remove_style(lv_obj_get_child(p, -1), pos_style(POS_STYLE_DIVIDER), 0);
}

static void repaint_root(struct settings_app *a)
{
    char line[SV_TEXT];
    int k;

    for (k = 0; k < SETTINGS_CATEGORIES; k++) {
        if (a->w.summary[k]) {
            settings_summary(a, (enum settings_page)(SETTINGS_FIRST_CATEGORY + k), line, sizeof(line));
            if (strcmp(lv_label_get_text(a->w.summary[k]), line) != 0) {
                lv_label_set_text(a->w.summary[k], line);
            }
        }
    }
}

/* ---- moving between pages ------------------------------------------------------ */

void settings_go(struct settings_app *a, enum settings_page page)
{
    a->page = page;
    a->tick = 0;
    if (page == PAGE_NETWORK) {
        settings_poll_status(a);
        settings_poll_networks(a);
    }
    pocketos_shell_set_title(page == PAGE_ROOT ? NULL : settings_page_title(page));
    settings_rebuild(a);
}

void settings_repaint(struct settings_app *a)
{
    switch (a->page) {
    case PAGE_ROOT:
        repaint_root(a);
        break;
    case PAGE_NETWORK:
        settings_network_repaint(a);
        break;
    case PAGE_SHEET:
        break;
    default:
        settings_page_repaint(a);
        break;
    }
}

/* ---- the layout ---------------------------------------------------------------------- *
 *
 * The page on show sits in one frame that is exactly the body's content box
 * - the whole of the room the shell gives the app - and is shaped from the
 * size of that box alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel, 528 x 764 with the
 *   keyboard up). The panels one above the other.
 *
 *   WIDE (landscape: 1192 x 396, and 1192 x 100 with the keyboard up). A
 *   column of panels 1192 px wide is the portrait screen stretched. So a
 *   page's panels go into two columns side by side, with the DS 7 panel gap
 *   between them; a page with one column (a single panel, or a list) keeps
 *   it across the body, and its lists (themes, time zones) go two to a line.
 *   The columns never scroll: the page does. The network sheet is one panel
 *   across the body with the network described on the left and the field and
 *   the buttons on the right, because height is what landscape lacks: above
 *   the keyboard the body is 100 px tall, and a field under the text would
 *   be out of sight while it is typed into. Chosen only when each half keeps
 *   SETTINGS_COLUMN_W, so no panel, row or field is narrower than in
 *   portrait. No control's size is taken from the height, so no body can
 *   bring one under 64 px.
 *
 * In either shape the page is the one box that scrolls, and it scrolls only
 * when what it holds is taller than the room.
 *
 * A page is built when it is shown and shaped when it is built. A change of
 * the body's size only shapes: flow and widths. The objects, their values,
 * the typed passphrase, the focus and the keyboard are not touched by it.
 *
 * Whatever the shape, nothing is drawn into the panel's unsafe area (DS 21.1,
 * 22.2): a scrolled page passes the foot of the body, which reaches 10 px
 * into the 30 px corner squares of the reference panel, so the frame pads its
 * foot by however far a corner square reaches into the body, from the
 * platform's description (pos_display_rect_insets, the rule Calculator and
 * Notes use). With the keyboard up the foot is far from the corners and the
 * pad is 0; on a panel with square corners it is always 0. */

void settings_shape(struct settings_app *a)
{
    bool two;

    if (!a->body) {
        return;
    }
    if (a->page == PAGE_SHEET) {
        settings_sheet_shape(a);
        return;
    }
    if (!a->cols) {
        return;
    }
    two = lv_obj_get_child_count(a->col[1]) > 0;
    lv_obj_set_flex_flow(a->cols, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->col[0], a->wide && two ? a->half_w : LV_PCT(100));
    lv_obj_set_width(a->col[1], a->wide ? a->half_w : LV_PCT(100));
    settings_set_hidden(a->col[1], !two);
    settings_page_shape(a);
}

static void layout(struct settings_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;

    /* Nothing to lay out in, or nothing the layout is chosen from has
     * changed: PocketUI owns that decision for every responsive app, and
     * hands back the corner clearance the platform rule gives this box. */
    if (!pocketui_layout_begin(&a->layout_guard, a->frame, &in)) {
        return;
    }
    box = &a->layout_guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    a->wide = w > h && w >= 2 * SETTINGS_COLUMN_W + SETTINGS_PANEL_GAP;
    a->full_w = w;
    a->half_w = (w - SETTINGS_PANEL_GAP) / 2;
    settings_shape(a);
    if (a->page == PAGE_SHEET) {
        settings_sheet_after_layout(a);
    }
}

/* The frame is the body's content box, so this is the body changing size:
 * the keyboard came up or went down, or this is the first layout pass after
 * the app was built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct settings_app *a)
{
    lv_obj_t *frame = lv_obj_create(a->root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the shape itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    a->frame = frame;
}

void settings_rebuild(struct settings_app *a)
{
    int i;

    if (a->body) {
        lv_obj_delete(a->body);
    }
    memset(&a->w, 0, sizeof(a->w));
    a->cols = NULL;
    memset(a->col, 0, sizeof(a->col));

    a->body = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->body);
    lv_obj_set_width(a->body, LV_PCT(100));
    lv_obj_set_height(a->body, LV_PCT(100));
    lv_obj_set_flex_flow(a->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->body, SETTINGS_PANEL_GAP, 0);
    lv_obj_set_scroll_dir(a->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->body, LV_SCROLLBAR_MODE_AUTO);

    if (a->page == PAGE_SHEET) {
        settings_sheet_build(a);
        return;
    }
    /* The two columns: boxes as tall as what they hold, which the page
     * scrolls; they stay clickable, so a finger in the gap between two
     * panels still drags the page (LVGL finds only a clickable object). */
    a->cols = lv_obj_create(a->body);
    lv_obj_remove_style_all(a->cols);
    lv_obj_set_size(a->cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->cols, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(a->cols, SETTINGS_PANEL_GAP, 0);
    lv_obj_set_style_pad_column(a->cols, SETTINGS_PANEL_GAP, 0);
    lv_obj_clear_flag(a->cols, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < 2; i++) {
        a->col[i] = settings_stack(a->cols, SETTINGS_PANEL_GAP);
    }

    switch (a->page) {
    case PAGE_ROOT:
        build_root(a);
        break;
    case PAGE_NETWORK:
        settings_network_build(a);
        break;
    default:
        settings_page_build(a);
        break;
    }
    settings_shape(a);
    settings_repaint(a);
}

/* ---- app lifecycle ------------------------------------------------------------------- */

static void *settings_create(lv_obj_t *root)
{
    struct settings_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->root = root;
    a->page = PAGE_ROOT;
    sv_wifi_init(&a->wifi);
    settings_poll_status(a);
    settings_poll_brightness(a);
    settings_poll_rotation(a);
    build_frame(a);
    settings_rebuild(a);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

static void settings_tick(void *priv)
{
    struct settings_app *a = priv;

    switch (a->page) {
    case PAGE_SHEET:
        settings_poll_status(a);
        return;
    case PAGE_NETWORK:
        settings_poll_status(a);
        /* An empty list is asked again every second, so networks appear as
         * soon as Wi-Fi has any to report. */
        if (a->wifi.scanning || a->wifi.net_count == 0 || ++a->tick >= SETTINGS_LIST_POLL_TICKS) {
            a->tick = 0;
            settings_poll_networks(a);
        }
        break;
    case PAGE_ROOT:
        /* The list's lines are live: Wi-Fi's state among them. */
        settings_poll_status(a);
        settings_poll_brightness(a);
        settings_poll_rotation(a);
        break;
    case PAGE_DISPLAY:
        settings_poll_brightness(a);
        settings_poll_rotation(a);
        break;
    case PAGE_POWER:
        /* The shell samples the gauge every 30 s; every few seconds here is
         * plenty to show a new reading and its age. */
        if (++a->battery_tick >= SETTINGS_BATTERY_POLL_TICKS) {
            a->battery_tick = 0;
            settings_poll_battery(a);
        }
        break;
    default:
        break;
    }
    settings_repaint(a);
}

static void settings_destroy(void *priv)
{
    struct settings_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    /* The shell deletes the objects after this; the passphrase goes first. */
    settings_wifi_teardown(a);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_settings);

/* The Back action and the header's back slab (app.h `back`,
 * `back_slab_in_app`): an open sheet (join, forget, disconnect) closes as
 * its CANCEL or BACK button closes it, passphrase wiped, back to Network; the
 * time zone list goes back to Time & Region; every category's page back to
 * the list; the list is the top level. */
static int settings_back(void *priv)
{
    struct settings_app *a = priv;

    if (!a || a->page == PAGE_ROOT) {
        return 0;
    }
    if (a->page == PAGE_SHEET) {
        settings_sheet_close(a);
    } else if (a->page == PAGE_ZONES) {
        settings_go(a, PAGE_TIME);
    } else {
        settings_go(a, PAGE_ROOT);
    }
    return 1;
}

const struct pocketos_app app_settings = {
    .id = "settings",
    .name = "Settings",
    /* The launcher draws the Doors icon (DS §20). For the text icon: the
     * gear is System's and the list glyph is Timber's; the pencil reads as
     * "change things" without claiming either. */
    .icon = LV_SYMBOL_EDIT,
    .icon_mask = &pos_app_icon_settings,
    .create = settings_create,
    .tick = settings_tick,
    .destroy = settings_destroy,
    .back = settings_back,
    .back_slab_in_app = true,
};
