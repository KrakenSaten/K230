/*
 * Settings > Network: Wi-Fi, its networks and the join sheet. Moved here from
 * the one screen Settings used to be (DS §52); what it does is unchanged.
 *
 * Wi-Fi is netd's (wifi.* over pocketipc, docs/api/network.md). Every call
 * carries the UI deadline, and netd answers every request at once, so
 * nothing here waits on a scan or a join - the app polls wifi.status once a
 * second and shows what it says.
 *
 * A passphrase exists only in the text field while it is typed and in the
 * one wifi.connect request it is sent with. It is cleared from the field
 * before the sheet closes, never logged, and never shown unless the person
 * taps Show.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Wi-Fi actions --------------------------------------------------------------- */

static void on_toggle(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    cJSON *params = cJSON_CreateObject();
    char err[96];
    cJSON *res;

    cJSON_AddBoolToObject(params, "enabled", !a->wifi.enabled);
    res = settings_netd("wifi.set_enabled", params, err, sizeof(err));
    if (res) {
        sv_wifi_apply_status(&a->wifi, res);
        cJSON_Delete(res);
    } else {
        settings_poll_status(a);
    }
    settings_poll_networks(a);
    settings_rebuild(a);
}

static void on_scan(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[96];
    cJSON *res = settings_netd("wifi.scan", NULL, err, sizeof(err));

    if (res) {
        a->wifi.scanning = 1;
        a->wifi.can_scan = 0;
        cJSON_Delete(res);
    }
    settings_poll_networks(a);
    settings_repaint(a);
}

static void on_disconnect(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[96];
    cJSON *res;

    pocketos_shell_keyboard_hide();
    res = settings_netd("wifi.disconnect", NULL, err, sizeof(err));
    if (res) {
        sv_wifi_apply_status(&a->wifi, res);
        cJSON_Delete(res);
    }
    settings_poll_networks(a);
    settings_go(a, PAGE_NETWORK);
}

static void on_row(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(row);

    if (i < 0 || i >= a->wifi.net_count) {
        return;
    }
    a->sel = a->wifi.nets[i];
    a->sel_kind = sv_join_kind(&a->sel);
    /* One level in, as a category is from the list: the header names the
     * page, and its back slab and Back come back here. */
    a->page = PAGE_SHEET;
    pocketos_shell_set_title(settings_page_title(PAGE_SHEET));
    settings_rebuild(a);
}

/* ---- the network sheet --------------------------------------------------------------- */

static void clear_field(struct settings_app *a)
{
    if (a->w.field) {
        lv_textarea_set_text(a->w.field, "");
    }
}

void settings_sheet_close(struct settings_app *a)
{
    clear_field(a);
    pocketos_shell_keyboard_hide();
    explicit_bzero(&a->sel, sizeof(a->sel));
    settings_go(a, PAGE_NETWORK);
}

static void on_cancel(lv_event_t *e)
{
    settings_sheet_close(lv_event_get_user_data(e));
}

/* The field and its error caption, brought into view in the sheet. Above the
 * keyboard in landscape the body is 100 px tall, and a caption that appears
 * under the field lands below it: the sheet scrolls just far enough for the
 * caption to be read, which leaves the field in view with it. Anywhere the
 * sheet already fits, nothing moves. */
static void reveal_field(struct settings_app *a)
{
    if (a->page != PAGE_SHEET || !a->w.field) {
        return;
    }
    lv_obj_update_layout(a->body);
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(a->w.field), LV_ANIM_OFF);
}

/* The same once the body has changed size. That is learned in the middle of
 * LVGL's layout pass, before the sheet inside has been laid out for the new
 * size, so the field is brought into view straight after the pass instead. */
static void reveal_field_later(void *user)
{
    reveal_field(user);
}

void settings_sheet_after_layout(struct settings_app *a)
{
    if (a->w.field) {
        lv_async_call_cancel(reveal_field_later, a);
        lv_async_call(reveal_field_later, a);
    }
}

