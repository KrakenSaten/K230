/*
 * PacketShark's screen, inside the app (not an API): the state the three
 * sources of the app share - psk_app.c (lifecycle, the poll, the actions),
 * ui/psk_live.c (LIVE: setup, live status, the error panel; the capture
 * bar; PACKETS and INSPECT empty states) and ui/psk_sheets.c (the
 * confirmations).
 *
 * Milestone M3 (docs/apps/PACKETSHARK.md): setup, START, live counters,
 * HOLD, STOP, the error state. The traffic graph (M4), the packet list (M5)
 * and the inspector (M6) are not built yet: PACKETS shows how many packets
 * the helper holds, INSPECT is disabled.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_APP_H
#define PSK_APP_H

#include "app.h"
#include "pocketui.h"
#include "psk_model.h"
#include "psk_session.h"

#include <stdbool.h>
#include <stdint.h>

#define PSK_POLL_MS 33
#define PSK_IFACE_POLL_MS 2000
#define PSK_IFACES 6
#define PSK_TAB_H 56
#define PSK_BAR_H 64
#define PSK_GAP 8
#define PSK_PANEL_GAP 22

enum psk_tab { PSK_TAB_LIVE = 0, PSK_TAB_PACKETS, PSK_TAB_INSPECT, PSK_TAB_COUNT };

/* What LIVE shows. */
enum psk_view { PSK_VIEW_SETUP = 0, PSK_VIEW_LIVE, PSK_VIEW_ERROR };

enum psk_sheet {
    PSK_SHEET_NONE = 0,
    PSK_SHEET_FULL,     /* stored bytes FULL: confirm */
    PSK_SHEET_NOLIMIT,  /* no time limit: confirm */
    PSK_SHEET_DISCARD,  /* DISCARD: its packets go */
    PSK_SHEET_NEW,      /* NEW CAPTURE: its packets go */
    PSK_SHEET_LEAVE,    /* Back with packets in memory */
};

/* The capture bar's buttons, by what they do. */
enum psk_action {
    PSK_ACT_NONE = 0,
    PSK_ACT_START,
    PSK_ACT_FILTER,
    PSK_ACT_HOLD,
    PSK_ACT_RESUME,
    PSK_ACT_STOP,
    PSK_ACT_SAVE,
    PSK_ACT_NEW,
    PSK_ACT_DISCARD,
};

/* Stop-after choices, minutes; 0 is no limit. */
#define PSK_LIMITS 4
extern const uint32_t psk_limit_min[PSK_LIMITS];

struct psk_iface {
    char name[PPC_IFACE_MAX];
    bool wifi;
    bool synthetic;
    bool up;
    bool carrier;
    char ipv4[20];
};

struct psk_ifrow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *sub;
    lv_obj_t *chip;
};

struct psk_app {
    lv_obj_t *frame;
    struct pocketui_layout_guard guard;
    bool relayout;

    /* tabs and pages */
    lv_obj_t *tab[PSK_TAB_COUNT];
    lv_obj_t *tab_label[PSK_TAB_COUNT];
    lv_obj_t *tab_rule[PSK_TAB_COUNT];
    lv_obj_t *page[PSK_TAB_COUNT];
    enum psk_tab cur;

    /* LIVE: the status row and the banner, on every view */
    lv_obj_t *chip;
    lv_obj_t *status_text;
    lv_obj_t *elapsed;
    lv_obj_t *banner;
    lv_obj_t *banner_label;

    /* LIVE: setup */
    lv_obj_t *setup;
    lv_obj_t *if_panel;
    struct psk_ifrow ifrow[PSK_IFACES];
    lv_obj_t *if_none;
    lv_obj_t *opt_row[4];
    lv_obj_t *opt_val[4];

    /* LIVE: capturing, held, stopped */
    lv_obj_t *live;
    lv_obj_t *rate;
    lv_obj_t *throughput;
    lv_obj_t *waiting;
    lv_obj_t *cell[4];   /* PACKETS, BYTES, DROPPED, BUFFER */
    lv_obj_t *detail;    /* stored, held, shed */
    lv_obj_t *later;     /* what is not in this version yet */

    /* LIVE: the helper stopped */
    lv_obj_t *error;
    lv_obj_t *err_reason;
    lv_obj_t *err_iface;
    lv_obj_t *err_report;
    lv_obj_t *last[4];   /* PACKETS, BYTES, DROPPED, SAVED */

    /* PACKETS */
    lv_obj_t *pk_title;
    lv_obj_t *pk_text;

    /* the capture bar */
    lv_obj_t *bar;
    lv_obj_t *btn[3];
    enum psk_action act[3];

    /* the confirmation sheet */
    lv_obj_t *sheet;
    lv_obj_t *sheet_title;
    lv_obj_t *sheet_text;
    lv_obj_t *sheet_safe;
    lv_obj_t *sheet_go;
    enum psk_sheet sheet_kind;

    /* state */
    struct psk_session *session;
    struct psk_model *model;
    lv_timer_t *timer;
    struct psk_iface ifs[PSK_IFACES];
    int nifs;
    int sel;             /* index into ifs, or -1 */
    bool simulated;      /* a synthetic or replayed source stands in for the interfaces */
    enum ppc_keep keep;
    int limit_idx;
    enum ppc_cfilter cfilter;
    bool starting;       /* START pressed, the capture not yet confirmed */
    bool pending_start;  /* the capture command waits for the helper's hello */
    bool discarding;     /* discard sent: the helper is on its way out */
    bool failed_view;    /* the helper ended a capture badly: the error panel */
    enum psk_view view;
    char notice[96];     /* why the last START was refused, on the setup banner */
    int64_t next_ifs_ms;
    char hint[48];
};

/* psk_app.c */
int64_t psk_now(void);
void psk_do(struct psk_app *a, enum psk_action act);
void psk_select_tab(struct psk_app *a, enum psk_tab t);
void psk_select_iface(struct psk_app *a, int index);
void psk_option(struct psk_app *a, int which);
void psk_sheet_answer(struct psk_app *a, bool go);

/* ui/psk_live.c */
void psk_live_build(struct psk_app *a);
void psk_live_paint(struct psk_app *a, unsigned changed);
void psk_paint_ifaces(struct psk_app *a);
void psk_paint_options(struct psk_app *a);
void psk_paint_tabs(struct psk_app *a);
const char *psk_cfilter_label(enum ppc_cfilter f);

/* ui/psk_sheets.c */
void psk_sheet_build(struct psk_app *a);
void psk_sheet_show(struct psk_app *a, enum psk_sheet kind);
void psk_sheet_hide(struct psk_app *a);

/* small LVGL helpers, ui/psk_live.c */
void psk_set_hidden(lv_obj_t *obj, bool hidden);
void psk_text(lv_obj_t *label, const char *text);
lv_obj_t *psk_box(lv_obj_t *parent);
lv_obj_t *psk_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user);
void psk_button_look(lv_obj_t *b, bool primary, bool enabled);

#endif
