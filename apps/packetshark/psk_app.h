/*
 * PacketShark's screen, inside the app (not an API): the state the sources
 * of the app share - psk_app.c (lifecycle, the poll, the actions),
 * ui/psk_live.c (LIVE: setup, live status, the error panel; the tabs and
 * the capture bar), ui/psk_list.c (PACKETS, M5), ui/psk_inspect.c (INSPECT,
 * M6) and ui/psk_sheets.c (the confirmations, the filter sheet and the saved
 * captures).
 *
 * Milestones M3 to M9 (docs/apps/PACKETSHARK.md): setup, START, live
 * counters, HOLD, STOP, the error state; the traffic graph and the protocol
 * distribution (M4); the packet list with its display filter and sort (M5);
 * the packet inspector and pins (M6); findings on LIVE, in the list, on its
 * rail and in the inspector (M7); SAVE and the saved captures (M8); the wide
 * shape of its own and the keys (M9).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_APP_H
#define PSK_APP_H

#include "app.h"
#include "pocketui.h"
#include "psk_model.h"
#include "psk_remote.h"
#include "psk_saves.h"
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
#define PSK_LIST_ROWS 16 /* the row pool: a portrait list shows 15 (DS §59.4) */
#define PSK_ROW_H 56     /* DS §59.6 */
#define PSK_FIELD_H 36   /* DS §59.6 */
#define PSK_HELD_REFRESH_MS 1000
/* A following list is repainted twice a second: at a busy rate every row
 * changes each time, and a repaint of the whole list is the costliest
 * frame the app draws (measured on Unit B, docs/apps/PACKETSHARK.md §9.4). */
#define PSK_FOLLOW_REFRESH_MS 500
/* The wide shape (M9, mockup 08, DS §59.7): the app's own top row, the list
 * and the inspector side by side, list rows of one line. Chosen from the
 * room, not from the orientation. */
#define PSK_WIDE_MIN_W 900
#define PSK_TOP_H 56
#define PSK_ROW_H_WIDE 40
#define PSK_INSPECT_W 452
#define PSK_LIVE_LEFT_W 528
/* The rail's finding marks are asked for this often while packets arrive. */
#define PSK_MARKS_REFRESH_MS 1000
#define PSK_SAVES_ROWS 6
/* A start request's sheet refuses itself after this long (ADR-016 rule 1a). */
#define PSK_REQUEST_MS 60000
/* START on that sheet takes no tap for this long after the sheet appears:
 * a finger already on its way to something else is not an answer. */
#define PSK_REQUEST_SETTLE_MS 1500
/* The header says a companion is reading for this long after the last
 * capture.status (ADR-016 rule 20). Whoever reads for a phone asks at
 * least this often while the phone reads, and not when it does not. */
#define PSK_READER_MS 5000
#define PSK_CLIENT_TEXT 28
#define PSK_TOKEN_TEXT 33

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
    PSK_SHEET_SAVE,     /* SAVE: where the file goes and who can read it (ADR-016 rule 16) */
    PSK_SHEET_DELETE,   /* delete one saved capture (ADR-016 rule 10) */
    PSK_SHEET_REQUEST,  /* a companion asks for a capture: START or REFUSE (ADR-016 rule 1a) */
};

/* What a window was last asked to be anchored at, so its answer can put the
 * anchor at the top of the list. */
enum psk_jump { PSK_JUMP_NONE = 0, PSK_JUMP_SEQ, PSK_JUMP_RANK, PSK_JUMP_TIME };

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
    PSK_ACT_LIVE,       /* PACKETS: back to the newest packets (FOLLOW on) */
    PSK_ACT_FILTER_BY,  /* INSPECT: filter on the selected field */
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
    char ssid[48]; /* Wi-Fi: the joined network's display text from netd, or empty */
};

struct psk_ifrow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *sub;
    lv_obj_t *chip;
};

