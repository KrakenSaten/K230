/*
 * PacketShark's confirmations (UI_UX.md §4.3, §5.5; DS §17.5): one sheet
 * over the app, a sentence that says what happens, the safe choice first
 * and accented, the other beside it; and the packet list's filter sheet
 * (M5). The save and files sheets are milestone M8.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void on_safe(lv_event_t *e)
{
    psk_sheet_answer(lv_event_get_user_data(e), false);
}

static void on_go(lv_event_t *e)
{
    psk_sheet_answer(lv_event_get_user_data(e), true);
}

void psk_sheet_build(struct psk_app *a)
{
    lv_obj_t *card;
    lv_obj_t *buttons;

    /* Over everything in the frame, outside its flex column. */
    a->sheet = psk_box(a->frame);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_CLICKABLE); /* takes taps meant for what is under it */
    pos_style_add(a->sheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(a->sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(a->sheet, PSK_TAB_H + 14, 0);

    card = pocketui_card(a->sheet);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    a->sheet_title = pocketui_label(card, "", POS_STYLE_TITLE);
    lv_label_set_long_mode(a->sheet_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->sheet_title, LV_PCT(100));
    a->sheet_text = pocketui_label(card, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->sheet_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->sheet_text, LV_PCT(100));
    lv_obj_set_style_pad_bottom(a->sheet_text, 8, 0);

    buttons = psk_box(card);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, PSK_GAP, 0);
    a->sheet_safe = psk_button(buttons, "", true, on_safe, a);
    a->sheet_go = psk_button(buttons, "", false, on_go, a);
    lv_obj_set_flex_grow(a->sheet_safe, 1);
    lv_obj_set_flex_grow(a->sheet_go, 1);
    lv_obj_set_height(a->sheet_safe, POCKETUI_TOUCH_MIN);
    lv_obj_set_height(a->sheet_go, POCKETUI_TOUCH_MIN);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_HIDDEN);
    a->sheet_kind = PSK_SHEET_NONE;
}

void psk_sheet_show(struct psk_app *a, enum psk_sheet kind)
{
    char n[32];
    char text[320];
    const char *title = "";
    const char *safe = "STAY";
    const char *go = "";
    uint64_t packets = a->model->c.seen;
    bool saved = a->model->save == PSK_SAVE_DONE;
    bool saving = a->model->state == PPC_STATE_SAVING;

    psk_fmt_count(packets, n, sizeof(n));
    switch (kind) {
    case PSK_SHEET_FULL:
        title = "Keep full packets?";
        snprintf(text, sizeof(text),
                 "Full packets include whatever passes in clear text, such as passwords and message "
                 "contents. This applies to the next capture only.");
        safe = "HEADERS";
        go = "FULL";
        break;
    case PSK_SHEET_NOLIMIT:
        title = "Capture without a time limit?";
        snprintf(text, sizeof(text),
                 "The capture runs until you stop it or leave PacketShark, and the screen stays on "
                 "meanwhile. This applies to the next capture only.");
        safe = "CANCEL";
        go = "NO LIMIT";
        break;
    case PSK_SHEET_DISCARD:
    case PSK_SHEET_NEW:
        title = "Discard this capture?";
        snprintf(text, sizeof(text), "Its %s packets are removed from memory. %s", n,
                 saved ? "The file you saved stays in Captures." : "Nothing has been saved.");
        go = kind == PSK_SHEET_NEW ? "NEW CAPTURE" : "DISCARD";
        break;
    case PSK_SHEET_LEAVE:
        title = "Leave PacketShark?";
        snprintf(text, sizeof(text), "The capture stops and its %s packets are removed from memory. %s", n,
                 saving  ? "The file being saved is not finished and is removed."
                 : saved ? "The file you saved stays in Captures."
                         : "Nothing has been saved.");
        go = "LEAVE";
        break;
    case PSK_SHEET_SAVE: {
        char held[32];
        const struct ppc_counters *c = &a->model->c;
        uint64_t in_buffer = c->last_seq && c->last_seq >= c->first_seq ? (uint64_t)c->last_seq - c->first_seq + 1u : 0;

        /* Where the file goes, what is in it, and who can read it (ADR-016
         * rules 9 and 16). */
        title = "Save this capture?";
        snprintf(text, sizeof(text),
                 "The %s packets in the buffer are written to a .pcap file in Captures on this device, with %s. "
                 "Files in /root can be read over SSH on this image until a device password is set.",
                 psk_fmt_count(in_buffer, held, sizeof(held)),
                 a->keep == PPC_KEEP_FULL ? "everything that was captured of them" : "their headers");
        safe = "CANCEL";
        go = "SAVE";
        break;
    }
    case PSK_SHEET_DELETE:
        title = "Delete this capture?";
        snprintf(text, sizeof(text), "%s is removed from this device. This cannot be undone.", a->del_name);
        safe = "KEEP";
        go = "DELETE";
        break;
    case PSK_SHEET_NONE:
    default:
        psk_sheet_hide(a);
        return;
    }
    a->sheet_kind = kind;
    psk_text(a->sheet_title, title);
    psk_text(a->sheet_text, text);
    psk_text(lv_obj_get_child(a->sheet_safe, 0), safe);
    psk_text(lv_obj_get_child(a->sheet_go, 0), go);
    psk_set_hidden(a->sheet, false);
    lv_obj_move_foreground(a->sheet);
}

