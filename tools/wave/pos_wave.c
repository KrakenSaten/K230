/*
 * pos-wave: short messages as sound, and the only PocketOS program that
 * opens the audio device for Wave.
 *
 *   pos-wave info
 *       What board this is and which audio paths are allowed. Opens nothing.
 *   pos-wave encode [--protocol P] [--volume V] [--text T] OUT.wav
 *       Text (stdin, or --text) to a mono 48 kHz S16 WAV. No audio device.
 *   pos-wave decode [--channel C] IN.wav
 *       Every message in a 48 kHz S16 WAV. No audio device.
 *   pos-wave send [--protocol P] [--volume V] [--volume-percent L] [--text T] [--events]
 *                 [--allow-unverified]
 *       Text to the speaker. L is the system volume, 1 to 100 percent of the
 *       validated level (default 100): a digital gain applied by pocketaudio
 *       after the modem, so --volume shapes the signal and L sets how loud it
 *       is played. The Wave app passes the Controls volume here.
 *   pos-wave listen [--seconds N] [--channel C] [--events] [--allow-unverified]
 *       Messages from the microphone, for at most N seconds.
 *   pos-wave record [--seconds N] [--channel C] [--allow-unverified] OUT.wav
 *       A bench recording of one wire channel, N at most 30 (default 5), for
 *       checking which channel carries the microphone and decoding offline.
 *       Nothing is decoded while recording.
 *   pos-wave recover
 *       Undo what a pos-wave that died without closing (SIGKILL, a crash)
 *       left switched: amplifier off, route restored (pocketaudio.h,
 *       "Recovery"). Opens no stream. Idempotent. The Wave app runs it when
 *       its helper dies by a signal, and every send, listen and record does
 *       the same first.
 *
 * P is audible_normal, audible_fast (default) or audible_fastest; V is 1 to
 * 25 (default 10, a peak near -20 dBFS); N is 1 to 3600 (default 120).
 * Text read from stdin loses one trailing newline, so `echo DOORS | pos-wave
 * send` sends DOORS.
 *
 * GATE. An audio path that has not passed its hardware test (pocketaudio.h,
 * "Hardware gate") needs --allow-unverified on the command, which opens only
 * that command's own direction, or POCKETOS_AUDIO_ALLOW_UNVERIFIED naming the
 * direction: "capture", "playback", or both separated by a comma. Anything
 * else, "1" included, opens nothing, so an environment set for a microphone
 * test cannot also open the speaker.
 *
 * EVENTS. One line each on stdout, the protocol the Wave app reads
 * (apps/wave/wave_session.c):
 *
 *   ready <board>        audio device open
 *   sending <ms>         playback starting; its length in milliseconds
 *   listening            capture running
 *   level <0..100>       microphone peak, at most every 250 ms
 *   received <hex>       a decoded message, as hex bytes
 *   missed               a transmission was heard and could not be decoded
 *   sent                 playback finished
 *   stopped              ended by SIGTERM or SIGINT, device closed
 *   recovered            before opening, a dead owner's route or amplifier
 *                        state was restored (informational)
 *   error <code> <text>  <code> is one of the WAVE_ERR_* words
 *
 * Exit codes are WAVE_EXIT_* (wave_protocol.h). Without --events, decode and
 * listen also print "text <message>" after each clean-text message.
 *
 * SIGNALS. SIGTERM and SIGINT stop within one audio wait
 * (POCKETAUDIO_MAX_WAIT_MS): the amplifier goes off, the device is closed and
 * the route restored. SIGPIPE is ignored; a parent that is gone (stdout
 * write fails) is treated as a stop. Nothing is decoded into a log: ggwave's
 * own logging is compiled out, and this program writes messages only to
 * stdout.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketaudio/pocketaudio.h"
#include "wave_modem.h"
#include "wave_protocol.h"
#include "wave_text.h"
#include "wave_wav.h"
#ifdef POS_WAVE_TEST_HOOKS
/* tests/pos-wave-testhooks only: POS_WAVE_FAKE_AUDIO=<dir> replaces the
 * sound card and GPIO with files in <dir> (tests/fake_audio_backend.c), so a
 * test can SIGKILL a real pos-wave mid-operation and inspect what it left.
 * The shipped pos-wave is compiled without this, and the test checks that. */