/* One row of the list's pool (DS §59.6: two Mono 14 lines in 56 px). */
struct psk_listrow {
    lv_obj_t *row;
    lv_obj_t *bar;   /* the 4 px class bar */
    lv_obj_t *time;
    lv_obj_t *dir;
    lv_obj_t *word;
    lv_obj_t *addr;
    lv_obj_t *len;
    lv_obj_t *gap;
    lv_obj_t *info;
    lv_obj_t *pin;
    uint32_t seq;    /* 0: shows nothing */
    bool selected;   /* the selected outline is on (applied only on a change) */
};

struct psk_layerrow {
    lv_obj_t *row;
    lv_obj_t *bar;
    lv_obj_t *tri;
    lv_obj_t *name;
    lv_obj_t *summary;
    lv_obj_t *fields; /* the column its fields go in */
    lv_obj_t *finding; /* the packet's findings about this layer (M7) */
    lv_obj_t *detail;  /* what the DNS and TCP diagnosis says of it (M11.1) */
};

struct psk_fieldrow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *value;
    bool selected;
};

struct psk_app {
    lv_obj_t *frame;
    struct pocketui_layout_guard guard;
    bool relayout;

    /* the shape (M9) */
    bool wide;           /* the top row of its own, list and inspector side by side */
    bool shaped;         /* psk_shape() has run for `wide` */
    bool back_pending;   /* the app's own back slab was tapped */
    bool key_nav;        /* the inspected packet was last moved by a key */
    int row_h;           /* a list row: PSK_ROW_H, PSK_ROW_H_WIDE when wide */
    int32_t root_pad_top; /* the body's own top padding, given back in the tall shape */
    int32_t top_inset_l; /* the top row's own insets at the display's top edge */
    int32_t top_inset_r;
    lv_obj_t *top;       /* the row holding the tabs (and, wide, back, status and bar) */
    lv_obj_t *back;
    lv_obj_t *tabs;
    lv_obj_t *status_row;
    lv_obj_t *content;
    lv_obj_t *live_l;
    lv_obj_t *live_r;
    lv_obj_t *setup_l;
    lv_obj_t *setup_r;

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
    lv_obj_t *traffic;   /* the TRAFFIC panel (M4) */
    lv_obj_t *rate;
    lv_obj_t *throughput;
    lv_obj_t *graph;     /* ui/psk_graph.c */
    lv_obj_t *readout;
    lv_obj_t *protos;    /* the PROTOCOLS panel (M4) */
    lv_obj_t *proto_cell[PPC_CLASS_COUNT];
    lv_obj_t *proto_swatch[PPC_CLASS_COUNT];
    lv_obj_t *proto_word[PPC_CLASS_COUNT];
    lv_obj_t *proto_rate[PPC_CLASS_COUNT];
    lv_obj_t *proto_share[PPC_CLASS_COUNT];
    lv_obj_t *proto_bar[PPC_CLASS_COUNT];
    lv_obj_t *proto_paint;
    bool proto_rate_below; /* each rate on a line of its own under its word (DS §46.5) */
    lv_obj_t *cell[4];   /* PACKETS, BYTES, DROPPED, BUFFER */
    bool cells_two_by_two; /* SESSION's figures two by two, as they do not fit four in a line */
    bool sig_words_wrap;   /* tall: the findings' words on two lines, as one does not fit its cell */
    bool fit_due;          /* LIVE's fits to be made again once it is shown: its shape or the text
                            * size changed, which a quiet capture would not repaint */
    lv_obj_t *later;     /* what leaving does to the capture */
    lv_obj_t *signals;   /* the SIGNALS panel (M7) */
    lv_obj_t *sig_cell[PPC_SIGNAL_COUNT];
    lv_obj_t *sig_val[PPC_SIGNAL_COUNT];
    lv_obj_t *diag_box;                  /* the two below: in SIGNALS, or (wide) under TRAFFIC */
    lv_obj_t *diag_row[PSK_DIAG_KINDS];  /* the DNS and TCP diagnosis on SIGNALS (M11.1) */
    lv_obj_t *diag_sev[PSK_DIAG_KINDS];
    lv_obj_t *diag_text[PSK_DIAG_KINDS];
    lv_obj_t *saves_row; /* setup: the saved captures (M8) */
    lv_obj_t *saves_val;