void settings_wifi_teardown(struct settings_app *a)
{
    lv_async_call_cancel(reveal_field_later, a);
    clear_field(a);
    explicit_bzero(&a->sel, sizeof(a->sel));
}

static void show_sheet_error(struct settings_app *a, const char *msg)
{
    if (a->w.field && a->sel_kind == SV_JOIN_PASSPHRASE) {
        pocketui_text_field_set_error(a->w.field, msg);
        reveal_field(a);
    } else if (a->w.sheet_error) {
        lv_label_set_text(a->w.sheet_error, msg);
        settings_set_hidden(a->w.sheet_error, 0);
    }
}

static void do_join(struct settings_app *a)
{
    const char *pass = NULL;
    char why[96];
    char err[160];
    cJSON *params;
    cJSON *res;

    if (a->sel_kind == SV_JOIN_PASSPHRASE) {
        pass = a->w.field ? lv_textarea_get_text(a->w.field) : NULL;
        if (sv_passphrase_check(pass, why, sizeof(why)) < 0) {
            show_sheet_error(a, why);
            return;
        }
    }
    params = sv_connect_params(&a->sel, a->sel_kind, pass);
    if (!params) {
        return;
    }
    res = settings_netd("wifi.connect", params, err, sizeof(err));
    if (!res) {
        show_sheet_error(a, err[0] ? err : "Wi-Fi did not answer");
        return;
    }
    cJSON_Delete(res);
    settings_poll_status(a);
    settings_sheet_close(a);
}

static void on_join(lv_event_t *e)
{
    do_join(lv_event_get_user_data(e));
}

static void on_keyboard_done(void *user)
{
    do_join(user);
}

static void on_forget(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    char err[160];
    cJSON *res = settings_netd("wifi.forget", sv_ssid_params(&a->sel), err, sizeof(err));

    if (!res) {
        show_sheet_error(a, err[0] ? err : "Wi-Fi did not answer");
        return;
    }
    cJSON_Delete(res);
    settings_poll_status(a);
    settings_poll_networks(a);
    settings_sheet_close(a);
}

static void on_show(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    bool hidden;

    if (!a->w.field) {
        return;
    }
    hidden = !lv_textarea_get_password_mode(a->w.field);
    lv_textarea_set_password_mode(a->w.field, hidden);
    lv_label_set_text(a->w.show_label, hidden ? "SHOW" : "HIDE");
}

/* One panel in two sides: what the network is, then the field and what can be
 * done - one above the other when the body is tall, side by side when it is
 * wide (settings_sheet_shape). Tall, the sides are invisible: the panel's own
 * 12 px gap between them is the gap there always was between the text and
 * the field. */
