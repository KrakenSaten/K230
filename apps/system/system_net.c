/*
 * System > NETWORK (DS §52.5): the interfaces with their addresses and their
 * traffic, Wi-Fi, the LoRa radio with its packets and the last signal, and
 * the mesh. Two panels, side by side when the body is wide.
 *
 * The interfaces come with system.status, which the page polls with the
 * others; the traffic rate is taken between two of its answers
 * (system_view.c). The three links are asked for one a tick, in turn
 * (system_net_poll_step): radiod's radio.stats only while the status bar's
 * own poll says radiod answers, netd's wifi.status, meshcored's mesh.status.
 * Each call carries the UI deadline.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "shell_ipc.h"
#include "system_internal.h"

#include <stdio.h>
#include <string.h>

void system_net_poll_step(struct system_app *a)
{
    char err[96] = "";
    cJSON *r = NULL;

    switch (a->net_step) {
    case SYSTEM_NET_RADIO:
        r = pocketos_shell_radio_state()
                ? shell_ipc_call_timeout("radiod", "radio.stats", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err))
                : NULL;
        system_view_apply_radio_stats(&a->view, r);
        break;
    case SYSTEM_NET_WIFI:
        r = shell_ipc_call_timeout("netd", "wifi.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
        system_view_apply_wifi(&a->view, r);
        break;
    case SYSTEM_NET_MESH:
    default:
        r = shell_ipc_call_timeout("meshcored", "mesh.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
        system_view_apply_mesh(&a->view, r);
        break;
    }
    cJSON_Delete(r);
    a->net_step = (enum system_net_step)((a->net_step + 1) % SYSTEM_NET_STEPS);
}

/* A link's row: its name on the left, a line under it that wraps. */
static lv_obj_t *link_row(lv_obj_t *panel, const char *name)
{
    lv_obj_t *r = system_row(panel, POCKETUI_ROW_H);

    system_row_may_wrap(r);
    /* The name and the chip together at the start of the line, not spread
     * across it: the words under them carry the state. */
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(r, 8, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    pocketui_label(r, name, POS_STYLE_TEXT_SECONDARY);
    return r;
}

void system_net_build(struct system_app *a)
{
    const struct system_view *v = &a->view;
    struct system_net_widgets *w = &a->net;
    lv_obj_t *p;
    lv_obj_t *r;
    char buf[SYSTEM_VIEW_TEXT];
    int i;

    p = system_panel(a, SYSTEM_COL_LEFT);
    system_section_caption(p, "INTERFACES");
    if (v->iface_count == 0) {
        pocketui_label(p, "no interfaces", POS_STYLE_TEXT_MUTED);
    }
    for (i = 0; i < v->iface_count && i < SYSTEM_VIEW_MAX_IFACES; i++) {
        r = system_row(p, POCKETUI_ROW_H);
        system_row_may_wrap(r);
        lv_obj_set_style_pad_ver(r, 6, 0);
        pocketui_label(r, v->ifaces[i].name, POS_STYLE_TEXT_SECONDARY);
        w->iface_chip[i] = system_chip(r, v->ifaces[i].state, POS_STYLE_CHIP_OFF);
        w->iface_addr[i] = pocketui_label(r, v->ifaces[i].addr, POS_STYLE_VALUE);
        lv_label_set_long_mode(w->iface_addr[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(w->iface_addr[i], LV_PCT(100), 0);
        /* The traffic on a line of its own under the name, the address and
         * the state, so it is read whole at any text size. */
        w->iface_traffic[i] = system_wrap_label(r, v->ifaces[i].traffic, POS_STYLE_CAPTION);
    }
    if (v->ifaces_hidden > 0) {
        /* Never silently disagree with `pos system status`, which shows them. */
        snprintf(buf, sizeof(buf), "%d interface%s hidden", v->ifaces_hidden, v->ifaces_hidden == 1 ? "" : "s");
        pocketui_label(p, buf, POS_STYLE_TEXT_MUTED);
    }

    p = system_panel(a, SYSTEM_COL_RIGHT);
    system_section_caption(p, "LINKS");
    r = link_row(p, "Wi-Fi");
    w->wifi = system_wrap_label(r, v->wifi, POS_STYLE_VALUE);

    /* The radio: the chip the status bar draws, what it is configured for,
     * and what it has heard and sent since radiod started. */
    r = link_row(p, "LoRa radio");
    w->radio_chip = system_chip(r, v->radio_state, system_radio_chip_role(system_view_radio_chip_state(v)));
    w->radio_detail = pocketui_label(r, v->radio_detail, POS_STYLE_CAPTION);
    w->radio_packets = system_wrap_label(r, v->radio_packets, POS_STYLE_VALUE);
    w->radio_signal = system_wrap_label(r, v->radio_signal, POS_STYLE_CAPTION);

    r = link_row(p, "Mesh");
    w->mesh = system_wrap_label(r, v->mesh, POS_STYLE_VALUE);
}

static void set_text(lv_obj_t *lb, const char *text)
{
    if (lb && strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

void system_net_repaint(struct system_app *a)
{
    const struct system_view *v = &a->view;
    struct system_net_widgets *w = &a->net;
    int i;

    for (i = 0; i < v->iface_count && i < SYSTEM_VIEW_MAX_IFACES; i++) {
        if (w->iface_chip[i]) {
            set_text(w->iface_chip[i], v->ifaces[i].state);
            lv_obj_remove_style(w->iface_chip[i], pos_style(POS_STYLE_CHIP_ACTIVE), 0);
            lv_obj_remove_style(w->iface_chip[i], pos_style(POS_STYLE_CHIP_OFF), 0);
            pos_style_add(w->iface_chip[i], v->ifaces[i].up ? POS_STYLE_CHIP_ACTIVE : POS_STYLE_CHIP_OFF, 0);
        }
        set_text(w->iface_addr[i], v->ifaces[i].addr);
        set_text(w->iface_traffic[i], v->ifaces[i].traffic);
    }
    set_text(w->wifi, v->wifi);
    if (w->radio_chip) {
        set_text(w->radio_chip, v->radio_state);
        lv_obj_remove_style(w->radio_chip, pos_style(POS_STYLE_CHIP_RX), 0);
        lv_obj_remove_style(w->radio_chip, pos_style(POS_STYLE_CHIP_TX), 0);
        lv_obj_remove_style(w->radio_chip, pos_style(POS_STYLE_CHIP_OFF), 0);
        lv_obj_remove_style(w->radio_chip, pos_style(POS_STYLE_CHIP_NA), 0);
        pos_style_add(w->radio_chip, system_radio_chip_role(system_view_radio_chip_state(v)), 0);
    }
    if (w->radio_detail) {
        set_text(w->radio_detail, v->radio_detail);
        /* The region and backend are configuration and stay readable when
         * radiod is not answering - they are still what it is configured
         * for. Muted, so nobody reads them as live state. */
        lv_obj_remove_style(w->radio_detail, pos_style(POS_STYLE_TEXT_MUTED), 0);
        if (!v->radio_state_known) {
            pos_style_add(w->radio_detail, POS_STYLE_TEXT_MUTED, 0);
        }
    }
    set_text(w->radio_packets, v->radio_packets);
    set_text(w->radio_signal, v->radio_signal);
    if (w->radio_signal) {
        if (v->radio_signal[0]) {
            lv_obj_clear_flag(w->radio_signal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(w->radio_signal, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (w->mesh) {
        set_text(w->mesh, v->mesh);
        lv_obj_remove_style(w->mesh, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        if (v->mesh_warn) {
            pos_style_add(w->mesh, POS_STYLE_STATUS_WARN_TEXT, 0);
        }
    }
}
