/*
 * PacketShark's model in the shell: what its screens show, and nothing more
 * (DESIGN_DECISIONS D1, accepted: the helper owns the packets; the shell is
 * a viewport with a cache of at most PSK_ROW_CACHE rows).
 *
 * It is filled from the helper's lines (core/pocketpcap/ppc_proto.h) by
 * psk_session_poll() and read by the screens. It holds no packet bytes:
 * counters, the class bins, the last error, how the helper ended, and the
 * text of the rows a window last brought (none are asked for in M3; the
 * list is M5).
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
    struct ppc_evt_row rows[PSK_ROW_CACHE];
};

void psk_model_init(struct psk_model *m);

/* A new helper is starting: the session's figures are kept (they are the
 * LAST SESSION the error panel shows) until its hello. */
void psk_model_helper_started(struct psk_model *m, int64_t now_ms);
void psk_model_helper_stopped(struct psk_model *m, enum psk_exit why, int value, int64_t now_ms);

/* Apply one parsed line. Returns PSK_CHANGED_* bits. */
unsigned psk_model_apply(struct psk_model *m, const struct ppc_evt *e, int64_t now_ms);

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

#endif
