/*
 * RIFT for Doors, phase 1: the chrome, the sections and the lifecycle.
 * See rift_app.h for what lives where and why.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_app.h"

#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pos_input.h"
#include "pos_styles.h"
#include "rift_activity.h"
#include "rift_detail.h"
#include "rift_nodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRIP_H RIFT_TOUCH_H
#define CMDLINE_H RIFT_TOUCH_H
#define TAB_GAP 32
#define UNDERLINE_H 2

static const char *const section_name[RIFT_SEC_COUNT] = { "ACTIVITY", "NODES", "COMMS", "NET" };

/* ---- shared helpers for the screens -------------------------------------- */

int64_t rift_app_now(const struct rift_app *a)
{
    (void)a;
    /* meshcored stamps its events with CLOCK_MONOTONIC, and this app reads
     * the same clock: one board, one monotonic clock, and the only one an
     * interval may be measured on here. */
    return rift_mono_ms();
}

const struct rift_node *rift_app_selected(const struct rift_app *a)
{
    if (!a || !a->have_selected) {
        return NULL;
    }
    /* A selection that has left the cache is no selection: a snapshot the
     * service no longer holds it in means nobody can be asked about it. */
    return rift_model_find(&a->model, a->selected);
}

enum rift_glyph rift_app_glyph(const struct rift_node *n, int64_t now_ms)
{
    if (!n) {
        return RIFT_GLYPH_UNKNOWN;
    }
    if (rift_node_is_stale(n, now_ms)) {
        return RIFT_GLYPH_STALE;
    }
    switch (rift_link_of(n)) {
    case RIFT_LINK_DIRECT:
        return RIFT_GLYPH_DIRECT;
    case RIFT_LINK_RELAYED:
        return RIFT_GLYPH_RELAYED;
    case RIFT_LINK_UNKNOWN:
    default:
        return RIFT_GLYPH_UNKNOWN;
    }
}

const char *rift_app_resolve(const char *hop_id, void *user)
{
    const struct rift_app *a = user;

    return a ? rift_model_name_for_hash(&a->model, hop_id) : NULL;
}

/* ---- the section strip ---------------------------------------------------- */

