/*
 * pos-mp3: the MP3 app's audio helper, and the only program that decodes or
 * opens the audio device for it (docs/apps/MP3.md).
 *
 *   pos-mp3 info
 *       The board, the decoder and the limits. Opens nothing.
 *   pos-mp3 probe FILE
 *       What the decoder makes of FILE: codec, rate, channels, length, tags.
 *       Opens no audio.
 *   pos-mp3 play [--start-ms MS] [--volume-percent V] [--events]
 *                [--allow-unverified] FILE
 *       FILE to the speaker, from MS; V is the system volume, 1..100.
 *   pos-mp3 recover
 *       Undo what a helper that died without closing left switched
 *       (pocketaudio_recover).
 *
 * Events and commands are apps/mp3/mp3_protocol.h. With --events, stdin
 * carries commands and its end is a stop (the app is gone); without it stdin
 * is not read, and SIGINT or SIGTERM stops.
 *
 * THE FILE FIRST. The file is opened and probed before the audio device, so
 * a missing, empty or damaged file never switches the route or enables the
 * amplifier.
 *
 * LEVEL. Decoded music is full scale; pocketaudio clamps every sample at
 * POCKETAUDIO_PEAK_CEILING (-12 dBFS, the validated level). So the helper
 * scales the audio down by those 12 dB before the clamp instead of letting
 * the clamp clip it, and then by the system volume on pocketaudio's own
 * curve (pocketaudio_volume_gain_q15). The stream is opened at 100 % and the
 * volume is applied here, so a volume change is heard at once without
 * reopening the device.
 *
 * STOPPING. SIGTERM, SIGINT, "stop" and a closed stdin all end the loop
 * within one audio wait (POCKETAUDIO_MAX_WAIT_MS). A helper killed outright
 * leaves its audio route for the next audio open or `pos-mp3 recover`, as
 * pocketaudio.h describes.
 *
 * PAUSING closes the device (the amplifier is off, the audio lock is free
 * for Wave or Recorder) and keeps the decoder and the position; resuming
 * opens it again. A resume that cannot get the device stays paused and says
 * why. A seek while paused moves the position and stays paused.
 *
 * Nothing is written and nothing is logged: no names, no audio.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "mp3_decoder.h"
#include "mp3_protocol.h"
#include "pocketaudio/pocketaudio.h"
#ifdef POS_MP3_TEST_HOOKS
/* tests/pos-mp3-testhooks only: POS_MP3_FAKE_AUDIO=<dir> replaces the sound
 * card with files (tests/fake_audio_backend.c); POS_MP3_FAIL_AT_MS with
 * POS_MP3_FAIL_KIND=decode|storage makes the decoder fail at that position;
 * POS_MP3_OPEN_DELAY_MS makes opening the file that slow. The shipped
 * helper has none. */
#include "fake_audio_backend.h"
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAUSED_WAIT_MS 200
/* Consecutive device waits that took nothing before the device is taken to
 * have stopped: 10 x 200 ms. */
#define STALL_WAITS 10
#define DRAIN_MS 1000
/* 2^-2: the 12 dB between full scale and POCKETAUDIO_PEAK_CEILING. */
#define HEADROOM_SHIFT 2

static volatile sig_atomic_t stop_requested;
static int events_mode;
static int parent_gone;
static int pause_wanted;
static int resume_wanted;
static int seek_wanted;
static int64_t seek_ms;
static int volume_wanted;
static int volume_pct;
static char cmd_line[64];
static size_t cmd_len;
static int cmd_overlong;

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

    snprintf(clean, sizeof(clean), "%s", text && *text ? text : "failed");
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
    fprintf(stderr, "usage: pos-mp3 info\n"
                    "       pos-mp3 probe FILE\n"
                    "       pos-mp3 play [--start-ms MS] [--volume-percent V] [--events]\n"
                    "                    [--allow-unverified] FILE\n"
                    "       pos-mp3 recover\n");
}

/* ---- options ------------------------------------------------------------ */

struct opts {
    long long start_ms;
    int volume_percent;
    int allow_unverified;
    const char *path;
};

