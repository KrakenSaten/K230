/*
 * Wave's model: message rules; the one-screen workflow - LISTEN as a toggle,
 * a send that pauses the listen and resumes it, the preset's copies, capture
 * then decode; the microphone indicator across the gaps where the device may
 * be open; failures that never loop; the history the model keeps (repeats
 * folded, stops and failures recorded); CLEAR's two taps; the preset lock;
 * the words for every helper error and exit, and how entries are shown.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_view.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct wave_event ev_of(enum wave_event_kind k, int value, const char *text)
{
    struct wave_event e;

    memset(&e, 0, sizeof(e));
    e.kind = k;
    e.value = value;
    if (text) {
        e.len = strlen(text);
        memcpy(e.text, text, e.len);
    }
    return e;
}

static void apply(struct wave_view *v, enum wave_event_kind k, int value, const char *text, int64_t now)
{
    struct wave_event e = ev_of(k, value, text);

    wave_view_apply(v, &e, now, 1758900000 + now / 1000);
}

/* What wave_ctl does between requests: start whatever is due. Returns what
 * was started. */
static enum wave_action run_next(struct wave_view *v, int64_t now)
{
    enum wave_action what = wave_view_next(v);

    if (what != WAVE_DO_NOTHING) {
        wave_view_started(v, what, now);
    }
    return what;
}

/* A request that says STOP, obeyed the way wave_ctl obeys it. */
static void obey(struct wave_view *v, enum wave_action what, int64_t now)
{
    if (what == WAVE_DO_STOP) {
        wave_view_stopping(v, now);
    }
}

static const struct wave_history_entry *newest(const struct wave_view *v)
{
    return wave_history_at(&v->history, 0);
}

static int ok_msg(const char *s)
{
    char why[64];

    return wave_view_check_message(s, NULL, why, sizeof(why)) == 0;
}

static void test_messages(void)
{
    char why[64];
    char buf[WAVE_MAX_MESSAGE_BYTES + 2];
    size_t bytes = 99;

    check("message: empty is refused with a reason",
          wave_view_check_message("", &bytes, why, sizeof(why)) == -1 && bytes == 0 &&
              strcmp(why, "Type a message") == 0);
    check("message: NULL is empty", wave_view_check_message(NULL, &bytes, why, sizeof(why)) == -1);
    check("message: DOORS is fine, 5 bytes",
          wave_view_check_message("DOORS", &bytes, why, sizeof(why)) == 0 && bytes == 5);
    memset(buf, 'a', WAVE_MAX_MESSAGE_BYTES);
    buf[WAVE_MAX_MESSAGE_BYTES] = '\0';
    check("message: exactly the limit is fine", ok_msg(buf));
    buf[WAVE_MAX_MESSAGE_BYTES] = 'a';
    buf[WAVE_MAX_MESSAGE_BYTES + 1] = '\0';
    check("message: one byte over is refused and says how long",
          wave_view_check_message(buf, &bytes, why, sizeof(why)) == -1 &&
              bytes == WAVE_MAX_MESSAGE_BYTES + 1 && strstr(why, "65 of 64") != NULL);
    check("message: the limit is bytes, not characters (ae o-slash a-ring = 6 bytes)",
          wave_view_check_message("\xC3\xA6\xC3\xB8\xC3\xA5", &bytes, why, sizeof(why)) == 0 &&
              bytes == 6);
    check("message: a 4-byte character is text", ok_msg("\xF0\x9F\x98\x80"));
    check("message: a lone lead byte is refused", !ok_msg("A\xC3"));
    check("message: an overlong encoding is refused", !ok_msg("\xC0\x80"));
    check("message: an overlong 3-byte encoding is refused", !ok_msg("\xE0\x80\xAF"));
    check("message: a UTF-16 surrogate is refused", !ok_msg("\xED\xA0\x80"));
    check("message: above U+10FFFF is refused", !ok_msg("\xF4\x90\x80\x80"));
    check("message: a stray continuation byte is refused", !ok_msg("\x80"));
    check("message: tab is a control character", !ok_msg("a\tb"));
    check("message: newline is a control character", !ok_msg("a\nb"));
    check("message: DEL is refused", !ok_msg("a\x7F"));
    check("message: a C1 control is refused", !ok_msg("a\xC2\x85"));
    check("message: spaces and punctuation are fine", ok_msg("Hei, hvor er du? 12:30!"));
}