void psk_sheet_hide(struct psk_app *a)
{
    a->sheet_kind = PSK_SHEET_NONE;
    psk_set_hidden(a->sheet, true);
}

/* ---- the filter sheet (M5) -------------------------------------------------------------- *
 *
 * The list's display filter, as a draft: one protocol class or all of them,
 * and the host and the port a FILTER BY or an end's slab set (they can be
 * cleared here, not typed: there is no keyboard in this sheet), and PINNED
 * ONLY. SHOW applies it; Back or CANCEL leaves the list as it was. */

static const char *const class_choice[PPC_CLASS_COUNT + 1] = { "ALL", "TCP", "UDP", "DNS", "DHCP",
                                                                "ARP", "ICMP", "IPV6", "OTHER" };

/* The findings a view shows, in words: one finding's plural, or FINDINGS. */
static const char *signal_words(unsigned signals)
{
    unsigned i;

    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        if (signals == (1u << i)) {
            return psk_signal_plural(i);
        }
    }
    return signals ? "FINDINGS" : "ANY PACKET";
}

const char *psk_filter_words(uint8_t classes, const char *host, uint16_t port, bool pinned, unsigned signals,
                             unsigned diag, char *buf, size_t buflen)
{
    size_t o = 0;
    unsigned c;

    buf[0] = '\0';
    if (pinned) {
        o += (size_t)snprintf(buf + o, buflen - o, "PINNED");
    }
    if (signals && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, "%s%s", o ? " · " : "", signal_words(signals));
    }
    if (diag && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, "%s%s", o ? " · " : "", psk_diag_plural(diag));
    }
    if (classes != PPC_CLASSES_ALL && classes != 0) {
        for (c = 0; c < PPC_CLASS_COUNT && o < buflen; c++) {
            if (classes & (1u << c)) {
                o += (size_t)snprintf(buf + o, buflen - o, "%s%s", o ? " · " : "", psk_class_word(c));
            }
        }
    }
    if (host && host[0] && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, "%sHOST %s", o ? " · " : "", host);
    }
    if (port && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, "%sPORT %u", o ? " · " : "", (unsigned)port);
    }
    if (o == 0) {
        snprintf(buf, buflen, "ALL PROTOCOLS");
    }
    return buf;
}