static int parse_ll(const char *s, long long lo, long long hi, long long *out)
{
    char *end;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->volume_percent = 100;
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        long long n;

        if (strcmp(a, "--events") == 0) {
            events_mode = 1;
        } else if (strcmp(a, "--allow-unverified") == 0) {
            o->allow_unverified = 1;
        } else if (strcmp(a, "--start-ms") == 0 && v) {
            if (parse_ll(v, 0, 1000LL * 3600 * 48, &o->start_ms) != 0) {
                fail_event(MP3_ERR_USAGE, "start-ms out of range");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--volume-percent") == 0 && v) {
            if (parse_ll(v, 1, 100, &n) != 0) {
                fail_event(MP3_ERR_USAGE, "volume-percent must be 1 to 100");
                return -1;
            }
            o->volume_percent = (int)n;
            i++;
        } else if (a[0] != '-' && !o->path) {
            o->path = a;
        } else {
            fail_event(MP3_ERR_USAGE, "unknown argument");
            return -1;
        }
    }
    return 0;
}

/* ---- commands on stdin -------------------------------------------------- */

static void dispatch(const char *line)
{
    long long n;

    if (strcmp(line, "stop") == 0) {
        stop_requested = 1;
    } else if (strcmp(line, "pause") == 0) {
        pause_wanted = 1;
        resume_wanted = 0;
    } else if (strcmp(line, "resume") == 0) {
        resume_wanted = 1;
        pause_wanted = 0;
    } else if (strncmp(line, "seek ", 5) == 0 && parse_ll(line + 5, 0, 1000LL * 3600 * 48, &n) == 0) {
        seek_wanted = 1;
        seek_ms = n;
    } else if (strncmp(line, "volume ", 7) == 0 && parse_ll(line + 7, 1, 100, &n) == 0) {
        volume_wanted = 1;
        volume_pct = (int)n;
    }
    /* Anything else is ignored: a newer app may say more. */
}

/* Read whatever commands are waiting, waiting at most wait_ms for one. */
static void read_commands(int wait_ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    char buf[128];
    ssize_t n;
    ssize_t i;

    if (!events_mode) {
        if (wait_ms > 0) {
            poll(NULL, 0, wait_ms);
        }
        return;
    }
    if (poll(&p, 1, wait_ms) <= 0) {
        return;
    }
    n = read(0, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        /* The app's end is closed: nobody is left to say stop. */
        parent_gone = 1;
        return;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (!cmd_overlong) {
                cmd_line[cmd_len] = '\0';
                dispatch(cmd_line);
            }
            cmd_len = 0;
            cmd_overlong = 0;
        } else if (cmd_len + 1 >= sizeof(cmd_line)) {
            cmd_overlong = 1;
        } else if (!cmd_overlong) {
            cmd_line[cmd_len++] = buf[i];
        }
    }
}

/* ---- the device --------------------------------------------------------- */

static const char *audio_code(int err)
{
    switch (err) {
    case POCKETAUDIO_E_DISABLED: return MP3_ERR_AUDIO_DISABLED;
    case POCKETAUDIO_E_BUSY: return MP3_ERR_AUDIO_BUSY;
    case POCKETAUDIO_E_NODEV: return MP3_ERR_AUDIO_NODEV;
    default: return MP3_ERR_AUDIO;
    }
}

/* Whether POCKETOS_AUDIO_ALLOW_UNVERIFIED names playback (the same rule as
 * pos-wave's and pos-record's: "capture", "playback" or both). */
static int env_allows_playback(void)
{
    const char *p = getenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED");

    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == 8 && strncmp(p, "playback", 8) == 0) {
            return 1;
        }
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static const struct pocketaudio_backend *backend_for(void)
{
#ifdef POS_MP3_TEST_HOOKS
    const char *fake = getenv("POS_MP3_FAKE_AUDIO");

    if (fake && *fake) {
        return fake_audio_backend(fake);
    }
#endif
    return pocketaudio_alsa_backend();
}