static void test_idle(void)
{
    struct wave_view v;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    check("init: idle, STANDARD, nothing queued, microphone off",
          v.phase == WAVE_PHASE_IDLE && v.preset == WAVE_PRESET_STANDARD && !v.listen_on &&
              !v.send_pending && !v.mic_on && v.status_hint[0] == '\0');
    check("init: nothing starts by itself (the microphone never turns itself on)",
          wave_view_next(&v) == WAVE_DO_NOTHING);
    check("init: the chip says READY", strcmp(v.chip, "READY") == 0 && v.chip_kind == WAVE_CHIP_IDLE);
    check("init: status says ready and that the microphone is off",
          strcmp(v.status, "Ready. Microphone off") == 0);
    check("init: SEND is there, disabled with nothing typed",
          strcmp(v.send_label, "SEND") == 0 && !v.send_enabled && v.send_primary);
    check("init: LISTEN and CAPTURE are offered",
          strcmp(v.listen_label, "LISTEN") == 0 && v.listen_enabled && !v.listen_primary &&
              strcmp(v.capture_label, "CAPTURE") == 0 && v.capture_enabled);
    check("init: the preset is named with its speed",
          strcmp(v.preset_label, "STANDARD - FAST") == 0 && strcmp(v.preset_summary, "Fast speed, sent once") == 0);
    check("init: CLEAR is disabled with no history", !v.clear_enabled);
    wave_view_refresh(&v, "DOORS", 0);
    check("idle: SEND enabled once there is a message", v.send_enabled);
    check("idle: counter", strcmp(v.counter, "5 / 64 bytes") == 0 && v.counter_tone == WAVE_TONE_MUTED);
    wave_view_refresh(&v, "a\tb", 0);
    check("idle: an invalid message disables SEND and marks the counter",
          !v.send_enabled && v.counter_tone == WAVE_TONE_ERROR);
    check("idle: the field and the preset are open", v.can_edit && v.can_pick_preset);
    check("preset: pick ROBUST", wave_view_set_preset(&v, WAVE_PRESET_ROBUST) && v.preset == WAVE_PRESET_ROBUST);
    wave_view_refresh(&v, "", 0);
    check("preset: the label follows", strcmp(v.preset_label, "ROBUST - NORMAL") == 0);
    check("preset: out of range refused", !wave_view_set_preset(&v, 3) && !wave_view_set_preset(&v, -1));
    {
        struct wave_view w;

        wave_view_init(&w, 99);
        check("init: an unknown preset falls back to the default", w.preset == WAVE_PRESET_DEFAULT);
    }
    check("profile: names are the helper's words",
          strcmp(wave_view_profile_name(WAVE_PROFILE_NORMAL), "audible_normal") == 0 &&
              strcmp(wave_view_profile_name(WAVE_PROFILE_FAST), "audible_fast") == 0 &&
              strcmp(wave_view_profile_name(WAVE_PROFILE_FASTEST), "audible_fastest") == 0 &&
              wave_view_profile_name(WAVE_PROFILE_COUNT) == NULL);
    check("profile: labels", strcmp(wave_view_profile_label(WAVE_PROFILE_FASTEST), "FASTEST") == 0 &&
                                 wave_view_profile_label((enum wave_profile)-1) == NULL);
}