    /* LIVE: the helper stopped */
    lv_obj_t *error;
    lv_obj_t *err_reason;
    lv_obj_t *err_iface;
    lv_obj_t *err_report;
    lv_obj_t *last[4];   /* PACKETS, BYTES, DROPPED, SAVED */
    bool last_two_by_two;

    /* PACKETS (M5): the view the app asked for, and the list's position */
    uint32_t view_id;
    uint8_t sort;        /* enum ppc_sort: 0 time, 1 newest, 2 len */
    uint8_t classes;     /* PPC_CLASSES_ALL or one class's bit */
    char host[PPC_HOST_TEXT];
    uint16_t port;
    bool pinned_only;
    uint8_t sigmask;     /* PPC_SIG_* bits: only packets with one of these findings; 0: any */
    uint8_t diagf;       /* enum ppc_diag_code: only packets with this diagnosis; 0: any (M11.1) */
    bool marks_due;      /* the rail's finding marks are to be asked for */
    int64_t marks_sent_ms;
    bool follow;         /* the list keeps to the newest packets */
    uint32_t top_rank;   /* the rank of the top row shown */
    uint32_t top_seq;    /* its packet, which anchors the next window */
    enum psk_jump jump;  /* the anchor of the window asked for last */
    uint64_t jump_at;
    int vis;             /* rows that fit */
    uint32_t sel_seq;    /* the outlined row: the inspected packet */
    bool window_due;
    bool window_inflight;
    int64_t window_sent_ms;
    uint32_t window_last_seq;
    int64_t retry_ms;
    bool dragging;
    bool dragged;
    int32_t drag_y0;
    uint32_t drag_rank0;
    lv_obj_t *pk_tools;
    lv_obj_t *pk_filter;
    lv_obj_t *pk_sum1;
    lv_obj_t *pk_sum2;
    lv_obj_t *pk_sort;
    lv_obj_t *pk_follow;
    lv_obj_t *pk_head;
    lv_obj_t *pk_body;
    lv_obj_t *pk_list;
    lv_obj_t *pk_rail;
    lv_obj_t *pk_foot;
    lv_obj_t *pk_foot_l;
    lv_obj_t *pk_foot_r;
    lv_obj_t *pk_none;   /* a filter that matches nothing */
    lv_obj_t *pk_empty;  /* no capture: the panel */
    lv_obj_t *pk_title;
    lv_obj_t *pk_text;
    struct psk_listrow lrow[PSK_LIST_ROWS];
    int32_t char_w;      /* the fixed face's advance, for the columns */
    bool sum_below;      /* the filter in words on a line of its own (DS §46.5) */

    /* INSPECT (M6) */
    uint32_t insp_seq;   /* the packet asked for last; 0: none */
    uint32_t insp_shown; /* the packet the page shows */
    int insp_field;      /* the selected field, -1 none */
    bool layer_open[PSK_LAYERS];
    char insp_note[96];  /* "! THE PACKET HAS LEFT THE BUFFER", ... */
    lv_obj_t *in_empty;
    lv_obj_t *in_body;
    lv_obj_t *in_num;
    lv_obj_t *in_word;
    lv_obj_t *in_finding;
    lv_obj_t *in_meta1;
    lv_obj_t *in_meta2;
    lv_obj_t *in_note;
    lv_obj_t *in_pin;
    lv_obj_t *in_prev;
    lv_obj_t *in_next;
    lv_obj_t *in_end[2];
    lv_obj_t *in_end_addr[2];
    lv_obj_t *in_end_sub[2];
    struct psk_layerrow layer[PSK_LAYERS];
    struct psk_fieldrow field[PSK_FIELDS];
    lv_obj_t *in_bytes_row;
    lv_obj_t *in_bytes_tri;
    lv_obj_t *in_bytes_sum;
    lv_obj_t *in_hex;
    lv_obj_t *in_hex_note;
    bool bytes_open;
    bool pin_selected;

