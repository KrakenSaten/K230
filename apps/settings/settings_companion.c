/*
 * Settings > Companion: the phones paired with this unit, and for each one
 * the switch that lets it be shown Vision's picture (DS §52.8).
 *
 * The paired list and every permission are gatewayd's (ADR-019: "Settings
 * talks to gatewayd.sock"). This page reads gateway.status and
 * gateway.clients, and a tap on a switch sends gateway.scope {client, scope:
 * "vision", on}: the one scope of ADR-019 amendment 1, given on the unit, per
 * phone, off unless switched on here. Nothing is stored here and nothing is
 * decided here: the switch shows what gatewayd answers, and gatewayd ends a
 * preview in the same call that takes the scope away.
 *
 * The switch is the whole of the authorisation, so it is a tap on the
 * unit's own screen and nothing else: this page serves no IPC method, and a
 * phone can neither reach gatewayd's local socket nor raise its own scopes
 * (ADR-019 rule 24).
 *
 * Pairing and revoking are not on this page yet (issue #172); they stay on
 * the unit's command line.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings_internal.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <string.h>

/* A tap waits this long for gatewayd, which writes the paired list to
 * storage before it answers. A tap, not a tick: longer than the UI deadline,
 * still short enough that a gateway that hangs is not waited for. */
#define COMPANION_TAP_TIMEOUT_MS 1500
/* A gatewayd that has answered and then misses a poll is busy, not gone (it
 * writes its list to storage, it shakes hands with a phone, and there is one
 * hart): the list on show stays for this many polls in a row before the page
 * says it is not running. A gatewayd that never answered says so at once. */
#define COMPANION_MISSES 3

