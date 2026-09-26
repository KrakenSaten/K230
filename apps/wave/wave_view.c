/*
 * Wave's model. See wave_view.h.
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
#include <time.h>

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

static int valid_preset(int p)
{
    return wave_preset_get(p) != NULL;
}

void wave_view_init(struct wave_view *v, int preset)
{
    memset(v, 0, sizeof(*v));
    v->preset = valid_preset(preset) ? preset : WAVE_PRESET_DEFAULT;
    v->phase = WAVE_PHASE_IDLE;
    v->op = WAVE_OP_NONE;
    v->level = -1;
    wave_history_init(&v->history);
    wave_view_refresh(v, "", 0);
}

const struct wave_preset *wave_view_preset(const struct wave_view *v)
{
    return wave_preset_get(v->preset);
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

int wave_view_format_payload(const char *bytes, size_t len, char *out, size_t n)
{
    size_t i;
    size_t off;

    if (!n) {
        return 0;
    }
    if (len > 0 && len < n && wave_text_clean(bytes, len)) {
        memcpy(out, bytes, len);
        out[len] = '\0';
        return 0;
    }
    off = (size_t)snprintf(out, n, "hex:");
    for (i = 0; i < len && off + 4 <= n; i++) {
        off += (size_t)snprintf(out + off, n - off, " %02x", (unsigned char)bytes[i]);
    }
    return 1;
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
    /* The gate is per direction: say which one refused. */
    if (starts_with_word(t, WAVE_ERR_AUDIO_DISABLED) && strstr(t, "speaker")) {
        snprintf(out, n, "The speaker is not enabled on this device yet");
        return;
    }
    if (starts_with_word(t, WAVE_ERR_AUDIO_DISABLED) && strstr(t, "microphone")) {
        snprintf(out, n, "The microphone is not enabled on this device yet");
        return;
    }
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (starts_with_word(t, known[i].word)) {
            snprintf(out, n, "%s", known[i].words);
            return;
        }
    }
    snprintf(out, n, "Error: %.100s", t);
}

/* ---- intents ------------------------------------------------------------ */

static int running(const struct wave_view *v, enum wave_op op)
{
    return v->phase != WAVE_PHASE_IDLE && v->op == op;
}

/* A send, a capture or a decode is on its way: queued, running or stopping. */
static int exclusive_busy(const struct wave_view *v)
{
    return v->send_pending || v->capture_pending || v->decode_pending ||
           running(v, WAVE_OP_SEND) || running(v, WAVE_OP_CAPTURE) || running(v, WAVE_OP_DECODE);
}

static const char *preset_id(int p)
{
    const struct wave_preset *pr = wave_preset_get(p);

    return pr ? pr->id : "-";
}

int wave_view_set_preset(struct wave_view *v, int preset)
{
    if (!valid_preset(preset) || exclusive_busy(v)) {
        return 0;
    }
    v->preset = preset;
    return 1;
}

static void forget_outcome(struct wave_view *v)
{
    v->error[0] = '\0';
    v->last = WAVE_LAST_NONE;
}

/* A queued send that never got to play, or stopped partway: kept in the
 * history so the text is not lost with it. */
static void end_send(struct wave_view *v, enum wave_result result, int64_t wall_s)
{
    if (v->send_pending) {
        wave_history_add_tx(&v->history, preset_id(v->tx_preset), v->tx_text, v->tx_len, result,
                            v->tx_done, wall_s);
    }
    v->send_pending = 0;
}

enum wave_action wave_view_request_send(struct wave_view *v, const char *text, int *refused)
{
    const struct wave_preset *p = wave_view_preset(v);
    char why[64];
    size_t len;

    if (refused) {
        *refused = 1;
    }
    if (wave_view_check_message(text, &len, why, sizeof(why)) != 0 || exclusive_busy(v) || !p) {
        return WAVE_DO_NOTHING;
    }
    if (refused) {
        *refused = 0;
    }
    memcpy(v->tx_text, text, len);
    v->tx_text[len] = '\0';
    v->tx_len = len;
    v->tx_preset = v->preset;
    v->tx_copies = p->copies;
    v->tx_done = 0;
    v->send_pending = 1;
    forget_outcome(v);
    /* The listen pauses for the send and resumes after it: the toggle stays. */
    return v->phase == WAVE_PHASE_RUNNING && v->op == WAVE_OP_LISTEN ? WAVE_DO_STOP
                                                                     : WAVE_DO_NOTHING;
}