#include "fake_audio_backend.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Silence around a transmission: time for the amplifier to settle before the
 * start marker, and for the last tone to leave the speaker before the stream
 * is closed. Neither is measured on the hardware yet. */
#define LEAD_IN_MS 100
#define TAIL_MS 150
/* Consecutive device waits that returned nothing before giving up. */
#define STALL_WAITS 10
#define LEVEL_EVERY_MS 250
/* The longest bench recording: a bounded buffer (1.44 M samples, 2.9 MB). */
#define RECORD_MAX_SECONDS 30

static volatile sig_atomic_t stop_requested;
static int events_mode;
static int parent_gone;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int stopping(void)
{
    return stop_requested || parent_gone;
}

static void emit(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        parent_gone = 1;
    }
}

static void fail_event(const char *code, const char *text)
{
    char clean[200];
    size_t i;

    /* One line, whatever the reason contained. */
    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    for (i = 0; clean[i]; i++) {
        if (clean[i] == '\n' || clean[i] == '\r') {
            clean[i] = ' ';
        }
    }
    emit("error %s %s", code, clean);
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pos-wave info\n"
            "       pos-wave encode [--protocol P] [--volume V] [--text T] OUT.wav\n"
            "       pos-wave decode [--channel C] IN.wav\n"
            "       pos-wave send [--protocol P] [--volume V] [--volume-percent L] [--text T] [--events]\n"
            "                     [--allow-unverified]\n"
            "       pos-wave listen [--seconds N] [--channel C] [--events] [--allow-unverified]\n"
            "       pos-wave record [--seconds N] [--channel C] [--allow-unverified] OUT.wav\n"
            "       pos-wave recover\n");
}

