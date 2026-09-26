/*
 * Wave's host simulator: two Waves and the air between them, in memory.
 *
 *   sender model (wave_view)  --request_send/next-->  the real modem encodes
 *   the preset's copies, with the helper's lead-in and tail silence
 *     --> the simulated air (tests/wave_channel.c): gain, noise, hum, DC,
 *         clipping, lost samples, a late start
 *     --> the real modem decodes, in capture-sized chunks, live or from a
 *         capture buffer
 *     --> each result as the helper's own event line ("received <hex>",
 *         "missed"), parsed by the real session parser
 *     --> the receiver's model (wave_view_apply): history, folding, status.
 *
 * Nothing here predicts what the decoder "should" return and feeds that in:
 * every message on the receiving side came out of ggwave, through the same
 * text protocol the app reads from pos-wave. What is simulated is only what
 * has no host equivalent: the speaker, the room and the microphone (and the
 * process boundary, whose own tests are wave_session_test and wave_ctl_test).
 *
 * Covered: a normal message on every preset; empty and invalid input; noise,
 * hum, DC, gain and dropouts on a message; clipping and heavy noise (which
 * may lose a message but must never invent or corrupt one); garbage; a
 * transmission cut off halfway and the recovery after it; repeated messages
 * and folding; a capture that decodes and one that decodes nothing; the
 * longest message on every speed; and preset mismatch between the two ends.
 *
 * Needs ggwave (links wave_modem.o), like tests/wave_modem_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_channel.h"
#include "wave_modem.h"
#include "wave_session.h"
#include "wave_view.h"

#include <stdio.h>
#include <stdlib.h>
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

/* pos-wave send's silence around a transmission, and the time between two
 * copies (one helper exits, the next opens the device). */
#define LEAD_IN_SAMPLES (WAVE_MODEM_RATE / 10)
#define TAIL_SAMPLES (WAVE_MODEM_RATE * 15 / 100)
#define COPY_GAP_SAMPLES (WAVE_MODEM_RATE * 3 / 10)
/* The capture period pocketaudio reads in (20 ms). */
#define CHUNK 960

struct receiver {
    struct wave_view view;
    struct wave_decoder *dec;
    int16_t *capture;       /* WAVE_CAPTURE_MAX_SECONDS of samples */
    size_t captured;
    int decoded;            /* ggwave results this run, by kind */
    int missed;
    int wrong;              /* decoded bytes that were not the expected text */
    const char *expect;     /* what may legitimately be heard, NULL for anything */
};

struct sim {
    struct wave_channel air;
    uint64_t pos;           /* samples since the start, for the clock and the hum */
    struct receiver rx;
    struct wave_encoder *enc;
};

static struct sim sim;

static int64_t now_ms(void)
{
    return (int64_t)(sim.pos * 1000 / WAVE_MODEM_RATE);
}

static int64_t wall(void)
{
    return 1790426040 + now_ms() / 1000;
}

/* One helper event line into a model, through the real parser. */
static void deliver(struct wave_view *v, const char *line)
{
    struct wave_event ev;

    if (wave_session_parse_line(line, &ev)) {
        wave_view_apply(v, &ev, now_ms(), wall());
    } else {
        printf("     the parser refused \"%s\"\n", line);
        failed++;
    }
}

static void exited(struct wave_view *v, int code)
{
    struct wave_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = WAVE_EV_EXITED;
    ev.value = code;
    wave_view_apply(v, &ev, now_ms(), wall());
}

/* The decoder's result as pos-wave prints it (print_received, on_decode). */
static void on_decode(void *user, enum wave_decode_kind kind, const uint8_t *data, size_t len)
{
    struct receiver *r = user;
    char line[16 + 2 * WAVE_MAX_MESSAGE_BYTES];
    size_t i;

    if (kind != WAVE_DECODED) {
        r->missed++;
        deliver(&r->view, "missed");
        return;
    }
    r->decoded++;
    if (r->expect && (len != strlen(r->expect) || memcmp(data, r->expect, len) != 0)) {
        r->wrong++;
    }
    snprintf(line, sizeof(line), "received ");
    for (i = 0; i < len && i < WAVE_MAX_MESSAGE_BYTES; i++) {
        snprintf(line + 9 + 2 * i, 3, "%02x", data[i]);
    }
    deliver(&r->view, line);
}

/* Samples reaching the receiver's microphone: live-decoded while it
 * listens, kept while it captures, lost otherwise. */