static void test_listen(void)
{
    struct wave_view v;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    check("listen: the toggle needs no stop when nothing runs", wave_view_request_listen(&v) == WAVE_DO_NOTHING);
    check("listen: the toggle is on and a listen is due", v.listen_on && wave_view_next(&v) == WAVE_DO_LISTEN);
    run_next(&v, 1000);
    wave_view_refresh(&v, "", 1000);
    check("listen: the microphone indicator is on before the helper says listening",
          v.phase == WAVE_PHASE_RUNNING && v.op == WAVE_OP_LISTEN && v.mic_on &&
              strcmp(v.status_hint, "MIC ON") == 0);
    check("listen: the chip says LISTENING, in the receive look",
          strcmp(v.chip, "LISTENING") == 0 && v.chip_kind == WAVE_CHIP_RX);
    check("listen: status warns while starting",
          strcmp(v.status, "Microphone on, starting") == 0 && v.status_tone == WAVE_TONE_WARN);
    check("listen: the toggle reads STOP LISTEN, in the accent",
          strcmp(v.listen_label, "STOP LISTEN") == 0 && v.listen_primary);
    check("listen: nothing else is due while it runs", wave_view_next(&v) == WAVE_DO_NOTHING);
    check("listen: the preset can still be changed (the next listen uses it)", v.can_pick_preset);
    wave_view_refresh(&v, "hi", 1000);
    check("listen: SEND is available while listening", v.send_enabled);

    apply(&v, WAVE_EV_LISTENING, 0, NULL, 1100);
    apply(&v, WAVE_EV_LEVEL, 37, NULL, 1200);
    wave_view_refresh(&v, "", 1200);
    check("listen: the level is shown", strcmp(v.status, "Microphone on, level 37%") == 0);
    apply(&v, WAVE_EV_MISSED, 0, NULL, 1250);
    wave_view_refresh(&v, "", 1260);
    check("listen: a transmission it could not decode is said, as a warning",
          strcmp(v.status, "Microphone on, heard a signal it could not decode") == 0 &&
              v.status_tone == WAVE_TONE_WARN);
    wave_view_refresh(&v, "", 1250 + WAVE_VIEW_MISSED_MS);
    check("listen: and goes back to the level after a while", strcmp(v.status, "Microphone on, level 37%") == 0);
    apply(&v, WAVE_EV_MISSED, 0, NULL, 1280);
    apply(&v, WAVE_EV_RECEIVED, 0, "DOORS", 1300);
    wave_view_refresh(&v, "", 1301);
    check("listen: a message that does decode clears the miss", strstr(v.status, "could not") == NULL);
    check("listen: a message is kept in the history as RX, live, under the preset",
          wave_history_count(&v.history) == 1 && newest(&v)->dir == WAVE_DIR_RX &&
              newest(&v)->result == WAVE_RESULT_OK && !newest(&v)->captured &&
              strcmp(newest(&v)->data, "DOORS") == 0 && strcmp(newest(&v)->preset, "standard") == 0 &&
              newest(&v)->when == 1758900001);
    apply(&v, WAVE_EV_RECEIVED, 0, "DOORS", 2300);
    check("listen: the same message again folds into it (x2)",
          wave_history_count(&v.history) == 1 && newest(&v)->count == 2);
    apply(&v, WAVE_EV_RECEIVED, 0, "DOORS", 2300 + 10001);
    check("listen: after the preset's window it is a new entry", wave_history_count(&v.history) == 2);
    wave_view_refresh(&v, "", 13000);
    check("listen: CLEAR is enabled once there is history", v.clear_enabled && strcmp(v.clear_label, "CLEAR") == 0);

    obey(&v, wave_view_request_listen(&v), 14000);
    wave_view_refresh(&v, "", 14000);
    check("listen off: the toggle is off at once, the helper is stopping",
          !v.listen_on && v.phase == WAVE_PHASE_STOPPING && strcmp(v.listen_label, "LISTEN") == 0);
    check("stopping: the microphone indicator stays on until the helper is gone",
          v.mic_on && strcmp(v.status_hint, "MIC ON") == 0 && strcmp(v.chip, "STOPPING") == 0 &&
              v.chip_kind == WAVE_CHIP_RX);
    apply(&v, WAVE_EV_STOPPED, 0, NULL, 14050);
    wave_view_refresh(&v, "", 14050);
    check("stopping: 'stopped' alone does not turn the indicator off", v.mic_on);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 14100);
    wave_view_refresh(&v, "", 14100);
    check("stopped: idle, microphone off, no hint, nothing due",
          v.phase == WAVE_PHASE_IDLE && !v.mic_on && v.status_hint[0] == '\0' &&
              wave_view_next(&v) == WAVE_DO_NOTHING);
    check("stopped: says the microphone is off, not as an error",
          strcmp(v.status, "Microphone off") == 0 && v.status_tone == WAVE_TONE_MUTED);
    apply(&v, WAVE_EV_LEVEL, 99, NULL, 14200);
    apply(&v, WAVE_EV_MISSED, 0, NULL, 14200);
    apply(&v, WAVE_EV_RECEIVED, 0, "late", 14200);
    check("an event for no running helper changes nothing",
          v.level == -1 && v.missed_at_ms == 0 && wave_history_count(&v.history) == 2);

    wave_view_request_listen(&v);
    run_next(&v, 20000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 140000);
    wave_view_refresh(&v, "", 140000);
    check("listen: running out of time ends it and turns the toggle off",
          strcmp(v.status, "Listening ended after 120 s") == 0 && !v.mic_on && !v.listen_on &&
              wave_view_next(&v) == WAVE_DO_NOTHING);

    wave_view_request_listen(&v);
    run_next(&v, 150000);
    obey(&v, wave_view_request_stop(&v, 0), 150100);
    apply(&v, WAVE_EV_EXITED, 128 + SIGKILL, NULL, 151200);
    wave_view_refresh(&v, "", 151200);
    check("STOP: a listen that had to be killed is still just stopped, toggle off",
          strcmp(v.status, "Microphone off") == 0 && !v.listen_on && v.error[0] == '\0');

    wave_view_request_listen(&v);
    run_next(&v, 160000);
    wave_view_request_listen(&v);   /* off */
    wave_view_request_listen(&v);   /* on again before the helper left */
    apply(&v, WAVE_EV_EXITED, 0, NULL, 160500);
    check("listen: off and on again while stopping listens again", wave_view_next(&v) == WAVE_DO_LISTEN);
}