static void fs_paint(struct psk_app *a)
{
    char n[32];
    char b[64];
    int i;

    for (i = 0; i <= PPC_CLASS_COUNT; i++) {
        bool on = i == 0 ? a->fs_classes == PPC_CLASSES_ALL : a->fs_classes == (uint8_t)(1u << (i - 1));
        lv_obj_t *lb = lv_obj_get_child(a->fs_cls[i], 0);

        /* Only a change is applied: adding a style that is there removes
         * and re-adds it. */
        if (on && a->fs_shown != i) {
            pos_style_add(a->fs_cls[i], POS_STYLE_SELECTED, 0);
            pos_style_add(lb, POS_STYLE_ACCENT_TEXT, 0);
        } else if (!on && a->fs_shown == i) {
            lv_obj_remove_style(a->fs_cls[i], pos_style(POS_STYLE_SELECTED), 0);
            lv_obj_remove_style(lb, pos_style(POS_STYLE_ACCENT_TEXT), 0);
        }
    }
    for (i = 0; i <= PPC_CLASS_COUNT; i++) {
        if (i == 0 ? a->fs_classes == PPC_CLASSES_ALL : a->fs_classes == (uint8_t)(1u << (i - 1))) {
            break;
        }
    }
    a->fs_shown = i <= PPC_CLASS_COUNT ? i : -1;
    psk_text(a->fs_host, a->fs_hostv[0] ? a->fs_hostv : "ANY");
    psk_set_hidden(lv_obj_get_child(a->fs_host_row, 2), a->fs_hostv[0] == '\0');
    if (a->fs_portv) {
        snprintf(b, sizeof(b), "%u", (unsigned)a->fs_portv);
    } else {
        snprintf(b, sizeof(b), "ANY");
    }
    psk_text(a->fs_port, b);
    psk_set_hidden(lv_obj_get_child(a->fs_port_row, 2), a->fs_portv == 0);
    snprintf(b, sizeof(b), "%s · %s HELD", a->fs_pinnedv ? "ON" : "OFF", psk_fmt_count(a->model->pins, n, sizeof(n)));
    psk_text(a->fs_pinned, b);
    if (a->fs_diagv) {
        psk_text(a->fs_sig, psk_diag_plural(a->fs_diagv));
    } else {
        psk_text(a->fs_sig, signal_words(a->fs_sigv));
    }
}

static void on_fs_class(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    a->fs_classes = i == 0 ? PPC_CLASSES_ALL : (uint8_t)(1u << (i - 1));
    fs_paint(a);
}

static void on_fs_row(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i == 0) {
        a->fs_hostv[0] = '\0';
    } else if (i == 1) {
        a->fs_portv = 0;
    } else if (i == 3) {
        /* Findings (M7): any packet, any finding, then each finding. A
         * diagnosis a diagnosis line chose (M11.1) gives way to them. */
        if (a->fs_diagv) {
            a->fs_diagv = 0;
            a->fs_sigv = 0;
        } else if (a->fs_sigv == 0) {
            a->fs_sigv = PPC_SIG_FINDINGS_ALL;
        } else if (a->fs_sigv == PPC_SIG_FINDINGS_ALL) {
            a->fs_sigv = 0x01;
        } else if (a->fs_sigv >= 0x20) {
            a->fs_sigv = 0;
        } else {
            a->fs_sigv = (uint8_t)(a->fs_sigv << 1);
        }
    } else {
        a->fs_pinnedv = !a->fs_pinnedv;
    }
    fs_paint(a);
}

static void on_fs_clear(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    a->fs_classes = PPC_CLASSES_ALL;
    a->fs_hostv[0] = '\0';
    a->fs_portv = 0;
    a->fs_pinnedv = false;
    a->fs_sigv = 0;
    a->fs_diagv = 0;
    fs_paint(a);
}

static void on_fs_show(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    bool to_packets = a->fs_to_packets;

    psk_fsheet_hide(a);
    psk_set_view_diag(a, a->fs_classes, a->fs_hostv, a->fs_portv, a->fs_pinnedv, a->fs_sigv, a->fs_diagv);
    if (to_packets) {
        psk_select_tab(a, PSK_TAB_PACKETS);
    }
}