static void board_for(struct pocketaudio_board *b)
{
#ifdef POS_MP3_TEST_HOOKS
    if (getenv("POS_MP3_FAKE_AUDIO") && *getenv("POS_MP3_FAKE_AUDIO")) {
        *b = *fake_audio_board();
        return;
    }
#endif
    pocketaudio_board_detect(b, NULL);
}

/* Open the speaker at 100 %: the volume is applied to the samples here. NULL
 * after an error event. */
static struct pocketaudio_stream *open_audio(const struct opts *o, struct pocketaudio_board *b)
{
    struct pocketaudio_options ao;
    struct pocketaudio_stream *s = NULL;
    char err[200];
    int rc;

    board_for(b);
    memset(&ao, 0, sizeof(ao));
    ao.backend = backend_for();
    ao.board = b;
    ao.allow_unverified = o->allow_unverified || env_allows_playback();
    ao.volume_percent = 100;
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &ao, err, sizeof(err));
    if (rc != POCKETAUDIO_OK) {
        fail_event(audio_code(rc), err);
        return NULL;
    }
    if (pocketaudio_recovered(s)) {
        emit("recovered");
    }
    return s;
}

/* The gain applied to decoded samples, Q15: the headroom and the volume. */
static int32_t gain_for(int percent)
{
    return (int32_t)(pocketaudio_volume_gain_q15(percent) >> HEADROOM_SHIFT);
}

static void apply_gain(int16_t *s, size_t n, int32_t g)
{
    size_t i;

    for (i = 0; i < n; i++) {
        s[i] = (int16_t)(((int32_t)s[i] * g + (1 << 14)) >> 15);
    }
}

/* ---- the decoder, with the test hooks ----------------------------------- */

#ifdef POS_MP3_TEST_HOOKS
static int64_t fail_at_ms = -1;
static int fail_kind = MP3_DEC_E_DECODE;

static void install_hooks(void)
{
    const char *at = getenv("POS_MP3_FAIL_AT_MS");
    const char *kind = getenv("POS_MP3_FAIL_KIND");

    if (at && *at) {
        fail_at_ms = strtoll(at, NULL, 10);
    }
    if (kind && strcmp(kind, "storage") == 0) {
        fail_kind = MP3_DEC_E_IO;
    }
}
#endif

static long decode(struct mp3_decoder *d, int16_t *out, size_t max, int64_t pos_ms)
{
#ifdef POS_MP3_TEST_HOOKS
    if (fail_at_ms >= 0 && pos_ms >= fail_at_ms) {
        return fail_kind;
    }
#else
    (void)pos_ms;
#endif
    return mp3_decoder_read(d, out, max);
}

static const char *dec_code(int r)
{
    switch (r) {
    case MP3_DEC_E_MISSING: return MP3_ERR_MISSING;
    case MP3_DEC_E_IO: return MP3_ERR_STORAGE;
    case MP3_DEC_E_DECODE: return MP3_ERR_DECODE;
    default: return MP3_ERR_FORMAT;
    }
}

static int dec_exit(int r)
{
    return r == MP3_DEC_E_MISSING || r == MP3_DEC_E_IO ? MP3_EXIT_FILE : MP3_EXIT_FORMAT;
}

static int open_file(const char *path, struct mp3_decoder **d, struct mp3_dec_info *info)
{
    char err[200];
    int r;

#ifdef POS_MP3_TEST_HOOKS
    {
        const char *delay = getenv("POS_MP3_OPEN_DELAY_MS");

        if (delay && *delay) {
            poll(NULL, 0, atoi(delay));
        }
    }
#endif
    r = mp3_decoder_open(d, path, info, err, sizeof(err));
    if (r != MP3_DEC_OK) {
        fail_event(dec_code(r), err);
        return dec_exit(r);
    }
    return MP3_EXIT_OK;
}

/* ---- info and probe ----------------------------------------------------- */