static void test_send(void)
{
    struct wave_view v;
    int refused = -1;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    check("send: an empty message is refused", wave_view_request_send(&v, "", &refused) == WAVE_DO_NOTHING &&
                                                   refused == 1 && !v.send_pending);
    check("send: a control character is refused", (wave_view_request_send(&v, "a\nb", &refused), refused == 1));
    {
        char big[WAVE_MAX_MESSAGE_BYTES + 2];

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        check("send: one byte over the limit is refused", (wave_view_request_send(&v, big, &refused), refused == 1));
        big[WAVE_MAX_MESSAGE_BYTES] = '\0';
        check("send: exactly the limit is taken", wave_view_request_send(&v, big, &refused) == WAVE_DO_NOTHING &&
                                                      refused == 0 && v.tx_len == WAVE_MAX_MESSAGE_BYTES);
        v.send_pending = 0;
    }
    check("send: DOORS is taken, nothing to stop, a send is due",
          wave_view_request_send(&v, "DOORS", &refused) == WAVE_DO_NOTHING && refused == 0 &&
              wave_view_next(&v) == WAVE_DO_SEND && strcmp(v.tx_text, "DOORS") == 0 && v.tx_copies == 1);
    check("send: a second send while one is queued is refused",
          (wave_view_request_send(&v, "again", &refused), refused == 1) && strcmp(v.tx_text, "DOORS") == 0);
    run_next(&v, 1000);
    wave_view_refresh(&v, "next one", 1000);
    check("send: sending, no microphone, hint SENDING", v.op == WAVE_OP_SEND && !v.mic_on &&
                                                       strcmp(v.status_hint, "SENDING") == 0);
    check("send: the chip says SENDING in the transmit look",
          strcmp(v.chip, "SENDING") == 0 && v.chip_kind == WAVE_CHIP_TX);
    check("send: starting the speaker", strcmp(v.status, "Starting the speaker") == 0);
    check("send: the button becomes STOP", strcmp(v.send_label, "STOP") == 0 && v.send_enabled && !v.send_primary);
    check("send: the next message can be typed meanwhile", v.can_edit);
    check("send: the preset is locked while sending", !v.can_pick_preset &&
                                                         !wave_view_set_preset(&v, WAVE_PRESET_QUICK));
    check("send: CAPTURE waits", !v.capture_enabled);
    apply(&v, WAVE_EV_RECEIVED, 0, "echo", 1000);
    check("send: a received event from a send helper is ignored", wave_history_count(&v.history) == 0);
    apply(&v, WAVE_EV_SENDING, 2600, NULL, 1000);
    wave_view_refresh(&v, "", 2300);
    check("send: progress in tenths", strcmp(v.status, "Sending, 1.3 of 2.6 s") == 0);
    wave_view_refresh(&v, "", 99999);
    check("send: progress never passes the total", strcmp(v.status, "Sending, 2.6 of 2.6 s") == 0);
    apply(&v, WAVE_EV_SENT, 0, NULL, 3600);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 3700);
    wave_view_refresh(&v, "", 3700);
    check("send: sent, in the ok tone", v.phase == WAVE_PHASE_IDLE && strcmp(v.status, "Sent") == 0 &&
                                            v.status_tone == WAVE_TONE_OK);
    check("send: nothing more is due", !v.send_pending && wave_view_next(&v) == WAVE_DO_NOTHING);
    check("send: the history has it as TX, ok, one copy",
          wave_history_count(&v.history) == 1 && newest(&v)->dir == WAVE_DIR_TX &&
              newest(&v)->result == WAVE_RESULT_OK && newest(&v)->count == 1 &&
              strcmp(newest(&v)->data, "DOORS") == 0);

    /* ROBUST: two copies, one helper each. */
    wave_view_set_preset(&v, WAVE_PRESET_ROBUST);
    wave_view_request_send(&v, "TWICE", &refused);
    run_next(&v, 5000);
    wave_view_refresh(&v, "", 5000);
    check("copies: the chip counts them", strcmp(v.chip, "SENDING 1/2") == 0);
    check("copies: the status too", strcmp(v.status, "Copy 1 of 2, starting the speaker") == 0);
    apply(&v, WAVE_EV_SENT, 0, NULL, 6000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 6001);
    check("copies: after the first, the second is due", v.send_pending && v.tx_done == 1 &&
                                                          wave_view_next(&v) == WAVE_DO_SEND);
    check("copies: nothing in the history yet", wave_history_count(&v.history) == 1);
    run_next(&v, 6002);
    wave_view_refresh(&v, "", 6002);
    check("copies: second copy", strcmp(v.chip, "SENDING 2/2") == 0);
    apply(&v, WAVE_EV_SENT, 0, NULL, 7000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 7001);
    wave_view_refresh(&v, "", 7001);
    check("copies: done, one history entry with two copies",
          !v.send_pending && wave_history_count(&v.history) == 2 && newest(&v)->count == 2 &&
              strcmp(newest(&v)->preset, "robust") == 0 && strcmp(v.status, "Sent, 2 copies") == 0);

    /* STOP between copies' sound. */
    wave_view_request_send(&v, "CUT", &refused);
    run_next(&v, 8000);
    apply(&v, WAVE_EV_SENT, 0, NULL, 8500);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 8501);
    run_next(&v, 8502);
    obey(&v, wave_view_request_stop(&v, 1758900009), 8600);
    wave_view_refresh(&v, "", 8600);
    check("send stopping: hint still SENDING, no microphone, STOPPING",
          strcmp(v.status_hint, "SENDING") == 0 && !v.mic_on && strcmp(v.chip, "STOPPING") == 0);
    apply(&v, WAVE_EV_EXITED, 128 + SIGTERM, NULL, 8700);
    wave_view_refresh(&v, "", 8700);
    check("send stopped by SIGTERM: stopped, not an error", strcmp(v.status, "Sending stopped") == 0 &&
                                                                v.status_tone == WAVE_TONE_MUTED);
    check("send stopped: no third copy", !v.send_pending && wave_view_next(&v) == WAVE_DO_NOTHING);
    check("send stopped: kept as STOPPED with the one copy that played",
          newest(&v)->result == WAVE_RESULT_STOPPED && newest(&v)->count == 1 &&
              strcmp(newest(&v)->data, "CUT") == 0);

    wave_view_set_preset(&v, WAVE_PRESET_STANDARD);
    wave_view_request_send(&v, "X", &refused);
    run_next(&v, 9000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 9100);
    wave_view_refresh(&v, "", 9100);
    check("send: exit 0 without 'sent' is not success",
          strcmp(v.status, "Sending did not finish") == 0 && v.status_tone == WAVE_TONE_ERROR &&
              newest(&v)->result == WAVE_RESULT_FAILED && newest(&v)->count == 0);
    check("send: the chip says ERROR", strcmp(v.chip, "ERROR") == 0);

    wave_view_request_send(&v, "Y", &refused);
    run_next(&v, 10000);
    check("send: a new run clears the old error", v.error[0] == '\0');
    apply(&v, WAVE_EV_ERROR, 0, "audio_disabled k230-t-display speaker playback is not validated", 10001);
    apply(&v, WAVE_EV_EXITED, 3, NULL, 10002);
    wave_view_refresh(&v, "", 10002);
    check("send: the helper's error, in words",
          strcmp(v.status, "The speaker is not enabled on this device yet") == 0 &&
              v.status_tone == WAVE_TONE_ERROR);

    wave_view_request_send(&v, "Z", &refused);
    run_next(&v, 11000);
    apply(&v, WAVE_EV_EXITED, 128 + SIGSEGV, NULL, 11001);
    wave_view_refresh(&v, "", 11001);
    check("send: a helper that crashed says so", strstr(v.status, "stopped unexpectedly (139)") != NULL);
    check("a new request clears a shown error",
          (wave_view_request_listen(&v), v.error[0] == '\0'));
    wave_view_request_listen(&v);

    wave_view_request_send(&v, "M", &refused);
    wave_view_start_failed(&v, wave_view_next(&v), "Sound is muted. Turn it on in Controls to send.", 5);
    wave_view_refresh(&v, "", 12000);
    check("start failure: idle with the reason, kept as FAILED",
          v.phase == WAVE_PHASE_IDLE && strstr(v.status, "muted") && !v.send_pending &&
              newest(&v)->result == WAVE_RESULT_FAILED && newest(&v)->when == 5);
    wave_view_start_failed(&v, WAVE_DO_LISTEN, NULL, 0);
    check("start failure without a reason still says something", strcmp(v.error, "Could not start") == 0);
}

