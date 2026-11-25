/*
 * Settings > Companion: DOORS Companion on this unit, with no cable, SSH or
 * command line (ADR-019 amendment 2; DS §52.8).
 *
 *   Companion          the gateway's switch and state; the device password
 *                      and what SSH allows; the paired phones; Pair a phone
 *   > a phone          its name, key and connection; what it may do, one
 *                      switch per scope; Remove, asked twice
 *   > Pair a phone     a QR code with a countdown and Cancel; when a phone has
 *                      presented the code, its name and key, a "same key on
 *                      the phone" confirmation that ALLOW waits for, what it
 *                      will be allowed, REFUSE and ALLOW
 *   > Device password  typed twice, one card as the Wi-Fi sheet
 *
 * Nothing is decided or stored here. The switch and the password are sysd's
 * (companion.*: it refuses the gateway without a password, hardens SSH first
 * and runs S70gatewayd itself); the phones, their permissions and pairing
 * are gatewayd's (its root-only local socket, docs/api/gateway.md section
 * 10). Every control shows what its owner says after the tap, asked again,
 * never what was hoped for, and a refusal is said in words a person can act
 * on (settings_view.c, sv_companion_error_text).
 *
 * What makes the controls safe is where they are: taps on the unit's own
 * screen behind its lock. This app serves no IPC method; a phone reaches
 * neither socket and cannot raise its own scopes (ADR-019 rule 24). A
 * pairing window is opened only by a tap here and closes when the sheet
 * goes (Cancel, Back, Home, the app closing). The QR code is drawn by LVGL
 * from the URI (ADR-007: no TLS and no gateway code in the shell); the URI
 * holds the window's one-time token, lives in this app's memory only while
 * the window is open, and is wiped when it closes. A typed password lives in
 * the two fields and in the one companion.set_password request; the fields
 * are cleared before that request is sent, and when the sheet or the app
 * goes. Nothing here logs.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings_internal.h"
#include "shell_ipc.h"
#include "src/libs/qrcode/qrcodegen.h"

#include <stdio.h>
#include <string.h>

/* A tap waits this long for gatewayd or sysd. gatewayd writes its list to
 * storage before it answers; sysd answers at once and works in a child. */
#define COMPANION_TAP_TIMEOUT_MS 1500
/* ALLOW: gatewayd writes the paired list and flushes it before it answers. */
#define COMPANION_DECIDE_TIMEOUT_MS 3000
/* A gatewayd that has answered and then misses a poll is busy, not gone (it
 * writes its list to storage, it shakes hands with a phone, and there is one
 * hart): the list on show stays for this many polls in a row before the page
 * says it is not answering. */
#define COMPANION_MISSES 3
/* The gateway's switch says STARTING and STOPPING: wider than ON/OFF. */
#define COMPANION_SWITCH_W 168
/* The QR code is never drawn larger than this (px): bigger is not easier
 * for a phone's camera, and it would push the instructions off the page. */
#define COMPANION_QR_MAX 456

static void go_companion(struct settings_app *a);

/* ---- asking -------------------------------------------------------------------------- */

static cJSON *ask(const char *service, const char *method, cJSON *params, int timeout_ms, char *err, size_t n)
{
    err[0] = '\0';
    return shell_ipc_call_timeout(service, method, params, timeout_ms, err, n);
}

/* What a refusal means, for a person: sysd's "<word>: <sentence>" or
 * gatewayd's own message, turned into what to do. */
static void say_refusal(char *out, size_t n, const char *err, const char *what)
{
    char word[48];

    sv_companion_error_word(err, word, sizeof(word));
    if (!err || !err[0]) {
        snprintf(out, n, "%s did not answer. Try again in a moment.", what);
        return;
    }
    snprintf(out, n, "%s", sv_companion_error_text(word, ""));
    if (!out[0]) {
        snprintf(out, n, "%s refused: %.120s", what, err);
    }
}

void settings_poll_companion(struct settings_app *a)
{
    char err[160];
    /* sysd first: it says whether the gateway is meant to run, and a change
     * it reports done has already been answered by gatewayd. */
    cJSON *sys = ask("sysd", "companion.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    cJSON *st = ask("gatewayd", "gateway.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    cJSON *cl = st ? ask("gatewayd", "gateway.clients", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err)) : NULL;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(sys, "state");
    /* Switched off, and not being switched: a silent gatewayd is gone, not
     * busy, and its list goes at once. */
    bool off = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(sys, "enabled")) &&
               !(cJSON_IsString(state) && strcmp(state->valuestring, "running") == 0);

    if ((!st || !cl) && !off && a->companion.reachable && ++a->companion_missed < COMPANION_MISSES) {
        /* Kept as it was: a missed poll does not blank the list (as Wi-Fi's).
         * sysd's side is read again, so a change it runs is still followed. */
        sv_companion_system(&a->companion, sys);
    } else {
        if (st && cl) {
            a->companion_missed = 0;
        }
        sv_companion_apply(&a->companion, st, cl);
        sv_companion_system(&a->companion, sys);
    }
    /* "Password set." is for the person who set it, not for every visit
     * afterwards while sysd remembers its last job. */
    if (!a->pw_sent && a->companion.pw_note_tone == SV_TONE_OK) {
        a->companion.pw_note[0] = '\0';
    }
    cJSON_Delete(st);
    cJSON_Delete(cl);
    cJSON_Delete(sys);
}

