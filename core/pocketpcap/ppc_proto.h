/*
 * PacketShark's line protocol between the shell and pos-capture
 * (ARCHITECTURE §3; DESIGN_DECISIONS D2).
 *
 * Transport: one AF_UNIX stream socketpair, the helper's stdin and stdout.
 * Lines of at most PPC_LINE_MAX bytes ending in '\n'. Helper-to-shell lines
 * separate fields with a tab. Shell-to-helper lines may use tabs or spaces,
 * so the helper can be driven by hand from a serial console during bring-up
 * (BusyBox ash completes a typed tab). Text derived from a packet never
 * carries a tab or newline (ppc_format.h sanitizes it).
 *
 * Implemented (M2/M3; view, the window's rank and time anchors, inspect and
 * pin since M5/M6, protocol 2; the view's findings, marks, save and the
 * finding line since M7/M8, protocol 3):
 *
 *   shell -> helper
 *     start <iface> <headers|full> <promisc 0|1> <cfilter> <limit_s>
 *     hold <0|1>
 *     stop
 *     discard
 *     view <id> <time|newest|len> <classes> <host|-> <port> <pinned 0|1> [<signals>]
 *                                     the list's display filter and sort;
 *                                     classes is a hex mask of enum ppc_class,
 *                                     signals a hex mask of PPC_SIG_* (M7): only
 *                                     packets with one of those findings
 *     window <req> <tail|seq:N|rank:N|us:N> <before> <after>
 *                                     at most PPC_WINDOW_MAX rows of the view
 *     inspect <req> <seq> <step -1|0|1>
 *                                     one packet's layers, fields and bytes; a
 *                                     step moves to its neighbour in the view
 *     pin <seq> <0|1>
 *     marks <req> <buckets>           where the view's findings are (M7): the
 *                                     view cut into at most 64 equal parts
 *     save                            write the stopped capture to a file (M8)
 *     stats                           one snapshot now
 *     quit
 *
 *   helper -> shell
 *     hello <proto> <version> <backend>
 *     state <idle|capturing|hold|saving|stopped> <reason> <iface|-> <speed_mbps> <clock_set>
 *     stats <elapsed_ms> <seen> <stored> <held> <kdrop> <ifdrop> <shed> <bytes> <pps> <bps>
 *           <first_seq> <last_seq> <match> <fill_permille> <ui_skipped> <c0..c7> <s0..s4>
 *     bin <second> <c0..c7> <held>
 *     rows.begin <req> <filter_id> <first_rank> <match> <first_seq> <last_seq> [<anchor_rank>]
 *     row <seq> <rel_us> <gap_us> <dir> <class> <word> <len> <signal> <src> <dst> <info> [<flags>]
 *     rows.end <req>
 *     pkt.begin <req> <seq> <rank> <match> <rel_us> <gap_us> <dir> <class> <word>
 *               <wire_len> <stored> <signal> <flags>
 *     pkt.ends <src> <src_sub> <src_filter> <dst> <dst_sub> <dst_filter>
 *     layer <index> <class|-> <name> <summary>
 *     field <layer> <name> <value> <off|-> <len> <filter|->
 *     hex <off> <hex> <ascii>
 *     pkt.end <req>
 *     pinned <seq> <0|1> <pins>
 *     finding <layer> <text>          inside pkt.begin..pkt.end: a finding of
 *                                     the packet, under that layer (M7)
 *     marks <req> <view_id> <match> <buckets> <hex mask>
 *     saved <run|done|fail> <packets> <total> <bytes> <name|code>
 *                                     SAVE's progress and outcome (M8); name is
 *                                     the file's name, without its folder
 *     error <code> <text>
 *     note <key> <value>
 *     bye
 *
 * Unknown words are ignored on both sides, so an older app and a newer
 * helper (or the reverse) keep working on what they share.
 *
 * Pure C, no LVGL, no allocation (tests/ppc_proto_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_PROTO_H
#define PPC_PROTO_H

#include "ppc_format.h"
#include "ppc_record.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_PROTO_VERSION 3
#define PPC_LINE_MAX 1024
#define PPC_IFACE_MAX 16
#define PPC_WINDOW_MAX 48
#define PPC_TEXT_MAX 160
#define PPC_FIELDS_MAX 40

enum ppc_state {
    PPC_STATE_IDLE = 0,
    PPC_STATE_CAPTURING,
    PPC_STATE_HOLD,
    PPC_STATE_SAVING,
    PPC_STATE_STOPPED,
};

/* Why a session is in its state. */
enum ppc_reason {
    PPC_REASON_NONE = 0,
    PPC_REASON_USER,   /* STOP pressed */
    PPC_REASON_LIMIT,  /* the session's time limit */
    PPC_REASON_LINK,   /* the interface went down or lost its carrier */
    PPC_REASON_END,    /* a replayed file ended */
    PPC_REASON_ERROR,  /* the source failed */
};