/* The heart of the one-screen workflow: talk while listening. */
static void test_send_while_listening(void)
{
    struct wave_view v;
    int refused;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    wave_view_request_listen(&v);
    run_next(&v, 1000);
    apply(&v, WAVE_EV_LISTENING, 0, NULL, 1100);
    check("pause: a send while listening asks for the listen to stop",
          wave_view_request_send(&v, "HELLO", &refused) == WAVE_DO_STOP && refused == 0);
    wave_view_stopping(&v, 1200);
    wave_view_refresh(&v, "", 1200);
    check("pause: says it is pausing the microphone to send",
          strcmp(v.status, "Pausing the microphone to send") == 0 && v.mic_on);
    check("pause: the toggle stays on", v.listen_on && strcmp(v.listen_label, "STOP LISTEN") == 0);
    check("pause: SEND is not offered twice", !v.send_enabled);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 1300);
    wave_view_refresh(&v, "", 1300);
    check("pause: a paused listen says nothing about stopping", v.last == WAVE_LAST_NONE && !v.mic_on);
    check("pause: the send is due first", wave_view_next(&v) == WAVE_DO_SEND);
    run_next(&v, 1301);
    apply(&v, WAVE_EV_SENT, 0, NULL, 2000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 2001);
    check("resume: after the send, the listen resumes by itself", wave_view_next(&v) == WAVE_DO_LISTEN);
    run_next(&v, 2002);
    wave_view_refresh(&v, "", 2002);
    check("resume: listening again, microphone indicator back on", v.op == WAVE_OP_LISTEN && v.mic_on);
    check("resume: the send is in the history", newest(&v)->dir == WAVE_DIR_TX &&
                                                   strcmp(newest(&v)->data, "HELLO") == 0);

    /* Many round trips, no drift. */
    {
        int i;
        int ok = 1;

        for (i = 0; i < 20; i++) {
            ok &= wave_view_request_send(&v, "again", &refused) == WAVE_DO_STOP;
            wave_view_stopping(&v, 3000 + i * 100);
            apply(&v, WAVE_EV_EXITED, 0, NULL, 3001 + i * 100);
            ok &= run_next(&v, 3002 + i * 100) == WAVE_DO_SEND;
            apply(&v, WAVE_EV_SENT, 0, NULL, 3050 + i * 100);
            apply(&v, WAVE_EV_EXITED, 0, NULL, 3051 + i * 100);
            ok &= run_next(&v, 3052 + i * 100) == WAVE_DO_LISTEN;
        }
        check("twenty send/listen round trips, each one pause, one send, one resume", ok);
    }

    /* A STOP while the listen is pausing for the send drops the send too. */
    wave_view_request_send(&v, "never", &refused);
    wave_view_stopping(&v, 9000);
    wave_view_request_stop(&v, 77);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 9100);
    check("STOP while pausing: nothing more runs, the text is kept as stopped",
          wave_view_next(&v) == WAVE_DO_NOTHING && !v.listen_on &&
              newest(&v)->result == WAVE_RESULT_STOPPED && newest(&v)->count == 0 &&
              strcmp(newest(&v)->data, "never") == 0);

    /* The toggle turned off while a send runs: no resume. */
    wave_view_request_listen(&v);
    run_next(&v, 10000);
    wave_view_request_send(&v, "bye", &refused);
    wave_view_stopping(&v, 10001);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 10002);
    run_next(&v, 10003);
    wave_view_request_listen(&v);
    apply(&v, WAVE_EV_SENT, 0, NULL, 10100);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 10101);
    check("a listen turned off during a send does not resume", wave_view_next(&v) == WAVE_DO_NOTHING);

    /* A send failure while the toggle is on: nothing loops. */
    wave_view_request_listen(&v);
    run_next(&v, 11000);
    wave_view_request_send(&v, "fail", &refused);
    wave_view_stopping(&v, 11001);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 11002);
    run_next(&v, 11003);
    apply(&v, WAVE_EV_ERROR, 0, "audio_busy another stream is open", 11004);
    apply(&v, WAVE_EV_EXITED, 3, NULL, 11005);
    wave_view_refresh(&v, "", 11005);
    check("a failed send turns listening off rather than retrying anything",
          wave_view_next(&v) == WAVE_DO_NOTHING && !v.listen_on &&
              strcmp(v.status, "Audio is in use by another program") == 0);

    /* A listen that fails: the microphone is not reopened. */
    wave_view_request_listen(&v);
    run_next(&v, 12000);
    apply(&v, WAVE_EV_ERROR, 0, "audio_nodev cannot open hw", 12001);
    apply(&v, WAVE_EV_EXITED, 3, NULL, 12002);
    wave_view_refresh(&v, "", 12002);
    check("a failed listen is not restarted", wave_view_next(&v) == WAVE_DO_NOTHING && !v.listen_on &&
                                                  strcmp(v.status, "No audio device found") == 0);
    check("and its indicator is off", !v.mic_on && v.status_hint[0] == '\0');

    /* A listen that fails while a send waits for it: the send still goes. */
    wave_view_request_listen(&v);
    run_next(&v, 13000);
    wave_view_request_send(&v, "still", &refused);
    wave_view_stopping(&v, 13001);
    apply(&v, WAVE_EV_EXITED, 128 + SIGSEGV, NULL, 13002);
    check("a microphone failure does not drop a queued send", wave_view_next(&v) == WAVE_DO_SEND &&
                                                                 !v.listen_on);
}