/* Whether the rows on screen are the rows of the list as it is now. */
static bool rows_current(const struct settings_app *a)
{
    char sig[sizeof(a->phone_sig)];

    sv_companion_signature(&a->companion, sig, sizeof(sig));
    return strcmp(sig, a->phone_sig) == 0;
}

/* ---- small builders --------------------------------------------------------------------- */

/* A row: a column of words on the left that takes the room, and whatever the
 * caller puts on its right. As tall as its words need, never under a touch
 * target. */
static lv_obj_t *word_row(lv_obj_t *panel, lv_obj_t **left)
{
    lv_obj_t *row = settings_hrow(panel, LV_SIZE_CONTENT);

    lv_obj_set_style_min_height(row, SETTINGS_BTN_H, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    *left = lv_obj_create(row);
    lv_obj_remove_style_all(*left);
    lv_obj_set_height(*left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(*left, 1);
    lv_obj_set_flex_flow(*left, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(*left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static void paint_label(lv_obj_t *lb, const char *text, enum sv_tone tone)
{
    if (!lb) {
        return;
    }
    if (strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
    settings_set_tone(lb, tone);
    settings_set_hidden(lb, text[0] == '\0');
}

/* The six permissions as switches, each with what it means under its title. */
static void scope_rows(struct settings_app *a, lv_obj_t *panel, lv_event_cb_t cb)
{
    int i;

    for (i = 0; i < SV_SCOPES; i++) {
        lv_obj_t *left;
        lv_obj_t *row = word_row(panel, &left);

        pos_style_add(row, POS_STYLE_DIVIDER, 0);
        settings_wrap_label(left, sv_scope_table[i].title, POS_STYLE_ROW_TITLE);
        settings_wrap_label(left, sv_scope_table[i].caption, POS_STYLE_CAPTION);
        a->w.scope_btn[i] = settings_button(row, "OFF", cb, a, 0);
        lv_obj_set_width(a->w.scope_btn[i], SETTINGS_TOGGLE_W);
        lv_obj_set_user_data(a->w.scope_btn[i], (void *)(intptr_t)i);
    }
    lv_obj_remove_style(lv_obj_get_child(panel, -1), pos_style(POS_STYLE_DIVIDER), 0);
}

static void paint_scopes(struct settings_app *a, unsigned scopes)
{
    int i;

    for (i = 0; i < SV_SCOPES; i++) {
        settings_switch_paint(a->w.scope_btn[i], (scopes & sv_scope_table[i].bit) != 0);
    }
}

/* ---- Companion ----------------------------------------------------------------------------- */

static void on_gateway_switch(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    cJSON *params = cJSON_CreateObject();
    char err[240];
    cJSON *res;

    if (!a->companion.switch_can) {
        return;
    }
    cJSON_AddBoolToObject(params, "enabled", !a->companion.enabled);
    res = ask("sysd", "companion.set_enabled", params, COMPANION_TAP_TIMEOUT_MS, err, sizeof(err));
    if (res) {
        a->companion_error[0] = '\0';
    } else {
        say_refusal(a->companion_error, sizeof(a->companion_error), err, "The system service");
    }
    cJSON_Delete(res);
    settings_poll_companion(a);
    settings_companion_repaint(a);
}

static void on_password_open(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    a->companion_error[0] = '\0';
    settings_go(a, PAGE_PASSWORD);
}

static void open_pairing(struct settings_app *a)
{
    char err[160];
    cJSON *res = ask("gatewayd", "gateway.pair_begin", NULL, COMPANION_TAP_TIMEOUT_MS, err, sizeof(err));

    sv_pair_begin(&a->pair, res, err);
    cJSON_Delete(res);
    a->qr_scale = 0;
    a->page = PAGE_PAIR;
    a->tick = 0;
    pocketos_shell_set_title(settings_page_title(PAGE_PAIR));
    settings_rebuild(a);
}

static void on_pair_open(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    if (!a->companion.pair_can) {
        return;
    }
    a->companion_error[0] = '\0';
    open_pairing(a);
}

static void on_phone_row(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    /* A list that changed under the rows: nothing is opened for a row that
     * may no longer be the phone it names. The next tick builds it again. */
    if (i < 0 || i >= a->companion.count || !rows_current(a)) {
        return;
    }
    snprintf(a->phone_key, sizeof(a->phone_key), "%s", a->companion.phones[i].key);
    a->remove_confirm = 0;
    a->companion_error[0] = '\0';
    settings_go(a, PAGE_PHONE);
}

/* One phone: its name over its key and what it may do, and a chevron. The
 * whole row is the target. */
static void phone_row(struct settings_app *a, lv_obj_t *panel, int i)
{
    const struct sv_phone *p = &a->companion.phones[i];
    lv_obj_t *left;
    lv_obj_t *row = word_row(panel, &left);
    lv_obj_t *lb;

    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)i);
    lv_obj_add_event_cb(row, on_phone_row, LV_EVENT_CLICKED, a);
    /* The whole name, on two lines if it needs them: it is how the person
     * tells one phone from another. */
    settings_wrap_label(left, p->name, POS_STYLE_ROW_TITLE);
    a->w.phone_detail[i] = settings_wrap_label(left, p->detail, POS_STYLE_CAPTION);
    a->w.phone_perms[i] = settings_wrap_label(left, p->perms, POS_STYLE_TEXT_SECONDARY);
    lb = pocketui_label(row, LV_SYMBOL_RIGHT, POS_STYLE_ROW_TITLE);
    pos_style_add(lb, POS_STYLE_SYMBOL, 0);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
}

static void build_companion(struct settings_app *a)
{
    const struct sv_companion *c = &a->companion;
    lv_obj_t *p = settings_panel(a, "COMPANION GATEWAY", COL_LEFT);
    lv_obj_t *left;
    lv_obj_t *row = word_row(p, &left);
    int i;

    settings_wrap_label(left, "Gateway", POS_STYLE_ROW_TITLE);
    settings_wrap_label(left, "Lets paired phones with DOORS Companion reach this unit over the local network.",
                        POS_STYLE_TEXT_SECONDARY);
    a->w.gw_switch = settings_button(row, "OFF", on_gateway_switch, a, 0);
    lv_obj_set_width(a->w.gw_switch, COMPANION_SWITCH_W);
    a->w.gw_headline = settings_wrap_label(p, "", POS_STYLE_VALUE);
    a->w.gw_detail = settings_wrap_label(p, "", POS_STYLE_CAPTION);
    a->w.gw_hint = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    a->w.phone_error = settings_wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);

    p = settings_panel(a, "DEVICE PASSWORD", COL_LEFT);
    a->w.pw_headline = settings_wrap_label(p, "", POS_STYLE_VALUE);
    settings_wrap_label(p, "Root's password on this unit. The gateway needs one; the serial console asks for it.",
                        POS_STYLE_TEXT_SECONDARY);
    a->w.pw_ssh = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    a->w.pw_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    a->w.pw_btn = settings_button(p, c->pw_button[0] ? c->pw_button : "SET PASSWORD", on_password_open, a,
                                  !c->password_set);
    lv_obj_set_width(a->w.pw_btn, LV_PCT(100));

    p = settings_panel(a, "PAIRED PHONES", COL_RIGHT);
    for (i = 0; i < c->count; i++) {
        phone_row(a, p, i);
    }
    if (c->count > 0) {
        /* A list's last row needs no line under it. */
        lv_obj_remove_style(lv_obj_get_child(p, -1), pos_style(POS_STYLE_DIVIDER), 0);
    }
    a->w.gw_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    a->w.pair_btn = settings_button(p, "PAIR A PHONE", on_pair_open, a, 1);
    lv_obj_set_width(a->w.pair_btn, LV_PCT(100));
    a->w.pair_hint = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    sv_companion_signature(&a->companion, a->phone_sig, sizeof(a->phone_sig));
}

static void repaint_companion(struct settings_app *a)
{
    const struct sv_companion *c = &a->companion;
    char line[SV_TEXT];
    int i;

    if (!a->w.gw_headline) {
        return;
    }
    lv_label_set_text(lv_obj_get_child(a->w.gw_switch, 0), c->switch_label);
    settings_accent(a->w.gw_switch, c->switch_on);
    settings_set_enabled(a->w.gw_switch, c->switch_can);
    paint_label(a->w.gw_headline, c->headline, c->tone);
    if (c->device_key[0]) {
        snprintf(line, sizeof(line), "This unit's key %s", c->device_key);
    } else {
        line[0] = '\0';
    }
    paint_label(a->w.gw_detail, line, SV_TONE_MUTED);
    paint_label(a->w.gw_hint, c->hint, c->hint_tone);
    paint_label(a->w.phone_error, a->companion_error, SV_TONE_ERROR);
    paint_label(a->w.pw_headline, c->pw_headline, c->pw_tone);
    paint_label(a->w.pw_ssh, c->pw_ssh, c->pw_ssh_tone);
    paint_label(a->w.pw_note, c->pw_note, c->pw_note_tone);
    lv_label_set_text(lv_obj_get_child(a->w.pw_btn, 0), c->pw_button);
    settings_accent(a->w.pw_btn, !c->password_set);
    settings_set_enabled(a->w.pw_btn, c->pw_can);
    paint_label(a->w.gw_note, c->note, SV_TONE_MUTED);
    settings_set_enabled(a->w.pair_btn, c->pair_can);
    paint_label(a->w.pair_hint, c->pair_hint, SV_TONE_MUTED);
    if (!rows_current(a)) {
        return; /* the tick builds the page again */
    }
    for (i = 0; i < c->count; i++) {
        if (a->w.phone_detail[i] && strcmp(lv_label_get_text(a->w.phone_detail[i]), c->phones[i].detail) != 0) {
            lv_label_set_text(a->w.phone_detail[i], c->phones[i].detail);
        }
        if (a->w.phone_perms[i] && strcmp(lv_label_get_text(a->w.phone_perms[i]), c->phones[i].perms) != 0) {
            lv_label_set_text(a->w.phone_perms[i], c->phones[i].perms);
        }
    }
}

/* ---- Companion > a phone --------------------------------------------------------------------- */

static const struct sv_phone *this_phone(const struct settings_app *a)
{
    return sv_companion_find(&a->companion, a->phone_key);
}

static void on_phone_scope(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    const struct sv_phone *p = this_phone(a);
    char err[160];
    cJSON *params;
    cJSON *res;

    if (!p || i < 0 || i >= SV_SCOPES) {
        return;
    }
    params = sv_companion_set_scopes_params(p, sv_scope_toggle(p->scopes, sv_scope_table[i].bit));
    if (!params) {
        return;
    }
    res = ask("gatewayd", "gateway.set_scopes", params, COMPANION_TAP_TIMEOUT_MS, err, sizeof(err));
    if (res) {
        a->companion_error[0] = '\0';
    } else if (err[0]) {
        /* "changed, but not durable" is one of gatewayd's answers: said as
         * it is; the switches then show what gatewayd holds. */
        snprintf(a->companion_error, sizeof(a->companion_error), "The gateway answered: %.100s", err);
    } else {
        snprintf(a->companion_error, sizeof(a->companion_error), "The gateway did not answer.");
    }
    cJSON_Delete(res);
    settings_poll_companion(a);
    settings_companion_repaint(a);
}

static void on_remove(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    a->remove_confirm = 1;
    settings_rebuild(a);
}

static void on_remove_cancel(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    a->remove_confirm = 0;
    settings_rebuild(a);
}

static void on_remove_yes(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    const struct sv_phone *p = this_phone(a);
    char err[160];
    cJSON *res;

    if (!p) {
        go_companion(a);
        return;
    }
    res = ask("gatewayd", "gateway.revoke", sv_companion_revoke_params(p), COMPANION_TAP_TIMEOUT_MS, err,
              sizeof(err));
    if (res || err[0]) {
        /* Revoked: gone at once, even when gatewayd could not yet write it
         * down ("revoked, but not durable": it retries, and says so). */
        if (!res) {
            snprintf(a->companion_error, sizeof(a->companion_error), "Removed. The gateway said: %.100s", err);
        } else {
            a->companion_error[0] = '\0';
        }
        cJSON_Delete(res);
        a->remove_confirm = 0;
        settings_poll_companion(a);
        if (!this_phone(a)) {
            go_companion(a);
            return;
        }
    } else {
        snprintf(a->companion_error, sizeof(a->companion_error), "The gateway did not answer; the phone was not "
                                                                  "removed. Try again.");
    }
    settings_rebuild(a);
}

static void build_phone(struct settings_app *a)
{
    const struct sv_phone *ph = this_phone(a);
    lv_obj_t *p;
    lv_obj_t *r;
    char line[2 * SV_TEXT];

    if (!ph) {
        p = settings_panel(a, "PHONE", COL_LEFT);
        settings_wrap_label(p, "This phone is no longer paired.", POS_STYLE_TEXT_MUTED);
        return;
    }
    p = settings_panel(a, "PHONE", COL_LEFT);
    settings_wrap_label(p, ph->name, POS_STYLE_TITLE);
    snprintf(line, sizeof(line), "Key %s", ph->key_show);
    settings_wrap_label(p, line, POS_STYLE_VALUE);
    a->w.phone_state = settings_wrap_label(p, "", POS_STYLE_TEXT_PRIMARY);
    sv_phone_paired_text(ph, line, sizeof(line));
    if (line[0]) {
        settings_wrap_label(p, line, POS_STYLE_CAPTION);
    }
    settings_wrap_label(p, "The name is the phone's own. The key is what DOORS Companion shows as This phone's key.",
                        POS_STYLE_TEXT_SECONDARY);
    a->w.phone_error = settings_wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);
    if (!a->remove_confirm) {
        r = settings_button(p, "REMOVE PHONE", on_remove, a, 0);
        lv_obj_set_width(r, LV_PCT(100));
    } else {
        snprintf(line, sizeof(line), "Remove %s? It is disconnected now, and can connect again only if it is "
                                     "paired again.",
                 ph->name);
        settings_wrap_label(p, line, POS_STYLE_STATUS_WARN_TEXT);
        r = settings_hrow(p, SETTINGS_BTN_H);
        /* Keeping it is the accented choice: removing is the one to think
         * twice about (DS §17.5). */
        lv_obj_set_flex_grow(settings_button(r, "CANCEL", on_remove_cancel, a, 1), 1);
        lv_obj_set_flex_grow(settings_button(r, "REMOVE", on_remove_yes, a, 0), 1);
    }

    p = settings_panel(a, "WHAT IT MAY DO", COL_RIGHT);
    scope_rows(a, p, on_phone_scope);
}

static void repaint_phone(struct settings_app *a)
{
    const struct sv_phone *ph = this_phone(a);

    if (!ph || !a->w.phone_state) {
        return;
    }
    paint_label(a->w.phone_state, ph->connected ? "Connected now" : "Not connected", ph->connected ? SV_TONE_OK
                                                                                                 : SV_TONE_MUTED);
    paint_label(a->w.phone_error, a->companion_error, SV_TONE_ERROR);
    paint_scopes(a, ph->scopes);
}

/* ---- Companion > Pair a phone ----------------------------------------------------------------- */

static void cancel_pairing(struct settings_app *a)
{
    char err[96];

    if (sv_pair_open(&a->pair)) {
        cJSON_Delete(ask("gatewayd", "gateway.pair_cancel", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err)));
    }
    sv_pair_wipe(&a->pair);
    memset(&a->pair, 0, sizeof(a->pair));
    a->pair.phase = SV_PAIR_CLOSED;
}

static void on_pair_cancel(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    cancel_pairing(a);
    go_companion(a);
}

static void on_pair_done(lv_event_t *e)
{
    go_companion(lv_event_get_user_data(e));
}

static void on_pair_again(lv_event_t *e)
{
    open_pairing(lv_event_get_user_data(e));
}

static void on_keys_match(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    if (a->pair.phase != SV_PAIR_DECIDE) {
        return;
    }
    a->pair.keys_match = !a->pair.keys_match;
    settings_companion_repaint(a);
}

static void on_pair_scope(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i < 0 || i >= SV_SCOPES || a->pair.phase != SV_PAIR_DECIDE) {
        return;
    }
    a->pair.scopes = sv_scope_toggle(a->pair.scopes, sv_scope_table[i].bit);
    settings_companion_repaint(a);
}

