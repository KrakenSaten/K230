/*
 * PacketShark's model in the shell: what its screens show, and nothing more
 * (DESIGN_DECISIONS D1, accepted: the helper owns the packets; the shell is
 * a viewport with a cache of at most PSK_ROW_CACHE rows).
 *
 * It is filled from the helper's lines (core/pocketpcap/ppc_proto.h) by
 * psk_session_poll() and read by the screens. It holds no packet bytes:
 * counters, the class bins, the last error, how the helper ended, the text
 * of the rows a window last brought (the list, M5), and the text of the one
 * packet the inspector shows (M6) - its fields and the hex lines the helper
 * formatted, never the bytes themselves.
 *
 * Pure C, no LVGL, no allocation (tests/psk_model_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_MODEL_H
#define PSK_MODEL_H

#include "pocketpcap/ppc_proto.h"

#include <stdbool.h>
#include <stdint.h>

#define PSK_ROW_CACHE 48
#define PSK_BINS 120
#define PSK_LAYERS 8
#define PSK_FIELDS 64
#define PSK_HEX_LINES 64
#define PSK_FINDINGS 4
#define PSK_DETAILS 6     /* what the DNS and TCP diagnosis says of the packet (M11.1) */
#define PSK_DIAG_LINES 16 /* the diagnosis sheet's lines, at most */

/* How the helper ended, as the app shows it. */
enum psk_exit {
    PSK_EXIT_NONE = 0,  /* still running, or never started */
    PSK_EXIT_NORMAL,    /* it left when asked (quit, discard) */
    PSK_EXIT_START,     /* it could not be started: no helper at that path */
    PSK_EXIT_CRASHED,   /* a signal */
    PSK_EXIT_HUNG,      /* no hello in time, or silent while capturing: killed */
    PSK_EXIT_PROTOCOL,  /* it said things that do not parse: killed */
    PSK_EXIT_UNEXPECTED,/* it left on its own while a capture was alive */
};

/* What changed in one poll, so the screens repaint only that. */
#define PSK_CHANGED_HELLO 0x01u
#define PSK_CHANGED_STATE 0x02u
#define PSK_CHANGED_STATS 0x04u
#define PSK_CHANGED_BIN 0x08u
#define PSK_CHANGED_ERROR 0x10u
#define PSK_CHANGED_EXITED 0x20u
#define PSK_CHANGED_ROWS 0x40u
#define PSK_CHANGED_NOTE 0x80u
#define PSK_CHANGED_PACKET 0x100u /* the inspector's packet arrived whole */
#define PSK_CHANGED_PINS 0x200u
#define PSK_CHANGED_MARKS 0x400u /* where the view's findings are (M7) */
#define PSK_CHANGED_SAVE 0x800u  /* SAVE's progress or outcome (M8) */
#define PSK_CHANGED_DIAG 0x1000u /* the DNS and TCP diagnosis (M11.1) */
#define PSK_CHANGED_NET 0x2000u  /* the network intelligence's totals (M12.1) */
#define PSK_CHANGED_NETLIST 0x4000u /* its devices, servers, leases and events arrived whole */

/* The inspector's packet, as text (M6). */
struct psk_layer {
    int cls;                       /* -1: none */
    char name[PPC_FIELD_NAME];
    char text[PPC_FIELD_TEXT];
};

struct psk_field {
    uint8_t layer;
    char name[PPC_FIELD_NAME];
    char text[PPC_FIELD_TEXT];
    int32_t off;                   /* -1: no bytes */
    uint32_t len;
    char filter[PPC_FILTER_TEXT];  /* "" when it filters nothing */
};

/* A finding of the inspected packet, under one of its layers (M7). */
struct psk_finding {
    uint8_t layer;
    char text[PPC_FIELD_TEXT];
};

struct psk_hex {
    uint32_t off;
    char hex[PPC_HEX_TEXT];
    char ascii[PPC_ASCII_TEXT];
};

struct psk_packet {
    struct ppc_evt_pkt head;
    struct ppc_evt_end src;
    struct ppc_evt_end dst;
    uint32_t nlayers;
    struct psk_layer layer[PSK_LAYERS];
    uint32_t nfields;
    struct psk_field field[PSK_FIELDS];
    uint32_t nhex;
    struct psk_hex hex[PSK_HEX_LINES];
    uint32_t nfindings;
    struct psk_finding finding[PSK_FINDINGS];
    uint32_t ndetails;                    /* M11.1: not faults, said plainly */
    struct psk_finding detail[PSK_DETAILS];
};