enum wave_action wave_view_request_listen(struct wave_view *v)
{
    if (v->listen_on) {
        v->listen_on = 0;
        if (v->phase == WAVE_PHASE_RUNNING && v->op == WAVE_OP_LISTEN) {
            v->stop_asked = 1;
            return WAVE_DO_STOP;
        }
        return WAVE_DO_NOTHING;
    }
    v->listen_on = 1;
    forget_outcome(v);
    return WAVE_DO_NOTHING;
}

enum wave_action wave_view_request_capture(struct wave_view *v)
{
    if (v->phase == WAVE_PHASE_RUNNING && v->op == WAVE_OP_CAPTURE) {
        /* Finish early: what was recorded is still decoded. */
        return WAVE_DO_STOP;
    }
    if (exclusive_busy(v)) {
        return WAVE_DO_NOTHING;
    }
    v->capture_pending = 1;
    forget_outcome(v);
    return v->phase == WAVE_PHASE_RUNNING && v->op == WAVE_OP_LISTEN ? WAVE_DO_STOP
                                                                     : WAVE_DO_NOTHING;
}

enum wave_action wave_view_request_stop(struct wave_view *v, int64_t wall_s)
{
    v->capture_pending = 0;
    if (v->phase == WAVE_PHASE_IDLE) {
        /* Nothing runs, so nothing queued can be waiting on a helper; the
         * toggle is the listen's own button's to turn off. */
        return WAVE_DO_NOTHING;
    }
    if (v->op == WAVE_OP_LISTEN) {
        v->listen_on = 0;
        /* A send queued behind this listen is dropped, kept in history. */
        end_send(v, WAVE_RESULT_STOPPED, wall_s);
    }
    if (v->op == WAVE_OP_DECODE || v->op == WAVE_OP_CAPTURE) {
        v->decode_pending = 0;
    }
    if (v->phase == WAVE_PHASE_RUNNING) {
        v->stop_asked = 1;
        return WAVE_DO_STOP;
    }
    return WAVE_DO_NOTHING;
}

int wave_view_request_clear(struct wave_view *v, int64_t now_ms)
{
    if (wave_history_count(&v->history) == 0) {
        v->clear_armed_ms = 0;
        return 0;
    }
    if (v->clear_armed_ms && now_ms >= v->clear_armed_ms &&
        now_ms - v->clear_armed_ms < WAVE_VIEW_CLEAR_ARM_MS) {
        wave_history_clear(&v->history);
        v->clear_armed_ms = 0;
        return 1;
    }
    /* 0 means "not armed", so an arm at the clock's first tick still counts. */
    v->clear_armed_ms = now_ms > 0 ? now_ms : 1;
    return 0;
}

enum wave_action wave_view_next(const struct wave_view *v)
{
    if (v->phase != WAVE_PHASE_IDLE) {
        return WAVE_DO_NOTHING;
    }
    if (v->decode_pending) {
        return WAVE_DO_DECODE;
    }
    if (v->send_pending) {
        return WAVE_DO_SEND;
    }
    if (v->capture_pending) {
        return WAVE_DO_CAPTURE;
    }
    if (v->listen_on) {
        return WAVE_DO_LISTEN;
    }
    return WAVE_DO_NOTHING;
}

static enum wave_op op_for(enum wave_action what)
{
    switch (what) {
    case WAVE_DO_SEND: return WAVE_OP_SEND;
    case WAVE_DO_LISTEN: return WAVE_OP_LISTEN;
    case WAVE_DO_CAPTURE: return WAVE_OP_CAPTURE;
    case WAVE_DO_DECODE: return WAVE_OP_DECODE;
    default: return WAVE_OP_NONE;
    }
}

void wave_view_started(struct wave_view *v, enum wave_action what, int64_t now_ms)
{
    enum wave_op op = op_for(what);

    if (op == WAVE_OP_NONE) {
        return;
    }
    v->phase = WAVE_PHASE_RUNNING;
    v->op = op;
    v->phase_since_ms = now_ms;
    v->expected_ms = 0;
    v->got_sent = 0;
    v->got_error = 0;
    v->decoded = 0;
    v->stop_asked = 0;
    v->level = -1;
    v->missed_at_ms = 0;
    v->error[0] = '\0';
    v->run_preset = v->preset;
    if (op == WAVE_OP_CAPTURE) {
        v->capture_pending = 0;
        v->last = WAVE_LAST_NONE;
    } else if (op == WAVE_OP_DECODE) {
        v->decode_pending = 0;
    } else if (op == WAVE_OP_SEND) {
        v->last = WAVE_LAST_NONE;
    }
}