static void test_capture(void)
{
    struct wave_view v;
    int refused;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    check("capture: requested when idle, nothing to stop",
          wave_view_request_capture(&v) == WAVE_DO_NOTHING && wave_view_next(&v) == WAVE_DO_CAPTURE);
    run_next(&v, 1000);
    wave_view_refresh(&v, "hi", 1000);
    check("capture: the microphone is on from the start", v.mic_on && strcmp(v.status_hint, "MIC ON") == 0 &&
                                                            strcmp(v.chip, "CAPTURING") == 0);
    check("capture: the button finishes it early", strcmp(v.capture_label, "DECODE NOW") == 0 &&
                                                        v.capture_enabled);
    check("capture: SEND waits", !v.send_enabled);
    wave_view_refresh(&v, "", 4500);
    check("capture: counts its seconds", strcmp(v.status, "Microphone on, recording 3 of 10 s") == 0);
    wave_view_refresh(&v, "", 99000);
    check("capture: never counts past its length", strcmp(v.status, "Microphone on, recording 10 of 10 s") == 0);
    apply(&v, WAVE_EV_RECEIVED, 0, "not while recording", 2000);
    check("capture: nothing is decoded while recording", wave_history_count(&v.history) == 0);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 11000);
    wave_view_refresh(&v, "", 11000);
    check("capture: then the decode is due, microphone off",
          wave_view_next(&v) == WAVE_DO_DECODE && !v.mic_on);
    run_next(&v, 11001);
    wave_view_refresh(&v, "", 11001);
    check("decode: says so, no microphone", strcmp(v.chip, "DECODING") == 0 && !v.mic_on &&
                                                 strcmp(v.status, "Decoding the recording") == 0);
    apply(&v, WAVE_EV_RECEIVED, 0, "DOORS", 11500);
    apply(&v, WAVE_EV_RECEIVED, 0, "SECOND", 11600);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 11700);
    wave_view_refresh(&v, "", 11700);
    check("decode: the messages are in the history, marked as from a capture",
          wave_history_count(&v.history) == 2 && newest(&v)->captured &&
              strcmp(newest(&v)->data, "SECOND") == 0);
    check("decode: the status counts them", strcmp(v.status, "Decoded 2 messages from the recording") == 0 &&
                                                v.status_tone == WAVE_TONE_OK);

    wave_view_request_capture(&v);
    run_next(&v, 20000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 30000);
    run_next(&v, 30001);
    apply(&v, WAVE_EV_MISSED, 0, NULL, 30100);
    apply(&v, WAVE_EV_EXITED, WAVE_EXIT_FAILED, NULL, 30200);
    wave_view_refresh(&v, "", 30200);
    check("decode failure: exit 1 is 'nothing decoded', not an error",
          strcmp(v.status, "Nothing decoded in the recording") == 0 && v.error[0] == '\0');
    check("decode failure: kept as an undecoded capture",
          newest(&v)->result == WAVE_RESULT_UNDECODED && newest(&v)->captured);

    wave_view_request_capture(&v);
    run_next(&v, 40000);
    obey(&v, wave_view_request_capture(&v), 41000);
    wave_view_refresh(&v, "", 41000);
    check("finish early: says it is finishing the recording",
          strcmp(v.status, "Finishing the recording") == 0 && v.mic_on);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 41100);
    check("finish early: what was recorded is decoded", wave_view_next(&v) == WAVE_DO_DECODE);
    run_next(&v, 41101);
    obey(&v, wave_view_request_stop(&v, 0), 41200);
    apply(&v, WAVE_EV_EXITED, 128 + SIGTERM, NULL, 41300);
    wave_view_refresh(&v, "", 41300);
    check("a stopped decode adds nothing", newest(&v)->result == WAVE_RESULT_UNDECODED &&
                                               wave_view_next(&v) == WAVE_DO_NOTHING);

    wave_view_request_capture(&v);
    run_next(&v, 50000);
    obey(&v, wave_view_request_stop(&v, 0), 50100);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 50200);
    check("STOP during a capture throws the recording away",
          wave_view_next(&v) == WAVE_DO_NOTHING && !v.decode_pending);

    wave_view_request_capture(&v);
    run_next(&v, 51000);
    apply(&v, WAVE_EV_ERROR, 0, "audio_busy another stream", 51001);
    apply(&v, WAVE_EV_EXITED, 3, NULL, 51002);
    check("a capture that fails decodes nothing", wave_view_next(&v) == WAVE_DO_NOTHING);

    wave_view_request_capture(&v);
    run_next(&v, 52000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 52001);
    run_next(&v, 52002);
    apply(&v, WAVE_EV_ERROR, 0, "usage cannot read the file", 52003);
    apply(&v, WAVE_EV_EXITED, 2, NULL, 52004);
    wave_view_refresh(&v, "", 52004);
    check("a decode that fails says so", strcmp(v.status, "Wave helper refused the request") == 0);
    wave_view_request_capture(&v);
    run_next(&v, 53000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 53001);
    run_next(&v, 53002);
    apply(&v, WAVE_EV_EXITED, 128 + SIGSEGV, NULL, 53003);
    wave_view_refresh(&v, "", 53003);
    check("a decode that crashes says so rather than 'nothing decoded'",
          strstr(v.status, "stopped unexpectedly") != NULL);

    /* Capture while listening: pause, capture, decode, resume. */
    wave_view_request_listen(&v);
    run_next(&v, 60000);
    check("capture while listening asks for the listen to stop", wave_view_request_capture(&v) == WAVE_DO_STOP);
    wave_view_stopping(&v, 60001);
    wave_view_refresh(&v, "", 60001);
    check("and says why", strcmp(v.status, "Pausing the microphone to capture") == 0);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 60002);
    check("the capture comes next", run_next(&v, 60003) == WAVE_DO_CAPTURE);
    check("no send while it runs", (wave_view_request_send(&v, "x", &refused), refused == 1));
    check("no second capture", wave_view_request_capture(&v) == WAVE_DO_STOP); /* = decode now */
    apply(&v, WAVE_EV_EXITED, 0, NULL, 70000);
    check("then its decode", run_next(&v, 70001) == WAVE_DO_DECODE);
    apply(&v, WAVE_EV_EXITED, 1, NULL, 70002);
    check("then listening resumes", run_next(&v, 70003) == WAVE_DO_LISTEN && v.listen_on);
}

