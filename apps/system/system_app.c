/*
 * System Status: the screen. Everything it *decides* is in system_view.c;
 * this builds the panels and turns taps into those decisions.
 *
 * It reads nothing from the machine directly - no /proc, no /sys, no
 * /run/pocketos. system.info once at create, system.status every two seconds
 * over pocketipc, and the radio state the shell's status bar has already
 * polled. Every call carries SHELL_IPC_UI_TIMEOUT_MS, so a service that has
 * stopped answering costs one frame and never the session (unit A, M5).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "app.h"
#include "pocketui.h"
#include "shell_ipc.h"
#include "system_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* system.status every other tick. The shell ticks apps once a second and
 * already polls radiod at that rate; a status screen does not need to be
 * faster, and this halves what the two of them ask for together. */
#define SYSTEM_POLL_TICKS 2
#define SYSTEM_METER_H 6
#define SYSTEM_MOUNT_ROW_H 72
#define SYSTEM_ACTION_BTN_H 56

struct system_app {
    struct system_view view;
    lv_obj_t *root;
    lv_obj_t *body;        /* rebuilt whenever the shape of the screen changes */
    lv_obj_t *freshness;
    /* live labels, refreshed in place while the shape does not change */
    lv_obj_t *vital[6];
    lv_obj_t *mount_detail[SYSTEM_VIEW_MAX_MOUNTS];
    lv_obj_t *mount_bar[SYSTEM_VIEW_MAX_MOUNTS];
    lv_obj_t *iface_chip[SYSTEM_VIEW_MAX_IFACES];
    lv_obj_t *iface_addr[SYSTEM_VIEW_MAX_IFACES];
    lv_obj_t *svc_chip[SYSTEM_VIEW_MAX_SERVICES];
    lv_obj_t *svc_detail[SYSTEM_VIEW_MAX_SERVICES];
    lv_obj_t *radio_chip;
    lv_obj_t *radio_detail;
    lv_obj_t *toast;
    unsigned long clock_ms;
    int tick;
    /* what the body was built for, so it is only rebuilt when it must be */
    int built_mounts;
    int built_ifaces;
    int built_services;
    int built_card;
    enum system_view_phase built_phase;
};

static void rebuild(struct system_app *a);

/* ---- small builders ---------------------------------------------------- */

static lv_obj_t *panel(lv_obj_t *parent)
{
    lv_obj_t *p = pocketui_card(parent);

    lv_obj_set_style_pad_row(p, 0, 0);
    return p;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *c = lv_label_create(parent);

    pos_style_add(c, POS_STYLE_CHIP, 0);
    pos_style_add(c, role, 0);
    lv_label_set_text(c, text);
    return c;
}

/* A row with a label on the left and two objects on the right is the shape
 * most of this screen is made of; this is the container for one. */
static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    pos_style_add(r, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* One vitals cell: caption above, value below, half the panel wide. Two per
 * row, three rows, so the six numbers fit in the height of four list rows. */
static lv_obj_t *vitals_cell(lv_obj_t *parent, const char *label)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_t *value;

    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, LV_PCT(50));
    lv_obj_set_height(cell, POCKETUI_ROW_H);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    pocketui_label(cell, label, POS_STYLE_CAPTION);
    value = pocketui_label(cell, SYSTEM_VIEW_UNKNOWN, POS_STYLE_VALUE);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, LV_PCT(96));
    return value;
}

static lv_obj_t *vitals_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, POCKETUI_ROW_H);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* The freshness line. The app header belongs to the shell, so this sits at
 * the top of the body instead, right-aligned and directly under the title: it
 * has to be the first thing seen, because every number below it is only worth
 * what this line says it is. */
static void build_freshness(struct system_app *a)
{
    lv_obj_t *r = lv_obj_create(a->body);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    a->freshness = pocketui_label(r, a->view.freshness, POS_STYLE_CAPTION);
}