static void hear(const int16_t *samples, size_t n)
{
    struct receiver *r = &sim.rx;
    int16_t buf[CHUNK];
    size_t off = 0;

    while (off < n) {
        size_t k = n - off < CHUNK ? n - off : CHUNK;

        if (samples) {
            wave_channel_apply(&sim.air, samples + off, buf, k, sim.pos);
        } else {
            wave_channel_room(&sim.air, buf, k, sim.pos);
        }
        sim.pos += k;
        if (r->view.op == WAVE_OP_LISTEN && r->view.phase == WAVE_PHASE_RUNNING) {
            wave_decoder_feed(r->dec, buf, k, on_decode, r);
        } else if (r->view.op == WAVE_OP_CAPTURE && r->view.phase == WAVE_PHASE_RUNNING) {
            size_t room = (size_t)WAVE_CAPTURE_MAX_SECONDS * WAVE_MODEM_RATE - r->captured;
            size_t keep = k < room ? k : room;

            memcpy(r->capture + r->captured, buf, keep * sizeof(int16_t));
            r->captured += keep;
        }
        off += k;
    }
}

static void quiet(double seconds)
{
    hear(NULL, (size_t)(seconds * WAVE_MODEM_RATE));
}

/* ---- the receiving Wave --------------------------------------------------- */

static void rx_reset(int preset, const char *expect)
{
    struct receiver *r = &sim.rx;
    char err[128];

    wave_decoder_free(r->dec);
    wave_view_init(&r->view, preset);
    r->dec = wave_decoder_new(err, sizeof(err));
    r->captured = 0;
    r->decoded = r->missed = r->wrong = 0;
    r->expect = expect;
}

static void rx_listen(void)
{
    struct receiver *r = &sim.rx;

    wave_view_request_listen(&r->view);
    if (wave_view_next(&r->view) == WAVE_DO_LISTEN) {
        wave_view_started(&r->view, WAVE_DO_LISTEN, now_ms());
        deliver(&r->view, "ready sim");
        deliver(&r->view, "listening");
    }
}

static void rx_capture(void)
{
    struct receiver *r = &sim.rx;

    wave_view_request_capture(&r->view);
    if (wave_view_next(&r->view) == WAVE_DO_CAPTURE) {
        wave_view_started(&r->view, WAVE_DO_CAPTURE, now_ms());
        r->captured = 0;
        deliver(&r->view, "listening");
    }
}

/* The capture ends; its decode runs over the recording the way pos-wave
 * decode does: a fresh decoder, the whole file, half a second of silence. */
static void rx_capture_done(void)
{
    struct receiver *r = &sim.rx;
    struct wave_decoder *d;
    static const int16_t silence[CHUNK];
    char err[128];
    size_t off;
    int i;
    int before = r->decoded;

    exited(&r->view, 0);
    if (wave_view_next(&r->view) != WAVE_DO_DECODE) {
        return;
    }
    wave_view_started(&r->view, WAVE_DO_DECODE, now_ms());
    d = wave_decoder_new(err, sizeof(err));
    for (off = 0; d && off < r->captured; off += CHUNK) {
        size_t k = r->captured - off < CHUNK ? r->captured - off : CHUNK;

        wave_decoder_feed(d, r->capture + off, k, on_decode, r);
    }
    for (i = 0; d && i < 25; i++) {
        wave_decoder_feed(d, silence, CHUNK, on_decode, r);
    }
    wave_decoder_free(d);
    exited(&r->view, r->decoded > before ? WAVE_EXIT_OK : WAVE_EXIT_FAILED);
}

/* ---- the sending Wave ----------------------------------------------------- */

/* Send text from a Wave on preset through the air; returns copies played. */
static int tx(int preset, const char *text)
{
    struct wave_view sender;
    int refused = 1;
    int copies = 0;

    wave_view_init(&sender, preset);
    wave_view_request_send(&sender, text, &refused);
    if (refused) {
        return -1;
    }
    while (wave_view_next(&sender) == WAVE_DO_SEND) {
        const struct wave_preset *p = wave_preset_get(sender.tx_preset);
        const int16_t *s;
        char err[128];
        char line[32];
        long n;

        wave_view_started(&sender, WAVE_DO_SEND, now_ms());
        n = wave_encoder_encode(sim.enc, (const uint8_t *)sender.tx_text, sender.tx_len,
                                p->profile, WAVE_DEFAULT_VOLUME, &s, err, sizeof(err));
        if (n <= 0) {
            deliver(&sender, "error encode ggwave refused the message");
            exited(&sender, WAVE_EXIT_FAILED);
            break;
        }
        snprintf(line, sizeof(line), "sending %ld", n * 1000 / WAVE_MODEM_RATE);
        deliver(&sender, line);
        if (copies > 0) {
            quiet((double)COPY_GAP_SAMPLES / WAVE_MODEM_RATE);
        }
        quiet((double)LEAD_IN_SAMPLES / WAVE_MODEM_RATE);
        hear(s, (size_t)n);
        quiet((double)TAIL_SAMPLES / WAVE_MODEM_RATE);
        deliver(&sender, "sent");
        exited(&sender, 0);
        copies++;
    }
    return copies;
}