void settings_sheet_build(struct settings_app *a)
{
    lv_obj_t *p = pocketui_card(a->body);
    lv_obj_t *buttons;
    lv_obj_t *b;
    char body[256];
    int i;

    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_style_pad_column(p, POCKETUI_PAD, 0);
    a->w.sheet = p;
    for (i = 0; i < 2; i++) {
        a->w.sheet_side[i] = settings_stack(p, 12);
        /* Layout boxes, not targets: a tap between the controls reaches the
         * panel as it always did, and never takes focus off the field. */
        lv_obj_clear_flag(a->w.sheet_side[i], LV_OBJ_FLAG_CLICKABLE);
    }
    p = a->w.sheet_side[0];
    pocketui_label(p, "NETWORK", POS_STYLE_CAPTION);
    settings_wrap_label(p, a->sel.ssid, POS_STYLE_TITLE);
    sv_join_text(&a->sel, body, sizeof(body));
    settings_wrap_label(p, body, a->sel_kind == SV_JOIN_OPEN ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_TEXT_SECONDARY);

    p = a->w.sheet_side[1];
    if (a->sel_kind == SV_JOIN_PASSPHRASE) {
        lv_obj_t *r;

        a->w.field = pocketui_text_field(p, "Passphrase", true);
        lv_textarea_set_password_mode(a->w.field, true);
        lv_textarea_set_max_length(a->w.field, 63);
        r = settings_hrow(p, SETTINGS_BTN_H);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        b = settings_button(r, "SHOW", on_show, a, 0);
        lv_obj_set_width(b, SETTINGS_TOGGLE_W);
        a->w.show_label = lv_obj_get_child(b, 0);
    }
    a->w.sheet_error = settings_wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);
    settings_set_hidden(a->w.sheet_error, 1);

    buttons = settings_hrow(p, SETTINGS_BTN_H);
    /* Cancel first, and the accent on the action a person came here for -
     * except on an open network, where joining is the one to think twice
     * about (DS §17.5 dialog rule). */
    switch (a->sel_kind) {
    case SV_JOIN_PASSPHRASE:
        lv_obj_set_flex_grow(settings_button(buttons, "CANCEL", on_cancel, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "JOIN", on_join, a, 1), 1);
        break;
    case SV_JOIN_SAVED:
        lv_obj_set_flex_grow(settings_button(buttons, "CANCEL", on_cancel, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "FORGET", on_forget, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "JOIN", on_join, a, 1), 1);
        break;
    case SV_JOIN_OPEN:
        lv_obj_set_flex_grow(settings_button(buttons, "CANCEL", on_cancel, a, 1), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "JOIN ANYWAY", on_join, a, 0), 1);
        break;
    case SV_JOIN_CONNECTED:
        lv_obj_set_flex_grow(settings_button(buttons, "CANCEL", on_cancel, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "FORGET", on_forget, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(buttons, "DISCONNECT", on_disconnect, a, 1), 1);
        break;
    case SV_JOIN_UNSUPPORTED:
    default:
        lv_obj_set_flex_grow(settings_button(buttons, "BACK", on_cancel, a, 1), 1);
        break;
    }

    /* Shaped before the field takes focus, so that anything focus scrolls
     * into view is where it will be seen. */
    settings_shape(a);
    if (a->w.field) {
        pos_input_focus(a->w.field);
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, on_keyboard_done, a);
    }
}

/* The sheet's two sides: stacked when tall, equal halves side by side when
 * wide, both from the top of the panel. The page scrolls in either shape. */
void settings_sheet_shape(struct settings_app *a)
{
    int i;

    if (!a->w.sheet || !a->w.sheet_side[0] || !a->w.sheet_side[1]) {
        return;
    }
    lv_obj_set_width(a->w.sheet, LV_PCT(100));
    lv_obj_set_flex_flow(a->w.sheet, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(a->w.sheet_side[i], a->wide ? 1 : 0);
    }
}

/* ---- the Network page ------------------------------------------------------------------ */

static void build_list(struct settings_app *a)
{
    int i;

    lv_obj_clean(a->w.list);
    for (i = 0; i < a->wifi.net_count; i++) {
        const struct sv_network *n = &a->wifi.nets[i];
        lv_obj_t *row = lv_obj_create(a->w.list);
        lv_obj_t *left;
        lv_obj_t *lb;

        lv_obj_remove_style_all(row);
        pos_style_add(row, POS_STYLE_DIVIDER, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, POCKETUI_ROW_H + 8);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);

        left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lb = pocketui_label(left, n->ssid, POS_STYLE_ROW_TITLE);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, LV_PCT(100));
        /* No taller than the row leaves above its caption, so the dots come
         * on the last line that fits: a dotted label of automatic height
         * wraps instead, and at a larger text size a long SSID ran out of
         * the row (DS §46.5). What fitted before fits as it did. */
        pocketui_label_fit(lb, (int)((POCKETUI_ROW_H + 8 - pocketui_role_line_height(POS_STYLE_CAPTION)) /
                                     pocketui_role_line_height(POS_STYLE_ROW_TITLE)));
        /* "not supported" is in the words; a muted role would also change
         * the font, and colour must not carry it alone (DS §2). */
        pocketui_label(left, n->detail, POS_STYLE_CAPTION);
        if (n->badge[0]) {
            lb = lv_label_create(row);
            pos_style_add(lb, POS_STYLE_CHIP, 0);
            pos_style_add(lb, n->connected ? POS_STYLE_CHIP_ACTIVE : POS_STYLE_CHIP_OFF, 0);
            lv_label_set_text(lb, n->badge);
        }
    }
}