static void decide(struct settings_app *a, int allow)
{
    char err[160];
    cJSON *params = sv_pair_decide_params(&a->pair, allow);
    cJSON *res;

    if (!params) {
        return; /* ALLOW before the key was confirmed: nothing is sent */
    }
    res = ask("gatewayd", "gateway.pair_decide", params, COMPANION_DECIDE_TIMEOUT_MS, err, sizeof(err));
    if (!res) {
        char err2[96];

        /* Undecided: whoever waits is not left waiting behind a page that
         * has stopped showing it. */
        cJSON_Delete(ask("gatewayd", "gateway.pair_cancel", NULL, SHELL_IPC_UI_TIMEOUT_MS, err2, sizeof(err2)));
    }
    sv_pair_decided(&a->pair, allow, res, err);
    cJSON_Delete(res);
    settings_poll_companion(a);
    settings_rebuild(a);
}

static void on_allow(lv_event_t *e)
{
    decide(lv_event_get_user_data(e), 1);
}

static void on_refuse(lv_event_t *e)
{
    decide(lv_event_get_user_data(e), 0);
}

static lv_obj_t *pair_words(struct settings_app *a, lv_obj_t *p, int headline)
{
    if (!headline) {
        a->w.gw_hint = settings_wrap_label(p, a->pair.text, POS_STYLE_TEXT_SECONDARY);
        return a->w.gw_hint;
    }
    a->w.gw_headline = settings_wrap_label(p, a->pair.headline, POS_STYLE_TITLE);
    /* A title stays a title: the status roles are colour alone, so the
     * title's font is kept (settings_set_tone would bring the body font). */
    if (a->pair.tone == SV_TONE_OK || a->pair.tone == SV_TONE_WARN || a->pair.tone == SV_TONE_ERROR) {
        pos_style_add(a->w.gw_headline, a->pair.tone == SV_TONE_OK     ? POS_STYLE_STATUS_OK_TEXT
                                        : a->pair.tone == SV_TONE_WARN ? POS_STYLE_STATUS_WARN_TEXT
                                                                       : POS_STYLE_STATUS_ERROR_TEXT,
                      0);
    }
    a->w.gw_hint = settings_wrap_label(p, a->pair.text, POS_STYLE_TEXT_SECONDARY);
    return a->w.gw_hint;
}