/* Capture-filter presets (ARCHITECTURE §7.1). Fixed strings: they compile on
 * the device because they compile in the host test. */
enum ppc_cfilter {
    PPC_CFILTER_ALL = 0,
    PPC_CFILTER_IP,
    PPC_CFILTER_NAMES,
    PPC_CFILTER_NOSELF,
    PPC_CFILTER_COUNT
};

/* Replace what is not printable ASCII with '.', in place. */
void ppc_sanitize(char *s);
/* The same, keeping exactly two well-formed UTF-8 marks the inspector's own
 * words use, the middle dot (U+00B7) and the right arrow (U+2192). Never
 * used on bytes taken from a packet, which ppc_sanitize() makes ASCII. */
void ppc_sanitize_marks(char *s);

const char *ppc_state_name(enum ppc_state s);
const char *ppc_reason_name(enum ppc_reason r);
const char *ppc_cfilter_id(enum ppc_cfilter f);
/* -1 for an unknown id. */
int ppc_cfilter_parse(const char *id);
/* The tcpdump expression of a preset, "" for ALL. self is this device's
 * IPv4 address in dotted form, or NULL when it has none: NOSELF then
 * excludes SSH by port alone. Returns buf. */
const char *ppc_cfilter_expr(enum ppc_cfilter f, const char *self, char *buf, size_t buflen);

/* ---- commands -------------------------------------------------------------------- */

enum ppc_cmd_kind {
    PPC_CMD_NONE = 0, /* an empty line, or a word this helper does not know */
    PPC_CMD_BAD,      /* a known word with bad arguments */
    PPC_CMD_START,
    PPC_CMD_HOLD,
    PPC_CMD_STOP,
    PPC_CMD_DISCARD,
    PPC_CMD_WINDOW,
    PPC_CMD_STATS,
    PPC_CMD_QUIT,
    PPC_CMD_VIEW,
    PPC_CMD_INSPECT,
    PPC_CMD_PIN,
    PPC_CMD_MARKS,
    PPC_CMD_SAVE,
};

/* Where a window is anchored. */
enum ppc_anchor {
    PPC_ANCHOR_TAIL = 0, /* the newest packet's end of the view */
    PPC_ANCHOR_SEQ,      /* this packet, or its nearest neighbour in the view */
    PPC_ANCHOR_RANK,     /* this position in the view */
    PPC_ANCHOR_US,       /* the first packet at or after this time (us since the first) */
};

#define PPC_HOST_TEXT 48
/* A view's classes mask for every class. */
#define PPC_CLASSES_ALL 0xffu
/* The inspector's hex lines carry this many bytes each. */
#define PPC_HEX_PER_LINE 8u

/* Row and packet flags (the row's last field, pkt.begin's flags). */
#define PPC_ROWF_PINNED 0x01u
#define PPC_ROWF_GONE 0x02u      /* the bytes left the buffer: summary only */
#define PPC_ROWF_TRUNCATED 0x04u
#define PPC_ROWF_MALFORMED 0x08u
#define PPC_ROWF_GRO 0x10u