/* The network intelligence as the helper last told it (M12.1): the totals
 * from its `net` line, and the lists of one whole answer to `net`. Text and
 * figures only, like the rest of the model. */
struct psk_net_lists {
    uint32_t nhosts;
    struct ppc_evt_net_host host[PPC_NET_WIRE_HOSTS];
    uint32_t nservers;
    struct ppc_evt_net_srv srv[PPC_NET_WIRE_SERVERS];
    uint32_t nleases;
    struct ppc_evt_net_lease lease[PPC_NET_WIRE_LEASES];
    uint32_t nevents;
    struct ppc_evt_net_event evt[PPC_NET_WIRE_EVENTS];
};

/* SAVE as the app shows it (M8). */
enum psk_save { PSK_SAVE_NONE = 0, PSK_SAVE_RUNNING, PSK_SAVE_DONE, PSK_SAVE_FAILED };

struct psk_model {
    /* the helper */
    bool running;
    bool hello;
    char version[32];
    char backend[16];
    enum psk_exit exit;
    int exit_value;           /* exit status, or 128 + signal */
    bool had_capture;         /* a start was accepted in this helper's life */
    char caps[64];            /* the helper's privilege note, for the docs and the gate */
    bool shed;

    /* the session */
    enum ppc_state state;
    enum ppc_reason reason;
    char iface[PPC_IFACE_MAX];
    uint32_t speed;
    bool clock_set;
    bool have_stats;
    struct ppc_counters c;
    int64_t stats_ms;         /* when the last stats line came */

    /* the graph's bins, by elapsed second (M4 draws them) */
    uint32_t bins[PSK_BINS][PPC_CLASS_COUNT];
    uint32_t bin_held[PSK_BINS];
    uint64_t bin_next;        /* the second after the newest bin */

    /* the last refusal */
    char err_code[16];
    char err_text[PPC_TEXT_MAX];
    int64_t err_ms;

    /* the row cache (M5 fills it); a window answer is taken only for the
     * request asked last, so a late answer to an older one is dropped */
    uint32_t want_req;
    bool rows_open;
    uint32_t row_count;
    uint32_t rows_first_seq;
    uint32_t rows_last_seq;
    uint32_t rows_filter_id;  /* the view the rows belong to */
    uint32_t rows_first_rank; /* rows[i] is at rank rows_first_rank + i */
    uint32_t rows_match;      /* packets in that view */
    uint32_t rows_anchor;     /* the rank the window was anchored at */
    struct ppc_evt_row rows[PSK_ROW_CACHE];

    /* the inspector's packet: filled in pkt_next while its lines arrive,
     * copied to pkt when the last one has (a half packet is never shown);
     * only the answer to the newest request is taken */
    uint32_t want_pkt;
    bool pkt_open;
    bool have_pkt;
    struct psk_packet pkt;
    struct psk_packet pkt_next;

    /* pins: how many the helper holds, and the last change */
    uint32_t pins;
    uint32_t pin_seq;
    bool pin_on;

    /* the rail's marks (M7): the view cut into marks_buckets parts, a bit
     * for each part that holds a finding; only the newest request's answer */
    uint32_t want_marks;
    uint32_t marks_view;
    uint32_t marks_match;
    uint32_t marks_buckets;
    uint64_t marks_mask;

    /* SAVE (M8): the last save of this capture */
    enum psk_save save;
    uint32_t save_packets;
    uint32_t save_total;
    uint64_t save_bytes;
    char save_name[PPC_SAVE_NAME]; /* the file's name once it is whole */
    char save_fail[16];            /* why it failed: nospace, nodir, io, empty, name */

    /* the DNS and TCP diagnosis (M11.1): the helper's last diag line */
    bool have_diag;
    struct ppc_diag d;

