/*
 * RIFT for Doors: the chrome, the sections and the lifecycle.
 * See rift_app.h for what lives where and why.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_app.h"

#include "rift_strip.h"

#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pos_input.h"
#include "pos_styles.h"
#include "rift_activity.h"
#include "rift_comms.h"
#include "rift_detail.h"
#include "rift_nodes.h"
#include "rift_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMDLINE_H RIFT_TOUCH_H
/* Landscape (DS §37.2): the strip is the app's top row, where the shell's
 * header was, and holds a back slab; it and the command line are data-row
 * height there, and the composer's field is a line of type with its
 * padding. Every pixel they give up is the thread's. */
#define CMDLINE_H_WIDE RIFT_ROW_H
#define COMPOSER_H_WIDE 32


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

/* ---- the command line ----------------------------------------------------- */

/* Enter in the landscape composer. A single-line field raises READY on
 * Enter whether the character came from a physical key or the touch
 * keyboard's Done (DS §17.4), and committing a composer means sending. */
static void on_composer_ready(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    rift_comms_submit(a, lv_textarea_get_text(a->composer));
}

static void build_cmdline(struct rift_app *a)
{
    lv_obj_t *prompt;

    /* The command line is there only when it has something to hold.
     *
     * The approved design made it permanent chrome, with a command parser
     * behind it; there is no parser, and on ACTIVITY and NODES - and on
     * every portrait screen, where COMMS has its own composer - it held one
     * line of key hints: 56 px of the 378 a landscape body has under its
     * strip (568 less the 32 px bar, the 72 px header and the 30 px corner
     * inset). So it is shown for exactly two things: the landscape composer
     * (landscape COMMS with a conversation open), and the words for a
     * service that is not answering. The key hints moved to the section
     * strip's right caption, which had the room. */
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
    lv_obj_add_flag(a->cmdline, LV_OBJ_FLAG_HIDDEN);

    prompt = lv_label_create(a->cmdline);
    lv_obj_remove_style_all(prompt);
    pos_style_add(prompt, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(prompt, 12);
    lv_label_set_text(prompt, "\xE2\x80\xBA");

    a->cmd_status = lv_label_create(a->cmdline);
    lv_obj_remove_style_all(a->cmd_status);
    pos_style_add(a->cmd_status, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(a->cmd_status, 1);
    lv_label_set_text(a->cmd_status, "");
    /* One line, and "..." where it does not fit rather than a silent cut:
     * at a larger text size (DS §46.5) the reason in brackets no longer
     * fits the line. Where it fits, as it always was. */
    pocketui_label_fit(a->cmd_status, 1);

    /* The one key sink, outside the command line so the line can go away
     * without taking the keys with it. It stays in the layout at 1 px and is
     * never hidden: it is the object the arrows arrive on, and a hidden
     * object is not one LVGL will move focus to reliably. */
    a->keysink = lv_label_create(a->frame);
    lv_obj_remove_style_all(a->keysink);
    lv_obj_add_flag(a->keysink, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(a->keysink, 1, 1);
    lv_obj_set_pos(a->keysink, 0, 0);
    lv_label_set_text(a->keysink, "");

    /* The landscape composer. The design makes the command line the
     * composer in landscape (handoff §8), so the field is chrome and lives
     * here; it is shown only in COMMS, only when the split is on, and only
     * when there is a conversation for it to write to. */
    a->composer = pocketui_text_field(a->cmdline, "Type to send", true);
    if (a->composer) {
        lv_obj_t *wrap = lv_obj_get_parent(a->composer);

        lv_obj_set_flex_grow(wrap, 1);
        lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(a->composer, on_composer_ready, LV_EVENT_READY, a);
    }

    a->cmd_send_hint = lv_label_create(a->cmdline);
    lv_obj_remove_style_all(a->cmd_send_hint);
    pos_style_add(a->cmd_send_hint, POS_STYLE_CAPTION, 0);
    lv_label_set_long_mode(a->cmd_send_hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(a->cmd_send_hint, "");
    lv_obj_add_flag(a->cmd_send_hint, LV_OBJ_FLAG_HIDDEN);
}

/* Whether the landscape command line is currently a composer. */
int rift_app_composer_live(const struct rift_app *a)
{
    return a->composer && a->wide && a->section == RIFT_SEC_COMMS &&
           rift_comms_open_peer(a) != NULL;
}

/* The service is not answering, and the command line says so. */
static int service_down(const struct rift_app *a)
{
    return a->model.stale || a->model.state == RIFT_SVC_ABSENT;
}

static void paint_cmdline(struct rift_app *a)
{
    lv_obj_t *wrap = a->composer ? lv_obj_get_parent(a->composer) : NULL;
    int live = rift_app_composer_live(a);
    int down = !live && service_down(a);

    /* The command line is one of two things, or it is not there: the
     * composer, in landscape COMMS with a conversation open, or the line
     * that says the service is not answering. It is never both. */
    if (live || down) {
        lv_obj_remove_flag(a->cmdline, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->cmdline, LV_OBJ_FLAG_HIDDEN);
    }
    if (wrap) {
        if (live) {
            lv_obj_remove_flag(wrap, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (a->composer_focused) {
                /* Focus does not stay in a field that is no longer there.
                 * Moved before it is hidden, so the field's own DEFOCUSED
                 * event is what clears the flag. */
                pos_input_focus(a->keysink);
            }
            lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (a->cmd_send_hint) {
        if (live) {
            char to[RIFT_LABEL_MAX];

            rift_comms_target_label(a, to, sizeof(to));
            /* What the keys do from where the focus actually is. TAB is how
             * the focus group reaches the field; once it is there, Esc is
             * the way back, because TAB inside a text area types a tab. */
            if (a->composer_focused) {
                lv_label_set_text_fmt(a->cmd_send_hint,
                                      "TO %s" RIFT_SEP "ENTER SEND" RIFT_SEP "ESC CLEAR", to);
            } else {
                lv_label_set_text_fmt(a->cmd_send_hint,
                                      "TO %s" RIFT_SEP "TAB TO WRITE" RIFT_SEP
                                      "\xE2\x86\x91\xE2\x86\x93 CHOOSE",
                                      to);
            }
            lv_obj_remove_flag(a->cmd_send_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(a->cmd_send_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (live) {
        lv_obj_add_flag(a->cmd_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(a->cmd_status, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(a->cmd_status, a->ipc.last_error[0]
                                         ? a->ipc.last_error
                                         : "meshcored is not answering; reconnecting");
}

/* The strip's right caption, landscape only: what the keys do here, then
 * the counts the design puts in the header's right caption - that header is
 * Doors's, so they come here (handoff §2). Portrait has no room for either
 * beside four section names, and a caption clipped to its tail is worse than
 * none; the touch actions there say what they do. */
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
    /* An empty view here would read as a quiet mesh. The section keeps its
     * place in the navigation - four sections is the approved design - and
     * says what it is instead of pretending to be empty. */
    lv_label_set_text(label, "NET is designed and is not in this build. The hop rings, the "
                             "path chain and the relay view arrive in a later phase; nothing "
                             "is missing from the mesh. The routes NET would draw are in "
                             "NODES, on each node's own detail.");
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
        /* Leaving NODES is a Cancel for any confirmation left up there. */
        rift_nodes_cancel_confirm(a);
    }
    rift_tabs_paint(a);
    switch (section) {
    case RIFT_SEC_ACTIVITY:
        show_only(a, a->activity_root);
        break;
    case RIFT_SEC_NODES:
        show_only(a, a->nodes_root);
        break;
    case RIFT_SEC_COMMS:
        show_only(a, a->comms_root);
        break;
    default:
        show_only(a, a->placeholder);
        break;
    }
    rift_app_refresh(a);
}

void rift_app_open_conversation(struct rift_app *a, const char *key)
{
    if (!a || !key || !key[0]) {
        return;
    }
    if (a->section != RIFT_SEC_COMMS) {
        rift_app_show_section(a, RIFT_SEC_COMMS);
    }
    if (a->have_conv && strcmp(a->conv, key) == 0) {
        return;
    }
    snprintf(a->conv, sizeof(a->conv), "%s", key);
    a->have_conv = 1;
    /* A failure belongs to the message it was about, not to the next
     * conversation opened. */
    rift_model_send_clear(&a->model);
    /* Ask the service about this peer: the thread header draws its route,
     * and the node may have been heard since the last snapshot. */
    rift_ipc_request_node(&a->ipc, key);
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
    if (!a->detail_open) {
        rift_nodes_cancel_confirm(a);
    }
    rift_app_refresh(a);
}

void rift_app_refresh(struct rift_app *a)
{
    if (!a) {
        return;
    }
    /* The command line first. Whether it is there decides how tall the
     * section's body is, and the section below lays itself out - a thread
     * scrolls to its newest line - against that height. Painted after, it
     * took its 56 px from under a thread that had just been scrolled to the
     * end, and hid the newest message. It depends only on the section, the
     * open conversation and the service, none of which the refresh changes. */
    paint_cmdline(a);
    if (a->section == RIFT_SEC_ACTIVITY) {
        rift_activity_refresh(a);
    } else if (a->section == RIFT_SEC_NODES) {
        rift_nodes_refresh(a);
    } else if (a->section == RIFT_SEC_COMMS) {
        rift_comms_refresh(a);
    }
    /* The unread pill moves with the messages, not with the section. */
    rift_tabs_paint(a);
    rift_tabs_paint_caption(a);
}

/* ---- keys ------------------------------------------------------------------ */

/* Which of the two has the focus is LVGL's business, not this app's: the
 * command line's field and the key sink are both in the one Doors focus
 * group, and TAB moves between them because that is what a focus group
 * does (DS §17.2). This only *follows* that, so the hint line and the
 * arrow keys know where the keys are going. */
static void on_composer_focus(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    a->composer_focused = (lv_event_get_code(e) == LV_EVENT_FOCUSED);
    /* Asked for, not done here. This runs inside lv_group_focus_obj, which
     * sends DEFOCUSED to the old object and *abandons the focus change* if
     * that event does not come back clean - and a refresh rebuilds rows,
     * which is exactly the kind of thing that does not. Leaving the group's
     * bookkeeping alone and repainting on the next timer pass keeps Esc's
     * way out of the composer working. */
    a->refresh_pending = 1;
}

static void on_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    if (a->section == RIFT_SEC_NODES && rift_nodes_key(a, key)) {
        return;
    }
    if (a->section == RIFT_SEC_COMMS && rift_comms_key(a, key)) {
        return;
    }
    if (key == LV_KEY_ESC && a->section != RIFT_SEC_ACTIVITY) {
        rift_app_show_section(a, RIFT_SEC_ACTIVITY);
    }
}

/* Keys while the landscape composer holds focus. Enter is the field's own
 * READY event; this is Esc, which clears what was typed and, when there is
 * nothing left to clear, hands the list its focus back.
 *
 * TAB is deliberately not handled. It reaches the field as character 9 and
 * the text area inserts it, which is a tab in the message - legal text, one
 * of the two control characters mesh.send takes. Taking it back off the
 * field to move focus would mean undoing an edit the widget has already
 * made; Esc is the way out, and the hint line says so. */
static void on_composer_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    const char *text;
    int typed = 0;
    int i;

    if (key != LV_KEY_ESC || !a->composer) {
        return;
    }
    /* The field already holds this Esc.
     *
     * A text area's own class handler runs before any callback added to it
     * and puts the key in the buffer, so by the time this is reached the
     * field contains character 27 whether or not anything was typed before
     * it. Asking whether the field is empty would therefore always answer
     * no, and Esc would never do anything but clear itself. What counts as
     * typed is a character somebody could have meant: a control character
     * is not one, and mesh.send would refuse it anyway. */
    text = lv_textarea_get_text(a->composer);
    for (i = 0; text && text[i]; i++) {
        if ((unsigned char)text[i] >= 0x20 && (unsigned char)text[i] != 0x7F) {
            typed = 1;
            break;
        }
    }
    lv_textarea_set_text(a->composer, "");
    if (typed) {
        /* There was something to clear, and now it is cleared. */
        rift_model_send_clear(&a->model);
        rift_app_refresh(a);
        return;
    }
    /* Nothing to clear, so Esc means "give the list its focus back". Asked
     * for rather than done here: changing the group's focus from inside the
     * event LVGL is dispatching does not stick. */
    a->focus_list_pending = 1;
    a->refresh_pending = 1;
}

/* ---- layout ---------------------------------------------------------------- */

/* The chrome for the shape (DS §37.2): in landscape the strip is the app's
 * top row - data-row height, a back slab at its left, the tabs beside it -
 * and the command line and its field are as short as a line of type; in
 * portrait both are the 56 px rows of the handoff, and the shell's header
 * is above them. */
static void shape_chrome(struct rift_app *a)
{
    int wide = a->wide;

    rift_tabs_shape(a);
    lv_obj_set_height(a->cmdline, wide ? CMDLINE_H_WIDE : CMDLINE_H);
    if (a->composer) {
        /* 64 is the single-line field's own height (pocketui_text_field),
         * and 16 its padding (DS §17.1); the short one keeps the line of
         * type and 4 px around it. */
        lv_obj_set_height(a->composer, wide ? COMPOSER_H_WIDE : 64);
        lv_obj_set_style_pad_ver(a->composer, wide ? 4 : 16, 0);
    }
}

void rift_app_toggle_details(struct rift_app *a)
{
    if (!a || !a->wide) {
        return;
    }
    a->details_open = !a->details_open;
    rift_comms_shape(a);
    rift_app_refresh(a);
    a->refresh_pending = 1;
}

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
        /* A pane left open in one shape does not follow into the other. */
        a->details_open = 0;
    }
    /* The chrome's heights from the timer, not from inside this layout
     * pass (rift_app.h, chrome_pending). */
    a->chrome_pending = 1;
    rift_activity_shape(a);
    rift_nodes_shape(a);
    rift_comms_shape(a);
    /* Draw now, so the new shape is not empty for a frame, and ask for
     * another pass from the timer: this one is inside LVGL's layout update,
     * where no width can be settled on demand and anything fitted to a
     * column would be fitted to a half-finished one. */
    rift_app_refresh(a);
    a->refresh_pending = 1;
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
    rift_app_notify_pass(a, now);
    if (a->chrome_pending) {
        a->chrome_pending = 0;
        shape_chrome(a);
        a->refresh_pending = 1;
    }
    if (a->refresh_pending || a->ipc.revision != a->drawn_revision ||
        now - a->last_repaint_ms >= RIFT_REPAINT_MS) {
        a->refresh_pending = 0;
        a->drawn_revision = a->ipc.revision;
        a->last_repaint_ms = now;
        rift_app_refresh(a);
    }
    /* After the refresh, not before: a refresh re-enables the portrait
     * composer's field, and enabling a field puts it back in the focus
     * group - which LVGL does by removing and re-appending it, moving the
     * focus. Asking for the list's focus first and then repainting would
     * hand it straight back. */
    if (a->focus_list_pending) {
        a->focus_list_pending = 0;
        pos_input_focus(a->keysink);
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
    /* The reader's choices, or the defaults when there are none (or none
     * that could be read): opening the app writes nothing. */
    if (rift_store_load(&a->prefs) < 0) {
        LOG_WARN("rift: %s could not be read; using the defaults", rift_store_path());
    }
    a->prefs_saved = 1;
    /* Everything the model holds now is history: nothing has arrived yet. */
    rift_notify_init(&a->notify, &a->model, a->prefs.dm_sound);

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

    rift_tabs_build(a);

    a->content = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->content);
    lv_obj_set_width(a->content, LV_PCT(100));
    lv_obj_set_flex_grow(a->content, 1);
    lv_obj_remove_flag(a->content, LV_OBJ_FLAG_SCROLLABLE);

    a->activity_root = rift_activity_create(a, a->content);
    a->nodes_root = rift_nodes_create(a, a->content);
    a->comms_root = rift_comms_create(a, a->content);
    build_placeholder(a);
    build_cmdline(a);
    if (a->composer) {
        lv_obj_add_event_cb(a->composer, on_composer_key, LV_EVENT_KEY, a);
        lv_obj_add_event_cb(a->composer, on_composer_focus, LV_EVENT_FOCUSED, a);
        lv_obj_add_event_cb(a->composer, on_composer_focus, LV_EVENT_DEFOCUSED, a);
    }

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
        rift_app_notify_pass(a, rift_mono_ms());
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
    /* Nothing of RIFT's sounds after RIFT has gone. */
    rift_sound_stop();
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
    /* The touch keyboard is the shell's and outlives this app. An app that
     * left it up would hand the next screen a sheet over a third of it. */
    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    rift_comms_destroy(a);
    rift_nodes_destroy(a);
    rift_activity_destroy(a);
    /* The LVGL objects are children of the shell's body and are deleted
     * with it; the private blocks were this app's to release. */
    free(a);
}

/* The Back action (app.h `back`, hw_actions.h): the ways out RIFT already has on
 * screen and on Esc - a node's detail back to the list ("‹ NODES"), any
 * section back to Activity - and at Activity the shell's own back slab. */
static int rift_back(void *priv)
{
    struct rift_app *a = priv;

    if (!a) {
        return 0;
    }
    if (a->section == RIFT_SEC_NODES && a->detail_open) {
        rift_app_open_detail(a, 0);
        return 1;
    }
    if (a->section != RIFT_SEC_ACTIVITY) {
        rift_app_show_section(a, RIFT_SEC_ACTIVITY);
        return 1;
    }
    return 0;
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
    /* Fullscreen (DS §30.4 stage 2): no status bar in either orientation,
     * the body from the header down. RIFT writes no hint; its own tab row
     * and status line carry everything it has to say. */
    .chrome = POCKETOS_CHROME_NONE,
    /* Landscape draws its own top row - the section strip, with a back slab
     * - where the shell's 72 px header was (DS §37.2). */
    .header = POCKETOS_HEADER_NONE_LANDSCAPE,
    .back = rift_back,
};