/* A transmission cut short: the first part of the waveform, then the room. */
static void tx_cut(enum wave_profile profile, const char *text, int percent)
{
    const int16_t *s;
    char err[128];
    long n = wave_encoder_encode(sim.enc, (const uint8_t *)text, strlen(text), profile,
                                 WAVE_DEFAULT_VOLUME, &s, err, sizeof(err));

    if (n > 0) {
        hear(s, (size_t)n * (size_t)percent / 100);
    }
}

static const struct wave_history_entry *rx_newest(void)
{
    return wave_history_at(&sim.rx.view.history, 0);
}

static int rx_count(void)
{
    return wave_history_count(&sim.rx.view.history);
}

/* ---- the scenarios --------------------------------------------------------- */

static void test_normal(void)
{
    int p;

    for (p = 0; p < wave_preset_count(); p++) {
        const struct wave_preset *pr = wave_preset_get(p);
        char what[128];
        int copies;

        rx_reset(p, "HELLO DOORS");
        rx_listen();
        quiet(0.5);
        copies = tx(p, "HELLO DOORS");
        quiet(0.5);
        snprintf(what, sizeof(what), "%s: every copy is decoded, nothing else", pr->label);
        check(what, copies == pr->copies && sim.rx.decoded == pr->copies && sim.rx.wrong == 0);
        snprintf(what, sizeof(what), "%s: one history entry, heard %d time(s), filed under %s",
                 pr->label, pr->copies, pr->id);
        check(what, rx_count() == 1 && rx_newest()->dir == WAVE_DIR_RX &&
                        rx_newest()->result == WAVE_RESULT_OK && rx_newest()->count == pr->copies &&
                        strcmp(rx_newest()->data, "HELLO DOORS") == 0 &&
                        strcmp(rx_newest()->preset, pr->id) == 0 && !rx_newest()->captured);
    }
}

static void test_invalid(void)
{
    char big[WAVE_MAX_MESSAGE_BYTES + 2];
    const int16_t *s;
    char err[128];

    check("empty: the sender refuses it", tx(WAVE_PRESET_STANDARD, "") == -1);
    check("control characters: refused", tx(WAVE_PRESET_STANDARD, "a\tb") == -1);
    check("broken UTF-8: refused", tx(WAVE_PRESET_STANDARD, "A\xC3") == -1);
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    check("65 bytes: refused by the sender", tx(WAVE_PRESET_STANDARD, big) == -1);
    check("65 bytes: and by the modem too",
          wave_encoder_encode(sim.enc, (const uint8_t *)big, WAVE_MAX_MESSAGE_BYTES + 1,
                              WAVE_PROFILE_FAST, WAVE_DEFAULT_VOLUME, &s, err, sizeof(err)) == -1);
    check("0 bytes: refused by the modem",
          wave_encoder_encode(sim.enc, (const uint8_t *)"", 0, WAVE_PROFILE_FAST,
                              WAVE_DEFAULT_VOLUME, &s, err, sizeof(err)) == -1);
}