    /* the network intelligence (M12.1): the last totals, and the lists of
     * the newest request's answer, filled in net_next while its lines come
     * and copied when the last one has (half an answer is never shown) */
    bool have_net;
    struct ppc_net_totals nt;
    uint32_t want_net;
    bool net_open;
    bool have_net_lists;
    struct psk_net_lists net;
    struct psk_net_lists net_next;
};

void psk_model_init(struct psk_model *m);

/* A new helper is starting: the session's figures are kept (they are the
 * LAST SESSION the error panel shows) until its hello. */
void psk_model_helper_started(struct psk_model *m, int64_t now_ms);
void psk_model_helper_stopped(struct psk_model *m, enum psk_exit why, int value, int64_t now_ms);

/* Apply one parsed line. Returns PSK_CHANGED_* bits. */
unsigned psk_model_apply(struct psk_model *m, const struct ppc_evt *e, int64_t now_ms);

/* Forget the rows and the inspected packet (a new capture, a helper gone). */
void psk_model_clear_packets(struct psk_model *m);

/* The session is in a state where its packets are in the helper. */
bool psk_model_has_packets(const struct psk_model *m);

/* "CRASHED (SIG 11)", "NOT RESPONDING", "HELPER MISSING", ... upper case. */
const char *psk_exit_text(enum psk_exit e, int value, char *buf, size_t buflen);

/* Figures as the screens print them, all upper case and ASCII:
 *   count    48 417            (groups of three, a space between)
 *   bytes    980 B, 12.4 KB, 31.4 MB, 2.1 GB
 *   bits     0 BIT/S, 812 KBIT/S, 3.4 MBIT/S
 *   elapsed  00:07, 04:13, 1:02:03 */
const char *psk_fmt_count(uint64_t n, char *buf, size_t buflen);
const char *psk_fmt_bytes(uint64_t n, char *buf, size_t buflen);
const char *psk_fmt_bits(uint64_t bps, char *buf, size_t buflen);
const char *psk_fmt_elapsed(uint64_t ms, char *buf, size_t buflen);

/* ---- the graph and the protocol distribution (M4, DS §59.2, §59.5) -------- */

#define PSK_GRAPH_SECONDS 60
#define PSK_GRAPH_BAND 208 /* px above the baseline */

/* Column height on the fixed logarithmic ladder: round(4 + 50 * log10 n),
 * capped at PSK_GRAPH_BAND; 0 for none. 1 -> 4, 10 -> 54, 100 -> 104,
 * 1,000 -> 154, 10,000 -> 204. */
int32_t psk_graph_height_of(uint64_t n);

/* The class's identity hue (DS §59.2: TCP 6, UDP 5, DNS 4, DHCP 1, ARP 2,
 * ICMP 7, IPv6 3), or -1 for OTHER, which is drawn in text_secondary. */
int psk_class_hue(unsigned cls);
/* "TCP", "UDP", "DNS", "DHCP", "ARP", "ICMP", "IPV6", "OTHER". */
const char *psk_class_word(unsigned cls);

/* The last PSK_GRAPH_SECONDS bins, oldest first: per-class counts, held
 * counts, whether the second was watched at all (a second before the capture
 * started was not), and the absolute second of the newest column. */
struct psk_graph_window {
    uint32_t cls[PSK_GRAPH_SECONDS][PPC_CLASS_COUNT];
    uint32_t held[PSK_GRAPH_SECONDS];
    uint8_t watched[PSK_GRAPH_SECONDS];
    int64_t newest;     /* -1 before the first second has been binned */
};

void psk_model_graph(const struct psk_model *m, struct psk_graph_window *w);

/* The readout of one second (DS §59.5): "03:55 · 6 885 PKT · TCP 99%", or
 * "03:56 · HELD · 1 204 PASSED" for a held second. sec < 0: the newest. */
const char *psk_model_readout(const struct psk_model *m, int64_t sec, char *buf, size_t buflen);

/* Packets per second of a class in the newest complete second, and its share
 * of the session in tenths of a percent (from the stats line's totals). */
uint32_t psk_model_class_rate(const struct psk_model *m, unsigned cls);
uint32_t psk_model_class_share_permille(const struct psk_model *m, unsigned cls);

/* ---- the list and the inspector (M5, M6) ------------------------------------- */

/* A row's time and gap as the list prints them: "04:11.902114" (minutes
 * past 99 grow the field) and "+0.210447". */