static void build_pair(struct settings_app *a)
{
    const struct sv_pair *pr = &a->pair;
    lv_obj_t *p;
    lv_obj_t *r;
    lv_obj_t *left;
    char line[SV_TEXT];

    switch (pr->phase) {
    case SV_PAIR_SHOWING:
    case SV_PAIR_CONNECTING:
        p = settings_panel(a, "PAIRING CODE", COL_LEFT);
        lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        a->w.qr_border = lv_obj_create(p);
        lv_obj_remove_style_all(a->w.qr_border);
        pos_style_add(a->w.qr_border, POS_STYLE_SCAN_SURFACE, 0);
        lv_obj_clear_flag(a->w.qr_border, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        a->w.qr = lv_qrcode_create(a->w.qr_border);
        lv_obj_remove_style_all(a->w.qr);
        lv_qrcode_set_quiet_zone(a->w.qr, false);
        lv_qrcode_set_dark_color(a->w.qr, pos_scan_dark());
        lv_qrcode_set_light_color(a->w.qr, pos_scan_light());
        lv_image_set_antialias(a->w.qr, false);
        lv_obj_add_flag(a->w.qr_border, LV_OBJ_FLAG_HIDDEN);
        a->w.qr_error = settings_wrap_label(p, "The code cannot be drawn. Cancel, then try again.",
                                            POS_STYLE_STATUS_ERROR_TEXT);
        a->qr_scale = 0;

        p = settings_panel(a, "PAIR A PHONE", COL_RIGHT);
        pair_words(a, p, 1);
        if (a->companion.device_key[0]) {
            snprintf(line, sizeof(line), "This unit's key %s", a->companion.device_key);
            settings_wrap_label(p, line, POS_STYLE_CAPTION);
        }
        a->w.pair_countdown = settings_wrap_label(p, pr->countdown, POS_STYLE_VALUE);
        r = settings_button(p, "CANCEL", on_pair_cancel, a, 0);
        lv_obj_set_width(r, LV_PCT(100));
        break;
    case SV_PAIR_DECIDE:
        /* The decision beside the key, so ALLOW is in view with what it is
         * about in either shape; the permissions take the other column. */
        p = settings_panel(a, "A PHONE WANTS TO PAIR", COL_LEFT);
        settings_wrap_label(p, "ITS NAME FOR ITSELF", POS_STYLE_CAPTION);
        /* The phone's own claim, smaller than its key: a name made to look
         * like a key is not the thing the eye goes to. */
        settings_wrap_label(p, pr->cand_name, POS_STYLE_ROW_TITLE);
        settings_wrap_label(p, "ITS KEY", POS_STYLE_CAPTION);
        /* Mono: 0 and D, 8 and B are told apart when keys are compared. */
        settings_wrap_label(p, pr->cand_key_show, POS_STYLE_VALUE);
        pair_words(a, p, 0);
        r = word_row(p, &left);
        settings_wrap_label(left, "Same key on the phone", POS_STYLE_ROW_TITLE);
        a->w.match_btn = settings_button(r, "NO", on_keys_match, a, 0);
        lv_obj_set_width(a->w.match_btn, SETTINGS_TOGGLE_W);
        a->w.phone_state = settings_wrap_label(p, "", POS_STYLE_TEXT_SECONDARY);
        a->w.pair_countdown = settings_wrap_label(p, pr->countdown, POS_STYLE_CAPTION);
        r = settings_hrow(p, SETTINGS_BTN_H);
        lv_obj_set_flex_grow(settings_button(r, "REFUSE", on_refuse, a, 0), 1);
        a->w.allow_btn = settings_button(r, "ALLOW", on_allow, a, 1);
        lv_obj_set_flex_grow(a->w.allow_btn, 1);
        a->w.pair_hint = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);

        p = settings_panel(a, "WHAT IT MAY DO", COL_RIGHT);
        scope_rows(a, p, on_pair_scope);
        break;
    case SV_PAIR_DONE:
        p = settings_panel(a, "PAIR A PHONE", COL_LEFT);
        pair_words(a, p, 1);
        r = settings_button(p, "DONE", on_pair_done, a, 1);
        lv_obj_set_width(r, LV_PCT(100));
        break;
    default:
        p = settings_panel(a, "PAIR A PHONE", COL_LEFT);
        pair_words(a, p, 1);
        r = settings_hrow(p, SETTINGS_BTN_H);
        lv_obj_set_flex_grow(settings_button(r, "BACK", on_pair_done, a, 0), 1);
        lv_obj_set_flex_grow(settings_button(r, "SHOW A NEW CODE", on_pair_again, a, 1), 1);
        break;
    }
}