static void test_impaired(void)
{
    /* A message under moderate noise, a hum, a DC offset and half the
     * level: the tones are far above 50 Hz and the decoder does not care
     * about offset or level. */
    rx_reset(WAVE_PRESET_STANDARD, "noisy but fine");
    sim.air.noise = 300;
    sim.air.hum = 300;
    sim.air.dc = 400;
    sim.air.gain_pct = 50;
    rx_listen();
    quiet(0.3);
    tx(WAVE_PRESET_STANDARD, "noisy but fine");
    quiet(0.5);
    check("impaired: noise, hum, DC and half the level still decode", sim.rx.decoded == 1 &&
                                                                         sim.rx.wrong == 0 &&
                                                                         rx_count() == 1);
    wave_channel_init(&sim.air, 77);

    /* A late start: the listener opens partway into a frame. */
    rx_reset(WAVE_PRESET_STANDARD, "misaligned");
    quiet(0.0123);
    rx_listen();
    tx(WAVE_PRESET_STANDARD, "misaligned");
    quiet(0.5);
    check("a start that is not on a frame boundary still decodes", sim.rx.decoded == 1 &&
                                                                       sim.rx.wrong == 0);

    /* Harsh paths: a message may be lost, but never invented or changed. */
    {
        static const struct {
            const char *what;
            int noise;
            int clip;
            int drop;
        } harsh[] = {
            { "heavy noise (6x the signal)", 20000, 0, 0 },
            { "hard clipping at 1/4 of the peak", 0, 800, 0 },
            { "one sample in 50 lost", 0, 0, 50 },
        };
        size_t i;

        for (i = 0; i < sizeof(harsh) / sizeof(harsh[0]); i++) {
            char what[128];

            rx_reset(WAVE_PRESET_ROBUST, "robust text");
            sim.air.noise = harsh[i].noise;
            sim.air.clip = harsh[i].clip;
            sim.air.drop_every = harsh[i].drop;
            rx_listen();
            tx(WAVE_PRESET_ROBUST, "robust text");
            quiet(0.5);
            snprintf(what, sizeof(what), "%s: nothing wrong is ever decoded (%d decoded, %d missed)",
                     harsh[i].what, sim.rx.decoded, sim.rx.missed);
            check(what, sim.rx.wrong == 0 &&
                            (rx_count() == 0 || strcmp(rx_newest()->data, "robust text") == 0));
            wave_channel_init(&sim.air, 77);
        }
    }

    /* Garbage alone. */
    rx_reset(WAVE_PRESET_STANDARD, NULL);
    sim.air.noise = 32767;
    rx_listen();
    quiet(5.0);
    check("5 s of full-scale garbage: no message", sim.rx.decoded == 0 && rx_count() == 0);
    wave_channel_init(&sim.air, 77);

    /* Cut off halfway: missed, and the listener is back for the next. */
    rx_reset(WAVE_PRESET_STANDARD, "after the cut");
    rx_listen();
    tx_cut(WAVE_PROFILE_FAST, "this one is cut off halfway", 50);
    quiet(8.0); /* past the watchdog */
    check("a transmission cut off halfway produces no message", sim.rx.decoded == 0);
    check("and is reported as missed", sim.rx.missed >= 1);
    wave_view_refresh(&sim.rx.view, "", now_ms());
    tx(WAVE_PRESET_STANDARD, "after the cut");
    quiet(0.5);
    check("the next whole message decodes after it", sim.rx.decoded == 1 && sim.rx.wrong == 0 &&
                                                         strcmp(rx_newest()->data, "after the cut") == 0);
}

static void test_repeats(void)
{
    rx_reset(WAVE_PRESET_STANDARD, NULL);
    rx_listen();
    tx(WAVE_PRESET_STANDARD, "same");
    tx(WAVE_PRESET_STANDARD, "same");
    tx(WAVE_PRESET_STANDARD, "same");
    quiet(0.5);
    check("the same message three times: three decodes, one entry x3",
          sim.rx.decoded == 3 && rx_count() == 1 && rx_newest()->count == 3);
    tx(WAVE_PRESET_STANDARD, "other");
    quiet(0.5);
    check("a different message is its own entry", rx_count() == 2 &&
                                                     strcmp(rx_newest()->data, "other") == 0);
    tx(WAVE_PRESET_STANDARD, "other");
    quiet(12.0); /* past STANDARD's 10 s window */
    tx(WAVE_PRESET_STANDARD, "other");
    quiet(0.5);
    check("the same message after the window is a new entry",
          rx_count() == 3 && rx_newest()->count == 1 && wave_history_at(&sim.rx.view.history, 1)->count == 2);
}

static void test_capture(void)
{
    rx_reset(WAVE_PRESET_STANDARD, "captured");
    rx_capture();
    quiet(0.4);
    tx(WAVE_PRESET_STANDARD, "captured");
    quiet(0.5);
    check("capture: nothing is decoded while recording", sim.rx.decoded == 0);
    rx_capture_done();
    wave_view_refresh(&sim.rx.view, "", now_ms());
    check("capture: decoded afterwards, from the recording",
          sim.rx.decoded == 1 && rx_count() == 1 && rx_newest()->captured &&
              strcmp(rx_newest()->data, "captured") == 0);
    check("capture: the status says so", strcmp(sim.rx.view.status,
                                                "Decoded 1 message from the recording") == 0);

    rx_reset(WAVE_PRESET_STANDARD, NULL);
    sim.air.noise = 500;
    rx_capture();
    quiet(3.0);
    rx_capture_done();
    wave_view_refresh(&sim.rx.view, "", now_ms());
    check("decode failure: a recording of a room decodes to nothing, said plainly",
          sim.rx.decoded == 0 && rx_count() == 1 && rx_newest()->result == WAVE_RESULT_UNDECODED &&
              strcmp(sim.rx.view.status, "Nothing decoded in the recording") == 0);
    wave_channel_init(&sim.air, 77);

    rx_reset(WAVE_PRESET_STANDARD, "half");
    rx_capture();
    tx_cut(WAVE_PROFILE_FAST, "half", 40);
    rx_capture_done();
    check("decode failure: a recording that ends mid-message decodes to nothing",
          sim.rx.decoded == 0 && rx_newest()->result == WAVE_RESULT_UNDECODED);
}