    /* the filter sheet (M5): a draft, applied by SHOW */
    lv_obj_t *fsheet;
    lv_obj_t *fs_cls[PPC_CLASS_COUNT + 1];
    lv_obj_t *fs_host;
    lv_obj_t *fs_port;
    lv_obj_t *fs_pinned;
    lv_obj_t *fs_sig;
    uint8_t fs_sigv;
    uint8_t fs_diagv;    /* the diagnosis a diagnosis line chose (M11.1); cleared by the Findings row */
    lv_obj_t *fs_host_row;
    lv_obj_t *fs_port_row;
    uint8_t fs_classes;
    char fs_hostv[PPC_HOST_TEXT];
    uint16_t fs_portv;
    bool fs_pinnedv;
    bool fs_to_packets;  /* opened from LIVE: SHOW goes to PACKETS */
    int fs_shown;        /* the class choice outlined, -1 none */

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

    /* the saved captures (M8): the newest few, read from the folder */
    lv_obj_t *ssheet;
    lv_obj_t *ss_row[PSK_SAVES_ROWS];
    lv_obj_t *ss_name[PSK_SAVES_ROWS];
    lv_obj_t *ss_sub[PSK_SAVES_ROWS];
    lv_obj_t *ss_note;
    struct psk_saved saves[PSK_SAVES_ROWS];
    int nsaves;
    uint32_t saves_count;
    uint64_t saves_bytes;
    char del_name[PPC_SAVE_NAME];

    /* the diagnosis sheet (M11.1): DNS or TCP, its lines, each a way to the
     * packets it counts or the packet it names */
    lv_obj_t *dsheet;
    lv_obj_t *ds_title;
    lv_obj_t *ds_note;
    lv_obj_t *ds_row[PSK_DIAG_LINES];
    lv_obj_t *ds_text[PSK_DIAG_LINES];
    lv_obj_t *ds_why[PSK_DIAG_LINES];
    lv_obj_t *ds_go[PSK_DIAG_LINES];
    int ds_tone[PSK_DIAG_LINES];
    struct psk_diag_line ds_lines[PSK_DIAG_LINES];
    unsigned ds_n;
    unsigned ds_kind;

    /* state */
    struct psk_session *session;
    struct psk_model *model;
    lv_timer_t *timer;
    struct psk_iface ifs[PSK_IFACES];
    int nifs;
    int sel;             /* index into ifs, or -1 */
    char chosen[PPC_IFACE_MAX]; /* the interface last tapped: chosen again when usable */
    bool simulated;      /* a synthetic or replayed source stands in for the interfaces */
    enum ppc_keep keep;
    int limit_idx;
    enum ppc_cfilter cfilter;
    bool starting;       /* START pressed, the capture not yet confirmed */
    bool run_full;       /* what the capture command last sent asked for: FULL, */
    uint32_t run_limit_s; /* and its time limit (capture.status reports these) */
    struct psk_remote_emit emit; /* which of capture.state and capture.stats were raised, and when */
    /* a start request (capture.request_start; ADR-016 rule 1a): what its
     * sheet says and what START on it starts. Held only while the sheet is
     * up: never queued, never remembered */
    char req_client[PSK_CLIENT_TEXT];
    char req_iface[PPC_IFACE_MAX];
    uint32_t req_limit_s;
    int64_t req_deadline_ms;
    int64_t req_shown_ms;
    /* the person's own setup choices, put back when a requested capture is over */
    bool req_saved;
    int req_saved_limit;
    char req_saved_chosen[PPC_IFACE_MAX];
    uint8_t req_outcome; /* enum psk_remote_reason: REFUSED or TIMEOUT until it has been said; 0 none */
    uint8_t req_last;    /* enum psk_remote_request: how the last request ended, for capture.status */
    char req_token[PSK_TOKEN_TEXT]; /* what the asker called this request: a cancel must name it */
    bool by_companion;   /* the capture was started from that sheet */
    /* rule 20: somebody outside the app is reading */
    bool reader_seen;
    int64_t reader_ms;   /* the last capture.* request */
    bool reading;        /* the header says so */
    bool pending_start;  /* the capture command waits for the helper's hello */
    bool discarding;     /* discard sent: the helper is on its way out */
    bool failed_view;    /* the helper ended a capture badly: the error panel */
    enum psk_view view;
    char notice[96];     /* why the last START was refused, on the setup banner */
    int64_t next_ifs_ms;
    char hint[48];