static void repaint_pair(struct settings_app *a)
{
    const struct sv_pair *pr = &a->pair;

    if (a->w.pair_countdown && strcmp(lv_label_get_text(a->w.pair_countdown), pr->countdown) != 0) {
        lv_label_set_text(a->w.pair_countdown, pr->countdown);
    }
    if (pr->phase != SV_PAIR_DECIDE || !a->w.match_btn) {
        return;
    }
    lv_label_set_text(lv_obj_get_child(a->w.match_btn, 0), pr->keys_match ? "YES" : "NO");
    settings_accent(a->w.match_btn, pr->keys_match);
    settings_set_enabled(a->w.allow_btn, sv_pair_can_allow(pr));
    paint_label(a->w.pair_hint, sv_pair_can_allow(pr) ? "" : "Compare the keys first: ALLOW waits for YES.",
                SV_TONE_MUTED);
    paint_scopes(a, pr->scopes);
    {
        char words[SV_TEXT];
        char line[SV_TEXT + 16];

        sv_scopes_words(pr->scopes, words, sizeof(words));
        snprintf(line, sizeof(line), "It will get: %s", words);
        paint_label(a->w.phone_state, line, SV_TONE_NORMAL);
    }
}

/* The QR code at whole modules, as large as the panel allows up to
 * COMPANION_QR_MAX, with exactly four modules of quiet zone on every side
 * (the border's padding), as System's feedback code is drawn. */