/* Whatever failed, nothing queued runs by itself afterwards. */
static void give_up(struct wave_view *v, int64_t wall_s)
{
    end_send(v, WAVE_RESULT_FAILED, wall_s);
    v->listen_on = 0;
    v->capture_pending = 0;
    v->decode_pending = 0;
    v->last = WAVE_LAST_NONE;
}

void wave_view_start_failed(struct wave_view *v, enum wave_action what, const char *why,
                            int64_t wall_s)
{
    (void)what;
    v->phase = WAVE_PHASE_IDLE;
    v->op = WAVE_OP_NONE;
    give_up(v, wall_s);
    snprintf(v->error, sizeof(v->error), "%s", why && *why ? why : "Could not start");
}

void wave_view_stopping(struct wave_view *v, int64_t now_ms)
{
    if (v->phase == WAVE_PHASE_RUNNING) {
        v->phase = WAVE_PHASE_STOPPING;
        v->phase_since_ms = now_ms;
    }
}

/* The preset a received message is filed under: the one the listen ran on. */
static const char *rx_preset(const struct wave_view *v)
{
    return preset_id(v->run_preset);
}

static void exited(struct wave_view *v, int status, int64_t wall_s)
{
    int stopping = v->phase == WAVE_PHASE_STOPPING;
    int by_stop = stopping && (status == 0 || status == 128 + SIGTERM || status == 128 + SIGKILL);
    int failed = v->got_error || (status != 0 && !by_stop);
    enum wave_op op = v->op;

    v->phase = WAVE_PHASE_IDLE;
    v->op = WAVE_OP_NONE;
    v->level = -1;
    v->expected_ms = 0;

    if (failed && op != WAVE_OP_DECODE) {
        if (!v->got_error) {
            snprintf(v->error, sizeof(v->error), "Wave helper stopped unexpectedly (%d)", status);
        }
        if (op == WAVE_OP_LISTEN) {
            /* The microphone failed, the speaker may not have: a send queued
             * behind this listen still goes. Nothing reopens the microphone. */
            v->listen_on = 0;
            v->capture_pending = 0;
            v->decode_pending = 0;
            v->last = WAVE_LAST_NONE;
        } else {
            give_up(v, wall_s);
        }
        return;
    }

    switch (op) {
    case WAVE_OP_SEND:
        if (v->got_sent) {
            v->tx_done++;
        }
        if (v->stop_asked && v->tx_done < v->tx_copies) {
            end_send(v, WAVE_RESULT_STOPPED, wall_s);
            v->last = WAVE_LAST_SEND_STOPPED;
        } else if (!v->got_sent) {
            snprintf(v->error, sizeof(v->error), "Sending did not finish");
            give_up(v, wall_s);
        } else if (v->tx_done >= v->tx_copies) {
            end_send(v, WAVE_RESULT_OK, wall_s);
            v->last = WAVE_LAST_SENT;
        }
        /* Otherwise the next copy is due: send_pending stays. */
        break;
    case WAVE_OP_LISTEN:
        if (v->stop_asked) {
            v->last = WAVE_LAST_LISTEN_STOPPED;
        } else if (!stopping) {
            /* It reached its length: the microphone stays off until asked. */
            v->listen_on = 0;
            v->last = WAVE_LAST_LISTEN_ENDED;
        }
        /* A listen paused for a send or a capture says nothing. */
        break;
    case WAVE_OP_CAPTURE:
        if (v->stop_asked || status != 0) {
            /* Stopped, not finished early: the recording is thrown away. */
            v->last = WAVE_LAST_NONE;
        } else {
            v->decode_pending = 1;
        }
        break;
    case WAVE_OP_DECODE:
        /* Exit 1 is the helper's "ran and found nothing", not a failure. */
        if (v->got_error || v->stop_asked) {
            v->last = WAVE_LAST_NONE;
        } else if (status != 0 && status != WAVE_EXIT_FAILED) {
            snprintf(v->error, sizeof(v->error), "Wave helper stopped unexpectedly (%d)", status);
            v->last = WAVE_LAST_NONE;
        } else if (v->decoded > 0) {
            v->last = WAVE_LAST_DECODED;
            v->last_count = v->decoded;
        } else {
            wave_history_add_undecoded(&v->history, rx_preset(v), wall_s);
            v->last = WAVE_LAST_NOTHING_DECODED;
        }
        break;
    case WAVE_OP_NONE:
    default:
        break;
    }
}