    /* Cost, measured (DS §59.9, M4-M6 brief B5): the worst poll (read, apply,
     * paint) and the worst display refresh while the app is open, in
     * microseconds, logged once a minute at debug level. */
    int64_t worst_poll_us;
    int64_t worst_frame_us;
    int64_t frame_start_us;
    int64_t next_cost_log_ms;
};

/* Test seams (tests/packetshark_app_test.c): a named part of the screen
 * ("traffic", "graph", "protocols", "session", "status", "tab0".."tab2",
 * "bar0".."bar2", "readout", "proto0".."proto7"; M5/M6: "list", "rail",
 * "row0".."row9", "filter", "sort", "follow", "prev", "next", "pin",
 * "end0", "end1", "hex", "layer0".."layer7", "field0".."field9"; M7-M9:
 * "signals", "sig0".."sig5", "saves", "save0".."save5", "top", "back",
 * "inspect", "finding0".."finding7"; M11.1: "diag0" (DNS), "diag1" (TCP),
 * "dsheet", "dline0".."dline9", "detail0".."detail7"), and the measured
 * costs. */
lv_obj_t *psk_app_part(void *priv, const char *name);
void psk_app_cost(void *priv, int64_t *worst_poll_us, int64_t *worst_frame_us, int reset);

/* psk_app.c */
int64_t psk_now(void);
void psk_do(struct psk_app *a, enum psk_action act);
/* Whether the capture bar offers HOLD, RESUME or STOP now: a capture is on
 * the screen and its state has that button. */
bool psk_action_offered(const struct psk_app *a, enum psk_action act);
void psk_select_tab(struct psk_app *a, enum psk_tab t);
void psk_select_iface(struct psk_app *a, int index);
/* Whether an interface can be captured on: it has a carrier (Ethernet with a
 * cable, Wi-Fi associated), or it is the simulator's stand-in. */
bool psk_iface_usable(const struct psk_iface *f);
void psk_option(struct psk_app *a, int which);
void psk_sheet_answer(struct psk_app *a, bool go);
/* PACKETS and INSPECT (M5, M6) */
void psk_set_view(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned);
/* The same with the findings to show (PPC_SIG_* bits, 0: any packet). */
void psk_set_view_signals(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned,
                          unsigned signals);
void psk_show_signal(struct psk_app *a, unsigned index);
/* The same with a diagnosis to show (enum ppc_diag_code, 0: any; M11.1). */
void psk_set_view_diag(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned,
                       unsigned signals, unsigned diag);
/* A diagnosis line's way: its packets in PACKETS, or its packet in INSPECT. */
void psk_diag_go(struct psk_app *a, const struct psk_diag_line *l);
/* INSPECT is on screen: its tab, or beside the list in the wide shape. */
bool psk_inspect_visible(const struct psk_app *a);
/* The saved captures (M8). */
void psk_saves_refresh(struct psk_app *a);
void psk_saved_delete_ask(struct psk_app *a, int index);
/* A key from the keyboard (M9). Returns true when it was used. */
bool psk_key(struct psk_app *a, uint32_t key);
void psk_set_sort(struct psk_app *a, unsigned sort);
void psk_set_follow(struct psk_app *a, bool on);
void psk_show_second(struct psk_app *a, int64_t second); /* < 0: the newest */
void psk_show_class(struct psk_app *a, unsigned cls);
void psk_list_scroll_to(struct psk_app *a, uint32_t rank, bool final);
void psk_open_packet(struct psk_app *a, uint32_t seq, int step);
void psk_select_field(struct psk_app *a, int index);
void psk_filter_by(struct psk_app *a, const char *token);
void psk_toggle_pin(struct psk_app *a);
bool psk_capture_present(const struct psk_app *a);

