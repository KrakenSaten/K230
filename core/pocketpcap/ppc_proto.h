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
 * Implemented in this milestone (M2/M3):
 *
 *   shell -> helper
 *     start <iface> <headers|full> <promisc 0|1> <cfilter> <limit_s>
 *     hold <0|1>
 *     stop
 *     discard
 *     window <req> <tail|seq:N> <before> <after>     at most PPC_WINDOW_MAX rows
 *     stats                                          one snapshot now
 *     quit
 *
 *   helper -> shell
 *     hello <proto> <version> <backend>
 *     state <idle|capturing|hold|saving|stopped> <reason> <iface|-> <speed_mbps> <clock_set>
 *     stats <elapsed_ms> <seen> <stored> <held> <kdrop> <ifdrop> <shed> <bytes> <pps> <bps>
 *           <first_seq> <last_seq> <match> <fill_permille> <ui_skipped> <c0..c7> <s0..s4>
 *     bin <second> <c0..c7> <held>
 *     rows.begin <req> <filter_id> <first_rank> <match> <first_seq> <last_seq>
 *     row <seq> <rel_us> <gap_us> <dir> <class> <word> <len> <signal> <src> <dst> <info>
 *     rows.end <req>
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

#define PPC_PROTO_VERSION 1
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
};

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
    uint32_t seq;      /* window anchored at this packet */
    uint32_t before;
    uint32_t after;
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
};

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
int ppc_evt_format_rows_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_error(char *buf, size_t n, const char *code, const char *text);
int ppc_evt_format_note(char *buf, size_t n, const char *key, const char *value);
int ppc_evt_format_bye(char *buf, size_t n);

#endif
