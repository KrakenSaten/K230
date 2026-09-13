/*
 * Wave's view model. See wave_view.h.
 *
 * Status text is ASCII on purpose: the DS fonts carry Latin-1 and nothing
 * beyond it, so an ellipsis or a dash from general punctuation would be a
 * missing glyph on the panel.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_view.h"

#include "wave_text.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static const char *const profile_names[WAVE_PROFILE_COUNT] = WAVE_PROFILE_NAMES;
static const char *const profile_labels[WAVE_PROFILE_COUNT] = WAVE_PROFILE_LABELS;

const char *wave_view_profile_name(enum wave_profile p)
{
    return (unsigned)p < WAVE_PROFILE_COUNT ? profile_names[p] : NULL;
}

const char *wave_view_profile_label(enum wave_profile p)
{
    return (unsigned)p < WAVE_PROFILE_COUNT ? profile_labels[p] : NULL;
}

void wave_view_init(struct wave_view *v)
{
    memset(v, 0, sizeof(*v));
    v->mode = WAVE_MODE_SEND;
    v->phase = WAVE_PHASE_IDLE;
    v->profile = WAVE_DEFAULT_PROFILE;
    v->level = -1;
    wave_view_refresh(v, "", 0);
}

int wave_view_check_message(const char *text, size_t *bytes, char *why, size_t whylen)
{
    size_t len = text ? strlen(text) : 0;

    if (bytes) {
        *bytes = len;
    }
    if (len == 0) {
        snprintf(why, whylen, "Type a message");
        return -1;
    }
    if (len > WAVE_MAX_MESSAGE_BYTES) {
        snprintf(why, whylen, "Too long: %zu of %d bytes", len, WAVE_MAX_MESSAGE_BYTES);
        return -1;
    }
    if (!wave_text_clean(text, len)) {
        snprintf(why, whylen, "Only plain text can be sent");
        return -1;
    }
    if (whylen) {
        why[0] = '\0';
    }
    return 0;
}

void wave_view_format_received(const char *bytes, size_t len, struct wave_received *out)
{
    size_t i;
    size_t off;

    memset(out, 0, sizeof(*out));
    if (len > WAVE_EVENT_TEXT_MAX) {
        len = WAVE_EVENT_TEXT_MAX;
    }
    if (len > 0 && wave_text_clean(bytes, len)) {
        memcpy(out->shown, bytes, len);
        out->shown[len] = '\0';
        return;
    }
    out->is_hex = 1;
    off = (size_t)snprintf(out->shown, sizeof(out->shown), "hex:");
    for (i = 0; i < len && off + 4 <= sizeof(out->shown); i++) {
        off += (size_t)snprintf(out->shown + off, sizeof(out->shown) - off, " %02x",
                                (unsigned char)bytes[i]);
    }
}

static int starts_with_word(const char *text, const char *word)
{
    size_t n = strlen(word);

    return strncmp(text, word, n) == 0 && (text[n] == '\0' || text[n] == ' ');
}

void wave_view_error_text(const char *t, char *out, size_t n)
{
    static const struct {
        const char *word;
        const char *words;
    } known[] = {
        { WAVE_ERR_AUDIO_DISABLED, "Audio is not enabled on this device yet" },
        { WAVE_ERR_AUDIO_BUSY, "Audio is in use by another program" },
        { WAVE_ERR_AUDIO_NODEV, "No audio device found" },
        { WAVE_ERR_AUDIO, "Audio device error" },
        { WAVE_ERR_TOO_LONG, "Message is too long" },
        { WAVE_ERR_INVALID_TEXT, "Only plain text can be sent" },
        { WAVE_ERR_ENCODE, "Message could not be encoded" },
        { WAVE_ERR_DECODE, "Receiver could not start" },
        { WAVE_ERR_USAGE, "Wave helper refused the request" },
        { "exec", "Wave helper is not installed" },
        { "protocol", "Wave helper sent something unexpected" },
    };
    size_t i;

    if (!t) {
        t = "";
    }
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (starts_with_word(t, known[i].word)) {
            snprintf(out, n, "%s", known[i].words);
            return;
        }
    }
    snprintf(out, n, "Error: %.100s", t);
}

int wave_view_set_mode(struct wave_view *v, enum wave_mode mode)
{
    if (v->phase != WAVE_PHASE_IDLE || (mode != WAVE_MODE_SEND && mode != WAVE_MODE_RECEIVE)) {
        return 0;
    }
    if (v->mode != mode) {
        v->mode = mode;
        v->error[0] = '\0';
        v->last = WAVE_LAST_NONE;
    }
    return 1;
}

int wave_view_set_profile(struct wave_view *v, enum wave_profile profile)
{
    if (v->phase != WAVE_PHASE_IDLE || (unsigned)profile >= WAVE_PROFILE_COUNT) {
        return 0;
    }
    v->profile = profile;
    return 1;
}

enum wave_action wave_view_action(const struct wave_view *v, const char *message)
{
    char why[64];

    switch (v->phase) {
    case WAVE_PHASE_IDLE:
        if (v->mode == WAVE_MODE_RECEIVE) {
            return WAVE_DO_LISTEN;
        }
        return wave_view_check_message(message, NULL, why, sizeof(why)) == 0 ? WAVE_DO_SEND
                                                                               : WAVE_DO_NOTHING;
    case WAVE_PHASE_SENDING:
    case WAVE_PHASE_LISTENING:
        return WAVE_DO_STOP;
    case WAVE_PHASE_STOPPING:
    default:
        return WAVE_DO_NOTHING;
    }
}

void wave_view_started(struct wave_view *v, int64_t now_ms)
{
    v->phase = v->mode == WAVE_MODE_SEND ? WAVE_PHASE_SENDING : WAVE_PHASE_LISTENING;
    v->running_mode = v->mode;
    v->phase_since_ms = now_ms;
    v->expected_ms = 0;
    v->got_sent = 0;
    v->got_error = 0;
    v->error[0] = '\0';
    v->last = WAVE_LAST_NONE;
    v->level = -1;
    v->missed_at_ms = 0;
}

void wave_view_start_failed(struct wave_view *v, const char *why)
{
    v->phase = WAVE_PHASE_IDLE;
    snprintf(v->error, sizeof(v->error), "%s", why && *why ? why : "Could not start");
}

void wave_view_stopping(struct wave_view *v, int64_t now_ms)
{
    if (v->phase == WAVE_PHASE_SENDING || v->phase == WAVE_PHASE_LISTENING) {
        v->phase = WAVE_PHASE_STOPPING;
        v->phase_since_ms = now_ms;
    }
}

static void keep_received(struct wave_view *v, const struct wave_event *ev)
{
    int i;

    for (i = WAVE_VIEW_KEEP - 1; i > 0; i--) {
        v->received[i] = v->received[i - 1];
    }
    wave_view_format_received(ev->text, ev->len, &v->received[0]);
    if (v->received_count < WAVE_VIEW_KEEP) {
        v->received_count++;
    }
}

void wave_view_apply(struct wave_view *v, const struct wave_event *ev, int64_t now_ms)
{
    int was_stopping;

    switch (ev->kind) {
    case WAVE_EV_SENDING:
        if (v->phase == WAVE_PHASE_SENDING) {
            v->expected_ms = ev->value;
            v->phase_since_ms = now_ms;
        }
        break;
    case WAVE_EV_LISTENING:
        if (v->phase == WAVE_PHASE_LISTENING) {
            v->level = 0;
        }
        break;
    case WAVE_EV_LEVEL:
        if (v->phase == WAVE_PHASE_LISTENING) {
            v->level = ev->value;
        }
        break;
    case WAVE_EV_RECEIVED:
        /* Only from a listen: whatever a send helper says, it did not hear. */
        if (v->phase != WAVE_PHASE_IDLE && v->running_mode == WAVE_MODE_RECEIVE) {
            keep_received(v, ev);
            v->missed_at_ms = 0;
        }
        break;
    case WAVE_EV_MISSED:
        if (v->phase == WAVE_PHASE_LISTENING) {
            /* 0 means "none", so a miss at the clock's first tick still counts. */
            v->missed_at_ms = now_ms > 0 ? now_ms : 1;
        }
        break;
    case WAVE_EV_SENT:
        v->got_sent = 1;
        break;
    case WAVE_EV_ERROR:
        v->got_error = 1;
        wave_view_error_text(ev->text, v->error, sizeof(v->error));
        break;
    case WAVE_EV_EXITED:
        if (v->phase == WAVE_PHASE_IDLE) {
            break;
        }
        was_stopping = v->phase == WAVE_PHASE_STOPPING;
        v->phase = WAVE_PHASE_IDLE;
        v->level = -1;
        v->expected_ms = 0;
        if (v->got_error) {
            v->last = WAVE_LAST_NONE;
        } else if (was_stopping && (ev->value == 0 || ev->value == 128 + SIGTERM ||
                                    ev->value == 128 + SIGKILL)) {
            v->last = WAVE_LAST_STOPPED;
        } else if (ev->value != 0) {
            snprintf(v->error, sizeof(v->error), "Wave helper stopped unexpectedly (%d)", ev->value);
            v->last = WAVE_LAST_NONE;
        } else if (v->running_mode == WAVE_MODE_SEND) {
            if (v->got_sent) {
                v->last = WAVE_LAST_SENT;
            } else {
                snprintf(v->error, sizeof(v->error), "Sending did not finish");
                v->last = WAVE_LAST_NONE;
            }
        } else {
            v->last = WAVE_LAST_ENDED;
        }
        break;
    case WAVE_EV_READY:
    case WAVE_EV_STOPPED:
    default:
        break;
    }
}

