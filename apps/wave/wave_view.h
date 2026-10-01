/*
 * Wave's model: one screen, one selected preset, and everything the screen
 * says and allows, decided without LVGL.
 *
 * THE WORKFLOW. There are no separate send and receive screens. The person
 * picks a preset and stays in it; from there they can
 *
 *   - LISTEN: a sticky toggle. While it is on the microphone listens and
 *     decodes live, up to the preset's listen length.
 *   - SEND a message with the same preset, at any time. A send while
 *     listening pauses the listen (the K230 needs opposite audio routes for
 *     the two), plays the preset's copies, and resumes listening afterwards:
 *     the toggle stays on through it.
 *   - CAPTURE: record the preset's capture length without decoding, then
 *     decode the recording. For a sender that started before the button was
 *     pressed was released, for a noisy room, and for a board busy enough
 *     that live decoding would fall behind. Pausing and resuming a listen
 *     works as for a send.
 *   - STOP whatever runs.
 *
 * Half duplex stays: one helper process at a time (wave_session.h), and the
 * model queues what comes next instead of running two.
 *
 * HOW THE APP DRIVES IT. A request (send, listen, capture, stop) returns what
 * to do now: nothing, or stop the running helper. What to start is asked
 * separately, whenever no helper runs: wave_view_next() says whether a send
 * copy, a listen, a capture or a decode is due, and wave_view_started() or
 * wave_view_start_failed() tells the model how that went. Helper events go to
 * wave_view_apply(), which also keeps the history. wave_ctl.c is that loop.
 *
 * THE MICROPHONE RULE, stated once: mic_on is 1 from the moment a listen or a
 * capture is started until its helper is known to have exited. It does not
 * wait for the helper's "listening" and does not end at a stop request,
 * because the device may be open in both of those gaps.
 *
 * FAILURES never loop. A helper that fails turns the listen toggle off and
 * drops whatever was queued, so a broken microphone is not reopened every
 * 50 ms; the error stays on screen until the next request.
 *
 * Pure C, no LVGL, clocks passed in: tests/wave_view_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_VIEW_H
#define POCKETWAVE_VIEW_H

#include "wave_history.h"
#include "wave_preset.h"
#include "wave_protocol.h"
#include "wave_session.h"

#include <stddef.h>
#include <stdint.h>

/* How long "heard something it could not decode" stays in the status. */
#define WAVE_VIEW_MISSED_MS 3000
/* A first tap on CLEAR arms it for this long; a second tap clears. */
#define WAVE_VIEW_CLEAR_ARM_MS 4000

/* What the running helper does. */
enum wave_op {
    WAVE_OP_NONE = 0,
    WAVE_OP_SEND,
    WAVE_OP_LISTEN,
    WAVE_OP_CAPTURE, /* pos-wave record */
    WAVE_OP_DECODE   /* pos-wave decode of the capture */
};

enum wave_phase {
    WAVE_PHASE_IDLE = 0, /* no helper */
    WAVE_PHASE_RUNNING,  /* a helper runs op */
    WAVE_PHASE_STOPPING  /* stop requested, helper not gone yet */
};

enum wave_tone {
    WAVE_TONE_PRIMARY = 0,
    WAVE_TONE_MUTED,
    WAVE_TONE_OK,
    WAVE_TONE_WARN,
    WAVE_TONE_ERROR
};

/* The state chip's look: what the one word at the top of the screen means. */
enum wave_chip {
    WAVE_CHIP_IDLE = 0,
    WAVE_CHIP_RX,     /* the microphone is (or may be) on */
    WAVE_CHIP_TX,     /* the speaker is playing */
    WAVE_CHIP_BUSY    /* decoding, or stopping something that was neither */
};

/* How the last run ended, for the idle status line. */
enum wave_last {
    WAVE_LAST_NONE = 0,
    WAVE_LAST_SENT,
    WAVE_LAST_SEND_STOPPED,
    WAVE_LAST_LISTEN_STOPPED,
    WAVE_LAST_LISTEN_ENDED,    /* a listen that reached its length */
    WAVE_LAST_DECODED,         /* a capture decoded into messages */
    WAVE_LAST_NOTHING_DECODED  /* a capture held nothing */
};

/* What wave_view_next() says to start. */
enum wave_action {
    WAVE_DO_NOTHING = 0,
    WAVE_DO_SEND,     /* start one copy of v->tx_text with the tx preset */
    WAVE_DO_LISTEN,   /* start a listen for the preset's listen length */
    WAVE_DO_CAPTURE,  /* start a recording of the preset's capture length */
    WAVE_DO_DECODE,   /* start decoding the recording */
    WAVE_DO_STOP      /* (requests only) stop the running helper */
};

struct wave_view {
    int preset;                    /* index into wave_preset_get() */
    enum wave_phase phase;
    enum wave_op op;               /* what the running (or stopping) helper does */

    /* intents */
    int listen_on;                 /* the LISTEN toggle */
    int send_pending;              /* a send is queued or partway through its copies */
    int capture_pending;           /* a capture is queued */
    int decode_pending;            /* a recording is waiting to be decoded */
    char tx_text[WAVE_MAX_MESSAGE_BYTES + 1];
    size_t tx_len;
    int tx_preset;                 /* the preset the send was asked with */
    int tx_copies;                 /* copies asked for */
    int tx_done;                   /* copies played so far */

