/*
 * gatewayd: PacketShark's capture, as a paired client may see and steer it
 * (ADR-016 amendment 1, accepted 2026-11-22; ADR-019 rule 6;
 * docs/api/gateway.md section 9).
 *
 * The gateway asks PacketShark, through the shell's socket, for five things
 * and nothing else: capture.status, capture.hold, capture.stop,
 * capture.request_start and, for itself alone, capture.request_cancel. It
 * does not listen to the shell: what a client is told of a capture is made
 * here from the answers to the gateway's own capture.status, asked once a
 * second while a client holds the topic.
 *
 * This file is everything of that which needs no socket:
 *
 *   - the filters. An answer of the unit is never passed on: it is read by
 *     name, checked, and written again from nothing. A count is a whole
 *     number in range, a word is a word of the list, the interface name is
 *     a kernel's own or empty. Nothing else of an answer reaches a client,
 *     whatever a later PacketShark adds to it.
 *   - the parameters. What a client sent is checked and written again.
 *   - what clients were last told, so that a state is said once per change
 *     and figures never before their state.
 *   - the one start request that may be waiting: whose it is, what it is
 *     called, since when; and how often a client may ask.
 *
 * Pure: cJSON only, no I/O, the clock passed in.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_CAPTURE_H
#define GATEWAYD_GW_CAPTURE_H

#include "gw.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define GW_CAPTURE_POLL_MS 1000        /* capture.status, while a client holds the topic */
#define GW_CAPTURE_CALL_MS 2000        /* any request to the shell: answered by then, or given up */
#define GW_CAPTURE_ASK_MS 62000        /* a start request is over for the gateway by then (the unit's 60 s, and a margin) */
#define GW_CAPTURE_ASK_EVERY_MS 30000  /* one start request per client in this time */
#define GW_CAPTURE_TOKEN 17            /* a request's name: 16 hex digits */
#define GW_CAPTURE_FIGURE_MAX 999999999999999.0

/* ---- filters and parameters (rows of gw_policy.c) --------------------------- */

/* capture.status for a client: {"available": false}, or the contract's
 * fields and no other. NULL when the answer is not an object or memory is
 * short. */
cJSON *gw_capture_status(const cJSON *result);
/* capture.hold and capture.stop: {state, changing}. */
cJSON *gw_capture_command(const cJSON *result);
/* capture.request_start: {"shown": true}, or NULL when the unit did not say
 * so. */
cJSON *gw_capture_shown(const cJSON *result);
/* capture.hold {on}: a boolean and nothing else. */
int gw_capture_params_hold(const cJSON *in, cJSON **out);
/* capture.request_start {iface, limit_s}: an interface name of the kind the
 * status may show (eth, wlan, usb or synth and one or two digits - so that
 * asking cannot be used to find out a name the status would hide) and one
 * of 300, 900, 3600. Nothing else may be asked for or sent
 * along: who asks and what the request is called are added by the gateway. */
int gw_capture_params_ask(const cJSON *in, cJSON **out);


/* ---- what clients were told, and the request that may be waiting ----------- */

struct gw_capture {
    /* what was last said as capture.state */
    bool told;
    char state[12];
    char reason[16];
    char iface[16];
    char keep[8];
    double limit_s;
    bool stats_owed;        /* the figures of that state have not gone out */
    /* the one start request that may be waiting on the unit */
    bool asking;
    bool shown;             /* the unit said its sheet is up */
    int64_t shown_ms;
    int64_t asked_ms;
    uint64_t sid;           /* the session that asked */
    uint8_t fp[GW_FP_LEN];  /* and its client */
    char token[GW_CAPTURE_TOKEN];
    /* when each client last asked */
    struct {
        bool used;
        uint8_t fp[GW_FP_LEN];
        int64_t ms;
    } last[GW_MAX_CLIENTS];
};

void gw_capture_init(struct gw_capture *c);

/* One answer to the gateway's own capture.status, as the unit gave it, asked
 * at sent_ms.
 *   *state    the capture.state event to raise now, or NULL
 *   *stats    the capture.stats event to raise after it, or NULL
 *   *outcome  "refused" or "timeout" when the unit says that the waiting
 *             request ended so, or PacketShark is gone (the caller says so
 *             and forgets it), else NULL. Only the unit's own word ends a
 *             request here: while it says neither - the sheet is up, or
 *             START was tapped and the capture is starting - the request
 *             waits, for a capture or for the gateway's own time.
 * A waiting request that led to a capture is forgotten here. Every object
 * returned is the caller's. */
void gw_capture_step(struct gw_capture *c, const cJSON *raw, int64_t sent_ms, cJSON **state, cJSON **stats,
                     const char **outcome);
/* The shell is gone, or PacketShark with it: *state is the last word on a
 * capture clients were told of ("ended", "left"), or NULL. */
void gw_capture_lost(struct gw_capture *c, cJSON **state);
/* What clients were last told, again, for one that has just begun to listen;
 * NULL when nothing is known. */
cJSON *gw_capture_told(const struct gw_capture *c);
/* The capture.state event for a request that ended without a capture. */
cJSON *gw_capture_outcome(const char *reason);

/* May this client ask now: nobody's request is waiting, and its own last one
 * is GW_CAPTURE_ASK_EVERY_MS old. */
enum gw_capture_ask { GW_ASK_OK = 0, GW_ASK_BUSY, GW_ASK_RATE };
enum gw_capture_ask gw_capture_may_ask(const struct gw_capture *c, const uint8_t *fp, int64_t now_ms);
/* It asks: the request is `token`, of this session and client, from now. */
void gw_capture_ask(struct gw_capture *c, const uint8_t *fp, uint64_t sid, const char *token, int64_t now_ms);
/* The unit answered the waiting request: its sheet is up. (Whose answer it
 * is, the caller knows by the tag of its own request.) */
void gw_capture_ask_shown(struct gw_capture *c, int64_t now_ms);
/* The request is over, however. */
void gw_capture_ask_end(struct gw_capture *c);

#endif