static int cmd_info(void)
{
    struct pocketaudio_board b;

    board_for(&b);
    printf("board %s\n", b.name);
    printf("pcm %s\n", b.pcm);
    printf("device_rate %d\n", POCKETAUDIO_RATE);
    printf("playback %s\n", b.playback_verified ? "validated" : "not validated (gated)");
#ifdef MP3_HAVE_FFMPEG
    printf("decoder ffmpeg\n");
#else
    printf("decoder wav\n");
#endif
    printf("output 48000 mono s16le, -12 dB headroom\n");
    printf("playback_peak_ceiling %d\n", POCKETAUDIO_PEAK_CEILING);
    return MP3_EXIT_OK;
}

static int cmd_probe(const struct opts *o)
{
    struct mp3_decoder *d = NULL;
    struct mp3_dec_info info;
    int rc;

    if (!o->path) {
        fail_event(MP3_ERR_USAGE, "probe needs a file");
        return MP3_EXIT_USAGE;
    }
    rc = open_file(o->path, &d, &info);
    if (rc != MP3_EXIT_OK) {
        return rc;
    }
    printf("codec %s\n", info.codec);
    printf("rate %u\n", info.rate);
    printf("channels %u\n", info.channels);
    printf("total_ms %lld\n", (long long)info.total_ms);
    printf("seekable %d\n", info.seekable);
    printf("title %s\n", info.title);
    printf("artist %s\n", info.artist);
    mp3_decoder_close(d);
    return MP3_EXIT_OK;
}

/* ---- play --------------------------------------------------------------- */

struct play {
    struct mp3_decoder *dec;
    struct mp3_dec_info info;
    int64_t base_ms;     /* the position the samples below count from */
    uint64_t heard;      /* samples written to the device since base_ms */
    int32_t gain;
    unsigned stalls;
    size_t len;
    size_t off;
    int16_t buf[POCKETAUDIO_PERIOD_FRAMES];
};

static int64_t pos_ms(const struct play *p)
{
    return p->base_ms + (int64_t)(p->heard * 1000u / MP3_DEC_RATE);
}

static void do_seek(struct play *p, int64_t ms)
{
    int r;

    if (!p->info.seekable) {
        return; /* the app does not offer it; a stray one is ignored */
    }
    if (p->info.total_ms > 0 && ms > p->info.total_ms) {
        ms = p->info.total_ms;
    }
    r = mp3_decoder_seek(p->dec, ms);
    if (r == MP3_DEC_OK) {
        p->base_ms = ms;
        p->heard = 0;
        p->len = p->off = 0;
    }
    emit("progress %lld", (long long)pos_ms(p));
}