static void on_fs_cancel(lv_event_t *e)
{
    psk_fsheet_hide(lv_event_get_user_data(e));
}

static lv_obj_t *fs_row(lv_obj_t *parent, const char *key, lv_obj_t **value, const char *action, struct psk_app *a,
                        intptr_t index)
{
    lv_obj_t *r = psk_box(parent);
    lv_obj_t *k;
    lv_obj_t *act;

    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, POCKETUI_ROW_H, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    pos_style_add(r, POS_STYLE_DIVIDER, 0);
    pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(r, (void *)index);
    lv_obj_add_event_cb(r, on_fs_row, LV_EVENT_CLICKED, a);
    k = pocketui_label(r, key, POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_width(k, LV_SIZE_CONTENT);
    *value = pocketui_label(r, "", POS_STYLE_VALUE);
    lv_label_set_long_mode(*value, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(*value, 1);
    lv_obj_set_flex_grow(*value, 1);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    act = pocketui_label(r, action, POS_STYLE_CAPTION);
    pos_style_add(act, POS_STYLE_ACCENT_TEXT, 0);
    return r;
}

void psk_fsheet_build(struct psk_app *a)
{
    lv_obj_t *card;
    lv_obj_t *grid;
    lv_obj_t *buttons;
    lv_obj_t *b;
    int i;

    a->fsheet = psk_box(a->frame);
    lv_obj_add_flag(a->fsheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->fsheet, LV_OBJ_FLAG_CLICKABLE);
    pos_style_add(a->fsheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(a->fsheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->fsheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(a->fsheet, PSK_TAB_H + 14, 0);
    lv_obj_set_style_pad_bottom(a->fsheet, 8, 0);
    /* Tall at a large text size and in landscape: the sheet scrolls. */
    lv_obj_add_flag(a->fsheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->fsheet, LV_DIR_VER);

    card = pocketui_card(a->fsheet);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(card, "Filter packets", POS_STYLE_TITLE);
    pocketui_label(card, "PROTOCOL", POS_STYLE_CAPTION);
    grid = psk_box(card);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, PSK_GAP, 0);
    lv_obj_set_style_pad_row(grid, PSK_GAP, 0);
    /* Room for the selected outline, which is drawn outside its button. */
    lv_obj_set_style_pad_all(grid, 3, 0);
    for (i = 0; i <= PPC_CLASS_COUNT; i++) {
        b = psk_button(grid, class_choice[i], false, on_fs_class, a);
        lv_obj_set_height(b, PSK_TAB_H);
        lv_obj_set_width(b, LV_PCT(31)); /* three to a row */
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        a->fs_cls[i] = b;
    }
    a->fs_host_row = fs_row(card, "Host", &a->fs_host, "CLEAR", a, 0);
    a->fs_port_row = fs_row(card, "Port", &a->fs_port, "CLEAR", a, 1);
    fs_row(card, "Pinned only", &a->fs_pinned, "", a, 2);
    fs_row(card, "Findings", &a->fs_sig, "CHANGE", a, 3);
    b = pocketui_label(card, "Host and port come from FILTER BY in INSPECT or from a packet's ends.",
                       POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, LV_PCT(100));

    buttons = psk_box(card);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, PSK_GAP, 0);
    b = psk_button(buttons, "CANCEL", false, on_fs_cancel, a);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, POCKETUI_TOUCH_MIN);
    b = psk_button(buttons, "CLEAR ALL", false, on_fs_clear, a);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, POCKETUI_TOUCH_MIN);
    b = psk_button(buttons, "SHOW", true, on_fs_show, a);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, POCKETUI_TOUCH_MIN);
    lv_obj_add_flag(a->fsheet, LV_OBJ_FLAG_HIDDEN);
    a->fs_shown = -1;
}