struct ppc_cmd {
    enum ppc_cmd_kind kind;
    char iface[PPC_IFACE_MAX];
    uint8_t keep;      /* enum ppc_keep */
    uint8_t promisc;
    uint8_t cfilter;   /* enum ppc_cfilter */
    uint32_t limit_s;  /* 0: no limit */
    uint8_t on;        /* hold */
    uint32_t req;
    uint8_t tail;      /* window anchored at the newest packet */
    uint32_t seq;      /* window anchored at this packet; inspect, pin: the packet */
    uint32_t before;
    uint32_t after;
    uint8_t anchor;    /* enum ppc_anchor (tail and seq say it too) */
    uint64_t at;       /* the rank or the microseconds of those anchors */
    /* view */
    uint32_t view_id;
    uint8_t sort;      /* enum ppc_sort (ppc_view.h) */
    uint8_t classes;
    char host[PPC_HOST_TEXT]; /* "" for any */
    uint16_t port;
    uint8_t pinned;
    uint8_t signals;   /* PPC_SIG_* bits, 0 for any packet */
    /* inspect */
    int8_t step;
    /* marks */
    uint32_t buckets;
};

/* An interface name as the kernel allows it: 1..15 characters, no slash,
 * no space, not "." or "..". */
int ppc_iface_ok(const char *name);

enum ppc_cmd_kind ppc_cmd_parse(const char *line, struct ppc_cmd *c);

/* Command lines for the shell, without the newline. Each returns the length,
 * or -1 when an argument cannot be sent or the buffer is too small. */
int ppc_cmd_format_start(char *buf, size_t n, const char *iface, enum ppc_keep keep, int promisc,
                         enum ppc_cfilter cf, uint32_t limit_s);
int ppc_cmd_format_hold(char *buf, size_t n, int on);
int ppc_cmd_format_window(char *buf, size_t n, uint32_t req, uint32_t seq, uint32_t before,
                          uint32_t after);
int ppc_cmd_format_window_at(char *buf, size_t n, uint32_t req, enum ppc_anchor anchor, uint64_t at,
                             uint32_t before, uint32_t after);
/* sort: 0 time, 1 newest, 2 len (enum ppc_sort). host NULL or "" for any. */
int ppc_cmd_format_view(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                        uint16_t port, int pinned);
/* The same with the findings to show (PPC_SIG_* bits, 0: any packet). */
int ppc_cmd_format_view_signals(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                                uint16_t port, int pinned, unsigned signals);
int ppc_cmd_format_inspect(char *buf, size_t n, uint32_t req, uint32_t seq, int step);
int ppc_cmd_format_marks(char *buf, size_t n, uint32_t req, uint32_t buckets);
int ppc_cmd_format_pin(char *buf, size_t n, uint32_t seq, int on);

/* ---- events ---------------------------------------------------------------------- */

enum ppc_evt_kind {
    PPC_EVT_NONE = 0, /* a word this app does not know */
    PPC_EVT_BAD,      /* a known word that did not parse */
    PPC_EVT_HELLO,
    PPC_EVT_STATE,
    PPC_EVT_STATS,
    PPC_EVT_BIN,
    PPC_EVT_ROWS_BEGIN,
    PPC_EVT_ROW,
    PPC_EVT_ROWS_END,
    PPC_EVT_ERROR,
    PPC_EVT_NOTE,
    PPC_EVT_BYE,
    PPC_EVT_PKT_BEGIN,
    PPC_EVT_PKT_ENDS,
    PPC_EVT_LAYER,
    PPC_EVT_FIELD,
    PPC_EVT_HEX,
    PPC_EVT_PKT_END,
    PPC_EVT_PINNED,
    PPC_EVT_FINDING,
    PPC_EVT_MARKS,
    PPC_EVT_SAVED,
};