void wave_view_apply(struct wave_view *v, const struct wave_event *ev, int64_t now_ms,
                     int64_t wall_s)
{
    const struct wave_preset *p = wave_preset_get(v->run_preset);
    int window = p ? p->dedupe_ms : 0;

    switch (ev->kind) {
    case WAVE_EV_SENDING:
        if (running(v, WAVE_OP_SEND)) {
            v->expected_ms = ev->value;
            v->phase_since_ms = now_ms;
        }
        break;
    case WAVE_EV_LISTENING:
        if (running(v, WAVE_OP_LISTEN) || running(v, WAVE_OP_CAPTURE)) {
            v->level = 0;
        }
        break;
    case WAVE_EV_LEVEL:
        if (v->phase == WAVE_PHASE_RUNNING && (v->op == WAVE_OP_LISTEN || v->op == WAVE_OP_CAPTURE)) {
            v->level = ev->value;
        }
        break;
    case WAVE_EV_RECEIVED:
        /* Only from something that listens: whatever a send helper says, it
         * did not hear. */
        if (running(v, WAVE_OP_LISTEN)) {
            wave_history_add_rx(&v->history, rx_preset(v), ev->text, ev->len, 0, wall_s, now_ms,
                                window);
            v->missed_at_ms = 0;
        } else if (running(v, WAVE_OP_DECODE)) {
            wave_history_add_rx(&v->history, rx_preset(v), ev->text, ev->len, 1, wall_s, now_ms,
                                window);
            v->decoded++;
        }
        break;
    case WAVE_EV_MISSED:
        if (running(v, WAVE_OP_LISTEN)) {
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
        if (v->phase != WAVE_PHASE_IDLE) {
            exited(v, ev->value, wall_s);
        }
        break;
    case WAVE_EV_READY:
    case WAVE_EV_STOPPED:
    default:
        break;
    }
}

/* ---- presentation ------------------------------------------------------- */

static void tenths(char *buf, size_t n, int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    snprintf(buf, n, "%d.%d", ms / 1000, (ms % 1000) / 100);
}

static void copy_prefix(const struct wave_view *v, char *out, size_t n)
{
    if (v->tx_copies > 1) {
        snprintf(out, n, "Copy %d of %d, ", v->tx_done + 1, v->tx_copies);
    } else {
        out[0] = '\0';
    }
}

static void status_running(struct wave_view *v, int64_t now_ms)
{
    const struct wave_preset *p = wave_preset_get(v->run_preset);
    char pre[48];

    switch (v->op) {
    case WAVE_OP_SEND:
        copy_prefix(v, pre, sizeof(pre));
        if (v->expected_ms <= 0) {
            snprintf(v->status, sizeof(v->status), "%s%s", pre,
                     pre[0] ? "starting the speaker" : "Starting the speaker");
        } else {
            char done[16];
            char total[16];
            int64_t elapsed = now_ms - v->phase_since_ms;

            if (elapsed > v->expected_ms) {
                elapsed = v->expected_ms;
            }
            tenths(done, sizeof(done), (int)elapsed);
            tenths(total, sizeof(total), v->expected_ms);
            snprintf(v->status, sizeof(v->status), "%s%s, %s of %s s", pre,
                     pre[0] ? "sending" : "Sending", done, total);
        }
        v->status_tone = WAVE_TONE_PRIMARY;
        break;
    case WAVE_OP_LISTEN:
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
    case WAVE_OP_CAPTURE: {
        int64_t secs = (now_ms - v->phase_since_ms) / 1000;
        int total = p ? p->capture_seconds : 0;

        if (secs > total) {
            secs = total;
        }
        snprintf(v->status, sizeof(v->status), "Microphone on, recording %d of %d s", (int)secs,
                 total);
        v->status_tone = WAVE_TONE_WARN;
        break;
    }
    case WAVE_OP_DECODE:
        snprintf(v->status, sizeof(v->status), "Decoding the recording");
        v->status_tone = WAVE_TONE_PRIMARY;
        break;
    case WAVE_OP_NONE:
    default:
        break;
    }
}

static void status_idle(struct wave_view *v)
{
    const struct wave_preset *p = wave_view_preset(v);

    v->status_tone = WAVE_TONE_MUTED;
    switch (v->last) {
    case WAVE_LAST_SENT:
        if (v->tx_done > 1) {
            snprintf(v->status, sizeof(v->status), "Sent, %d copies", v->tx_done);
        } else {
            snprintf(v->status, sizeof(v->status), "Sent");
        }
        v->status_tone = WAVE_TONE_OK;
        break;
    case WAVE_LAST_SEND_STOPPED:
        snprintf(v->status, sizeof(v->status), "Sending stopped");
        break;
    case WAVE_LAST_LISTEN_STOPPED:
        snprintf(v->status, sizeof(v->status), "Microphone off");
        break;
    case WAVE_LAST_LISTEN_ENDED:
        snprintf(v->status, sizeof(v->status), "Listening ended after %d s",
                 p ? p->listen_seconds : WAVE_LISTEN_SECONDS);
        break;
    case WAVE_LAST_DECODED:
        snprintf(v->status, sizeof(v->status), "Decoded %d message%s from the recording",
                 v->last_count, v->last_count == 1 ? "" : "s");
        v->status_tone = WAVE_TONE_OK;
        break;
    case WAVE_LAST_NOTHING_DECODED:
        snprintf(v->status, sizeof(v->status), "Nothing decoded in the recording");
        v->status_tone = WAVE_TONE_WARN;
        break;
    case WAVE_LAST_NONE:
    default:
        snprintf(v->status, sizeof(v->status), "Ready. Microphone off");
        break;
    }
}

void wave_view_refresh(struct wave_view *v, const char *message, int64_t now_ms)
{
    const struct wave_preset *p = wave_view_preset(v);
    size_t bytes = 0;
    char why[64];
    int valid = wave_view_check_message(message, &bytes, why, sizeof(why)) == 0;
    int sending = running(v, WAVE_OP_SEND);
    int capturing = running(v, WAVE_OP_CAPTURE);

    v->busy = v->phase != WAVE_PHASE_IDLE;
    v->mic_on = running(v, WAVE_OP_LISTEN) || capturing;
    v->can_pick_preset = !exclusive_busy(v);
    /* The field is always open: a send copies the text when it is asked. */
    v->can_edit = 1;

    snprintf(v->counter, sizeof(v->counter), "%zu / %d bytes", bytes, WAVE_MAX_MESSAGE_BYTES);
    v->counter_tone = bytes > WAVE_MAX_MESSAGE_BYTES || (bytes > 0 && !valid) ? WAVE_TONE_ERROR
                                                                              : WAVE_TONE_MUTED;

    /* The one STOP on the screen: it ends a send, and throws a capture away
     * (DECODE NOW is the other way out of a capture, keeping it). */
    if ((sending || capturing) && v->phase == WAVE_PHASE_RUNNING) {
        v->send_label = "STOP";
        v->send_enabled = 1;
        v->send_primary = 0;
    } else {
        v->send_label = "SEND";
        v->send_enabled = valid && !exclusive_busy(v);
        v->send_primary = 1;
    }

    v->listen_label = v->listen_on ? "STOP LISTEN" : "LISTEN";
    v->listen_enabled = 1;
    v->listen_primary = v->listen_on;

    if (capturing && v->phase == WAVE_PHASE_RUNNING) {
        v->capture_label = "DECODE NOW";
        v->capture_enabled = 1;
    } else {
        v->capture_label = "CAPTURE";
        v->capture_enabled = !exclusive_busy(v);
    }

    v->clear_enabled = wave_history_count(&v->history) > 0;
    v->clear_label = v->clear_enabled && v->clear_armed_ms && now_ms >= v->clear_armed_ms &&
                             now_ms - v->clear_armed_ms < WAVE_VIEW_CLEAR_ARM_MS
                         ? "CONFIRM"
                         : "CLEAR";

    snprintf(v->preset_label, sizeof(v->preset_label), "%s - %s", p ? p->label : "?",
             wave_preset_speed_label(p));
    v->preset_summary = p ? p->summary : "";

    /* The chip: one word for what is happening. */
    if (v->phase == WAVE_PHASE_STOPPING) {
        snprintf(v->chip, sizeof(v->chip), "STOPPING");
        v->chip_kind = v->mic_on ? WAVE_CHIP_RX : sending ? WAVE_CHIP_TX : WAVE_CHIP_BUSY;
    } else if (sending) {
        if (v->tx_copies > 1) {
            snprintf(v->chip, sizeof(v->chip), "SENDING %d/%d", v->tx_done + 1, v->tx_copies);
        } else {
            snprintf(v->chip, sizeof(v->chip), "SENDING");
        }
        v->chip_kind = WAVE_CHIP_TX;
    } else if (running(v, WAVE_OP_LISTEN)) {
        snprintf(v->chip, sizeof(v->chip), "LISTENING");
        v->chip_kind = WAVE_CHIP_RX;
    } else if (capturing) {
        snprintf(v->chip, sizeof(v->chip), "CAPTURING");
        v->chip_kind = WAVE_CHIP_RX;
    } else if (running(v, WAVE_OP_DECODE)) {
        snprintf(v->chip, sizeof(v->chip), "DECODING");
        v->chip_kind = WAVE_CHIP_BUSY;
    } else {
        snprintf(v->chip, sizeof(v->chip), "%s", v->error[0] ? "ERROR" : "READY");
        v->chip_kind = WAVE_CHIP_IDLE;
    }

    if (v->error[0] && v->phase != WAVE_PHASE_STOPPING) {
        snprintf(v->status, sizeof(v->status), "%s", v->error);
        v->status_tone = WAVE_TONE_ERROR;
    } else if (v->phase == WAVE_PHASE_STOPPING) {
        if (v->op == WAVE_OP_LISTEN && (v->send_pending || v->capture_pending)) {
            snprintf(v->status, sizeof(v->status), "Pausing the microphone to %s",
                     v->send_pending ? "send" : "capture");
        } else if (v->op == WAVE_OP_CAPTURE && !v->stop_asked) {
            snprintf(v->status, sizeof(v->status), "Finishing the recording");
        } else {
            snprintf(v->status, sizeof(v->status), "Stopping");
        }
        v->status_tone = v->mic_on ? WAVE_TONE_WARN : WAVE_TONE_MUTED;
    } else if (v->phase == WAVE_PHASE_RUNNING) {
        status_running(v, now_ms);
    } else {
        status_idle(v);
    }

    if (v->mic_on) {
        v->status_hint = "MIC ON";
    } else if (sending) {
        v->status_hint = "SENDING";
    } else {
        v->status_hint = "";
    }
}

/* ---- history entries ---------------------------------------------------- */

static void clock_text(int64_t when, int64_t now_wall, char *out, size_t n)
{
    time_t t = (time_t)when;
    time_t now = (time_t)now_wall;
    struct tm tm;
    struct tm today;

    if (when <= 0 || !localtime_r(&t, &tm)) {
        snprintf(out, n, "--:--");
        return;
    }
    if (now_wall > 0 && localtime_r(&now, &today) &&
        (today.tm_year != tm.tm_year || today.tm_yday != tm.tm_yday)) {
        snprintf(out, n, "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        return;
    }
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static const char *preset_label_for(const char *id)
{
    const struct wave_preset *p = wave_preset_get(wave_preset_find(id));

    return p ? p->label : id;
}

void wave_view_format_entry(const struct wave_history_entry *e, int64_t now_wall, char *meta,
                               size_t meta_n, char *text, size_t text_n)
{
    char when[24];
    char extra[32] = "";

    clock_text(e->when, now_wall, when, sizeof(when));
    if (e->dir == WAVE_DIR_TX) {
        switch (e->result) {
        case WAVE_RESULT_STOPPED:
            snprintf(extra, sizeof(extra), "  STOPPED");
            break;
        case WAVE_RESULT_FAILED:
            snprintf(extra, sizeof(extra), "  FAILED");
            break;
        default:
            if (e->count > 1) {
                snprintf(extra, sizeof(extra), "  x%d", e->count);
            }
            break;
        }
    } else {
        if (e->result == WAVE_RESULT_OK && e->count > 1) {
            snprintf(extra, sizeof(extra), "  x%d", e->count);
        }
        if (e->captured) {
            size_t used = strlen(extra);

            snprintf(extra + used, sizeof(extra) - used, "  CAPTURE");
        }
    }
    snprintf(meta, meta_n, "%s %s  %s%s", e->dir == WAVE_DIR_TX ? "TX" : "RX", when,
             preset_label_for(e->preset), extra);
    if (e->result == WAVE_RESULT_UNDECODED) {
        snprintf(text, text_n, "Nothing decoded in the recording");
    } else if (wave_view_format_payload(e->data, e->len, text, text_n) == 0 && e->truncated) {
        size_t used = strlen(text);

        snprintf(text + used, text_n - used, "...");
    }
}
