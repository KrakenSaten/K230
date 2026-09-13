/*
 * Wave's view model: what the screen says and allows, decided without LVGL.
 *
 * Every rule that can go wrong on the glass lives here and is unit-tested
 * (tests/wave_view_test.c): which mode may be switched to, when Transmit is
 * allowed, what the one action button reads in each phase, what a helper's
 * error word means to a person, how a message is measured against the byte
 * limit, how a received message that is not clean text is shown, and that
 * the microphone indicator is on for exactly as long as the microphone can
 * be.
 *
 * The microphone rule, stated once: mic_on is 1 from the moment a listen is
 * started until the helper is known to have exited. It does not wait for the
 * helper's "listening" and does not end at a stop request, because the device
 * may be open in both of those gaps.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_VIEW_H
#define POCKETWAVE_VIEW_H

#include "wave_protocol.h"
#include "wave_session.h"

#include <stddef.h>
#include <stdint.h>

/* Received messages kept on screen, newest first. */
#define WAVE_VIEW_KEEP 5
/* How long "heard something it could not decode" stays in the status. */
#define WAVE_VIEW_MISSED_MS 3000

enum wave_mode {
    WAVE_MODE_SEND = 0,
    WAVE_MODE_RECEIVE
};

enum wave_phase {
    WAVE_PHASE_IDLE = 0,
    WAVE_PHASE_SENDING,   /* a send helper is running */
    WAVE_PHASE_LISTENING, /* a listen helper is running */
    WAVE_PHASE_STOPPING   /* stop requested, helper not gone yet */
};

enum wave_tone {
    WAVE_TONE_PRIMARY = 0,
    WAVE_TONE_MUTED,
    WAVE_TONE_OK,
    WAVE_TONE_WARN,
    WAVE_TONE_ERROR
};

/* How the last run ended, for the idle status line. */
enum wave_last {
    WAVE_LAST_NONE = 0,
    WAVE_LAST_SENT,
    WAVE_LAST_STOPPED,
    WAVE_LAST_ENDED    /* a listen that reached WAVE_LISTEN_SECONDS */
};

struct wave_received {
    char shown[2 * WAVE_EVENT_TEXT_MAX + 16]; /* text or "hex: ..." */
    int is_hex;
};

struct wave_view {
    enum wave_mode mode;
    enum wave_phase phase;
    enum wave_mode running_mode;   /* what the running helper does */
    enum wave_profile profile;
    int level;                     /* microphone peak %, -1 when unknown */
    int expected_ms;               /* a send's announced duration, 0 until known */
    int64_t phase_since_ms;
    int64_t missed_at_ms;          /* last undecodable transmission, 0 when none */
    int got_sent;                  /* the helper said "sent" */
    int got_error;                 /* the helper reported an error this run */
    enum wave_last last;
    char error[128];               /* last error, in words; "" when none */
    struct wave_received received[WAVE_VIEW_KEEP];
    int received_count;

    /* derived by wave_view_refresh() */
    int mic_on;
    int can_switch_mode;
    int can_pick_profile;
    int can_edit;
    int action_enabled;
    const char *action_label;
    int action_primary;            /* accent fill when starting, not when stopping */
    char status[128];              /* as large as error, which it may show */
    enum wave_tone status_tone;
    char counter[32];              /* "5 / 64 bytes" */
    enum wave_tone counter_tone;
    const char *status_hint;       /* for the status bar: "MIC ON", "SENDING" or "" */
};

void wave_view_init(struct wave_view *v);

/* A message as typed. 0 when it may be sent, with *bytes set; -1 with a
 * reason when it is empty, longer than WAVE_MAX_MESSAGE_BYTES, not valid
 * UTF-8, or contains a control character. */
int wave_view_check_message(const char *text, size_t *bytes, char *why, size_t whylen);

/* Mode and profile changes are refused (0) while a helper runs. */
int wave_view_set_mode(struct wave_view *v, enum wave_mode mode);
int wave_view_set_profile(struct wave_view *v, enum wave_profile profile);

/* The action button was pressed. What the app must do about it. */
enum wave_action {
    WAVE_DO_NOTHING = 0,
    WAVE_DO_SEND,
    WAVE_DO_LISTEN,
    WAVE_DO_STOP
};
enum wave_action wave_view_action(const struct wave_view *v, const char *message);

/* The app started a helper (ok) or could not (why). */
void wave_view_started(struct wave_view *v, int64_t now_ms);
void wave_view_start_failed(struct wave_view *v, const char *why);
/* The app asked the running helper to stop. */
void wave_view_stopping(struct wave_view *v, int64_t now_ms);

/* One event from the session. */
void wave_view_apply(struct wave_view *v, const struct wave_event *ev, int64_t now_ms);

/* Recompute everything derived from the state, the typed message and now. */
void wave_view_refresh(struct wave_view *v, const char *message, int64_t now_ms);

/* A helper error ("<code> <message>") in words for a person. */
void wave_view_error_text(const char *event_text, char *out, size_t n);

/* A received message for display: as is when it is valid UTF-8 without
 * control characters, otherwise "hex: 41 00 ff". */
void wave_view_format_received(const char *bytes, size_t len, struct wave_received *out);

/* The helper's protocol word and the button label for a profile. */
const char *wave_view_profile_name(enum wave_profile p);
const char *wave_view_profile_label(enum wave_profile p);

#endif
