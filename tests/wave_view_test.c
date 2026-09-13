/*
 * Wave's view model: message rules, the action button in every phase, the
 * microphone indicator across the gaps where the device may be open, the
 * words for every helper error and exit, and how received bytes are shown.
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

    wave_view_apply(v, &e, now);
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

    wave_view_init(&v);
    check("init: send mode, idle, fast profile",
          v.mode == WAVE_MODE_SEND && v.phase == WAVE_PHASE_IDLE && v.profile == WAVE_PROFILE_FAST);
    check("init: TRANSMIT, disabled with nothing typed",
          strcmp(v.action_label, "TRANSMIT") == 0 && !v.action_enabled && v.action_primary);
    check("init: ready, microphone off, no hint",
          strcmp(v.status, "Ready to send") == 0 && !v.mic_on && v.status_hint[0] == '\0');
    wave_view_refresh(&v, "DOORS", 0);
    check("idle send: TRANSMIT enabled once there is a message", v.action_enabled);
    check("idle send: counter", strcmp(v.counter, "5 / 64 bytes") == 0 && v.counter_tone == WAVE_TONE_MUTED);
    check("idle send: the action is send", wave_view_action(&v, "DOORS") == WAVE_DO_SEND);
    check("idle send: nothing to send is nothing", wave_view_action(&v, "") == WAVE_DO_NOTHING);
    wave_view_refresh(&v, "a\tb", 0);
    check("idle send: an invalid message disables TRANSMIT and marks the counter",
          !v.action_enabled && v.counter_tone == WAVE_TONE_ERROR);
    check("idle: everything is editable", v.can_switch_mode && v.can_pick_profile && v.can_edit);

    check("mode: switch to receive", wave_view_set_mode(&v, WAVE_MODE_RECEIVE) && v.mode == WAVE_MODE_RECEIVE);
    wave_view_refresh(&v, "", 0);
    check("idle receive: START LISTENING, enabled, accent",
          strcmp(v.action_label, "START LISTENING") == 0 && v.action_enabled && v.action_primary);
    check("idle receive: says the microphone is off", strcmp(v.status, "Microphone off") == 0);
    check("idle receive: the action is listen", wave_view_action(&v, "") == WAVE_DO_LISTEN);
    check("mode: an unknown mode is refused", !wave_view_set_mode(&v, (enum wave_mode)7));
    check("profile: pick normal", wave_view_set_profile(&v, WAVE_PROFILE_NORMAL) && v.profile == WAVE_PROFILE_NORMAL);
    check("profile: out of range refused", !wave_view_set_profile(&v, WAVE_PROFILE_COUNT));
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

    wave_view_init(&v);
    wave_view_set_mode(&v, WAVE_MODE_RECEIVE);
    wave_view_started(&v, 1000);
    wave_view_refresh(&v, "", 1000);
    check("listen: the microphone indicator is on before the helper says listening",
          v.phase == WAVE_PHASE_LISTENING && v.mic_on && strcmp(v.status_hint, "MIC ON") == 0);
    check("listen: status warns while starting",
          strcmp(v.status, "Microphone on, starting") == 0 && v.status_tone == WAVE_TONE_WARN);
    check("listen: the button stops, without the accent",
          strcmp(v.action_label, "STOP LISTENING") == 0 && v.action_enabled && !v.action_primary);
    check("listen: the action is stop", wave_view_action(&v, "") == WAVE_DO_STOP);
    check("listen: mode and profile are locked", !wave_view_set_mode(&v, WAVE_MODE_SEND) &&
                                                     !wave_view_set_profile(&v, WAVE_PROFILE_NORMAL));
    wave_view_refresh(&v, "", 1000);
    check("listen: nothing is editable", !v.can_switch_mode && !v.can_pick_profile && !v.can_edit);

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
    check("listen: a message is kept", v.received_count == 1 && strcmp(v.received[0].shown, "DOORS") == 0 &&
                                           !v.received[0].is_hex);

    wave_view_stopping(&v, 1400);
    wave_view_refresh(&v, "", 1400);
    check("stopping: the microphone indicator stays on until the helper is gone",
          v.phase == WAVE_PHASE_STOPPING && v.mic_on && strcmp(v.status_hint, "MIC ON") == 0);
    check("stopping: the button is disabled", strcmp(v.action_label, "STOPPING") == 0 && !v.action_enabled);
    check("stopping: pressing it does nothing", wave_view_action(&v, "") == WAVE_DO_NOTHING);
    apply(&v, WAVE_EV_STOPPED, 0, NULL, 1450);
    wave_view_refresh(&v, "", 1450);
    check("stopping: 'stopped' alone does not turn the indicator off", v.mic_on);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 1500);
    wave_view_refresh(&v, "", 1500);
    check("stopped: idle, microphone off, no hint",
          v.phase == WAVE_PHASE_IDLE && !v.mic_on && v.status_hint[0] == '\0');
    check("stopped: says so, not as an error", strcmp(v.status, "Stopped") == 0 && v.status_tone == WAVE_TONE_MUTED);
    check("stopped: the message is still on screen", v.received_count == 1);
    apply(&v, WAVE_EV_LEVEL, 99, NULL, 1600);
    apply(&v, WAVE_EV_MISSED, 0, NULL, 1600);
    check("an event for no running helper changes nothing", v.level == -1 && v.missed_at_ms == 0);

    wave_view_started(&v, 2000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 122000);
    wave_view_refresh(&v, "", 122000);
    check("listen: running out of time ends quietly", strcmp(v.status, "Listening ended after 120 s") == 0 &&
                                                           !v.mic_on);

    wave_view_started(&v, 3000);
    wave_view_stopping(&v, 3100);
    apply(&v, WAVE_EV_EXITED, 128 + SIGKILL, NULL, 4200);
    wave_view_refresh(&v, "", 4200);
    check("stop: a helper that had to be killed is still just stopped", strcmp(v.status, "Stopped") == 0);

    apply(&v, WAVE_EV_RECEIVED, 0, "late", 4300);
    check("a message after the helper exited is not kept", v.received_count == 1);
    wave_view_started(&v, 4400);
    {
        int i;
        char msg[8];

        for (i = 0; i < 7; i++) {
            snprintf(msg, sizeof(msg), "m%d", i);
            apply(&v, WAVE_EV_RECEIVED, 0, msg, 5000 + i);
        }
        check("received: only the last five are kept, newest first",
              v.received_count == WAVE_VIEW_KEEP && strcmp(v.received[0].shown, "m6") == 0 &&
                  strcmp(v.received[4].shown, "m2") == 0);
    }
}

static void test_send(void)
{
    struct wave_view v;

    wave_view_init(&v);
    wave_view_started(&v, 1000);
    wave_view_refresh(&v, "DOORS", 1000);
    check("send: sending phase, no microphone", v.phase == WAVE_PHASE_SENDING && !v.mic_on);
    check("send: the hint says sending", strcmp(v.status_hint, "SENDING") == 0);
    check("send: starting the speaker", strcmp(v.status, "Starting the speaker") == 0);
    check("send: the button stops", strcmp(v.action_label, "STOP") == 0 && v.action_enabled &&
                                         wave_view_action(&v, "DOORS") == WAVE_DO_STOP);
    apply(&v, WAVE_EV_RECEIVED, 0, "echo", 1000);
    check("send: a received event from a send helper is ignored", v.received_count == 0);
    apply(&v, WAVE_EV_SENDING, 2600, NULL, 1000);
    wave_view_refresh(&v, "DOORS", 2300);
    check("send: progress in tenths", strcmp(v.status, "Sending, 1.3 of 2.6 s") == 0);
    wave_view_refresh(&v, "DOORS", 99999);
    check("send: progress never passes the total", strcmp(v.status, "Sending, 2.6 of 2.6 s") == 0);
    apply(&v, WAVE_EV_SENT, 0, NULL, 3600);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 3700);
    wave_view_refresh(&v, "DOORS", 3700);
    check("send: sent, in the ok tone", v.phase == WAVE_PHASE_IDLE && strcmp(v.status, "Sent") == 0 &&
                                            v.status_tone == WAVE_TONE_OK);
    check("send: the hint is gone", v.status_hint[0] == '\0');

    wave_view_started(&v, 5000);
    apply(&v, WAVE_EV_EXITED, 0, NULL, 5100);
    wave_view_refresh(&v, "DOORS", 5100);
    check("send: exit 0 without 'sent' is not success",
          strcmp(v.status, "Sending did not finish") == 0 && v.status_tone == WAVE_TONE_ERROR);

    wave_view_started(&v, 6000);
    check("send: a new run clears the old error", v.error[0] == '\0');
    apply(&v, WAVE_EV_ERROR, 0, "audio_disabled k230-t-display speaker playback is not validated", 6001);
    apply(&v, WAVE_EV_EXITED, 3, NULL, 6002);
    wave_view_refresh(&v, "DOORS", 6002);
    check("send: the helper's error, in words, and not a crash report",
          strcmp(v.status, "Audio is not enabled on this device yet") == 0 &&
              v.status_tone == WAVE_TONE_ERROR);

    wave_view_started(&v, 7000);
    apply(&v, WAVE_EV_EXITED, 128 + SIGSEGV, NULL, 7001);
    wave_view_refresh(&v, "DOORS", 7001);
    check("send: a helper that crashed says so", strstr(v.status, "stopped unexpectedly (139)") != NULL);
    check("mode switch clears a shown error", wave_view_set_mode(&v, WAVE_MODE_RECEIVE) && v.error[0] == '\0');
    wave_view_set_mode(&v, WAVE_MODE_SEND);

    wave_view_started(&v, 8000);
    wave_view_stopping(&v, 8100);
    wave_view_refresh(&v, "DOORS", 8100);
    check("send stopping: hint still says sending, no microphone", strcmp(v.status_hint, "SENDING") == 0 &&
                                                                     !v.mic_on);
    apply(&v, WAVE_EV_EXITED, 128 + SIGTERM, NULL, 8200);
    wave_view_refresh(&v, "DOORS", 8200);
    check("send stopped by SIGTERM is stopped", strcmp(v.status, "Stopped") == 0);

    wave_view_start_failed(&v, "fork: out of memory");
    wave_view_refresh(&v, "DOORS", 9000);
    check("start failure: idle with the reason", v.phase == WAVE_PHASE_IDLE &&
                                                      strcmp(v.status, "fork: out of memory") == 0);
    wave_view_start_failed(&v, NULL);
    check("start failure without a reason still says something", strcmp(v.error, "Could not start") == 0);
}

static void test_words(void)
{
    char out[128];
    struct wave_received r;

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

    wave_view_format_received("Hei \xC3\xA5", 6, &r);
    check("received: clean UTF-8 is shown as text", !r.is_hex && strcmp(r.shown, "Hei \xC3\xA5") == 0);
    wave_view_format_received("A\0\xff", 3, &r);
    check("received: bytes that are not text are shown as hex", r.is_hex && strcmp(r.shown, "hex: 41 00 ff") == 0);
    wave_view_format_received("", 0, &r);
    check("received: an empty payload is shown as empty hex", r.is_hex && strcmp(r.shown, "hex:") == 0);
    {
        char big[WAVE_EVENT_TEXT_MAX];

        memset(big, 0x01, sizeof(big));
        wave_view_format_received(big, sizeof(big), &r);
        check("received: a long binary payload is cut, never overflowed",
              r.is_hex && strlen(r.shown) < sizeof(r.shown) && strncmp(r.shown, "hex: 01 01", 10) == 0);
    }
}

int main(void)
{
    test_messages();
    test_idle();
    test_listen();
    test_send();
    test_words();
    printf("wave_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