void psk_fsheet_show(struct psk_app *a, bool to_packets)
{
    a->fs_classes = a->classes;
    snprintf(a->fs_hostv, sizeof(a->fs_hostv), "%s", a->host);
    a->fs_portv = a->port;
    a->fs_pinnedv = a->pinned_only;
    a->fs_sigv = a->sigmask;
    a->fs_diagv = a->diagf;
    a->fs_to_packets = to_packets;
    fs_paint(a);
    psk_set_hidden(a->fsheet, false);
    lv_obj_scroll_to_y(a->fsheet, 0, LV_ANIM_OFF);
    lv_obj_move_foreground(a->fsheet);
}

void psk_fsheet_hide(struct psk_app *a)
{
    psk_set_hidden(a->fsheet, true);
}

bool psk_fsheet_shown(const struct psk_app *a)
{
    return a->fsheet && !lv_obj_has_flag(a->fsheet, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the saved captures (M8; ADR-016 rules 9, 10 and 16) -------------------------------- *
 *
 * The newest PSK_SAVES_ROWS files of the captures folder, each with its size
 * and age; a tap on one asks whether to delete it. The app lists and deletes
 * (psk_saves.c); it never opens a capture. */

static void on_ss_row(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    psk_saved_delete_ask(a, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)));
}

static void on_ss_close(lv_event_t *e)
{
    psk_ssheet_hide(lv_event_get_user_data(e));
}