struct opts {
    enum wave_profile profile;
    int volume;
    int volume_percent; /* the system volume for playback, 1..100 */
    int seconds;
    int seconds_given;
    int channel; /* -1: the board's microphone channel */
    int allow_unverified;
    const char *text;
    const char *path;
};

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->profile = WAVE_DEFAULT_PROFILE;
    o->volume = WAVE_DEFAULT_VOLUME;
    o->volume_percent = 100;
    o->seconds = WAVE_LISTEN_SECONDS;
    o->channel = -1;
    o->text = NULL;
    o->path = NULL;
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "--events") == 0) {
            events_mode = 1;
        } else if (strcmp(a, "--allow-unverified") == 0) {
            o->allow_unverified = 1;
        } else if (strcmp(a, "--protocol") == 0 && v) {
            if (wave_modem_parse_profile(v, &o->profile) != 0) {
                fail_event(WAVE_ERR_USAGE, "unknown protocol");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--volume") == 0 && v) {
            if (parse_int(v, 1, WAVE_MODEM_MAX_VOLUME, &o->volume) != 0) {
                fail_event(WAVE_ERR_USAGE, "volume must be 1 to 25");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--volume-percent") == 0 && v) {
            if (parse_int(v, 1, 100, &o->volume_percent) != 0) {
                fail_event(WAVE_ERR_USAGE, "volume-percent must be 1 to 100");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--seconds") == 0 && v) {
            if (parse_int(v, 1, 3600, &o->seconds) != 0) {
                fail_event(WAVE_ERR_USAGE, "seconds must be 1 to 3600");
                return -1;
            }
            o->seconds_given = 1;
            i++;
        } else if (strcmp(a, "--channel") == 0 && v) {
            if (parse_int(v, 0, POCKETAUDIO_MAX_CHANNELS - 1, &o->channel) != 0) {
                fail_event(WAVE_ERR_USAGE, "channel must be 0 or 1");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--text") == 0 && v) {
            o->text = v;
            i++;
        } else if (a[0] != '-' && !o->path) {
            o->path = a;
        } else {
            fail_event(WAVE_ERR_USAGE, "unknown argument");
            return -1;
        }
    }
    return 0;
}

/* The message, from --text or stdin: at most WAVE_MAX_MESSAGE_BYTES of clean
 * text. 0 with len set, or -1 after an error event. */
static int read_message(const struct opts *o, char *buf, size_t *len)
{
    size_t n = 0;

    if (o->text) {
        n = strlen(o->text);
        if (n > WAVE_MAX_MESSAGE_BYTES) {
            fail_event(WAVE_ERR_TOO_LONG, "message longer than 64 bytes");
            return -1;
        }
        memcpy(buf, o->text, n);
    } else {
        size_t r;

        /* One byte more than allowed, and one more for a trailing newline,
         * so an overlong message is seen as overlong rather than cut. */
        while (n < WAVE_MAX_MESSAGE_BYTES + 2 &&
               (r = fread(buf + n, 1, WAVE_MAX_MESSAGE_BYTES + 2 - n, stdin)) > 0) {
            n += r;
        }
        if (n > 0 && buf[n - 1] == '\n') {
            n--;
            if (n > 0 && buf[n - 1] == '\r') {
                n--;
            }
        }
        if (n > WAVE_MAX_MESSAGE_BYTES || fgetc(stdin) != EOF) {
            fail_event(WAVE_ERR_TOO_LONG, "message longer than 64 bytes");
            return -1;
        }
    }
    if (n == 0) {
        fail_event(WAVE_ERR_INVALID_TEXT, "empty message");
        return -1;
    }
    if (!wave_text_clean(buf, n)) {
        fail_event(WAVE_ERR_INVALID_TEXT, "message is not plain UTF-8 text");
        return -1;
    }
    *len = n;
    return 0;
}

static void print_received(const uint8_t *data, size_t len)
{
    char hex[2 * WAVE_MAX_MESSAGE_BYTES + 1];
    size_t i;

    for (i = 0; i < len && i < WAVE_MAX_MESSAGE_BYTES; i++) {
        snprintf(hex + 2 * i, 3, "%02x", data[i]);
    }
    hex[2 * i] = '\0';
    emit("received %s", hex);
    if (!events_mode && wave_text_clean((const char *)data, len)) {
        emit("text %.*s", (int)len, (const char *)data);
    }
}

struct decode_count {
    int decoded;
    int missed;
};

static void on_decode(void *user, enum wave_decode_kind kind, const uint8_t *data, size_t len)
{
    struct decode_count *c = user;

    if (kind == WAVE_DECODED) {
        c->decoded++;
        print_received(data, len);
    } else {
        c->missed++;
        emit("missed");
    }
}

static const char *audio_code(int err)
{
    switch (err) {
    case POCKETAUDIO_E_DISABLED: return WAVE_ERR_AUDIO_DISABLED;
    case POCKETAUDIO_E_BUSY: return WAVE_ERR_AUDIO_BUSY;
    case POCKETAUDIO_E_NODEV: return WAVE_ERR_AUDIO_NODEV;
    default: return WAVE_ERR_AUDIO;
    }
}

/* Whether POCKETOS_AUDIO_ALLOW_UNVERIFIED names this direction. */
static int env_allows(enum pocketaudio_dir dir)
{
    const char *env = getenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED");
    const char *want = dir == POCKETAUDIO_CAPTURE ? "capture" : "playback";
    size_t n = strlen(want);
    const char *p = env;

    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == n && strncmp(p, want, n) == 0) {
            return 1;
        }
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static const struct pocketaudio_backend *backend_for(void)
{
#ifdef POS_WAVE_TEST_HOOKS
    const char *fake = getenv("POS_WAVE_FAKE_AUDIO");

    if (fake && *fake) {
        return fake_audio_backend(fake);
    }
#endif
    return pocketaudio_alsa_backend();
}

static void board_for(struct pocketaudio_board *b)
{
    const char *force = getenv("POCKETOS_AUDIO_BOARD");
    const char *pcm = getenv("POCKETOS_AUDIO_PCM");

#ifdef POS_WAVE_TEST_HOOKS
    if (getenv("POS_WAVE_FAKE_AUDIO") && *getenv("POS_WAVE_FAKE_AUDIO")) {
        *b = *fake_audio_board();
        return;
    }
#endif
    /* "k230" is accepted here as well as "generic", so the gate can be
     * exercised on a bench host that has no K230 card. */
    if (force && strcmp(force, "k230") == 0) {
        *b = *pocketaudio_board_k230();
        if (pcm && *pcm) {
            b->pcm = pcm;
        }
        return;
    }
    pocketaudio_board_detect(b, NULL);
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_info(void)
{
    struct pocketaudio_board b;

    board_for(&b);
    printf("board %s\n", b.name);
    printf("pcm %s\n", b.pcm);
    printf("channels %u\n", b.channels);
    printf("mic_channel %u\n", b.capture_channel);
    printf("capture_settle_ms %u\n", (unsigned)(b.capture_settle_frames * 1000ULL / POCKETAUDIO_RATE));
    printf("route_control %s\n", b.route_control ? b.route_control : "none");
    printf("amplifier %s", b.amp_chip ? b.amp_chip : "none");
    if (b.amp_chip) {
        printf(" line %u active-%s", b.amp_line, b.amp_active_high ? "high" : "low");
    }
    printf("\n");
    printf("playback %s\n", b.playback_verified ? "validated" : "not validated (gated)");
    printf("capture %s\n", b.capture_verified ? "validated" : "not validated (gated)");
    printf("rate %d\nformat S16_LE\npeak_ceiling %d\n", POCKETAUDIO_RATE, POCKETAUDIO_PEAK_CEILING);
    printf("max_message_bytes %d\n", WAVE_MAX_MESSAGE_BYTES);
    return WAVE_EXIT_OK;
}

static int cmd_encode(const struct opts *o)
{
    char msg[WAVE_MAX_MESSAGE_BYTES + 2];
    char err[160];
    struct wave_encoder *e;
    const int16_t *s;
    size_t len;
    long n;
    int rc;

    if (!o->path) {
        fail_event(WAVE_ERR_USAGE, "encode needs an output file");
        return WAVE_EXIT_USAGE;
    }
    if (read_message(o, msg, &len) != 0) {
        return WAVE_EXIT_USAGE;
    }
    e = wave_encoder_new(err, sizeof(err));
    if (!e) {
        fail_event(WAVE_ERR_ENCODE, err);
        return WAVE_EXIT_FAILED;
    }
    n = wave_encoder_encode(e, (const uint8_t *)msg, len, o->profile, o->volume, &s, err, sizeof(err));
    if (n <= 0) {
        fail_event(WAVE_ERR_ENCODE, err);
        wave_encoder_free(e);
        return WAVE_EXIT_FAILED;
    }
    rc = wave_wav_write_mono16(o->path, s, (size_t)n, WAVE_MODEM_RATE, err, sizeof(err));
    if (rc != 0) {
        fail_event(WAVE_ERR_USAGE, err);
        wave_encoder_free(e);
        return WAVE_EXIT_FAILED;
    }
    printf("samples %ld\nduration_ms %ld\npeak %d\n", n, n * 1000L / WAVE_MODEM_RATE,
           pocketaudio_peak(s, (size_t)n));
    wave_encoder_free(e);
    return WAVE_EXIT_OK;
}

static int cmd_decode(const struct opts *o)
{
    struct decode_count count = { 0, 0 };
    struct wave_decoder *d;
    struct wave_wav w;
    char err[160];
    int16_t mono[960];
    unsigned ch;
    size_t off;
    int rc;

    if (!o->path) {
        fail_event(WAVE_ERR_USAGE, "decode needs an input file");
        return WAVE_EXIT_USAGE;
    }
    rc = wave_wav_read(o->path, &w, err, sizeof(err));
    if (rc != WAVE_WAV_OK) {
        fail_event(WAVE_ERR_USAGE, err);
        return WAVE_EXIT_USAGE;
    }
    ch = o->channel >= 0 ? (unsigned)o->channel : 0;
    if (w.rate != WAVE_MODEM_RATE || ch >= w.channels) {
        fail_event(WAVE_ERR_USAGE, w.rate != WAVE_MODEM_RATE ? "unsupported sample rate (need 48000)"
                                                              : "no such channel in the file");
        wave_wav_free(&w);
        return WAVE_EXIT_USAGE;
    }
    d = wave_decoder_new(err, sizeof(err));
    if (!d) {
        fail_event(WAVE_ERR_DECODE, err);
        wave_wav_free(&w);
        return WAVE_EXIT_FAILED;
    }
    for (off = 0; off < w.frames;) {
        size_t k = w.frames - off < 960 ? w.frames - off : 960;
        size_t i;

        for (i = 0; i < k; i++) {
            mono[i] = w.samples[(off + i) * w.channels + ch];
        }
        wave_decoder_feed(d, mono, k, on_decode, &count);
        off += k;
    }
    {
        /* Half a second of silence, so a message that ends at the very end
         * of the file still reaches its end marker's analysis. */
        static const int16_t quiet[960];
        int i;

        for (i = 0; i < 25; i++) {
            wave_decoder_feed(d, quiet, 960, on_decode, &count);
        }
    }
    if (w.truncated) {
        printf("note file data ended before its declared size\n");
    }
    wave_decoder_free(d);
    wave_wav_free(&w);
    return count.decoded > 0 ? WAVE_EXIT_OK : WAVE_EXIT_FAILED;
}

static struct pocketaudio_stream *open_audio(enum pocketaudio_dir dir, const struct opts *o,
                                             struct pocketaudio_board *b)
{
    struct pocketaudio_options ao;
    struct pocketaudio_stream *s = NULL;
    char err[200];
    int rc;

    board_for(b);
    if (dir == POCKETAUDIO_CAPTURE && o->channel >= 0) {
        if ((unsigned)o->channel >= b->channels) {
            fail_event(WAVE_ERR_USAGE, "this board has no such channel");
            return NULL;
        }
        b->capture_channel = (unsigned)o->channel;
    }
    memset(&ao, 0, sizeof(ao));
    ao.backend = backend_for();
    ao.board = b;
    ao.allow_unverified = o->allow_unverified || env_allows(dir);
    ao.peak_limit = POCKETAUDIO_PEAK_CEILING;
    if (dir == POCKETAUDIO_PLAYBACK) {
        ao.volume_percent = o->volume_percent;
    }
    rc = pocketaudio_open(&s, dir, &ao, err, sizeof(err));
    if (rc != POCKETAUDIO_OK) {
        fail_event(audio_code(rc), err);
        return NULL;
    }
    if (pocketaudio_recovered(s)) {
        emit("recovered");
    }
    return s;
}

static int cmd_recover(void)
{
    struct pocketaudio_options ao;
    char report[200];
    int rc;

    memset(&ao, 0, sizeof(ao));
    ao.backend = backend_for();
    rc = pocketaudio_recover(&ao, report, sizeof(report));
    if (rc < 0) {
        fail_event(audio_code(rc), report);
        return WAVE_EXIT_AUDIO;
    }
    if (rc == 1) {
        emit("recovered");
    }
    printf("note %s\n", report);
    return WAVE_EXIT_OK;
}

/* Write n samples (NULL: silence), stopping early when asked. 0, 1 when
 * stopped, -1 after an error event. */
static int play(struct pocketaudio_stream *s, const int16_t *samples, size_t n)
{
    static const int16_t quiet[POCKETAUDIO_PERIOD_FRAMES];
    size_t off = 0;
    int stalls = 0;

    while (off < n) {
        size_t k = n - off;
        long r;

        if (stopping()) {
            return 1;
        }
        if (k > POCKETAUDIO_PERIOD_FRAMES) {
            k = POCKETAUDIO_PERIOD_FRAMES;
        }
        r = pocketaudio_write(s, samples ? samples + off : quiet, k);
        if (r < 0) {
            fail_event(WAVE_ERR_AUDIO, pocketaudio_last_error(s));
            return -1;
        }
        if (r == 0) {
            if (++stalls >= STALL_WAITS) {
                fail_event(WAVE_ERR_AUDIO, "the audio device stopped accepting samples");
                return -1;
            }
            continue;
        }
        stalls = 0;
        off += (size_t)r;
    }
    return 0;
}

static int cmd_send(const struct opts *o)
{
    char msg[WAVE_MAX_MESSAGE_BYTES + 2];
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    struct wave_encoder *e;
    const int16_t *samples;
    char err[160];
    size_t len;
    long n;
    int r;

    if (read_message(o, msg, &len) != 0) {
        return WAVE_EXIT_USAGE;
    }
    /* Encode before opening anything: a message that cannot be encoded
     * never reaches the speaker's amplifier. */
    e = wave_encoder_new(err, sizeof(err));
    if (!e) {
        fail_event(WAVE_ERR_ENCODE, err);
        return WAVE_EXIT_FAILED;
    }
    n = wave_encoder_encode(e, (const uint8_t *)msg, len, o->profile, o->volume, &samples, err,
                            sizeof(err));
    if (n <= 0) {
        fail_event(WAVE_ERR_ENCODE, err);
        wave_encoder_free(e);
        return WAVE_EXIT_FAILED;
    }
    s = open_audio(POCKETAUDIO_PLAYBACK, o, &b);
    if (!s) {
        wave_encoder_free(e);
        return WAVE_EXIT_AUDIO;
    }
    emit("ready %s", b.name);
    emit("sending %ld", (long)(LEAD_IN_MS + TAIL_MS) + n * 1000L / WAVE_MODEM_RATE);

    r = play(s, NULL, (size_t)POCKETAUDIO_RATE * LEAD_IN_MS / 1000);
    if (r == 0) {
        r = play(s, samples, (size_t)n);
    }
    if (r == 0) {
        r = play(s, NULL, (size_t)POCKETAUDIO_RATE * TAIL_MS / 1000);
    }
    if (r == 0) {
        int d = pocketaudio_drain(s, 1000);

        if (d != POCKETAUDIO_OK) {
            fail_event(WAVE_ERR_AUDIO, pocketaudio_last_error(s));
            r = -1;
        }
    }
    pocketaudio_close(s);
    wave_encoder_free(e);
    if (r == 1) {
        emit("stopped");
        return WAVE_EXIT_OK;
    }
    if (r < 0) {
        return WAVE_EXIT_AUDIO;
    }
    emit("sent");
    return WAVE_EXIT_OK;
}

static int cmd_listen(const struct opts *o)
{
    struct decode_count count = { 0, 0 };
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    struct wave_decoder *d;
    int16_t buf[POCKETAUDIO_PERIOD_FRAMES];
    char err[160];
    uint64_t captured = 0;
    uint64_t limit = (uint64_t)o->seconds * POCKETAUDIO_RATE;
    int64_t wall_end;
    int64_t last_level = 0;
    uint64_t level_frames = 0;
    int level_peak = 0;
    int stalls = 0;
    int rc = WAVE_EXIT_OK;

    /* The decoder first: a receiver that cannot be built never opens the
     * microphone. */
    d = wave_decoder_new(err, sizeof(err));
    if (!d) {
        fail_event(WAVE_ERR_DECODE, err);
        return WAVE_EXIT_FAILED;
    }
    s = open_audio(POCKETAUDIO_CAPTURE, o, &b);
    if (!s) {
        wave_decoder_free(d);
        return WAVE_EXIT_AUDIO;
    }
    emit("ready %s", b.name);
    emit("listening");
    /* Bounded twice: by audio captured, and by the wall clock in case the
     * device delivers slower than real time. */
    wall_end = mono_ms() + (int64_t)o->seconds * 1000 + 2000;

    while (!stopping() && captured < limit && mono_ms() < wall_end) {
        long r = pocketaudio_read(s, buf, sizeof(buf) / sizeof(buf[0]));
        int p;

        if (r < 0) {
            fail_event(WAVE_ERR_AUDIO, pocketaudio_last_error(s));
            rc = WAVE_EXIT_AUDIO;
            break;
        }
        if (r == 0) {
            if (++stalls >= STALL_WAITS) {
                fail_event(WAVE_ERR_AUDIO, "the microphone stopped delivering samples");
                rc = WAVE_EXIT_AUDIO;
                break;
            }
            continue;
        }
        stalls = 0;
        captured += (uint64_t)r;
        if (wave_decoder_feed(d, buf, (size_t)r, on_decode, &count) != 0) {
            fail_event(WAVE_ERR_DECODE, "the receiver could not be restarted");
            rc = WAVE_EXIT_FAILED;
            break;
        }
        p = pocketaudio_peak(buf, (size_t)r);
        if (p > level_peak) {
            level_peak = p;
        }
        level_frames += (uint64_t)r;
        if (level_frames >= (uint64_t)POCKETAUDIO_RATE * LEVEL_EVERY_MS / 1000 &&
            mono_ms() - last_level >= LEVEL_EVERY_MS) {
            emit("level %d", level_peak * 100 / 32768);
            last_level = mono_ms();
            level_frames = 0;
            level_peak = 0;
        }
    }
    pocketaudio_close(s);
    wave_decoder_free(d);
    if (rc == WAVE_EXIT_OK && stopping()) {
        emit("stopped");
    }
    return rc;
}

static int cmd_record(struct opts *o)
{
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    size_t cap;
    size_t got = 0;
    int16_t *buf;
    char err[160];
    int stalls = 0;
    int rc = WAVE_EXIT_OK;
    int64_t wall_end;

    if (!o->path) {
        fail_event(WAVE_ERR_USAGE, "record needs an output file");
        return WAVE_EXIT_USAGE;
    }
    if (!o->seconds_given) {
        o->seconds = 5;
    }
    if (o->seconds > RECORD_MAX_SECONDS) {
        fail_event(WAVE_ERR_USAGE, "a recording is at most 30 seconds");
        return WAVE_EXIT_USAGE;
    }
    cap = (size_t)o->seconds * POCKETAUDIO_RATE;
    buf = malloc(cap * sizeof(int16_t));
    if (!buf) {
        fail_event(WAVE_ERR_AUDIO, "out of memory");
        return WAVE_EXIT_FAILED;
    }
    s = open_audio(POCKETAUDIO_CAPTURE, o, &b);
    if (!s) {
        free(buf);
        return WAVE_EXIT_AUDIO;
    }
    emit("ready %s", b.name);
    emit("listening");
    wall_end = mono_ms() + (int64_t)o->seconds * 1000 + 2000;
    while (!stopping() && got < cap && mono_ms() < wall_end) {
        long r = pocketaudio_read(s, buf + got, cap - got);

        if (r < 0) {
            fail_event(WAVE_ERR_AUDIO, pocketaudio_last_error(s));
            rc = WAVE_EXIT_AUDIO;
            break;
        }
        if (r == 0) {
            if (++stalls >= STALL_WAITS) {
                fail_event(WAVE_ERR_AUDIO, "the microphone stopped delivering samples");
                rc = WAVE_EXIT_AUDIO;
                break;
            }
            continue;
        }
        stalls = 0;
        got += (size_t)r;
    }
    /* The microphone is closed before anything is written to storage. */
    pocketaudio_close(s);
    if (got > 0 && wave_wav_write_mono16(o->path, buf, got, POCKETAUDIO_RATE, err, sizeof(err)) != 0) {
        fail_event(WAVE_ERR_USAGE, err);
        rc = WAVE_EXIT_FAILED;
    }
    printf("samples %zu\npeak %d\n", got, pocketaudio_peak(buf, got));
    free(buf);
    if (rc == WAVE_EXIT_OK && stopping()) {
        emit("stopped");
    }
    return rc;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    struct opts o;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        usage();
        return WAVE_EXIT_USAGE;
    }
    if (strcmp(argv[1], "info") == 0) {
        return cmd_info();
    }
    if (strcmp(argv[1], "recover") == 0) {
        return argc == 2 ? cmd_recover() : (usage(), WAVE_EXIT_USAGE);
    }
    if (parse_opts(argc, argv, &o) != 0) {
        return WAVE_EXIT_USAGE;
    }
    if (strcmp(argv[1], "encode") == 0) {
        return cmd_encode(&o);
    }
    if (strcmp(argv[1], "decode") == 0) {
        return cmd_decode(&o);
    }
    if (strcmp(argv[1], "send") == 0) {
        return o.path ? (fail_event(WAVE_ERR_USAGE, "send takes no file"), WAVE_EXIT_USAGE)
                      : cmd_send(&o);
    }
    if (strcmp(argv[1], "listen") == 0) {
        return o.path ? (fail_event(WAVE_ERR_USAGE, "listen takes no file"), WAVE_EXIT_USAGE)
                      : cmd_listen(&o);
    }
    if (strcmp(argv[1], "record") == 0) {
        return cmd_record(&o);
    }
    usage();
    return WAVE_EXIT_USAGE;
}
