/*
 * The repeater page's COMMAND console: one line to the repeater's own CLI,
 * three read-only shortcuts, and the transcript. See rift_repeater_view.h.
 *
 * The rule for what is sent is rift_repeater.h's, and it is applied here
 * before anything is written: a READ command goes at once, a CONFIRM one only
 * from its confirmation's SEND, and a REFUSED one never (rift_ipc_repeater_cli
 * applies the rule again, whatever reaches it). This is not a terminal: no
 * history, no completion, no scripting.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_repeater_view.h"

#include "pos_styles.h"
#include "rift_form.h"

#include <stdio.h>
#include <string.h>

#define SEND_W 120

static struct rift_repeater_view *view_of(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    return a ? a->repeater : NULL;
}

static const char *key_of(const struct rift_repeater_view *v)
{
    const struct rift_repeater *r = &v->app->model.repeater;

    return r->have_target ? r->target : NULL;
}

static void cmd_note(struct rift_repeater_view *v, const char *text)
{
    rift_label_set(v->part[RIFT_REPV_CMD_NOTE], text);
}

/* A typed or chosen command, by the rule. */
static void submit(struct rift_repeater_view *v, const char *command)
{
    struct rift_app *a = v->app;
    const char *why = NULL;
    char text[RIFT_TEXT_MAX];

    while (*command == ' ') {
        command++;
    }
    if (!command[0] || !key_of(v) || rift_rep_busy(&a->model.repeater)) {
        return;
    }
    switch (rift_rep_cli_class(command, &why)) {
    case RIFT_CLI_REFUSED:
        /* Gone from the field too: it may hold a password somebody typed. */
        lv_textarea_set_text(v->part[RIFT_REPV_COMMAND], "");
        snprintf(text, sizeof(text), "Not sent: %s.", why ? why : "not allowed");
        cmd_note(v, text);
        break;
    case RIFT_CLI_CONFIRM:
        snprintf(v->confirm_cmd, sizeof(v->confirm_cmd), "%s", command);
        v->confirming = 1;
        snprintf(text, sizeof(text), "Send \xE2\x80\x9C%s\xE2\x80\x9D?", command);
        lv_label_set_text(v->confirm_title, text);
        lv_label_set_text_fmt(v->confirm_text, "This %s. Nothing is sent until SEND.",
                              why ? why : "changes the repeater");
        break;
    case RIFT_CLI_READ:
    default:
        if (rift_ipc_repeater_cli(&a->ipc, key_of(v), command) == 0) {
            lv_textarea_set_text(v->part[RIFT_REPV_COMMAND], "");
        }
        break;
    }
    a->refresh_pending = 1;
}

static void on_send(lv_event_t *e)
{
    struct rift_repeater_view *v = view_of(e);
    char command[RIFT_REP_CMD_MAX + 1];

    if (!v || v->confirming) {
        return;
    }
    rift_form_typed(v->part[RIFT_REPV_COMMAND], command, sizeof(command));
    submit(v, command);
}

static void on_quick(lv_event_t *e)
{
    struct rift_repeater_view *v = view_of(e);
    lv_obj_t *button = lv_event_get_current_target_obj(e);

    if (!v || v->confirming) {
        return;
    }
    if (button == v->part[RIFT_REPV_QUICK_VER]) {
        submit(v, "ver");
    } else if (button == v->part[RIFT_REPV_QUICK_CLOCK]) {
        submit(v, "clock");
    } else if (button == v->part[RIFT_REPV_QUICK_NEIGHBORS]) {
        submit(v, "neighbors");
    }
}

/* The one place a command that changes the repeater is sent: its
 * confirmation, for the command it was asked about. */
static void on_confirm_send(lv_event_t *e)
{
    struct rift_repeater_view *v = view_of(e);

    if (!v || !v->confirming || !key_of(v)) {
        return;
    }
    v->confirming = 0;
    if (rift_ipc_repeater_cli(&v->app->ipc, key_of(v), v->confirm_cmd) == 0) {
        lv_textarea_set_text(v->part[RIFT_REPV_COMMAND], "");
    }
    v->confirm_cmd[0] = '\0';
    v->app->refresh_pending = 1;
}

static void on_cancel(lv_event_t *e)
{
    struct rift_repeater_view *v = view_of(e);

    if (v) {
        rift_repeater_cmd_cancel(v);
        v->app->refresh_pending = 1;
    }
}