static void list_signature(const struct settings_app *a, char *out, size_t n)
{
    size_t o = 0;
    int i;

    out[0] = '\0';
    for (i = 0; i < a->wifi.net_count && o < n; i++) {
        const struct sv_network *net = &a->wifi.nets[i];

        o += (size_t)snprintf(out + o, n - o, "%s|%s|%s;", net->ssid_hex, net->detail, net->badge);
    }
}

/* One panel: the switch and the state, SCAN and DISCONNECT, and the list,
 * which is the long part and scrolls with the page. */
void settings_network_build(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "WI-FI", COL_LEFT);
    lv_obj_t *r;

    r = settings_hrow(p, SETTINGS_BTN_H);
    pocketui_label(r, "Wi-Fi", POS_STYLE_ROW_TITLE);
    a->w.toggle = settings_button(r, "OFF", on_toggle, a, 0);
    lv_obj_set_width(a->w.toggle, SETTINGS_TOGGLE_W);
    a->w.toggle_label = lv_obj_get_child(a->w.toggle, 0);
    a->w.headline = settings_wrap_label(p, "", POS_STYLE_TEXT_PRIMARY);
    a->w.detail = settings_wrap_label(p, "", POS_STYLE_CAPTION);
    a->w.store_note = settings_wrap_label(p, "", POS_STYLE_STATUS_WARN_TEXT);

    r = settings_hrow(p, SETTINGS_BTN_H);
    a->w.scan_btn = settings_button(r, "SCAN", on_scan, a, 1);
    lv_obj_set_flex_grow(a->w.scan_btn, 1);
    a->w.disc_btn = settings_button(r, "DISCONNECT", on_disconnect, a, 0);
    lv_obj_set_flex_grow(a->w.disc_btn, 1);
    a->built_list_visible = a->wifi.list_visible;
    settings_set_hidden(r, !a->wifi.list_visible);

    a->w.list = lv_obj_create(p);
    lv_obj_remove_style_all(a->w.list);
    lv_obj_set_width(a->w.list, LV_PCT(100));
    lv_obj_set_height(a->w.list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->w.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(a->w.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    settings_set_hidden(a->w.list, !a->wifi.list_visible);
    list_signature(a, a->list_sig, sizeof(a->list_sig));
    build_list(a);
    a->w.list_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
}

void settings_network_repaint(struct settings_app *a)
{
    const struct sv_wifi *w = &a->wifi;
    char sig[sizeof(a->list_sig)];

    if (!a->w.toggle) {
        return;
    }
    if (w->list_visible != a->built_list_visible) {
        settings_rebuild(a);
        return;
    }
    list_signature(a, sig, sizeof(sig));
    if (strcmp(sig, a->list_sig) != 0) {
        memcpy(a->list_sig, sig, sizeof(sig));
        build_list(a);
    }
    lv_label_set_text(a->w.toggle_label, w->enabled ? "ON" : "OFF");
    settings_accent(a->w.toggle, w->enabled);
    settings_set_enabled(a->w.toggle, w->toggle_enabled);
    lv_label_set_text(a->w.headline, w->headline);
    settings_set_tone(a->w.headline, w->tone);
    lv_label_set_text(a->w.detail, w->detail);
    settings_set_hidden(a->w.detail, w->detail[0] == '\0');
    lv_label_set_text(a->w.store_note, w->store_note);
    settings_set_hidden(a->w.store_note, w->store_note[0] == '\0');
    settings_set_enabled(a->w.scan_btn, w->can_scan);
    lv_label_set_text(lv_obj_get_child(a->w.scan_btn, 0), w->scanning ? "SCANNING" : "SCAN");
    settings_set_enabled(a->w.disc_btn, w->can_disconnect);
    lv_label_set_text(a->w.list_note, w->list_visible ? w->list_note : "");
    settings_set_hidden(a->w.list_note, !w->list_visible || w->list_note[0] == '\0');
}