void settings_companion_shape(struct settings_app *a)
{
    int version;
    int modules;
    int32_t width;
    int32_t height;
    int32_t limit;
    int scale;
    bool ok = false;
    size_t len;

    if (a->page != PAGE_PAIR || !a->w.qr || !a->w.qr_border || a->full_w <= 0) {
        return;
    }
    len = strlen(a->pair.uri);
    version = len ? qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, len) : -1;
    modules = version > 0 ? qrcodegen_version2size(version) : 0;
    /* The panel's padding (20 a side) and its hairline. */
    width = (a->wide ? a->half_w : a->full_w) - 2 * POCKETUI_PAD - 2;
    /* Wide, the code is beside the words and the body is short: the panel's
     * padding, its caption and the gap under it come off the height. Tall,
     * the width decides. */
    height = a->wide ? lv_area_get_height(&a->layout_guard.area) - 2 * POCKETUI_PAD - 40 : COMPANION_QR_MAX;
    limit = LV_MIN(LV_MIN(width, height), COMPANION_QR_MAX);
    scale = modules > 0 ? limit / (modules + 8) : 0;
    if (scale == a->qr_scale) {
        return;
    }
    a->qr_scale = scale;
    if (modules > 0 && scale >= 2) {
        lv_draw_buf_t *buf;

        lv_obj_set_size(a->w.qr_border, (modules + 8) * scale, (modules + 8) * scale);
        lv_obj_set_style_pad_all(a->w.qr_border, 4 * scale, 0);
        lv_qrcode_set_size(a->w.qr, modules * scale);
        buf = lv_canvas_get_draw_buf(a->w.qr);
        ok = buf && buf->header.w == (unsigned)(modules * scale) && buf->header.h == (unsigned)(modules * scale) &&
             lv_qrcode_update(a->w.qr, a->pair.uri, (uint32_t)len) == LV_RESULT_OK;
    }
    settings_set_hidden(a->w.qr_border, !ok);
    settings_set_hidden(a->w.qr_error, ok);
}