void psk_ssheet_build(struct psk_app *a)
{
    lv_obj_t *card;
    lv_obj_t *b;
    lv_obj_t *buttons;
    int i;

    a->ssheet = psk_box(a->frame);
    lv_obj_add_flag(a->ssheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->ssheet, LV_OBJ_FLAG_CLICKABLE);
    pos_style_add(a->ssheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(a->ssheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->ssheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(a->ssheet, PSK_TAB_H + 14, 0);
    lv_obj_set_style_pad_bottom(a->ssheet, 8, 0);
    lv_obj_add_flag(a->ssheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->ssheet, LV_DIR_VER);

    card = pocketui_card(a->ssheet);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(card, "Saved captures", POS_STYLE_TITLE);
    a->ss_note = pocketui_label(card, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->ss_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->ss_note, LV_PCT(100));
    for (i = 0; i < PSK_SAVES_ROWS; i++) {
        lv_obj_t *r = psk_box(card);
        lv_obj_t *left = psk_box(r);
        lv_obj_t *act;

        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(r, POCKETUI_ROW_H, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 8, 0);
        lv_obj_set_style_pad_ver(r, 6, 0);
        pos_style_add(r, POS_STYLE_DIVIDER, 0);
        pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_ss_row, LV_EVENT_CLICKED, a);
        lv_obj_set_width(left, 1);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 2, 0);
        lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
        a->ss_name[i] = pocketui_label(left, "", POS_STYLE_VALUE);
        lv_label_set_long_mode(a->ss_name[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->ss_name[i], LV_PCT(100));
        a->ss_sub[i] = pocketui_label(left, "", POS_STYLE_CAPTION);
        lv_label_set_long_mode(a->ss_sub[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->ss_sub[i], LV_PCT(100));
        act = pocketui_label(r, "DELETE", POS_STYLE_CAPTION);
        pos_style_add(act, POS_STYLE_ACCENT_TEXT, 0);
        a->ss_row[i] = r;
    }
    buttons = psk_box(card);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, POCKETUI_TOUCH_MIN);
    lv_obj_set_style_margin_top(buttons, 8, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    b = psk_button(buttons, "CLOSE", true, on_ss_close, a);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, POCKETUI_TOUCH_MIN);
    lv_obj_add_flag(a->ssheet, LV_OBJ_FLAG_HIDDEN);
}

void psk_ssheet_paint(struct psk_app *a)
{
    char note[256];
    char n[32];
    int64_t now = (int64_t)time(NULL);
    int i;

    if (a->saves_count == 0) {
        snprintf(note, sizeof(note),
                 "No capture has been saved. STOP a capture and press SAVE to keep it as a .pcap file in Captures.");
    } else {
        snprintf(note, sizeof(note),
                 "%u in Captures on this device, %s%s. Files in /root can be read over SSH on this image until a "
                 "device password is set. Tap one to delete it.",
                 (unsigned)a->saves_count, psk_fmt_bytes(a->saves_bytes, n, sizeof(n)),
                 a->saves_count > (uint32_t)a->nsaves ? "; the newest are listed" : "");
    }
    psk_text(a->ss_note, note);
    for (i = 0; i < PSK_SAVES_ROWS; i++) {
        char sub[64];
        char age[24];

        psk_set_hidden(a->ss_row[i], i >= a->nsaves);
        if (i >= a->nsaves) {
            continue;
        }
        psk_text(a->ss_name[i], a->saves[i].name);
        psk_saves_age(a->saves[i].mtime, now, age, sizeof(age));
        snprintf(sub, sizeof(sub), "%s%s%s", psk_fmt_bytes(a->saves[i].bytes, n, sizeof(n)), age[0] ? " · " : "", age);
        psk_text(a->ss_sub[i], sub);
    }
}

void psk_ssheet_show(struct psk_app *a)
{
    psk_saves_refresh(a);
    psk_ssheet_paint(a);
    psk_set_hidden(a->ssheet, false);
    lv_obj_scroll_to_y(a->ssheet, 0, LV_ANIM_OFF);
    lv_obj_move_foreground(a->ssheet);
}

void psk_ssheet_hide(struct psk_app *a)
{
    psk_set_hidden(a->ssheet, true);
}

bool psk_ssheet_shown(const struct psk_app *a)
{
    return a->ssheet && !lv_obj_has_flag(a->ssheet, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the diagnosis sheet (M11.1) --------------------------------------------------------- *
 *
 * The DNS or TCP diagnosis line by line: how far it can be trusted, then the
 * summary and each kind of finding or non-finding there is, with its
 * severity in words and colour (status_ok, plain, status_warn with `!`), an
 * explanation, and where it leads: LIST its packets, or OPEN the packet it
 * names. Repainted while it shows, as the helper's diag line comes. */

static void on_ds_row(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    unsigned i = (unsigned)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i < a->ds_n) {
        struct psk_diag_line l = a->ds_lines[i];

        psk_diag_go(a, &l);
    }
}

static void on_ds_close(lv_event_t *e)
{
    psk_dsheet_hide(lv_event_get_user_data(e));
}

void psk_dsheet_build(struct psk_app *a)
{
    lv_obj_t *card;
    lv_obj_t *b;
    lv_obj_t *buttons;
    int i;

    a->dsheet = psk_box(a->frame);
    lv_obj_add_flag(a->dsheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->dsheet, LV_OBJ_FLAG_CLICKABLE);
    pos_style_add(a->dsheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(a->dsheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->dsheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(a->dsheet, PSK_TAB_H + 14, 0);
    lv_obj_set_style_pad_bottom(a->dsheet, 8, 0);
    lv_obj_add_flag(a->dsheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->dsheet, LV_DIR_VER);

    card = pocketui_card(a->dsheet);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    a->ds_title = pocketui_label(card, "", POS_STYLE_TITLE);
    a->ds_note = pocketui_label(card, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->ds_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->ds_note, LV_PCT(100));
    for (i = 0; i < PSK_DIAG_LINES; i++) {
        lv_obj_t *r = psk_box(card);
        lv_obj_t *left = psk_box(r);

        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(r, POCKETUI_ROW_H, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 8, 0);
        lv_obj_set_style_pad_ver(r, 8, 0);
        pos_style_add(r, POS_STYLE_DIVIDER, 0);
        pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_ds_row, LV_EVENT_CLICKED, a);
        lv_obj_set_width(left, 1);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 2, 0);
        lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
        a->ds_text[i] = pocketui_label(left, "", POS_STYLE_VALUE);
        lv_label_set_long_mode(a->ds_text[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->ds_text[i], LV_PCT(100));
        a->ds_why[i] = pocketui_label(left, "", POS_STYLE_CAPTION);
        lv_label_set_long_mode(a->ds_why[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->ds_why[i], LV_PCT(100));
        a->ds_go[i] = pocketui_label(r, "", POS_STYLE_CAPTION);
        pos_style_add(a->ds_go[i], POS_STYLE_ACCENT_TEXT, 0);
        a->ds_tone[i] = -1;
        a->ds_row[i] = r;
    }
    buttons = psk_box(card);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, POCKETUI_TOUCH_MIN);
    lv_obj_set_style_margin_top(buttons, 8, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    b = psk_button(buttons, "CLOSE", true, on_ds_close, a);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, POCKETUI_TOUCH_MIN);
    lv_obj_add_flag(a->dsheet, LV_OBJ_FLAG_HIDDEN);
}

/* A line's severity as a colour over its own role; only a change is applied. */
static void ds_tone(struct psk_app *a, int i, int sev)
{
    if (a->ds_tone[i] == sev) {
        return;
    }
    lv_obj_remove_style(a->ds_text[i], pos_style(POS_STYLE_STATUS_OK_TEXT), 0);
    lv_obj_remove_style(a->ds_text[i], pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    if (sev == PSK_SEV_OK) {
        pos_style_add(a->ds_text[i], POS_STYLE_STATUS_OK_TEXT, 0);
    } else if (sev == PSK_SEV_WARN) {
        pos_style_add(a->ds_text[i], POS_STYLE_STATUS_WARN_TEXT, 0);
    }
    a->ds_tone[i] = sev;
}

void psk_dsheet_paint(struct psk_app *a)
{
    char note[512];
    unsigned i;
    bool present = psk_capture_present(a);

    a->ds_n = psk_diag_lines(a->model, a->ds_kind, a->ds_lines, PSK_DIAG_LINES);
    psk_text(a->ds_title, a->ds_kind == PSK_DIAG_DNS ? "DNS diagnosis" : "TCP diagnosis");
    psk_text(a->ds_note, psk_diag_confidence(a->model, a->ds_kind, note, sizeof(note)));
    for (i = 0; i < PSK_DIAG_LINES; i++) {
        const struct psk_diag_line *l = &a->ds_lines[i];
        char t[sizeof(l->text) + 4];
        bool go;

        psk_set_hidden(a->ds_row[i], i >= a->ds_n);
        if (i >= a->ds_n) {
            continue;
        }
        /* status_warn always with `!` and a word (DS §59.2). */
        snprintf(t, sizeof(t), "%s%s", l->sev == PSK_SEV_WARN ? "! " : "", l->text);
        psk_text(a->ds_text[i], t);
        ds_tone(a, (int)i, l->sev);
        psk_text(a->ds_why[i], l->why);
        go = present && l->go != PSK_GO_NONE;
        psk_text(a->ds_go[i], !go ? "" : l->go == PSK_GO_OPEN ? "OPEN" : "LIST");
        psk_set_hidden(a->ds_go[i], !go);
        if (go) {
            lv_obj_add_flag(a->ds_row[i], LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_clear_flag(a->ds_row[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void psk_dsheet_show(struct psk_app *a, unsigned kind)
{
    a->ds_kind = kind < PSK_DIAG_KINDS ? kind : PSK_DIAG_DNS;
    psk_dsheet_paint(a);
    psk_set_hidden(a->dsheet, false);
    lv_obj_scroll_to_y(a->dsheet, 0, LV_ANIM_OFF);
    lv_obj_move_foreground(a->dsheet);
}

void psk_dsheet_hide(struct psk_app *a)
{
    if (a->dsheet) {
        psk_set_hidden(a->dsheet, true);
    }
}

bool psk_dsheet_shown(const struct psk_app *a)
{
    return a->dsheet && !lv_obj_has_flag(a->dsheet, LV_OBJ_FLAG_HIDDEN);
}