static void test_clear(void)
{
    struct wave_view v;

    wave_view_init(&v, WAVE_PRESET_DEFAULT);
    check("clear: nothing to clear does nothing", wave_view_request_clear(&v, 100) == 0 && !v.clear_armed_ms);
    wave_history_add_tx(&v.history, "standard", "a", 1, WAVE_RESULT_OK, 1, 0);
    check("clear: the first tap only arms", wave_view_request_clear(&v, 1000) == 0 &&
                                                wave_history_count(&v.history) == 1);
    wave_view_refresh(&v, "", 1000);
    check("clear: the button asks to confirm", strcmp(v.clear_label, "CONFIRM") == 0);
    wave_view_refresh(&v, "", 1000 + WAVE_VIEW_CLEAR_ARM_MS);
    check("clear: and forgets after a while", strcmp(v.clear_label, "CLEAR") == 0);
    check("clear: a late second tap arms again", wave_view_request_clear(&v, 1000 + WAVE_VIEW_CLEAR_ARM_MS) == 0);
    check("clear: a prompt second tap clears",
          wave_view_request_clear(&v, 2000 + WAVE_VIEW_CLEAR_ARM_MS) == 1 && wave_history_count(&v.history) == 0);
    wave_view_refresh(&v, "", 2000 + WAVE_VIEW_CLEAR_ARM_MS);
    check("clear: disabled again", !v.clear_enabled && strcmp(v.clear_label, "CLEAR") == 0);
}

