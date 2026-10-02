/*
 * System, inside: the app's state and what its two files share.
 *
 *   system_app.c  the tabs, Overview, Services, About, Diagnostics, the two
 *                 power actions, the layout and the lifecycle
 *   system_net.c  the Network tab: interfaces and their traffic, Wi-Fi, the
 *                 LoRa radio and the mesh (DS §52.5)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SYSTEM_INTERNAL_H
#define POCKETOS_SYSTEM_INTERNAL_H

#include "app.h"
#include "diag_view.h"
#include "pocketui.h"
#include "system_view.h"

#include <stdbool.h>

#define SYSTEM_METER_H 6
#define SYSTEM_MOUNT_ROW_H 72
#define SYSTEM_ACTION_BTN_H 56
#define SYSTEM_MARK_GAP 8 /* mark to name, the DS §7 glyph-to-label gap */
/* DS section 7: the gap between panels, whichever way they lie. */
#define SYSTEM_PANEL_GAP 22
/* The portrait body's width on the reference panel (568 less the 20 px side
 * padding of DS section 7). The wide shape is only chosen when each of its
 * two columns keeps at least this much, so no panel is narrower there than in
 * portrait, and a confirmation keeps exactly this width there (DS 23.4). */
#define SYSTEM_COLUMN_W 528

/* The four pages the tabs choose between (DS §52.5). */
enum system_tab {
    SYSTEM_TAB_OVERVIEW = 0, /* vitals, storage, Restart and Power off */
    SYSTEM_TAB_NETWORK,      /* interfaces and traffic, Wi-Fi, radio, mesh */
    SYSTEM_TAB_SERVICES,     /* the supervised services, and Diagnostics */
    SYSTEM_TAB_ABOUT,        /* what this device and this build are */
    SYSTEM_TAB_COUNT
};

/* Which of a page's two columns a panel goes in. A page whose second column
 * is empty has one column across the body. */
enum system_col {
    SYSTEM_COL_LEFT = 0,
    SYSTEM_COL_RIGHT,
};

/* The Network tab's live objects (system_net.c). */
struct system_net_widgets {
    lv_obj_t *iface_chip[SYSTEM_VIEW_MAX_IFACES];
    lv_obj_t *iface_addr[SYSTEM_VIEW_MAX_IFACES];
    lv_obj_t *iface_traffic[SYSTEM_VIEW_MAX_IFACES];
    lv_obj_t *wifi;
    lv_obj_t *radio_chip;
    lv_obj_t *radio_detail;
    lv_obj_t *radio_packets;
    lv_obj_t *radio_signal;
    lv_obj_t *mesh;
};

/* What the Network tab asks for besides system.status: one of these a tick,
 * in turn, so its three services cost one bounded call a second between
 * them and each is asked every three seconds. */
enum system_net_step {
    SYSTEM_NET_RADIO = 0,
    SYSTEM_NET_WIFI,
    SYSTEM_NET_MESH,
    SYSTEM_NET_STEPS
};

struct system_app {
    struct system_view view;
    lv_obj_t *root;
    lv_obj_t *frame; /* the app's own box in the body; built once */
    lv_obj_t *body;  /* rebuilt whenever the shape of the screen changes; the one box that scrolls */
    /* What the layout in force was chosen from, and the one place that
     * decides whether a pass is needed at all (pocketui.h). */
    struct pocketui_layout_guard layout_guard;
    bool wide;
    int32_t full_w; /* the frame's content width */
    int32_t half_w;
    lv_obj_t *cols; /* in the body: the page's two columns, which never scroll */
    lv_obj_t *col[2];
    enum system_tab tab;
    enum system_net_step net_step;
    /* the boxes the layout moves, rebuilt with the body */
    lv_obj_t *tabs;
    lv_obj_t *tab_btn[SYSTEM_TAB_COUNT];
    lv_obj_t *freshness_row;
    lv_obj_t *dialog; /* the confirmation, or the panel a power action leaves */
    lv_obj_t *freshness;
    /* live labels, refreshed in place while the shape does not change */
    lv_obj_t *vital[6];
    lv_obj_t *mount_detail[SYSTEM_VIEW_MAX_MOUNTS];
    lv_obj_t *mount_bar[SYSTEM_VIEW_MAX_MOUNTS];
    lv_obj_t *svc_chip[SYSTEM_VIEW_MAX_SERVICES];
    lv_obj_t *svc_detail[SYSTEM_VIEW_MAX_SERVICES];
    struct system_net_widgets net;
    lv_obj_t *toast;
    /* Diagnostics: the page, when it is the one shown */
    bool diag;
    struct diag_view dv;
    lv_obj_t *diag_value[DIAG_ROW_COUNT];
    lv_obj_t *diag_state;
    lv_obj_t *diag_crash_box;
    lv_obj_t *diag_log_box;
    lv_obj_t *diag_log_status;
    lv_obj_t *diag_filter[DIAG_FILTER_COUNT];
    unsigned diag_built_log;
    unsigned diag_built_crash;
    unsigned long clock_ms;
    int tick;
    /* what the body was built for, so it is only rebuilt when it must be */
    int built_mounts;
    int built_ifaces;
    int built_services;
    int built_card;
    enum system_view_phase built_phase;
    bool built_diag;
    enum system_tab built_tab;
};

/* ---- system_app.c: builders the Network tab shares ---- */
lv_obj_t *system_panel(struct system_app *a, enum system_col col);
void system_section_caption(lv_obj_t *parent, const char *text);
lv_obj_t *system_row(lv_obj_t *parent, int height);
void system_row_may_wrap(lv_obj_t *r);
lv_obj_t *system_chip(lv_obj_t *parent, const char *text, enum pos_style_role role);
enum pos_style_role system_radio_chip_role(enum system_view_radio_chip s);
lv_obj_t *system_wrap_label(lv_obj_t *parent, const char *text, enum pos_style_role role);

/* ---- system_net.c ---- */
void system_net_build(struct system_app *a);
void system_net_repaint(struct system_app *a);
/* One step of the tab's own polling (enum system_net_step), one call. */
void system_net_poll_step(struct system_app *a);

#endif