void settings_poll_companion(struct settings_app *a)
{
    char err[96];
    cJSON *st = shell_ipc_call_timeout("gatewayd", "gateway.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    cJSON *cl = st ? shell_ipc_call_timeout("gatewayd", "gateway.clients", NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                            sizeof(err))
                   : NULL;

    if ((!st || !cl) && a->companion.reachable && ++a->companion_missed < COMPANION_MISSES) {
        /* Kept as it was: a missed poll does not blank the list (as Wi-Fi's). */
        cJSON_Delete(st);
        cJSON_Delete(cl);
        return;
    }
    if (st && cl) {
        a->companion_missed = 0;
    }
    sv_companion_apply(&a->companion, st, cl);
    cJSON_Delete(st);
    cJSON_Delete(cl);
}

/* Whether the rows on screen are the rows of the list as it is now. */
static bool rows_current(const struct settings_app *a)
{
    char sig[sizeof(a->phone_sig)];

    sv_companion_signature(&a->companion, sig, sizeof(sig));
    return strcmp(sig, a->phone_sig) == 0;
}

static void on_phone_vision(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    char err[96] = "";
    cJSON *params;
    cJSON *res;
    int want;

    /* A list that changed under the rows: nothing is sent for a row that
     * may no longer be the phone it names. The next tick builds it again. */
    if (i < 0 || i >= a->companion.count || !rows_current(a)) {
        return;
    }
    want = !a->companion.phones[i].vision;
    params = sv_companion_vision_params(&a->companion.phones[i], want);
    if (!params) {
        return;
    }
    res = shell_ipc_call_timeout("gatewayd", "gateway.scope", params, COMPANION_TAP_TIMEOUT_MS, err, sizeof(err));
    if (res) {
        a->companion_error[0] = '\0';
    } else {
        /* Said as it is, in gatewayd's words ("changed, but not durable"
         * is one of them); what the switch shows next is what gatewayd
         * holds, asked again below, never what was hoped for. */
        if (err[0]) {
            snprintf(a->companion_error, sizeof(a->companion_error), "The gateway answered: %.80s", err);
        } else {
            snprintf(a->companion_error, sizeof(a->companion_error), "The gateway did not answer.");
        }
    }
    cJSON_Delete(res);
    settings_poll_companion(a);
    settings_companion_repaint(a);
}

/* One phone: its name over its key, and the switch. As tall as its words
 * need at the text size in force, never under a touch target. */
static void phone_row(struct settings_app *a, lv_obj_t *panel, int i)
{
    const struct sv_phone *p = &a->companion.phones[i];
    lv_obj_t *row = settings_hrow(panel, LV_SIZE_CONTENT);
    lv_obj_t *left;

    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    lv_obj_set_style_min_height(row, SETTINGS_BTN_H, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* The whole name, on two lines if it needs them: it is how the person
     * tells one phone from another before switching it on. */
    settings_wrap_label(left, p->name, POS_STYLE_ROW_TITLE);
    a->w.phone_detail[i] = settings_wrap_label(left, p->detail, POS_STYLE_CAPTION);
    a->w.phone_switch[i] = settings_button(row, "OFF", on_phone_vision, a, 0);
    lv_obj_set_width(a->w.phone_switch[i], SETTINGS_TOGGLE_W);
    lv_obj_set_user_data(a->w.phone_switch[i], (void *)(intptr_t)i);
}

void settings_companion_build(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "COMPANION GATEWAY", COL_LEFT);
    int i;

    a->w.gw_headline = settings_wrap_label(p, "", POS_STYLE_VALUE);
    settings_wrap_label(p, "A paired phone with DOORS Companion reads this unit over the local network. "
                           "Pairing is started on the unit.",
                        POS_STYLE_TEXT_SECONDARY);

    p = settings_panel(a, "SHOW VISION ON", COL_RIGHT);
    settings_wrap_label(p, "A phone switched on here is shown the camera's picture whenever Vision is open on this "
                           "unit, without being asked again. The Vision screen names the phone while it is, and has "
                           "STOP.",
                        POS_STYLE_TEXT_SECONDARY);
    for (i = 0; i < a->companion.count; i++) {
        phone_row(a, p, i);
    }
    if (a->companion.count > 0) {
        /* A list's last row needs no line under it. */
        lv_obj_remove_style(lv_obj_get_child(p, -1), pos_style(POS_STYLE_DIVIDER), 0);
    }
    a->w.gw_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
    a->w.phone_error = settings_wrap_label(p, "", POS_STYLE_STATUS_ERROR_TEXT);
    sv_companion_signature(&a->companion, a->phone_sig, sizeof(a->phone_sig));
}

void settings_companion_repaint(struct settings_app *a)
{
    int i;

    if (!a->w.gw_headline) {
        return;
    }
    if (strcmp(lv_label_get_text(a->w.gw_headline), a->companion.headline) != 0) {
        lv_label_set_text(a->w.gw_headline, a->companion.headline);
    }
    settings_set_tone(a->w.gw_headline, a->companion.tone);
    lv_label_set_text(a->w.gw_note, a->companion.note);
    settings_set_hidden(a->w.gw_note, a->companion.note[0] == '\0');
    lv_label_set_text(a->w.phone_error, a->companion_error);
    settings_set_hidden(a->w.phone_error, a->companion_error[0] == '\0');
    if (!rows_current(a)) {
        return; /* the tick builds the page again */
    }
    for (i = 0; i < a->companion.count; i++) {
        if (a->w.phone_switch[i]) {
            settings_switch_paint(a->w.phone_switch[i], a->companion.phones[i].vision);
        }
        if (a->w.phone_detail[i] && strcmp(lv_label_get_text(a->w.phone_detail[i]), a->companion.phones[i].detail) != 0) {
            lv_label_set_text(a->w.phone_detail[i], a->companion.phones[i].detail);
        }
    }
}

/* Once a second while the page shows: the list as gatewayd has it now. A
 * phone paired, revoked or renamed since the rows were built builds them
 * again; anything else is repainted in place. */
void settings_companion_tick(struct settings_app *a)
{
    settings_poll_companion(a);
    if (!rows_current(a)) {
        a->companion_error[0] = '\0';
        settings_rebuild(a);
    }
}