    /* the run in progress */
    int run_preset;                /* the preset the running helper was started on */
    int level;                     /* microphone peak %, -1 when unknown */
    int expected_ms;               /* a send copy's announced duration, 0 until known */
    int64_t phase_since_ms;
    int64_t missed_at_ms;          /* last undecodable transmission, 0 when none */
    int got_sent;                  /* the helper said "sent" */
    int got_error;                 /* the helper reported an error this run */
    int decoded;                   /* messages this decode found */
    int stop_asked;                /* the person stopped this run */

    enum wave_last last;
    int last_count;                /* WAVE_LAST_DECODED: how many */
    char error[128];               /* last error, in words; "" when none */
    struct wave_history history;
    int64_t clear_armed_ms;        /* when CLEAR was armed, 0 when not */

    /* derived by wave_view_refresh() */
    int mic_on;
    int busy;                      /* a helper runs or is stopping */
    int can_pick_preset;
    int can_edit;
    int send_enabled;
    const char *send_label;        /* "SEND", or "STOP" while a send or a capture runs */
    int send_primary;
    const char *listen_label;      /* "LISTEN" or "STOP LISTEN" */
    int listen_enabled;
    int listen_primary;            /* accent fill while the toggle is on */
    const char *capture_label;     /* "CAPTURE" or "DECODE NOW" */
    int capture_enabled;
    const char *clear_label;       /* "CLEAR" or "CONFIRM" */
    int clear_enabled;
    char preset_label[48];         /* "STANDARD - FAST" */
    const char *preset_summary;
    char chip[40];                 /* "READY", "MIC ON", "SENDING 1/2", ... */
    enum wave_chip chip_kind;
    char status[128];
    enum wave_tone status_tone;
    char counter[32];              /* "5 / 64 bytes" */
    enum wave_tone counter_tone;
    const char *status_hint;       /* for the header: "MIC ON", "SENDING" or "" */
};

/* An empty model on the given preset (the default when it is not one). */
void wave_view_init(struct wave_view *v, int preset);

/* A message as typed. 0 when it may be sent, with *bytes set; -1 with a
 * reason when it is empty, longer than WAVE_MAX_MESSAGE_BYTES, not valid
 * UTF-8, or contains a control character. */
int wave_view_check_message(const char *text, size_t *bytes, char *why, size_t whylen);

/* Refused (0) while a send or a capture is running or queued. A listen keeps
 * running on the preset it started with; the next one uses the new one. */
int wave_view_set_preset(struct wave_view *v, int preset);
const struct wave_preset *wave_view_preset(const struct wave_view *v);

/* ---- requests: each returns WAVE_DO_STOP when the running helper has to be
 * stopped first, otherwise WAVE_DO_NOTHING (the start comes from next()). */

/* Queue text for sending: refused with *refused = 1 (and nothing changes)
 * when it is not sendable or a send or capture is already on its way. */
enum wave_action wave_view_request_send(struct wave_view *v, const char *text, int *refused);
/* Flip the LISTEN toggle. */
enum wave_action wave_view_request_listen(struct wave_view *v);
/* Capture now; while capturing, finish the capture early and decode it. */
enum wave_action wave_view_request_capture(struct wave_view *v);
/* Stop whatever runs and drop whatever is queued behind it, the listen
 * toggle included when a listen is what runs. A send dropped before it
 * played is kept in the history (stopped), dated wall_s. */
enum wave_action wave_view_request_stop(struct wave_view *v, int64_t wall_s);

/* CLEAR history: the first tap arms, a second within WAVE_VIEW_CLEAR_ARM_MS
 * clears. 1 when this tap cleared. */
int wave_view_request_clear(struct wave_view *v, int64_t now_ms);

/* What to start now; WAVE_DO_NOTHING while a helper runs. */
enum wave_action wave_view_next(const struct wave_view *v);
/* The app started what next() said (ok) or could not (why). */
void wave_view_started(struct wave_view *v, enum wave_action what, int64_t now_ms);
void wave_view_start_failed(struct wave_view *v, enum wave_action what, const char *why,
                            int64_t wall_s);
/* The app asked the running helper to stop. */
void wave_view_stopping(struct wave_view *v, int64_t now_ms);

/* One event from the session. now_ms is monotonic, wall_s wall-clock
 * seconds (0 when the board's clock is not set). */
void wave_view_apply(struct wave_view *v, const struct wave_event *ev, int64_t now_ms,
                     int64_t wall_s);

/* Recompute everything derived from the state, the typed message and now. */
void wave_view_refresh(struct wave_view *v, const char *message, int64_t now_ms);

/* A helper error ("<code> <message>") in words for a person. */
void wave_view_error_text(const char *event_text, char *out, size_t n);

/* A payload for display: as is when it is valid UTF-8 without control
 * characters, otherwise "hex: 41 00 ff". Returns 1 when it was shown as hex. */
int wave_view_format_payload(const char *bytes, size_t len, char *out, size_t n);

/* One history entry as two lines for a person: meta ("RX 14:05  STANDARD  x2")
 * and text (the message, or what happened instead). now_wall (0 when
 * unknown) decides whether the date is shown too: it is, for an entry from
 * another day. */
void wave_view_format_entry(const struct wave_history_entry *e, int64_t now_wall, char *meta,
                            size_t meta_n, char *text, size_t text_n);

/* The helper's protocol word and the speed label for a profile. */
const char *wave_view_profile_name(enum wave_profile p);
const char *wave_view_profile_label(enum wave_profile p);

#endif