static void section_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lb = pocketui_label(parent, text, POS_STYLE_CAPTION);

    lv_obj_set_style_pad_bottom(lb, 8, 0);
}

/* ---- painting the live values ------------------------------------------ */

static enum pos_style_role service_chip_role(enum system_view_service_state s)
{
    if (s == SYSTEM_VIEW_SVC_RUNNING) {
        return POS_STYLE_CHIP_RX;
    }
    if (s == SYSTEM_VIEW_SVC_CRASHLOOP) {
        return POS_STYLE_CHIP_TX;
    }
    return POS_STYLE_CHIP_OFF;
}

static const char *service_chip_text(enum system_view_service_state s)
{
    if (s == SYSTEM_VIEW_SVC_RUNNING) {
        return "RUNNING";
    }
    if (s == SYSTEM_VIEW_SVC_CRASHLOOP) {
        return "CRASH LOOP";
    }
    return "STOPPED";
}

static void repaint(struct system_app *a)
{
    const struct system_view *v = &a->view;
    int i;

    if (a->freshness) {
        lv_label_set_text(a->freshness, v->freshness);
        lv_obj_remove_style(a->freshness, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        lv_obj_remove_style(a->freshness, pos_style(POS_STYLE_TEXT_MUTED), 0);
        pos_style_add(a->freshness, v->stale ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_TEXT_MUTED,
                      0);
    }
    for (i = 0; i < 6; i++) {
        if (!a->vital[i]) {
            continue;
        }
        lv_label_set_text(a->vital[i], v->vitals[i].value);
        lv_obj_remove_style(a->vital[i], pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        if (v->vitals[i].warn) {
            pos_style_add(a->vital[i], POS_STYLE_STATUS_WARN_TEXT, 0);
        }
    }
    for (i = 0; i < v->mount_count && i < SYSTEM_VIEW_MAX_MOUNTS; i++) {
        if (a->mount_detail[i]) {
            lv_label_set_text(a->mount_detail[i], v->mounts[i].detail);
        }
        if (a->mount_bar[i]) {
            /* No meter without a denominator: an unknown share is an empty
             * track, never a full bar or a zero that looks like one. */
            lv_obj_set_width(a->mount_bar[i],
                             LV_PCT(v->mounts[i].have_percent ? v->mounts[i].used_percent : 0));
        }
    }
    for (i = 0; i < v->iface_count && i < SYSTEM_VIEW_MAX_IFACES; i++) {
        if (a->iface_chip[i]) {
            lv_label_set_text(a->iface_chip[i], v->ifaces[i].state);
            lv_obj_remove_style(a->iface_chip[i], pos_style(POS_STYLE_CHIP_RX), 0);
            lv_obj_remove_style(a->iface_chip[i], pos_style(POS_STYLE_CHIP_OFF), 0);
            pos_style_add(a->iface_chip[i],
                          v->ifaces[i].up ? POS_STYLE_CHIP_RX : POS_STYLE_CHIP_OFF, 0);
        }
        if (a->iface_addr[i]) {
            lv_label_set_text(a->iface_addr[i], v->ifaces[i].addr);
        }
    }
    for (i = 0; i < v->service_count && i < SYSTEM_VIEW_MAX_SERVICES; i++) {
        if (a->svc_chip[i]) {
            lv_label_set_text(a->svc_chip[i], service_chip_text(v->services[i].state));
            lv_obj_remove_style(a->svc_chip[i], pos_style(POS_STYLE_CHIP_RX), 0);
            lv_obj_remove_style(a->svc_chip[i], pos_style(POS_STYLE_CHIP_TX), 0);
            lv_obj_remove_style(a->svc_chip[i], pos_style(POS_STYLE_CHIP_OFF), 0);
            pos_style_add(a->svc_chip[i], service_chip_role(v->services[i].state), 0);
        }
        if (a->svc_detail[i]) {
            lv_label_set_text(a->svc_detail[i], v->services[i].detail);
        }
    }
    if (a->radio_chip) {
        lv_label_set_text(a->radio_chip, v->radio_state);
    }
    if (a->radio_detail) {
        lv_label_set_text(a->radio_detail, v->radio_detail);
    }
    if (a->toast) {
        lv_label_set_text(a->toast, v->error);
        if (v->error[0]) {
            lv_obj_clear_flag(a->toast, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(a->toast, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- the two destructive actions --------------------------------------- */

static void on_restart(lv_event_t *e)
{
    struct system_app *a = lv_event_get_user_data(e);

    system_view_request(&a->view, SYSTEM_VIEW_ACTION_REBOOT);
    rebuild(a);
}

static void on_poweroff(lv_event_t *e)
{
    struct system_app *a = lv_event_get_user_data(e);

    system_view_request(&a->view, SYSTEM_VIEW_ACTION_POWEROFF);
    rebuild(a);
}

static void on_cancel(lv_event_t *e)
{
    struct system_app *a = lv_event_get_user_data(e);

    system_view_cancel(&a->view);
    rebuild(a);
}

/* The only place this screen can reach a method that stops the machine, and
 * system_view_confirm() hands one over only from a confirmation. */
static void on_confirm(lv_event_t *e)
{
    struct system_app *a = lv_event_get_user_data(e);
    const char *method = system_view_confirm(&a->view);
    char err[96];
    cJSON *result;

    if (!method) {
        return;
    }
    err[0] = '\0';
    result = shell_ipc_call_timeout("sysd", method, NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                    sizeof(err));
    if (result) {
        cJSON_Delete(result);
        system_view_action_ok(&a->view);
    } else {
        system_view_action_failed(&a->view, err);
    }
    rebuild(a);
}

/* ---- building the body ------------------------------------------------- */

static void build_confirm(struct system_app *a)
{
    lv_obj_t *p = panel(a->body);
    lv_obj_t *buttons;
    lv_obj_t *cancel;
    lv_obj_t *confirm;
    lv_obj_t *body;

    pocketui_label(p, system_view_dialog_title(&a->view), POS_STYLE_TITLE);
    body = pocketui_label(p, system_view_dialog_body(&a->view), POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);

    buttons = lv_obj_create(p);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel first and focused: the safe answer is the one already under the
     * finger and the one a stray key press takes. */
    cancel = pocketui_button(buttons, "Cancel", on_cancel, a);
    lv_obj_remove_style(cancel, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    pos_style_add(cancel, POS_STYLE_BUTTON_SECONDARY, 0);
    lv_obj_set_height(cancel, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_grow(cancel, 1);

    confirm = pocketui_button(buttons, system_view_dialog_confirm_label(&a->view), on_confirm, a);
    lv_obj_set_height(confirm, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_grow(confirm, 1);

    if (lv_group_get_default()) {
        lv_group_focus_obj(cancel);
    }
}

static void build_terminal(struct system_app *a)
{
    lv_obj_t *p = panel(a->body);
    lv_obj_t *lb;

    lb = pocketui_label(p, system_view_terminal_text(&a->view), POS_STYLE_TITLE);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
}

static void build_live(struct system_app *a)
{
    const struct system_view *v = &a->view;
    lv_obj_t *p;
    lv_obj_t *r;
    lv_obj_t *lb;
    lv_obj_t *buttons;
    lv_obj_t *poweroff;
    char buf[SYSTEM_VIEW_TEXT];
    int i;

    /* toast: the reason a refused action gave, above everything else */
    a->toast = pocketui_label(a->body, "", POS_STYLE_STATUS_ERROR_TEXT);
    lv_label_set_long_mode(a->toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->toast, LV_PCT(100));
    lv_obj_add_flag(a->toast, LV_OBJ_FLAG_HIDDEN);

    /* vitals */
    p = panel(a->body);
    for (i = 0; i < 3; i++) {
        r = vitals_row(p);
        a->vital[i * 2] = vitals_cell(r, v->vitals[i * 2].label);
        a->vital[i * 2 + 1] = vitals_cell(r, v->vitals[i * 2 + 1].label);
    }

    /* storage */
    p = panel(a->body);
    section_caption(p, "STORAGE");
    if (v->mount_count == 0) {
        lb = pocketui_label(p, "no mounts reported", POS_STYLE_TEXT_MUTED);
        (void)lb;
    }
    for (i = 0; i < v->mount_count && i < SYSTEM_VIEW_MAX_MOUNTS; i++) {
        lv_obj_t *cell = lv_obj_create(p);
        lv_obj_t *top;
        lv_obj_t *track;

        lv_obj_remove_style_all(cell);
        pos_style_add(cell, POS_STYLE_DIVIDER, 0);
        lv_obj_set_width(cell, LV_PCT(100));
        lv_obj_set_height(cell, SYSTEM_MOUNT_ROW_H);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        top = lv_obj_create(cell);
        lv_obj_remove_style_all(top);
        lv_obj_set_width(top, LV_PCT(100));
        lv_obj_set_height(top, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
        pocketui_label(top, v->mounts[i].mount, POS_STYLE_TEXT_SECONDARY);
        a->mount_detail[i] = pocketui_label(top, v->mounts[i].detail, POS_STYLE_VALUE);
        lv_label_set_long_mode(a->mount_detail[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(a->mount_detail[i], LV_PCT(70), 0);

        track = lv_obj_create(cell);
        lv_obj_remove_style_all(track);
        pos_style_add(track, POS_STYLE_SLAB, 0);
        lv_obj_set_width(track, LV_PCT(100));
        lv_obj_set_height(track, SYSTEM_METER_H);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        a->mount_bar[i] = lv_obj_create(track);
        lv_obj_remove_style_all(a->mount_bar[i]);
        pos_style_add(a->mount_bar[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_height(a->mount_bar[i], SYSTEM_METER_H);
        lv_obj_align(a->mount_bar[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(a->mount_bar[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* network */
    p = panel(a->body);
    section_caption(p, "NETWORK");
    if (v->iface_count == 0) {
        pocketui_label(p, "no interfaces", POS_STYLE_TEXT_MUTED);
    }
    for (i = 0; i < v->iface_count && i < SYSTEM_VIEW_MAX_IFACES; i++) {
        r = row(p, POCKETUI_ROW_H);
        pocketui_label(r, v->ifaces[i].name, POS_STYLE_TEXT_SECONDARY);
        a->iface_chip[i] = chip(r, v->ifaces[i].state, POS_STYLE_CHIP_OFF);
        a->iface_addr[i] = pocketui_label(r, v->ifaces[i].addr, POS_STYLE_VALUE);
        lv_label_set_long_mode(a->iface_addr[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(a->iface_addr[i], LV_PCT(45), 0);
    }
    if (v->ifaces_hidden > 0) {
        /* Never silently disagree with `pos system status`, which shows them. */
        snprintf(buf, sizeof(buf), "%d interface%s hidden", v->ifaces_hidden,
                 v->ifaces_hidden == 1 ? "" : "s");
        pocketui_label(p, buf, POS_STYLE_TEXT_MUTED);
    }

    /* services */
    p = panel(a->body);
    section_caption(p, "SERVICES");
    if (v->service_count == 0) {
        pocketui_label(p, "no services reported", POS_STYLE_TEXT_MUTED);
    }
    for (i = 0; i < v->service_count && i < SYSTEM_VIEW_MAX_SERVICES; i++) {
        r = row(p, POCKETUI_ROW_H);
        lb = pocketui_label(r, v->services[i].name, POS_STYLE_TEXT_SECONDARY);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(lb, LV_PCT(30), 0);
        a->svc_chip[i] = chip(r, service_chip_text(v->services[i].state),
                              service_chip_role(v->services[i].state));
        a->svc_detail[i] = pocketui_label(r, v->services[i].detail, POS_STYLE_CAPTION);
        lv_label_set_long_mode(a->svc_detail[i], LV_LABEL_LONG_DOT);
        /* Wider than the name: a crash-looped service's detail is the only
         * thing on this row anybody needs, and it must not lose its tail. */
        lv_obj_set_style_max_width(a->svc_detail[i], LV_PCT(50), 0);
    }

    /* radio: one row, from what the status bar already knows */
    p = panel(a->body);
    r = row(p, POCKETUI_ROW_H);
    pocketui_label(r, "Radio", POS_STYLE_TEXT_SECONDARY);
    a->radio_chip = chip(r, v->radio_state, POS_STYLE_CHIP_OFF);
    a->radio_detail = pocketui_label(r, v->radio_detail, POS_STYLE_CAPTION);
    lv_label_set_long_mode(a->radio_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(a->radio_detail, LV_PCT(45), 0);

    /* identity */
    p = panel(a->body);
    pocketui_kv_row(p, "PocketOS", v->os_version);
    if (v->show_card) {
        pocketui_kv_row(p, "Card", v->card_version);
    }
    pocketui_kv_row(p, "Model", v->model);
    pocketui_kv_row(p, "Kernel", v->kernel);

    /* actions, last on the screen and never under the thumb on arrival */
    p = panel(a->body);
    buttons = lv_obj_create(p);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    lb = pocketui_button(buttons, "Restart", on_restart, a);
    lv_obj_set_height(lb, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_grow(lb, 1);
    /* Power off is the secondary action: on this board it cannot be undone
     * from here (docs/hardware/V0.0.7_BLOCK2C_SMOKE.md). */
    poweroff = pocketui_button(buttons, "Power off", on_poweroff, a);
    lv_obj_remove_style(poweroff, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    pos_style_add(poweroff, POS_STYLE_BUTTON_SECONDARY, 0);
    lv_obj_set_height(poweroff, SYSTEM_ACTION_BTN_H);
    lv_obj_set_flex_grow(poweroff, 1);
}

/* The body is rebuilt only when its shape changes - a service appearing, the
 * card row starting to disagree, a dialog opening - and repainted in place
 * every other second otherwise, so a scroll position survives a poll. */
static void rebuild(struct system_app *a)
{
    const struct system_view *v = &a->view;

    if (a->body) {
        lv_obj_delete(a->body);
    }
    memset(a->vital, 0, sizeof(a->vital));
    memset(a->mount_detail, 0, sizeof(a->mount_detail));
    memset(a->mount_bar, 0, sizeof(a->mount_bar));
    memset(a->iface_chip, 0, sizeof(a->iface_chip));
    memset(a->iface_addr, 0, sizeof(a->iface_addr));
    memset(a->svc_chip, 0, sizeof(a->svc_chip));
    memset(a->svc_detail, 0, sizeof(a->svc_detail));
    a->freshness = NULL;
    a->radio_chip = NULL;
    a->radio_detail = NULL;
    a->toast = NULL;

    a->body = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->body);
    lv_obj_set_width(a->body, LV_PCT(100));
    lv_obj_set_height(a->body, LV_PCT(100));
    lv_obj_set_flex_flow(a->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->body, 22, 0);
    /* The content is taller than 1232 minus the bars, so it scrolls. Nothing
     * is dropped to avoid that: a status screen that hides diagnostics to fit
     * is not the trade to make. */
    lv_obj_set_scroll_dir(a->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->body, LV_SCROLLBAR_MODE_AUTO);

    if (v->phase == SYSTEM_VIEW_TERMINAL_REBOOT || v->phase == SYSTEM_VIEW_TERMINAL_POWEROFF) {
        /* Nothing is being polled any more, so there is no freshness to
         * report and claiming one would be a lie. */
        build_terminal(a);
    } else if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT ||
               v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        build_freshness(a);
        build_confirm(a);
    } else {
        build_freshness(a);
        build_live(a);
    }
    a->built_mounts = v->mount_count;
    a->built_ifaces = v->iface_count;
    a->built_services = v->service_count;
    a->built_card = v->show_card;
    a->built_phase = v->phase;
    repaint(a);
}

static int shape_changed(const struct system_app *a)
{
    const struct system_view *v = &a->view;

    return a->built_mounts != v->mount_count || a->built_ifaces != v->iface_count ||
           a->built_services != v->service_count || a->built_card != v->show_card ||
           a->built_phase != v->phase;
}

/* ---- app lifecycle ----------------------------------------------------- */

static void poll_status(struct system_app *a)
{
    char err[96];
    cJSON *st;

    err[0] = '\0';
    st = shell_ipc_call_timeout("sysd", "system.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                sizeof(err));
    system_view_apply_status(&a->view, st, a->clock_ms);
    if (st) {
        cJSON_Delete(st);
    }
}

static void *system_create(lv_obj_t *root)
{
    struct system_app *a = calloc(1, sizeof(*a));
    char err[96];
    cJSON *info;

    if (!a) {
        return NULL;
    }
    a->root = root;
    system_view_init(&a->view);

    /* Identity does not change while the system runs, so it is asked for once
     * and kept. A failure leaves it unknown rather than wrong; the next
     * screen visit asks again. */
    err[0] = '\0';
    info = shell_ipc_call_timeout("sysd", "system.info", NULL, SHELL_IPC_UI_TIMEOUT_MS, err,
                                  sizeof(err));
    system_view_apply_info(&a->view, info);
    if (info) {
        cJSON_Delete(info);
    }
    /* Region and backend are static too: one call, not a second poll. Skipped
     * entirely when the shell's own poll says radiod is not answering - there
     * is nothing to learn, and three bounded calls in a row on the LVGL
     * thread is 600 ms of no repaint on the one day every service is down. */
    err[0] = '\0';
    info = pocketos_shell_radio_state()
               ? shell_ipc_call_timeout("radiod", "radio.info", NULL, SHELL_IPC_UI_TIMEOUT_MS,
                                        err, sizeof(err))
               : NULL;
    if (info) {
        const cJSON *region = cJSON_GetObjectItemCaseSensitive(info, "region");
        const cJSON *backend = cJSON_GetObjectItemCaseSensitive(info, "backend");

        system_view_set_radio_detail(&a->view,
                                     cJSON_IsString(region) ? region->valuestring : NULL,
                                     cJSON_IsString(backend) ? backend->valuestring : NULL);
        cJSON_Delete(info);
    }
    system_view_set_radio_state(&a->view, pocketos_shell_radio_state());

    poll_status(a);
    rebuild(a);
    return a;
}

static void system_tick(void *priv)
{
    struct system_app *a = priv;

    a->clock_ms += 1000;
    /* A machine that is on its way down has nothing left to report, and
     * asking a service that is being stopped only produces a timeout. */
    if (!system_view_is_polling(&a->view)) {
        return;
    }
    /* The radio state is whatever the status bar's own poll last saw. */
    system_view_set_radio_state(&a->view, pocketos_shell_radio_state());
    if (++a->tick >= SYSTEM_POLL_TICKS) {
        a->tick = 0;
        poll_status(a);
    } else {
        /* No poll this second, but the age of the last one still moves. */
        system_view_refresh_freshness(&a->view, a->clock_ms);
    }
    if (shape_changed(a)) {
        rebuild(a);
    } else {
        repaint(a);
    }
}

static void system_destroy(void *priv)
{
    free(priv);
}

const struct pocketos_app app_system = {
    .id = "system",
    .name = "System",
    .icon = LV_SYMBOL_SETTINGS,
    .create = system_create,
    .tick = system_tick,
    .destroy = system_destroy,
};