/* ---- Companion > Device password ----------------------------------------------------------- */

static void clear_password_fields(struct settings_app *a)
{
    if (a->page != PAGE_PASSWORD) {
        return;
    }
    if (a->w.field) {
        lv_textarea_set_text(a->w.field, "");
    }
    if (a->w.field2) {
        lv_textarea_set_text(a->w.field2, "");
    }
}

static void show_password_error(struct settings_app *a, const char *msg)
{
    if (!a->w.sheet_error) {
        return;
    }
    lv_label_set_text(a->w.sheet_error, msg);
    settings_set_hidden(a->w.sheet_error, msg[0] == '\0');
    lv_obj_update_layout(a->body);
    lv_obj_scroll_to_view_recursive(a->w.sheet_error, LV_ANIM_OFF);
}

static void submit_password(struct settings_app *a)
{
    char why[SV_TEXT];
    char err[240];
    char msg[2 * SV_TEXT];
    cJSON *params;
    cJSON *res;

    if (!a->w.field || !a->w.field2) {
        return;
    }
    if (sv_device_password_check(lv_textarea_get_text(a->w.field), lv_textarea_get_text(a->w.field2), why,
                                 sizeof(why)) < 0) {
        show_password_error(a, why);
        return;
    }
    params = sv_device_password_params(lv_textarea_get_text(a->w.field));
    /* The fields are cleared before the request goes: from here the
     * password is in the request alone. */
    clear_password_fields(a);
    res = ask("sysd", "companion.set_password", params, COMPANION_TAP_TIMEOUT_MS, err, sizeof(err));
    if (!res) {
        say_refusal(msg, sizeof(msg), err, "The system service");
        show_password_error(a, msg);
        pos_input_focus(a->w.field);
        return;
    }
    cJSON_Delete(res);
    a->pw_sent = 1;
    pocketos_shell_keyboard_hide();
    go_companion(a);
}

static void on_password_set(lv_event_t *e)
{
    submit_password(lv_event_get_user_data(e));
}

static void on_password_cancel(lv_event_t *e)
{
    go_companion(lv_event_get_user_data(e));
}

static void on_password_show(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    bool hidden;

    if (!a->w.field || !a->w.field2) {
        return;
    }
    hidden = !lv_textarea_get_password_mode(a->w.field);
    lv_textarea_set_password_mode(a->w.field, hidden);
    lv_textarea_set_password_mode(a->w.field2, hidden);
    lv_label_set_text(a->w.show_label, hidden ? "SHOW" : "HIDE");
}

/* The keyboard's Done: from the first field to the second, from the second
 * it sets the password. */
static void on_password_done(void *user)
{
    struct settings_app *a = user;

    if (a->page != PAGE_PASSWORD) {
        return;
    }
    if (a->w.field && lv_obj_has_state(a->w.field, LV_STATE_FOCUSED)) {
        pos_input_focus(a->w.field2);
        lv_obj_update_layout(a->body);
        lv_obj_scroll_to_view_recursive(a->w.field2, LV_ANIM_OFF);
        return;
    }
    submit_password(a);
}