/* ui/psk_live.c */
void psk_live_build(struct psk_app *a);
void psk_live_paint(struct psk_app *a, unsigned changed);
/* Gives the figures the shape they fit in at the text size in force: four
 * SESSION figures in a line or two by two, a class's rate beside its word or
 * under it (DS §46.5). Lays out first if a part has not been yet, so it is
 * called from the poll timer and at create, never inside a layout pass. */
void psk_live_fit(struct psk_app *a);
void psk_paint_ifaces(struct psk_app *a);
void psk_paint_options(struct psk_app *a);
void psk_paint_tabs(struct psk_app *a);
/* Puts the parts where the shape has them: tall (the shell's header above,
 * the tabs, a page, the capture bar at the foot) or wide (M9). */
void psk_shape(struct psk_app *a);
const char *psk_cfilter_label(enum ppc_cfilter f);

/* ui/psk_sheets.c */
void psk_sheet_build(struct psk_app *a);
void psk_sheet_show(struct psk_app *a, enum psk_sheet kind);
void psk_sheet_hide(struct psk_app *a);
void psk_fsheet_build(struct psk_app *a);
void psk_fsheet_show(struct psk_app *a, bool to_packets);
void psk_fsheet_hide(struct psk_app *a);
bool psk_fsheet_shown(const struct psk_app *a);
/* The filter in words: "ALL PROTOCOLS", "TCP · HOST 192.0.2.10", "RESETS",
 * "REFUSED CONNECTIONS" (a diagnosis, M11.1). */
const char *psk_filter_words(uint8_t classes, const char *host, uint16_t port, bool pinned, unsigned signals,
                             unsigned diag, char *buf, size_t buflen);
/* The diagnosis sheet (M11.1): DNS or TCP (enum psk_diag_kind). */
void psk_dsheet_build(struct psk_app *a);
void psk_dsheet_show(struct psk_app *a, unsigned kind);
void psk_dsheet_paint(struct psk_app *a);
void psk_dsheet_hide(struct psk_app *a);
bool psk_dsheet_shown(const struct psk_app *a);
/* The saved captures' sheet (M8). */
void psk_ssheet_build(struct psk_app *a);
void psk_ssheet_show(struct psk_app *a);
void psk_ssheet_hide(struct psk_app *a);
bool psk_ssheet_shown(const struct psk_app *a);
void psk_ssheet_paint(struct psk_app *a);

/* ui/psk_list.c */
void psk_list_build(struct psk_app *a, lv_obj_t *page);
void psk_list_paint(struct psk_app *a);
/* Rows that fit and the columns, from the laid-out list (the poll timer). */
void psk_list_fit(struct psk_app *a);
/* The rows' height and lines for the shape in force (M9). */
void psk_list_shape(struct psk_app *a);
/* The class bar of a row or a layer: a 4 px object drawn in the class's
 * hue (cls < 0 or OTHER: text_secondary), DS §59.2. */
lv_obj_t *psk_class_bar(lv_obj_t *parent);
void psk_class_bar_set(lv_obj_t *bar, int cls);
/* A colour role over a label (or none: -1; -10 - hue: an identity hue);
 * the role in force is kept in the user data, so only a change is applied,
 * and only the style in force is removed. fixed puts the fixed Small face
 * back on top for the list's labels when the role brings a font of its own.
 * Every style added or removed re-lays the label out: the list's paint
 * changes as few as it can. */
void psk_tone(lv_obj_t *lb, int role, bool fixed);
/* A role style on or off, applied only on a change (adding a style that is
 * there removes and re-adds it, two refreshes). */
void psk_style_on(lv_obj_t *obj, enum pos_style_role role, bool on, bool *state);

/* ui/psk_inspect.c */
void psk_inspect_build(struct psk_app *a, lv_obj_t *page);
void psk_inspect_paint(struct psk_app *a);

/* small LVGL helpers, ui/psk_live.c */
void psk_set_hidden(lv_obj_t *obj, bool hidden);
void psk_text(lv_obj_t *label, const char *text);
lv_obj_t *psk_box(lv_obj_t *parent);
lv_obj_t *psk_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user);
void psk_button_look(lv_obj_t *b, bool primary, bool enabled);

#endif