static void test_longest(void)
{
    char msg[WAVE_MAX_MESSAGE_BYTES + 1];
    int p;
    size_t i;

    for (i = 0; i < WAVE_MAX_MESSAGE_BYTES; i++) {
        msg[i] = (char)('a' + i % 26);
    }
    msg[WAVE_MAX_MESSAGE_BYTES] = '\0';
    for (p = 0; p < wave_preset_count(); p++) {
        char what[96];

        rx_reset(p, msg);
        rx_listen();
        tx(p, msg);
        quiet(0.5);
        snprintf(what, sizeof(what), "64 bytes on %s arrive whole",
                 wave_preset_speed_label(wave_preset_get(p)));
        check(what, sim.rx.decoded >= 1 && sim.rx.wrong == 0 && rx_newest()->len == WAVE_MAX_MESSAGE_BYTES &&
                        !rx_newest()->truncated);
    }
    /* 64 bytes of two-byte characters: 32 of them. */
    {
        char utf[WAVE_MAX_MESSAGE_BYTES + 1];

        for (i = 0; i < WAVE_MAX_MESSAGE_BYTES; i += 2) {
            utf[i] = '\xC3';
            utf[i + 1] = '\xA5';
        }
        utf[WAVE_MAX_MESSAGE_BYTES] = '\0';
        rx_reset(WAVE_PRESET_STANDARD, utf);
        rx_listen();
        tx(WAVE_PRESET_STANDARD, utf);
        quiet(0.5);
        check("64 bytes of UTF-8 (32 characters) arrive as text", sim.rx.decoded == 1 &&
                                                                    sim.rx.wrong == 0);
    }
}

static void test_mismatch(void)
{
    rx_reset(WAVE_PRESET_ROBUST, "quick to robust");
    rx_listen();
    tx(WAVE_PRESET_QUICK, "quick to robust");
    quiet(0.5);
    check("mismatch: a QUICK sender is heard by a ROBUST listener", sim.rx.decoded == 1 &&
                                                                       sim.rx.wrong == 0);
    check("mismatch: filed under the listener's preset", strcmp(rx_newest()->preset, "robust") == 0);

    rx_reset(WAVE_PRESET_QUICK, "robust to quick");
    rx_listen();
    tx(WAVE_PRESET_ROBUST, "robust to quick");
    quiet(0.5);
    check("mismatch: a ROBUST sender's two copies reach a QUICK listener, folded",
          sim.rx.decoded == 2 && rx_count() == 1 && rx_newest()->count == 2);
}

int main(void)
{
    char err[128];

    memset(&sim, 0, sizeof(sim));
    wave_channel_init(&sim.air, 77);
    sim.enc = wave_encoder_new(err, sizeof(err));
    sim.rx.capture = malloc((size_t)WAVE_CAPTURE_MAX_SECONDS * WAVE_MODEM_RATE * sizeof(int16_t));
    if (!sim.enc || !sim.rx.capture) {
        printf("FAIL the simulator could not start: %s\n", err);
        return 1;
    }
    {
        int16_t a[4] = { 1000, -1000, 0, 32000 };
        int16_t b[4];
        struct wave_channel c;

        wave_channel_init(&c, 5);
        c.gain_pct = 50;
        c.clip = 8000;
        wave_channel_apply(&c, a, b, 4, 0);
        check("channel: gain and clipping", b[0] == 500 && b[1] == -500 && b[2] == 0 && b[3] == 8000);
        wave_channel_init(&c, 5);
        c.noise = 100;
        wave_channel_room(&c, b, 4, 0);
        check("channel: noise stays inside its amplitude", wave_channel_peak(b, 4) <= 100);
    }

    test_normal();
    test_invalid();
    test_impaired();
    test_repeats();
    test_capture();
    test_longest();
    test_mismatch();

    wave_decoder_free(sim.rx.dec);
    wave_encoder_free(sim.enc);
    free(sim.rx.capture);
    printf("wave_sim_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