#define PPC_FIELD_NAME 24
#define PPC_FIELD_TEXT 112
#define PPC_FILTER_TEXT 56
#define PPC_HEX_TEXT 32
#define PPC_ASCII_TEXT 12
#define PPC_MARK_BUCKETS 64u
#define PPC_SAVE_NAME 48

/* SAVE's phases (the `saved` line). */
enum ppc_save_phase { PPC_SAVE_RUN = 0, PPC_SAVE_DONE, PPC_SAVE_FAIL };

struct ppc_counters {
    uint64_t elapsed_ms;
    uint64_t seen;
    uint64_t stored;
    uint64_t held;
    uint64_t kdrop;
    uint64_t ifdrop;
    uint64_t shed;
    uint64_t bytes;
    uint64_t pps;
    uint64_t bps;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t match;
    uint32_t fill_permille;
    uint64_t ui_skipped;
    uint64_t cls[PPC_CLASS_COUNT];
    uint32_t sig[PPC_SIGNAL_COUNT];
};

struct ppc_evt_row {
    uint32_t seq;
    uint64_t rel_us;
    uint64_t gap_us;
    char dir[6];
    uint8_t cls;
    char word[8];
    uint32_t len;
    uint16_t signal;
    char src[PPC_ADDR_TEXT];
    char dst[PPC_ADDR_TEXT];
    char info[PPC_INFO_TEXT];
    uint8_t flags;     /* PPC_ROWF_* */
};

/* pkt.begin: the inspected packet's head. */
struct ppc_evt_pkt {
    uint32_t req;
    uint32_t seq;
    uint32_t rank;     /* its place in the view; match when it is not in it */
    uint32_t match;
    uint64_t rel_us;
    uint64_t gap_us;
    char dir[6];
    uint8_t cls;
    char word[8];
    uint32_t wire_len;
    uint32_t stored;
    uint16_t signal;
    uint8_t flags;     /* PPC_ROWF_* */
};

struct ppc_evt_end {
    char addr[PPC_ADDR_TEXT];
    char sub[PPC_ADDR_TEXT];
    char filter[PPC_FILTER_TEXT];
};

struct ppc_evt {
    enum ppc_evt_kind kind;
    union {
        struct {
            uint32_t proto;
            char version[32];
            char backend[16];
        } hello;
        struct {
            enum ppc_state state;
            enum ppc_reason reason;
            char iface[PPC_IFACE_MAX];
            uint32_t speed;
            uint8_t clock_set;
        } state;
        struct ppc_counters stats;
        struct {
            uint64_t second;
            uint32_t cls[PPC_CLASS_COUNT];
            uint32_t held;
        } bin;
        struct {
            uint32_t req;
            uint32_t filter_id;
            uint32_t first_rank;
            uint32_t match;
            uint32_t first_seq;
            uint32_t last_seq;
            uint32_t anchor_rank; /* the anchor's rank; first_rank when not sent */
        } rows;
        struct ppc_evt_row row;
        struct {
            char code[16];
            char text[PPC_TEXT_MAX];
        } error;
        struct {
            char key[16];
            char value[64];
        } note;
        struct ppc_evt_pkt pkt;
        struct {
            struct ppc_evt_end src;
            struct ppc_evt_end dst;
        } ends;
        struct {
            uint32_t index;
            int cls;           /* -1 for none */
            char name[PPC_FIELD_NAME];
            char text[PPC_FIELD_TEXT];
        } layer;
        struct {
            uint32_t layer;
            char name[PPC_FIELD_NAME];
            char text[PPC_FIELD_TEXT];
            int32_t off;       /* -1 for none */
            uint32_t len;
            char filter[PPC_FILTER_TEXT]; /* "" for none */
        } field;
        struct {
            uint32_t off;
            char hex[PPC_HEX_TEXT];
            char ascii[PPC_ASCII_TEXT];
        } hex;
        struct {
            uint32_t seq;
            uint8_t on;
            uint32_t pins;
        } pinned;
        struct {
            uint32_t layer;
            char text[PPC_FIELD_TEXT];
        } finding;
        struct {
            uint32_t req;
            uint32_t view_id;
            uint32_t match;
            uint32_t buckets;
            uint64_t mask;     /* bit i: a finding in part i of the view */
        } marks;
        struct {
            uint8_t phase;     /* enum ppc_save_phase */
            uint32_t packets;
            uint32_t total;
            uint64_t bytes;
            char name[PPC_SAVE_NAME]; /* the file's name, or the failure's code */
        } saved;
    } u;
};