static int cmd_play(const struct opts *o)
{
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    struct play *p;
    int64_t last_progress;
    int paused = 0;
    int rc;

    if (!o->path) {
        fail_event(MP3_ERR_USAGE, "play needs a file");
        return MP3_EXIT_USAGE;
    }
    p = calloc(1, sizeof(*p));
    if (!p) {
        fail_event(MP3_ERR_AUDIO, "out of memory");
        return MP3_EXIT_AUDIO;
    }
    /* The file first: one that cannot be played never enables the amplifier. */
    rc = open_file(o->path, &p->dec, &p->info);
    if (rc != MP3_EXIT_OK) {
        free(p);
        return rc;
    }
    if (o->start_ms > 0 && p->info.seekable) {
        do_seek(p, o->start_ms);
    }
    if (stopping()) {
        mp3_decoder_close(p->dec);
        free(p);
        emit("stopped");
        return MP3_EXIT_OK;
    }
    s = open_audio(o, &b);
    if (!s) {
        mp3_decoder_close(p->dec);
        free(p);
        return MP3_EXIT_AUDIO;
    }
    p->gain = gain_for(o->volume_percent);
    if (p->info.title[0]) {
        emit("meta title %s", p->info.title);
    }
    if (p->info.artist[0]) {
        emit("meta artist %s", p->info.artist);
    }
    emit("ready %s", b.name);
    emit("playing %lld %d %u %u %s", (long long)p->info.total_ms, p->info.seekable, p->info.rate,
         p->info.channels, p->info.codec);
    emit("progress %lld", (long long)pos_ms(p));
    last_progress = mono_ms();
    rc = MP3_EXIT_OK;
    while (!stopping()) {
        long w;

        read_commands(paused ? PAUSED_WAIT_MS : 0);
        if (stopping()) {
            break;
        }
        if (volume_wanted) {
            volume_wanted = 0;
            p->gain = gain_for(volume_pct);
        }
        if (seek_wanted) {
            seek_wanted = 0;
            do_seek(p, seek_ms);
            last_progress = mono_ms();
        }
        if (pause_wanted && !paused) {
            pause_wanted = 0;
            pocketaudio_close(s); /* amplifier off first, inside pocketaudio */
            s = NULL;
            paused = 1;
            emit("paused");
        }
        if (resume_wanted) {
            resume_wanted = 0;
            if (paused) {
                s = open_audio(o, &b);
                if (s) {
                    paused = 0;
                    p->stalls = 0;
                    emit("resumed");
                }
            }
        }
        if (paused) {
            continue;
        }
        if (p->off >= p->len) {
            long n = decode(p->dec, p->buf, POCKETAUDIO_PERIOD_FRAMES, pos_ms(p));

            if (n < 0) {
                fail_event(dec_code((int)n), n == MP3_DEC_E_IO ? "the file could not be read any more"
                                                               : "the file stopped decoding");
                rc = dec_exit((int)n);
                break;
            }
            if (n == 0) {
                if (pocketaudio_drain(s, DRAIN_MS) != POCKETAUDIO_OK) {
                    fail_event(MP3_ERR_AUDIO, pocketaudio_last_error(s));
                    rc = MP3_EXIT_AUDIO;
                } else {
                    emit("progress %lld", (long long)(p->info.total_ms > 0 ? p->info.total_ms : pos_ms(p)));
                    emit("played");
                }
                break;
            }
            apply_gain(p->buf, (size_t)n, p->gain);
            p->len = (size_t)n;
            p->off = 0;
        }
        w = pocketaudio_write(s, p->buf + p->off, p->len - p->off);
        if (w < 0) {
            fail_event(MP3_ERR_AUDIO, pocketaudio_last_error(s));
            rc = MP3_EXIT_AUDIO;
            break;
        }
        if (w == 0) {
            if (++p->stalls >= STALL_WAITS) {
                fail_event(MP3_ERR_AUDIO, "the speaker stopped accepting samples");
                rc = MP3_EXIT_AUDIO;
                break;
            }
            continue;
        }
        p->stalls = 0;
        p->off += (size_t)w;
        p->heard += (uint64_t)w;
        if (mono_ms() - last_progress >= MP3_PROGRESS_EVERY_MS) {
            emit("progress %lld", (long long)pos_ms(p));
            last_progress = mono_ms();
        }
    }
    pocketaudio_close(s);
    mp3_decoder_close(p->dec);
    free(p);
    if (rc == MP3_EXIT_OK && stopping()) {
        emit("stopped");
    }
    return rc;
}

/* ---- recover ------------------------------------------------------------ */

static int cmd_recover(void)
{
    struct pocketaudio_options ao;
    char report[200];
    int rc;

    memset(&ao, 0, sizeof(ao));
    ao.backend = backend_for();
    rc = pocketaudio_recover(&ao, report, sizeof(report));
    if (rc == 1) {
        emit("recovered");
    }
    if (rc < 0 && rc != POCKETAUDIO_E_BUSY) {
        fail_event(audio_code(rc), report);
        return MP3_EXIT_AUDIO;
    }
    return MP3_EXIT_OK;
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
#ifdef POS_MP3_TEST_HOOKS
    install_hooks();
#endif

    if (argc < 2) {
        usage();
        return MP3_EXIT_USAGE;
    }
    if (strcmp(argv[1], "info") == 0) {
        return cmd_info();
    }
    if (strcmp(argv[1], "recover") == 0) {
        return cmd_recover();
    }
    if (parse_opts(argc, argv, &o) != 0) {
        return MP3_EXIT_USAGE;
    }
    if (strcmp(argv[1], "probe") == 0) {
        return cmd_probe(&o);
    }
    if (strcmp(argv[1], "play") == 0) {
        return cmd_play(&o);
    }
    usage();
    return MP3_EXIT_USAGE;
}