static void paint_tabs(struct rift_app *a)
{
    int i;

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        int active = (i == (int)a->section);

        if (!a->tab[i]) {
            continue;
        }
        if (active) {
            pos_style_add(lv_obj_get_child(a->tab[i], 0), POS_STYLE_ACCENT_TEXT, 0);
            lv_obj_remove_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_style(lv_obj_get_child(a->tab[i], 0), pos_style(POS_STYLE_ACCENT_TEXT),
                                0);
            lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_tab(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    lv_obj_t *tab = lv_event_get_target_obj(e);
    int i;

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        if (a->tab[i] == tab) {
            rift_app_show_section(a, (enum rift_section)i);
            return;
        }
    }
}

static void build_strip(struct rift_app *a)
{
    int i;

    a->strip = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->strip);
    pos_style_add(a->strip, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(a->strip, LV_PCT(100));
    lv_obj_set_height(a->strip, STRIP_H);
    lv_obj_set_flex_flow(a->strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(a->strip, RIFT_PAD, 0);
    lv_obj_set_style_pad_column(a->strip, TAB_GAP, 0);
    lv_obj_remove_flag(a->strip, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        lv_obj_t *label;

        /* A tab is a 56 px navigation target, not a 36 px row: navigation
         * never shares the row exception (RIFT-DEV-1). */
        a->tab[i] = lv_obj_create(a->strip);
        lv_obj_remove_style_all(a->tab[i]);
        lv_obj_set_height(a->tab[i], STRIP_H);
        lv_obj_set_width(a->tab[i], LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(a->tab[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(a->tab[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(a->tab[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->tab[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(a->tab[i], on_tab, LV_EVENT_CLICKED, a);

        label = lv_label_create(a->tab[i]);
        lv_obj_remove_style_all(label);
        pos_style_add(label, POS_STYLE_CAPTION, 0);
        lv_label_set_text(label, section_name[i]);
        lv_obj_set_flex_grow(label, 1);

        /* The 2 px underline of handoff §3, in the accent. A fill role
         * rather than a colour set here (tests/style_lint.sh). */
        a->tab_rule[i] = lv_obj_create(a->tab[i]);
        lv_obj_remove_style_all(a->tab_rule[i]);
        pos_style_add(a->tab_rule[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->tab_rule[i], 0, 0);
        lv_obj_set_width(a->tab_rule[i], LV_PCT(100));
        lv_obj_set_height(a->tab_rule[i], UNDERLINE_H);
        lv_obj_remove_flag(a->tab_rule[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
    }

    a->cmd_hint = lv_label_create(a->strip);
    lv_obj_remove_style_all(a->cmd_hint);
    pos_style_add(a->cmd_hint, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(a->cmd_hint, 1);
    lv_obj_set_style_text_align(a->cmd_hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_bottom(a->cmd_hint, 18, 0);
    lv_label_set_long_mode(a->cmd_hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(a->cmd_hint, "");
}

/* ---- the command line ----------------------------------------------------- */

static void build_cmdline(struct rift_app *a)
{
    lv_obj_t *prompt;

    /* The command line is permanent chrome in the approved design and the
     * whole vertical budget is measured with it there (handoff §2). Phase 1
     * has no parser, so it is drawn in the DS §9 disabled treatment and
     * says what it is waiting for rather than offering an entry that would
     * do nothing. It is also where this app says the service has gone. */
    a->cmdline = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->cmdline);
    lv_obj_set_width(a->cmdline, LV_PCT(100));
    lv_obj_set_height(a->cmdline, CMDLINE_H);
    lv_obj_set_flex_flow(a->cmdline, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->cmdline, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(a->cmdline, RIFT_PAD, 0);
    lv_obj_set_style_pad_column(a->cmdline, 12, 0);
    lv_obj_remove_flag(a->cmdline, LV_OBJ_FLAG_SCROLLABLE);

    prompt = lv_label_create(a->cmdline);
    lv_obj_remove_style_all(prompt);
    pos_style_add(prompt, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(prompt, 12);
    lv_label_set_text(prompt, "\xE2\x80\xBA");

    a->keysink = lv_label_create(a->cmdline);
    lv_obj_remove_style_all(a->keysink);
    pos_style_add(a->keysink, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(a->keysink, 1);
    lv_label_set_long_mode(a->keysink, LV_LABEL_LONG_CLIP);
    lv_label_set_text(a->keysink, "");
}

static void paint_cmdline(struct rift_app *a)
{
    const struct rift_model *m = &a->model;

    if (m->stale || m->state == RIFT_SVC_ABSENT) {
        lv_label_set_text(a->keysink, a->ipc.last_error[0]
                                          ? a->ipc.last_error
                                          : "meshcored is not answering; reconnecting");
        return;
    }
    if (a->wide) {
        lv_label_set_text(a->keysink,
                          "\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP "ENTER DETAIL" RIFT_SEP
                          "commands arrive with COMMS");
        return;
    }
    lv_label_set_text(a->keysink, "Tap a node to select it; commands arrive with COMMS");
}

/* ---- sections -------------------------------------------------------------- */

static void build_placeholder(struct rift_app *a)
{
    lv_obj_t *panel;
    lv_obj_t *label;

    a->placeholder = lv_obj_create(a->content);
    lv_obj_remove_style_all(a->placeholder);
    lv_obj_set_size(a->placeholder, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->placeholder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(a->placeholder, RIFT_PAD, 0);
    lv_obj_remove_flag(a->placeholder, LV_OBJ_FLAG_SCROLLABLE);

    panel = rift_panel(a->placeholder, "NOT IN THIS BUILD");
    label = lv_label_create(panel);
    lv_obj_remove_style_all(label);
    pos_style_add(label, POS_STYLE_TEXT_SECONDARY, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    /* An empty list here would read as a quiet mesh. The section keeps its
     * place in the navigation - four sections is the approved design - and
     * says what it is instead of pretending to be empty. */
    lv_label_set_text(label, "COMMS and NET are designed and are not in this build. "
                             "Messages, contacts, channels and the network view arrive in a "
                             "later phase; nothing is missing from the mesh.");
}

static void show_only(struct rift_app *a, lv_obj_t *keep)
{
    uint32_t n = lv_obj_get_child_count(a->content);
    uint32_t i;

    for (i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(a->content, (int32_t)i);

        if (child == keep) {
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void rift_app_show_section(struct rift_app *a, enum rift_section section)
{
    if (!a || section < 0 || section >= RIFT_SEC_COUNT) {
        return;
    }
    a->section = section;
    if (section != RIFT_SEC_NODES) {
        a->detail_open = 0;
    }
    paint_tabs(a);
    switch (section) {
    case RIFT_SEC_ACTIVITY:
        show_only(a, a->activity_root);
        break;
    case RIFT_SEC_NODES:
        show_only(a, a->nodes_root);
        break;
    default:
        show_only(a, a->placeholder);
        break;
    }
    rift_app_refresh(a);
}

void rift_app_select(struct rift_app *a, const char *key)
{
    if (!a || !key || !key[0]) {
        return;
    }
    if (a->have_selected && strcmp(a->selected, key) == 0) {
        return;
    }
    snprintf(a->selected, sizeof(a->selected), "%s", key);
    a->have_selected = 1;
    /* Ask the service about this one node. It answers in the same shape as
     * a snapshot entry, so the row updates rather than doubling, and the
     * detail is drawn from what the service holds now rather than from
     * whatever the last snapshot happened to carry. */
    rift_ipc_request_node(&a->ipc, key);
    rift_app_refresh(a);
}

void rift_app_open_detail(struct rift_app *a, int open)
{
    if (!a) {
        return;
    }
    /* Landscape never pushes: the pane beside the list is the detail. */
    a->detail_open = (open && !a->wide) ? 1 : 0;
    rift_app_refresh(a);
}

void rift_app_refresh(struct rift_app *a)
{
    if (!a) {
        return;
    }
    paint_cmdline(a);
    if (a->section == RIFT_SEC_ACTIVITY) {
        rift_activity_refresh(a);
    } else if (a->section == RIFT_SEC_NODES) {
        rift_nodes_refresh(a);
    }
    /* The strip's right caption. The design puts these counts in the app
     * header's right caption, and that header belongs to Doors: RIFT
     * changes nothing there (handoff §2), so they go at the end of the
     * strip instead. In portrait the four section names leave too little
     * room for them, and a caption clipped to its own tail is worse than
     * none - the list's own footer carries the same counts in full. */
    if (a->cmd_hint && !a->wide) {
        lv_obj_add_flag(a->cmd_hint, LV_OBJ_FLAG_HIDDEN);
    } else if (a->cmd_hint) {
        const struct rift_model *m = &a->model;
        int64_t now = rift_app_now(a);
        int fresh = rift_model_fresh_count(m, now);
        int max_hops = -1;
        int i;

        for (i = 0; i < m->node_count; i++) {
            if (m->nodes[i].path_known && m->nodes[i].hops > max_hops) {
                max_hops = m->nodes[i].hops;
            }
        }
        if (max_hops < 0) {
            lv_label_set_text_fmt(a->cmd_hint, "%d KNOWN" RIFT_SEP "%d FRESH" RIFT_SEP
                                               "MAX %s HOPS",
                                  m->node_count, fresh, RIFT_UNKNOWN);
        } else {
            lv_label_set_text_fmt(a->cmd_hint, "%d KNOWN" RIFT_SEP "%d FRESH" RIFT_SEP
                                               "MAX %d HOPS",
                                  m->node_count, fresh, max_hops);
        }
        lv_obj_remove_flag(a->cmd_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- keys ------------------------------------------------------------------ */

static void on_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    if (a->section == RIFT_SEC_NODES && rift_nodes_key(a, key)) {
        return;
    }
    if (key == LV_KEY_ESC && a->section != RIFT_SEC_ACTIVITY) {
        rift_app_show_section(a, RIFT_SEC_ACTIVITY);
    }
}

/* ---- layout ---------------------------------------------------------------- */

static void layout(struct rift_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;
    int wide;

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
    a->body_w = w;
    a->body_h = h;
    /* The split is chosen from the room, not from the orientation: a
     * landscape body with a keyboard sheet over it may not have it
     * (DS §21.3). */
    wide = w > h && w >= RIFT_SPLIT_MIN_W;
    if (wide != a->wide) {
        a->wide = wide;
    }
    rift_activity_shape(a);
    rift_nodes_shape(a);
    rift_app_refresh(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- the IPC pass ----------------------------------------------------------- */

static void pump(lv_timer_t *t)
{
    struct rift_app *a = lv_timer_get_user_data(t);
    int64_t now = rift_mono_ms();

    rift_ipc_poll(&a->ipc, now);
    if (a->ipc.revision != a->drawn_revision || now - a->last_repaint_ms >= RIFT_REPAINT_MS) {
        a->drawn_revision = a->ipc.revision;
        a->last_repaint_ms = now;
        rift_app_refresh(a);
    }
}

/* ---- lifecycle --------------------------------------------------------------- */

static void on_theme_changed(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a && a->frame) {
        lv_obj_invalidate(a->frame);
    }
}

static void *rift_create(lv_obj_t *root)
{
    struct rift_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->root = root;
    rift_model_init(&a->model);
    rift_ipc_init(&a->ipc, &a->model, RIFT_SERVICE);

    /* RIFT draws full-width rules and edge-to-edge dense rows, and the
     * approved vertical budget (56 strip + data + 56 command line) is
     * measured from the whole body. The shell's body padding is the design's
     * own screen padding and is reapplied inside each region instead. The
     * body is the shell's object for this app only, and goes with it. */
    lv_obj_set_style_pad_all(root, 0, 0);

    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);

    build_strip(a);

    a->content = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->content);
    lv_obj_set_width(a->content, LV_PCT(100));
    lv_obj_set_flex_grow(a->content, 1);
    lv_obj_remove_flag(a->content, LV_OBJ_FLAG_SCROLLABLE);

    a->activity_root = rift_activity_create(a, a->content);
    a->nodes_root = rift_nodes_create(a, a->content);
    build_placeholder(a);
    build_cmdline(a);

    /* One key sink for the app, as the calculator has: the rows stay out of
     * the focus group, and the arrows, Enter and Esc reach whichever section
     * is showing (DS §17.2, §17.4). */
    lv_obj_add_event_cb(a->keysink, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->keysink);
    pos_input_focus(a->keysink);

    /* One object watched for theme changes; invalidating the frame repaints
     * every custom-drawn glyph and strip under it, because they read tokens
     * in a draw callback and shared styles alone would not reach them
     * (pos_styles.h). The table holds 32 across the whole platform, so the
     * documented fallback is taken if it is full. */
    if (pos_theme_watch(a->frame) != 0) {
        a->theme_host = lv_screen_active();
        if (a->theme_host) {
            lv_obj_add_event_cb(a->theme_host, on_theme_changed,
                                (lv_event_code_t)pos_event_theme_changed(), a);
        }
    }

    a->section = RIFT_SEC_ACTIVITY;
    rift_app_show_section(a, RIFT_SEC_ACTIVITY);

    /* Connect before the first layout, so the first frame shows the service
     * as it is rather than as unknown. */
    rift_ipc_poll(&a->ipc, rift_mono_ms());
    a->pump = lv_timer_create(pump, RIFT_POLL_MS, a);

    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    LOG_INFO("rift: open, %s", rift_ipc_connected(&a->ipc) ? "meshcored connected"
                                                           : "meshcored not answering");
    return a;
}

static void rift_tick(void *priv)
{
    struct rift_app *a = priv;

    /* The timer does the work. This exists so the app still moves if the
     * timer ever fails to be created, and because ages change without any
     * event arriving. */
    if (a && !a->pump) {
        rift_ipc_poll(&a->ipc, rift_mono_ms());
        rift_app_refresh(a);
    }
}

static void rift_destroy(void *priv)
{
    struct rift_app *a = priv;

    if (!a) {
        return;
    }
    /* Order matters. The timer goes first: it reaches the model, the
     * connection and the screens, and one more pass after any of them has
     * been released is a use after free. */
    if (a->pump) {
        lv_timer_delete(a->pump);
        a->pump = NULL;
    }
    if (a->frame) {
        lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
        a->frame = NULL;
    }
    /* The screen is the shell's and outlives this app, so a listener left
     * on it would fire into a freed block at the next theme change. */
    if (a->theme_host) {
        lv_obj_remove_event_cb_with_user_data(a->theme_host, on_theme_changed, a);
        a->theme_host = NULL;
    }
    /* The subscription is given back rather than merely dropped, and the
     * socket is closed here rather than left to the process. */
    rift_ipc_close(&a->ipc);
    rift_nodes_destroy(a);
    rift_activity_destroy(a);
    /* The LVGL objects are children of the shell's body and are deleted
     * with it; the private blocks were this app's to release. */
    free(a);
}

const struct pocketos_app app_rift = {
    .id = "rift",
    .name = "RIFT",
    /* No icon mask: the approved package carries the RIFT mark as a design
     * sheet (docs/design/rift/shots/identity-sheet.png) and not as the
     * png-32 tint artwork the launcher icons are generated from, so the
     * launcher draws the text icon until that artwork exists. Inventing one
     * here would be inventing branding. */
    .icon = LV_SYMBOL_SHUFFLE,
    .icon_mask = NULL,
    .create = rift_create,
    .tick = rift_tick,
    .destroy = rift_destroy,
};