void settings_password_build(struct settings_app *a)
{
    lv_obj_t *p = pocketui_card(a->body);
    lv_obj_t *r;
    lv_obj_t *b;
    int i;

    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_style_pad_column(p, POCKETUI_PAD, 0);
    a->w.sheet = p;
    for (i = 0; i < 2; i++) {
        a->w.sheet_side[i] = settings_stack(p, 12);
        lv_obj_clear_flag(a->w.sheet_side[i], LV_OBJ_FLAG_CLICKABLE);
    }
    p = a->w.sheet_side[0];
    pocketui_label(p, "DEVICE PASSWORD", POS_STYLE_CAPTION);
    settings_wrap_label(p, a->companion.password_set ? "Change root's password" : "Set root's password",
                        POS_STYLE_TITLE);
    settings_wrap_label(p, "DOORS Companion needs it before the gateway can be turned on, and the serial console "
                           "asks for it. SSH lets root in with a key only; if it does not yet, that changes first.",
                        POS_STYLE_TEXT_SECONDARY);
    settings_wrap_label(p, "8 to 64 characters: letters, digits, spaces and the symbols on the keyboard.",
                        POS_STYLE_TEXT_MUTED);

    p = a->w.sheet_side[1];
    a->w.field = pocketui_text_field(p, "New password", true);
    lv_textarea_set_password_mode(a->w.field, true);
    lv_textarea_set_max_length(a->w.field, 64);
    a->w.field2 = pocketui_text_field(p, "The same again", true);
    lv_textarea_set_password_mode(a->w.field2, true);
    lv_textarea_set_max_length(a->w.field2, 64);
    r = settings_hrow(p, SETTINGS_BTN_H);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    b = settings_button(r, "SHOW", on_password_show, a, 0);
    lv_obj_set_width(b, SETTINGS_TOGGLE_W);
    a->w.show_label = lv_obj_get_child(b, 0);
    a->w.sheet_error = settings_wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);
    settings_set_hidden(a->w.sheet_error, 1);
    r = settings_hrow(p, SETTINGS_BTN_H);
    lv_obj_set_flex_grow(settings_button(r, "CANCEL", on_password_cancel, a, 0), 1);
    lv_obj_set_flex_grow(settings_button(r, "SET PASSWORD", on_password_set, a, 1), 1);

    settings_shape(a);
    pos_input_focus(a->w.field);
    pocketos_shell_keyboard_show(POCKETOS_KB_DONE, on_password_done, a);
}

/* ---- moving between them ----------------------------------------------------------------------- */

static void go_companion(struct settings_app *a)
{
    clear_password_fields(a);
    if (a->page == PAGE_PASSWORD) {
        pocketos_shell_keyboard_hide();
    }
    sv_pair_wipe(&a->pair);
    a->remove_confirm = 0;
    settings_go(a, PAGE_COMPANION);
}

void settings_companion_back(struct settings_app *a)
{
    if (a->page == PAGE_PAIR) {
        cancel_pairing(a);
    }
    go_companion(a);
}

void settings_companion_teardown(struct settings_app *a)
{
    clear_password_fields(a);
    if (a->page == PAGE_PAIR) {
        cancel_pairing(a);
    }
    sv_pair_wipe(&a->pair);
}

void settings_companion_build(struct settings_app *a)
{
    switch (a->page) {
    case PAGE_PHONE:
        build_phone(a);
        break;
    case PAGE_PAIR:
        build_pair(a);
        break;
    default:
        build_companion(a);
        break;
    }
}

void settings_companion_repaint(struct settings_app *a)
{
    switch (a->page) {
    case PAGE_PHONE:
        repaint_phone(a);
        break;
    case PAGE_PAIR:
        repaint_pair(a);
        break;
    case PAGE_COMPANION:
        repaint_companion(a);
        break;
    default:
        break;
    }
}

/* Once a second while one of the pages shows. */
void settings_companion_tick(struct settings_app *a)
{
    char err[96];

    switch (a->page) {
    case PAGE_COMPANION:
        settings_poll_companion(a);
        if (!rows_current(a)) {
            settings_rebuild(a);
        }
        break;
    case PAGE_PHONE:
        settings_poll_companion(a);
        /* Removed elsewhere (or the gateway stopped): back to the list. */
        if (!this_phone(a) && a->companion_missed == 0) {
            go_companion(a);
        }
        break;
    case PAGE_PAIR:
        if (sv_pair_open(&a->pair)) {
            enum sv_pair_phase was = a->pair.phase;
            char key[SV_PHONE_KEY];
            cJSON *st = ask("gatewayd", "gateway.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));

            memcpy(key, a->pair.cand_key, sizeof(key));
            sv_pair_poll(&a->pair, st);
            cJSON_Delete(st);
            if (a->pair.phase != was || strcmp(key, a->pair.cand_key) != 0) {
                settings_rebuild(a);
            }
        }
        break;
    default:
        break;
    }
}