enum ppc_evt_kind ppc_evt_parse(const char *line, struct ppc_evt *e);

/* Event lines for the helper, WITH the newline. Each returns the length, or
 * -1 when the buffer is too small (nothing useful written). Free text is
 * sanitized and cut to fit. */
int ppc_evt_format_hello(char *buf, size_t n, const char *version, const char *backend);
int ppc_evt_format_state(char *buf, size_t n, enum ppc_state s, enum ppc_reason r, const char *iface,
                         uint32_t speed, int clock_set);
int ppc_evt_format_stats(char *buf, size_t n, const struct ppc_counters *c);
int ppc_evt_format_bin(char *buf, size_t n, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT],
                       uint32_t held);
int ppc_evt_format_rows_begin(char *buf, size_t n, uint32_t req, uint32_t filter_id,
                              uint32_t first_rank, uint32_t match, uint32_t first_seq,
                              uint32_t last_seq);
int ppc_evt_format_row(char *buf, size_t n, const struct ppc_evt_row *row);
/* The same with the rank the window was anchored at (protocol 2). */
int ppc_evt_format_rows_begin_at(char *buf, size_t n, uint32_t req, uint32_t filter_id, uint32_t first_rank,
                                 uint32_t match, uint32_t first_seq, uint32_t last_seq, uint32_t anchor_rank);
int ppc_evt_format_rows_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_error(char *buf, size_t n, const char *code, const char *text);
int ppc_evt_format_note(char *buf, size_t n, const char *key, const char *value);
int ppc_evt_format_bye(char *buf, size_t n);
int ppc_evt_format_pkt_begin(char *buf, size_t n, const struct ppc_evt_pkt *p);
int ppc_evt_format_pkt_ends(char *buf, size_t n, const struct ppc_evt_end *src, const struct ppc_evt_end *dst);
/* cls < 0: none. */
int ppc_evt_format_layer(char *buf, size_t n, uint32_t index, int cls, const char *name, const char *text);
/* off < 0: none; filter NULL or "": none. */
int ppc_evt_format_field(char *buf, size_t n, uint32_t layer, const char *name, const char *text, int32_t off,
                         uint32_t len, const char *filter);
int ppc_evt_format_hex(char *buf, size_t n, uint32_t off, const char *hex, const char *ascii);
int ppc_evt_format_pkt_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_pinned(char *buf, size_t n, uint32_t seq, int on, uint32_t pins);
int ppc_evt_format_finding(char *buf, size_t n, uint32_t layer, const char *text);
int ppc_evt_format_marks(char *buf, size_t n, uint32_t req, uint32_t view_id, uint32_t match, uint32_t buckets,
                         uint64_t mask);
int ppc_evt_format_saved(char *buf, size_t n, enum ppc_save_phase phase, uint32_t packets, uint32_t total,
                         uint64_t bytes, const char *name);
/* A saved capture's name as the helper makes it: "capture-" then digits
 * and dashes, ".pcap"; nothing else is ever shown or deleted. */
int ppc_save_name_ok(const char *name);
/* The folder SAVE writes to and the app lists (ADR-016 rule 9):
 * $POCKETOS_CAPTURES_DIR for the simulator and the tests, else
 * $HOME/Captures, which is /root/Captures on the unit. Returns buf, or NULL
 * when it does not fit or names a place under /tmp, where no capture goes. */
const char *ppc_captures_dir(char *buf, size_t n);

#endif