const char *psk_fmt_rel(uint64_t us, char *buf, size_t buflen);
const char *psk_fmt_gap(uint64_t us, char *buf, size_t buflen);
/* The direction glyph of a row: up for out, down for in, a dot for
 * broadcast and multicast (UTF-8). */
const char *psk_dir_glyph(const char *dir);

/* The word of a finding by its bit's index (PPC_SIG_*): "RETRANS",
 * "TCP RST", "DNS FAIL", "UNREACH", "NO ANSWER", "NO DNS REPLY"; and the
 * plural a filter is named by: "RETRANSMISSIONS", "RESETS", "DNS FAILURES",
 * "UNREACHABLES", "UNANSWERED SYNS", "UNANSWERED DNS QUERIES". */
const char *psk_signal_word(unsigned index);
const char *psk_signal_plural(unsigned index);

/* ---- the DNS and TCP diagnosis (M11.1, docs/apps/PACKETSHARK.md §4b) --------- */

/* How a line of the diagnosis reads: as it should be, worth knowing, or worth
 * a look (status_ok, text_secondary, status_warn with `!`; DS §59.2). */
enum psk_sev { PSK_SEV_OK = 0, PSK_SEV_NOTE, PSK_SEV_WARN };
/* Where a line leads: nowhere, the packets it counts, the packet it names,
 * the packets of a class and a host (M12.1). */
enum psk_go { PSK_GO_NONE = 0, PSK_GO_LIST, PSK_GO_OPEN, PSK_GO_HOST };
enum psk_diag_kind { PSK_DIAG_DNS = 0, PSK_DIAG_TCP, PSK_DIAG_KINDS };
/* M12.1: what else the same sheet shows (psk_net.h). LIVE's own two lines
 * stay the two above; these are reached from the network overview. */
enum {
    PSK_DIAG_DHCP = PSK_DIAG_KINDS,
    PSK_DIAG_ARP,
    PSK_DIAG_NET,     /* the overview's summary: a line, no sheet of its own */
    PSK_DIAG_FINDING, /* one event and its evidence */
    PSK_DIAG_ALL
};

struct psk_diag_line {
    uint8_t sev;       /* enum psk_sev */
    uint8_t go;        /* enum psk_go */
    uint8_t signals;   /* PSK_GO_LIST: the findings to show (PPC_SIG_*), or 0 */
    uint8_t diag;      /* PSK_GO_LIST: the diagnosis to show (enum ppc_diag_code), or 0 */
    uint32_t seq;      /* PSK_GO_OPEN: the packet */
    uint8_t classes;   /* PSK_GO_HOST: the classes to show (a mask), */
    char host[PPC_HOST_TEXT]; /* and the host, "" for any (M12.1) */
    char text[64];     /* "2 NAME NOT FOUND (NXDOMAIN)", upper case */
    char why[288];     /* the explanation, a sentence or two */
};

/* The line on LIVE: "12 ANSWERED · AVG 23 MS · 2 FAILED". Returns its
 * severity (enum psk_sev). */
unsigned psk_diag_summary(const struct psk_model *m, unsigned kind, char *buf, size_t buflen);
/* The diagnosis sheet's lines, the summary first; how many. */
unsigned psk_diag_lines(const struct psk_model *m, unsigned kind, struct psk_diag_line *out, unsigned max);
/* How far the diagnosis can be trusted: what it rests on, what passed
 * unseen, what the tables had to forget. */
const char *psk_diag_confidence(const struct psk_model *m, unsigned kind, char *buf, size_t buflen);
/* A diagnosis's plural, as a filter is named by: "REFUSED CONNECTIONS". */
const char *psk_diag_plural(unsigned diag);
/* A duration in microseconds: "0.4 MS", "23 MS", "1.2 S". */
const char *psk_fmt_ms(uint64_t us, char *buf, size_t buflen);
/* Why a save failed, as a sentence in upper case. */
const char *psk_save_fail_text(const char *code);

/* A filter token from a field ("host:ADDR", "port:N", "class:N") applied to
 * a view's parts. Returns 0, or -1 for a token it does not know. */
int psk_filter_token(const char *token, uint8_t *classes, char *host, size_t hostlen, uint16_t *port);

#endif