static void tenths(char *buf, size_t n, int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    snprintf(buf, n, "%d.%d", ms / 1000, (ms % 1000) / 100);
}

void wave_view_refresh(struct wave_view *v, const char *message, int64_t now_ms)
{
    size_t bytes = 0;
    char why[64];
    int valid = wave_view_check_message(message, &bytes, why, sizeof(why)) == 0;
    int idle = v->phase == WAVE_PHASE_IDLE;

    v->mic_on = v->phase == WAVE_PHASE_LISTENING ||
                (v->phase == WAVE_PHASE_STOPPING && v->running_mode == WAVE_MODE_RECEIVE);
    v->can_switch_mode = idle;
    v->can_pick_profile = idle;
    v->can_edit = idle;

    snprintf(v->counter, sizeof(v->counter), "%zu / %d bytes", bytes, WAVE_MAX_MESSAGE_BYTES);
    v->counter_tone = bytes > WAVE_MAX_MESSAGE_BYTES || (bytes > 0 && !valid) ? WAVE_TONE_ERROR
                                                                              : WAVE_TONE_MUTED;

    switch (v->phase) {
    case WAVE_PHASE_SENDING:
        v->action_label = "STOP";
        v->action_enabled = 1;
        v->action_primary = 0;
        break;
    case WAVE_PHASE_LISTENING:
        v->action_label = "STOP LISTENING";
        v->action_enabled = 1;
        v->action_primary = 0;
        break;
    case WAVE_PHASE_STOPPING:
        v->action_label = "STOPPING";
        v->action_enabled = 0;
        v->action_primary = 0;
        break;
    case WAVE_PHASE_IDLE:
    default:
        if (v->mode == WAVE_MODE_SEND) {
            v->action_label = "TRANSMIT";
            v->action_enabled = valid;
        } else {
            v->action_label = "START LISTENING";
            v->action_enabled = 1;
        }
        v->action_primary = 1;
        break;
    }

    v->status_tone = WAVE_TONE_MUTED;
    if (v->error[0] && v->phase != WAVE_PHASE_STOPPING) {
        snprintf(v->status, sizeof(v->status), "%s", v->error);
        v->status_tone = WAVE_TONE_ERROR;
    } else {
        switch (v->phase) {
        case WAVE_PHASE_SENDING:
            if (v->expected_ms <= 0) {
                snprintf(v->status, sizeof(v->status), "Starting the speaker");
            } else {
                char done[16];
                char total[16];
                int64_t elapsed = now_ms - v->phase_since_ms;

                if (elapsed > v->expected_ms) {
                    elapsed = v->expected_ms;
                }
                tenths(done, sizeof(done), (int)elapsed);
                tenths(total, sizeof(total), v->expected_ms);
                snprintf(v->status, sizeof(v->status), "Sending, %s of %s s", done, total);
            }
            v->status_tone = WAVE_TONE_PRIMARY;
            break;
        case WAVE_PHASE_LISTENING:
            if (v->missed_at_ms && now_ms - v->missed_at_ms < WAVE_VIEW_MISSED_MS) {
                snprintf(v->status, sizeof(v->status),
                         "Microphone on, heard a signal it could not decode");
            } else if (v->level < 0) {
                snprintf(v->status, sizeof(v->status), "Microphone on, starting");
            } else {
                snprintf(v->status, sizeof(v->status), "Microphone on, level %d%%", v->level);
            }
            v->status_tone = WAVE_TONE_WARN;
            break;
        case WAVE_PHASE_STOPPING:
            snprintf(v->status, sizeof(v->status), "Stopping");
            v->status_tone = v->mic_on ? WAVE_TONE_WARN : WAVE_TONE_MUTED;
            break;
        case WAVE_PHASE_IDLE:
        default:
            switch (v->last) {
            case WAVE_LAST_SENT:
                snprintf(v->status, sizeof(v->status), "Sent");
                v->status_tone = WAVE_TONE_OK;
                break;
            case WAVE_LAST_STOPPED:
                snprintf(v->status, sizeof(v->status), "Stopped");
                break;
            case WAVE_LAST_ENDED:
                snprintf(v->status, sizeof(v->status), "Listening ended after %d s",
                         WAVE_LISTEN_SECONDS);
                break;
            case WAVE_LAST_NONE:
            default:
                snprintf(v->status, sizeof(v->status), "%s",
                         v->mode == WAVE_MODE_SEND ? "Ready to send" : "Microphone off");
                break;
            }
            break;
        }
    }

    if (v->mic_on) {
        v->status_hint = "MIC ON";
    } else if (v->phase == WAVE_PHASE_SENDING ||
               (v->phase == WAVE_PHASE_STOPPING && v->running_mode == WAVE_MODE_SEND)) {
        v->status_hint = "SENDING";
    } else {
        v->status_hint = "";
    }
}