void rift_repeater_cmd_build(struct rift_repeater_view *v, lv_obj_t *parent)
{
    struct rift_app *a = v->app;
    lv_obj_t *panel = rift_panel(parent, "COMMAND");
    lv_obj_t *row = rift_form_row(panel, 64);
    lv_obj_t *quick = rift_form_row(panel, RIFT_TOUCH_H);
    lv_obj_t *field;
    lv_obj_t *bar;

    v->cmd_panel = panel;
    field = rift_form_field(a, row, "Repeater command", RIFT_REP_CMD_MAX);
    if (field) {
        /* The field sits in PocketUI's 100 %-wide wrapper; the wrapper takes
         * the row's room left over, so the button beside it fits. */
        lv_obj_set_width(lv_obj_get_parent(field), 1);
        lv_obj_set_flex_grow(lv_obj_get_parent(field), 1);
        lv_obj_add_event_cb(field, on_send, LV_EVENT_READY, a);
    }
    v->part[RIFT_REPV_COMMAND] = field;
    v->part[RIFT_REPV_SEND] = rift_action(row, "SEND", 1, 1, on_send, a);
    lv_obj_set_flex_grow(v->part[RIFT_REPV_SEND], 0);
    lv_obj_set_width(v->part[RIFT_REPV_SEND], SEND_W);
    v->part[RIFT_REPV_QUICK_VER] = rift_action(quick, "VER", 0, 1, on_quick, a);
    v->part[RIFT_REPV_QUICK_CLOCK] = rift_action(quick, "CLOCK", 0, 1, on_quick, a);
    v->part[RIFT_REPV_QUICK_NEIGHBORS] = rift_action(quick, "NEIGHBORS", 0, 1, on_quick, a);
    v->part[RIFT_REPV_CMD_NOTE] = rift_form_text(panel, POS_STYLE_CAPTION);

    /* The confirmation (DS §17.5): what it does, Cancel first and accented. */
    v->confirm = rift_form_column(panel);
    v->confirm_title = rift_form_text(v->confirm, POS_STYLE_TITLE);
    v->confirm_text = rift_form_text(v->confirm, POS_STYLE_TEXT_SECONDARY);
    bar = rift_form_row(v->confirm, RIFT_TOUCH_H);
    v->part[RIFT_REPV_CANCEL] = rift_action(bar, "CANCEL", 1, 1, on_cancel, a);
    v->part[RIFT_REPV_CONFIRM] = rift_action(bar, "SEND", 0, 1, on_confirm_send, a);
    lv_obj_add_flag(v->confirm, LV_OBJ_FLAG_HIDDEN);

    v->part[RIFT_REPV_TRANSCRIPT] = rift_form_text(panel, POS_STYLE_CAPTION);
}

void rift_repeater_cmd_cancel(struct rift_repeater_view *v)
{
    if (!v) {
        return;
    }
    v->confirming = 0;
    v->confirm_cmd[0] = '\0';
}

/* The transcript as one label, rebuilt only when its lines changed. */
static void refresh_transcript(struct rift_repeater_view *v)
{
    const struct rift_repeater *r = &v->app->model.repeater;
    char text[RIFT_REP_LINES * (RIFT_REP_LINE + 1) + 1];
    unsigned sig = 2166136261u;
    size_t at = 0;
    int i;

    text[0] = '\0';
    for (i = 0; i < r->line_count; i++) {
        const char *line = rift_rep_line(r, i);
        int n;

        if (!line) {
            continue;
        }
        n = snprintf(text + at, sizeof(text) - at, "%s%s", at ? "\n" : "", line);
        if (n < 0 || (size_t)n >= sizeof(text) - at) {
            break;
        }
        at += (size_t)n;
    }
    for (i = 0; text[i]; i++) {
        sig = (sig ^ (unsigned char)text[i]) * 16777619u;
    }
    if (sig == v->transcript_drawn) {
        return;
    }
    v->transcript_drawn = sig;
    lv_label_set_text(v->part[RIFT_REPV_TRANSCRIPT], at ? text : "");
    rift_form_show(v->part[RIFT_REPV_TRANSCRIPT], at > 0);
}

void rift_repeater_cmd_refresh(struct rift_repeater_view *v, int usable)
{
    const struct rift_repeater *r = &v->app->model.repeater;
    int guest = r->login == RIFT_REP_LOGIN_OK && r->admin_known && !r->admin;
    int open = usable && !guest;
    int can = open && !rift_rep_busy(r) && !v->confirming;
    int live = open && !v->confirming;

    rift_action_set_enabled(v->part[RIFT_REPV_SEND], 1, can);
    rift_action_set_enabled(v->part[RIFT_REPV_QUICK_VER], 0, can);
    rift_action_set_enabled(v->part[RIFT_REPV_QUICK_CLOCK], 0, can);
    rift_action_set_enabled(v->part[RIFT_REPV_QUICK_NEIGHBORS], 0, can);
    if (live != v->cmd_live) {
        rift_repeater_field_live(v->app, v->part[RIFT_REPV_COMMAND], live);
        v->cmd_live = live;
    }
    rift_form_show(v->confirm, v->confirming);
    if (!usable) {
        cmd_note(v, "Log in as admin to send the repeater commands.");
    } else if (guest) {
        cmd_note(v, "Logged in as guest: the repeater answers commands only for admin.");
    } else if (lv_label_get_text(v->part[RIFT_REPV_CMD_NOTE])[0] == '\0' ||
               strncmp(lv_label_get_text(v->part[RIFT_REPV_CMD_NOTE]), "Log in", 6) == 0 ||
               strncmp(lv_label_get_text(v->part[RIFT_REPV_CMD_NOTE]), "Logged in as guest", 18) ==
                   0) {
        cmd_note(v, "Read-only commands go at once; one that changes the repeater asks "
                    "first. Erase, firmware update, power-off and passwords are not sent from "
                    "RIFT.");
    }
    refresh_transcript(v);
}