static void test_entries(void)
{
    struct wave_history h;
    char meta[96];
    char text[160];
    int64_t now;

    /* 2026-09-26 12:34 UTC; TZ=UTC in make test. */
    now = 1790426040;
    wave_history_init(&h);
    wave_history_add_rx(&h, "standard", "DOORS", 5, 0, now, 1, 10000);
    wave_history_add_rx(&h, "standard", "DOORS", 5, 0, now, 2, 10000);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: RX, time, preset, repeat count", strcmp(meta, "RX 12:34  STANDARD  x2") == 0 &&
                                                        strcmp(text, "DOORS") == 0);
    wave_history_add_tx(&h, "robust", "HI", 2, WAVE_RESULT_OK, 2, now - 86400);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: another day shows the date", strcmp(meta, "TX 09-25 12:34  ROBUST  x2") == 0);
    wave_history_add_tx(&h, "quick", "no", 2, WAVE_RESULT_FAILED, 0, 0);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: an unknown time, and a failure said", strcmp(meta, "TX --:--  QUICK  FAILED") == 0 &&
                                                            strcmp(text, "no") == 0);
    wave_history_add_tx(&h, "standard", "s", 1, WAVE_RESULT_STOPPED, 0, now);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: a stopped send", strstr(meta, "STOPPED") != NULL);
    wave_history_add_undecoded(&h, "robust", now);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: an undecoded capture", strcmp(meta, "RX 12:34  ROBUST  CAPTURE") == 0 &&
                                             strcmp(text, "Nothing decoded in the recording") == 0);
    wave_history_add_rx(&h, "gone", "\x01\x02", 2, 1, now, 1000000, 0);
    wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
    check("entry: a preset that no longer exists shows its id; binary as hex",
          strcmp(meta, "RX 12:34  gone  CAPTURE") == 0 && strcmp(text, "hex: 01 02") == 0);
    {
        char big[WAVE_HISTORY_DATA_MAX + 4];

        memset(big, 'z', sizeof(big));
        wave_history_add_rx(&h, "standard", big, sizeof(big), 0, now, 2000000, 0);
        wave_view_format_entry(wave_history_at(&h, 0), now, meta, sizeof(meta), text, sizeof(text));
        check("entry: a cut message says so", strlen(text) == WAVE_HISTORY_DATA_MAX + 3 &&
                                                  strcmp(text + WAVE_HISTORY_DATA_MAX, "...") == 0);
    }
}

static void test_words(void)
{
    char out[128];
    char shown[2 * WAVE_EVENT_TEXT_MAX + 16];
    int hex;

    wave_view_error_text("audio_busy another stream is open", out, sizeof(out));
    check("error: busy", strcmp(out, "Audio is in use by another program") == 0);
    wave_view_error_text("audio_nodev cannot open hw", out, sizeof(out));
    check("error: no device", strcmp(out, "No audio device found") == 0);
    wave_view_error_text("audio playback failed: EIO", out, sizeof(out));
    check("error: generic audio", strcmp(out, "Audio device error") == 0);
    wave_view_error_text("too_long", out, sizeof(out));
    check("error: too long, bare word", strcmp(out, "Message is too long") == 0);
    wave_view_error_text("invalid_text x", out, sizeof(out));
    check("error: invalid text", strcmp(out, "Only plain text can be sent") == 0);
    wave_view_error_text("encode ggwave refused", out, sizeof(out));
    check("error: encode", strcmp(out, "Message could not be encoded") == 0);
    wave_view_error_text("exec /usr/bin/pos-wave: No such file", out, sizeof(out));
    check("error: helper missing", strcmp(out, "Wave helper is not installed") == 0);
    wave_view_error_text("protocol helper line too long", out, sizeof(out));
    check("error: protocol", strcmp(out, "Wave helper sent something unexpected") == 0);
    wave_view_error_text("audio_busyness x", out, sizeof(out));
    check("error: a word is matched whole", strcmp(out, "Error: audio_busyness x") == 0);
    wave_view_error_text(NULL, out, sizeof(out));
    check("error: NULL is survivable", strcmp(out, "Error: ") == 0);

    hex = wave_view_format_payload("Hei \xC3\xA5", 6, shown, sizeof(shown));
    check("payload: clean UTF-8 is shown as text", !hex && strcmp(shown, "Hei \xC3\xA5") == 0);
    hex = wave_view_format_payload("A\0\xff", 3, shown, sizeof(shown));
    check("payload: bytes that are not text are shown as hex", hex && strcmp(shown, "hex: 41 00 ff") == 0);
    hex = wave_view_format_payload("", 0, shown, sizeof(shown));
    check("payload: an empty payload is shown as empty hex", hex && strcmp(shown, "hex:") == 0);
    {
        char big[WAVE_EVENT_TEXT_MAX];
        char small[40];

        memset(big, 0x01, sizeof(big));
        hex = wave_view_format_payload(big, sizeof(big), shown, sizeof(shown));
        check("payload: a long binary payload is cut, never overflowed",
              hex && strlen(shown) < sizeof(shown) && strncmp(shown, "hex: 01 01", 10) == 0);
        hex = wave_view_format_payload("0123456789012345678901234567890123456789", 40, small,
                                       sizeof(small));
        check("payload: text longer than the buffer is shown as cut hex, never overflowed",
              hex && strlen(small) < sizeof(small));
    }
}

int main(void)
{
    test_messages();
    test_idle();
    test_listen();
    test_send();
    test_send_while_listening();
    test_capture();
    test_clear();
    test_entries();
    test_words();
    printf("wave_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
