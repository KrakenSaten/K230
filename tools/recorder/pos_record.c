/*
 * pos-record: the Recorder's audio helper, and the only program that opens
 * the audio device or writes a recording for it.
 *
 *   pos-record info
 *       The board, the formats and the limits. Opens nothing.
 *   pos-record record [--rate 16000|48000] [--seconds N] [--events]
 *                     [--allow-unverified] DIR/NAME.wav
 *       The microphone to DIR/NAME.wav (written as NAME.wav.part and renamed
 *       when complete; rec_file.h). NAME must be a recording name
 *       (apps/recorder/rec_names.h), so nothing outside the naming scheme,
 *       and nothing that exists, is ever written. Mono, 16-bit. 16000 is
 *       the Voice preset (the default), 48000 Standard. N (1..86400) stops
 *       the recording by itself.
 *   pos-record play [--start-ms MS] [--volume-percent L] [--events]
 *                   [--allow-unverified] FILE.wav
 *       A WAV to the speaker, from MS; L is the system volume, 1..100.
 *   pos-record recover [--events] [--dir DIR]
 *       Undo what a helper that died without closing left switched
 *       (pocketaudio_recover), then, with --dir, repair every interrupted
 *       recording in DIR (rec_file_recover_dir).
 *
 * Events and commands are apps/recorder/rec_protocol.h. With --events,
 * stdin carries commands and its end is a stop (the app is gone); without
 * it stdin is not read, and SIGINT or SIGTERM stops.
 *
 * STOPPING. SIGTERM, SIGINT, "stop" and a closed stdin all end the loop
 * within one audio wait (POCKETAUDIO_MAX_WAIT_MS). A recording then closes
 * the microphone first and finalizes the file second, so the microphone is
 * never on for the time the storage takes. A helper killed outright leaves
 * its .part for the next `recover --dir` and its audio route for the next
 * audio open, as pocketaudio.h describes.
 *
 * PAUSING closes the device (the microphone is off, the amplifier is off)
 * and keeps the file or the position; resuming opens it again, and a
 * recording discards the codec's start-up transient again inside
 * pocketaudio. A resume that cannot get the device stays paused and says
 * why.
 *
 * STORAGE. A recording does not start with less than REC_RESERVE_BYTES plus
 * REC_START_MARGIN_S of audio free, and stops itself ("limit space") once
 * the filesystem is down to the reserve; it also stops at the WAV size limit
 * ("limit length"). A full disk or a failing write finalizes what was
 * written. Nothing is logged: no names, no audio.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketaudio/pocketaudio.h"
#include "pocketwav/pocketwav.h"
#include "rec_engine.h"
#include "rec_file.h"
#include "rec_names.h"
#include "rec_protocol.h"
#ifdef POS_RECORD_TEST_HOOKS
/* tests/pos-record-testhooks only: POS_RECORD_FAKE_AUDIO=<dir> replaces the
 * sound card with files (tests/fake_audio_backend.c), POS_RECORD_FREE_FILE
 * names a file holding the free bytes to report, POS_RECORD_FAIL_AFTER makes
 * the disk full after that many data bytes. The shipped helper has none. */
#include "fake_audio_backend.h"
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK_SPACE_EVERY_MS 1000
#define MAX_SECONDS 86400
#define PAUSED_WAIT_MS 200

static volatile sig_atomic_t stop_requested;
static int events_mode;
static int parent_gone;
static int paused;
static int resume_wanted;
static int pause_wanted;
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
            "usage: pos-record info\n"
            "       pos-record record [--rate 16000|48000] [--seconds N] [--events]\n"
            "                         [--allow-unverified] DIR/NAME.wav\n"
            "       pos-record play [--start-ms MS] [--volume-percent L] [--events]\n"
            "                       [--allow-unverified] FILE.wav\n"
            "       pos-record recover [--events] [--dir DIR]\n");
}

/* ---- options ------------------------------------------------------------ */

struct opts {
    int rate;
    int seconds;
    long long start_ms;
    int volume_percent;
    int allow_unverified;
    const char *dir;
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
    o->rate = REC_RATE_VOICE;
    o->volume_percent = 100;
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        long long n;

        if (strcmp(a, "--events") == 0) {
            events_mode = 1;
        } else if (strcmp(a, "--allow-unverified") == 0) {
            o->allow_unverified = 1;
        } else if (strcmp(a, "--rate") == 0 && v) {
            if (parse_ll(v, 0, 1000000, &n) != 0 || (n != REC_RATE_VOICE && n != REC_RATE_STANDARD)) {
                fail_event(REC_ERR_USAGE, "rate must be 16000 or 48000");
                return -1;
            }
            o->rate = (int)n;
            i++;
        } else if (strcmp(a, "--seconds") == 0 && v) {
            if (parse_ll(v, 1, MAX_SECONDS, &n) != 0) {
                fail_event(REC_ERR_USAGE, "seconds must be 1 to 86400");
                return -1;
            }
            o->seconds = (int)n;
            i++;
        } else if (strcmp(a, "--start-ms") == 0 && v) {
            if (parse_ll(v, 0, 1000LL * 3600 * 48, &o->start_ms) != 0) {
                fail_event(REC_ERR_USAGE, "start-ms out of range");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--volume-percent") == 0 && v) {
            if (parse_ll(v, 1, 100, &n) != 0) {
                fail_event(REC_ERR_USAGE, "volume-percent must be 1 to 100");
                return -1;
            }
            o->volume_percent = (int)n;
            i++;
        } else if (strcmp(a, "--dir") == 0 && v) {
            o->dir = v;
            i++;
        } else if (a[0] != '-' && !o->path) {
            o->path = a;
        } else {
            fail_event(REC_ERR_USAGE, "unknown argument");
            return -1;
        }
    }
    return 0;
}

/* ---- commands on stdin -------------------------------------------------- */

static void dispatch(const char *line)
{
    if (strcmp(line, "stop") == 0) {
        stop_requested = 1;
    } else if (strcmp(line, "pause") == 0) {
        pause_wanted = 1;
        resume_wanted = 0;
    } else if (strcmp(line, "resume") == 0) {
        resume_wanted = 1;
        pause_wanted = 0;
    }
    /* Anything else is ignored: a newer app may say more. */
}

/* Read whatever commands are waiting, waiting at most wait_ms for one. */
static void read_commands(int wait_ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    char buf[64];
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
    case POCKETAUDIO_E_DISABLED: return REC_ERR_AUDIO_DISABLED;
    case POCKETAUDIO_E_BUSY: return REC_ERR_AUDIO_BUSY;
    case POCKETAUDIO_E_NODEV: return REC_ERR_AUDIO_NODEV;
    default: return REC_ERR_AUDIO;
    }
}

/* Whether POCKETOS_AUDIO_ALLOW_UNVERIFIED names this direction (the same
 * rule as pos-wave's: "capture", "playback" or both, comma-separated). */
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
#ifdef POS_RECORD_TEST_HOOKS
    const char *fake = getenv("POS_RECORD_FAKE_AUDIO");

    if (fake && *fake) {
        return fake_audio_backend(fake);
    }
#endif
    return pocketaudio_alsa_backend();
}

static void board_for(struct pocketaudio_board *b)
{
#ifdef POS_RECORD_TEST_HOOKS
    if (getenv("POS_RECORD_FAKE_AUDIO") && *getenv("POS_RECORD_FAKE_AUDIO")) {
        *b = *fake_audio_board();
        return;
    }
#endif
    pocketaudio_board_detect(b, NULL);
}

/* Open the device. NULL after an error event. */
static struct pocketaudio_stream *open_audio(enum pocketaudio_dir dir, const struct opts *o,
                                             struct pocketaudio_board *b)
{
    struct pocketaudio_options ao;
    struct pocketaudio_stream *s = NULL;
    char err[200];
    int rc;

    board_for(b);
    memset(&ao, 0, sizeof(ao));
    ao.backend = backend_for();
    ao.board = b;
    ao.allow_unverified = o->allow_unverified || env_allows(dir);
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

/* ---- info --------------------------------------------------------------- */

static int cmd_info(void)
{
    struct pocketaudio_board b;

    board_for(&b);
    printf("board %s\n", b.name);
    printf("pcm %s\n", b.pcm);
    printf("device_rate %d\n", POCKETAUDIO_RATE);
    printf("wire_channels %u\n", b.channels);
    printf("mic_channel %u\n", b.capture_channel);
    printf("capture %s\n", b.capture_verified ? "validated" : "not validated (gated)");
    printf("playback %s\n", b.playback_verified ? "validated" : "not validated (gated)");
    printf("capture_settle_ms %u\n", (unsigned)(b.capture_settle_frames * 1000ULL / POCKETAUDIO_RATE));
    printf("formats voice=%d standard=%d mono s16le wav\n", REC_RATE_VOICE, REC_RATE_STANDARD);
    printf("max_seconds voice=%llu standard=%llu\n",
           (unsigned long long)(POCKETWAV_MAX_DATA_BYTES / 2u / REC_RATE_VOICE),
           (unsigned long long)(POCKETWAV_MAX_DATA_BYTES / 2u / REC_RATE_STANDARD));
    printf("reserve_bytes %llu\n", (unsigned long long)REC_RESERVE_BYTES);
    printf("playback_peak_ceiling %d\n", POCKETAUDIO_PEAK_CEILING);
    return REC_EXIT_OK;
}

/* ---- record ------------------------------------------------------------- */

struct ticker {
    int64_t level;
    int64_t progress;
    int64_t space;
    int64_t checkpoint;
};

static void split_path(const char *path, char *dir, size_t dlen, const char **base)
{
    const char *slash = strrchr(path, '/');

    if (!slash) {
        snprintf(dir, dlen, ".");
        *base = path;
    } else if (slash == path) {
        snprintf(dir, dlen, "/");
        *base = slash + 1;
    } else {
        snprintf(dir, dlen, "%.*s", (int)(slash - path), path);
        *base = slash + 1;
    }
}

static int storage_code(int err)
{
    return err == ENOSPC || err == EDQUOT;
}

static int cmd_record(const struct opts *o)
{
    char dir[REC_FILE_PATH_MAX];
    char final[REC_NAME_MAX];
    const char *base;
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    struct rec_recorder *r;
    struct ticker t;
    int64_t free_bytes;
    int64_t end_ms = 0;
    uint64_t need;
    const char *why = NULL; /* the limit that ended it, if one did */
    int rc = REC_EXIT_OK;
    int e;

    if (!o->path || strlen(o->path) >= sizeof(dir)) {
        fail_event(REC_ERR_USAGE, "record needs DIR/NAME.wav");
        return REC_EXIT_USAGE;
    }
    split_path(o->path, dir, sizeof(dir), &base);
    if (!rec_name_parse(base, NULL)) {
        fail_event(REC_ERR_USAGE, "not a recording name");
        return REC_EXIT_USAGE;
    }
    need = REC_RESERVE_BYTES + (uint64_t)o->rate * 2u * REC_START_MARGIN_S;
    free_bytes = rec_free_bytes(dir);
    if (free_bytes < 0) {
        fail_event(REC_ERR_STORAGE, strerror((int)-free_bytes));
        return REC_EXIT_STORAGE;
    }
    if ((uint64_t)free_bytes < need) {
        fail_event(REC_ERR_STORAGE_FULL, "not enough free storage to record");
        return REC_EXIT_STORAGE;
    }
    r = malloc(sizeof(*r));
    if (!r) {
        fail_event(REC_ERR_STORAGE, "out of memory");
        return REC_EXIT_FAILED;
    }
    /* The device first: a busy or missing microphone leaves no file. */
    s = open_audio(POCKETAUDIO_CAPTURE, o, &b);
    if (!s) {
        free(r);
        return REC_EXIT_AUDIO;
    }
    e = rec_recorder_open(r, dir, base, (unsigned)o->rate);
    if (e != 0) {
        pocketaudio_close(s);
        fail_event(storage_code(-e) ? REC_ERR_STORAGE_FULL : REC_ERR_STORAGE,
                   e == -EEXIST ? "a recording with that name exists" : strerror(-e));
        free(r);
        return REC_EXIT_STORAGE;
    }
    emit("ready %s", b.name);
    emit("recording %d %s", o->rate, base);
    memset(&t, 0, sizeof(t));
    t.level = t.progress = t.space = t.checkpoint = mono_ms();
    if (o->seconds) {
        end_ms = mono_ms() + (int64_t)o->seconds * 1000;
    }

    while (!stopping()) {
        enum rec_step st;
        int64_t now;

        read_commands(paused ? PAUSED_WAIT_MS : 0);
        if (stopping()) {
            break;
        }
        if (pause_wanted && !paused) {
            pause_wanted = 0;
            r->gaps += pocketaudio_xruns(s);
            pocketaudio_close(s);
            s = NULL;
            paused = 1;
            rec_file_checkpoint(&r->file);
            emit("paused");
        }
        if (resume_wanted && paused) {
            resume_wanted = 0;
            s = open_audio(POCKETAUDIO_CAPTURE, o, &b);
            if (s) {
                paused = 0;
                rec_meter_reset(&r->meter);
                emit("resumed");
            }
        }
        if (paused) {
            continue;
        }
        st = rec_recorder_step(r, s);
        if (st == REC_STEP_AUDIO_ERROR) {
            fail_event(REC_ERR_AUDIO, r->stalls >= REC_STALL_WAITS
                                          ? "the microphone stopped delivering samples"
                                          : pocketaudio_last_error(s));
            rc = REC_EXIT_AUDIO;
            break;
        }
        if (st == REC_STEP_STORAGE_FULL) {
            why = "space";
            break;
        }
        if (st == REC_STEP_STORAGE_ERROR) {
            fail_event(REC_ERR_STORAGE, strerror(r->last_errno));
            rc = REC_EXIT_STORAGE;
            break;
        }
        if (st == REC_STEP_LIMIT_LENGTH) {
            why = "length";
            break;
        }
        now = mono_ms();
        if (now - t.level >= REC_LEVEL_EVERY_MS) {
            int peak;
            int rms;

            rec_meter_read(&r->meter, &peak, &rms);
            rec_meter_reset(&r->meter);
            emit("level %d %d", peak, rms);
            t.level = now;
        }
        if (now - t.progress >= REC_PROGRESS_EVERY_MS) {
            emit("progress %llu %llu", (unsigned long long)rec_recorder_ms(r),
                 (unsigned long long)(POCKETWAV_HEADER_BYTES + r->file.data_bytes));
            t.progress = now;
        }
        if (now - t.checkpoint >= REC_FILE_CHECKPOINT_MS) {
            e = rec_file_checkpoint(&r->file);
            if (e != 0) {
                r->last_errno = -e;
                fail_event(storage_code(-e) ? REC_ERR_STORAGE_FULL : REC_ERR_STORAGE, strerror(-e));
                rc = REC_EXIT_STORAGE;
                break;
            }
            t.checkpoint = now;
        }
        if (now - t.space >= CHECK_SPACE_EVERY_MS) {
            free_bytes = rec_free_bytes(dir);
            if (free_bytes >= 0 && (uint64_t)free_bytes <= REC_RESERVE_BYTES) {
                why = "space";
                break;
            }
            t.space = now;
        }
        if (end_ms && now >= end_ms) {
            why = "time";
            break;
        }
    }
    /* The microphone goes off before the storage is waited on. */
    if (s) {
        r->gaps += pocketaudio_xruns(s);
    }
    pocketaudio_close(s);
    if (why) {
        emit("limit %s", why);
    }
    {
        uint64_t ms = rec_recorder_ms(r);
        uint64_t bytes = POCKETWAV_HEADER_BYTES + r->file.data_bytes;
        e = rec_file_finish(&r->file, final, sizeof(final));
        if (e == 0) {
            emit("saved %llu %llu %u %s", (unsigned long long)ms, (unsigned long long)bytes,
                 r->gaps, final);
        } else if (e == -ENODATA) {
            emit("empty");
        } else {
            fail_event(storage_code(-e) ? REC_ERR_STORAGE_FULL : REC_ERR_STORAGE, strerror(-e));
            emit("kept %s", base);
            if (rc == REC_EXIT_OK) {
                rc = REC_EXIT_STORAGE;
            }
        }
    }
    free(r);
    if (rc == REC_EXIT_OK && stopping()) {
        emit("stopped");
    }
    return rc;
}

/* ---- play --------------------------------------------------------------- */

static int cmd_play(const struct opts *o)
{
    struct pocketaudio_board b;
    struct pocketaudio_stream *s;
    struct rec_player *p;
    struct ticker t;
    int rc = REC_EXIT_OK;
    int e;

    if (!o->path) {
        fail_event(REC_ERR_USAGE, "play needs a file");
        return REC_EXIT_USAGE;
    }
    p = malloc(sizeof(*p));
    if (!p) {
        fail_event(REC_ERR_AUDIO, "out of memory");
        return REC_EXIT_FAILED;
    }
    /* The file first: one that cannot be played never enables the amplifier. */
    e = rec_player_open(p, o->path, (uint64_t)o->start_ms);
    if (e != POCKETWAV_OK) {
        fail_event(e == POCKETWAV_E_IO ? REC_ERR_STORAGE : REC_ERR_FORMAT,
                   e == POCKETWAV_E_UNSUPPORTED ? "only 16 kHz and 48 kHz 16-bit PCM WAV files play"
                                                : pocketwav_strerror(e));
        free(p);
        return REC_EXIT_FAILED;
    }
    s = open_audio(POCKETAUDIO_PLAYBACK, o, &b);
    if (!s) {
        rec_player_close(p);
        free(p);
        return REC_EXIT_AUDIO;
    }
    emit("ready %s", b.name);
    emit("playing %llu %u %u", (unsigned long long)rec_player_total_ms(p), p->info.rate,
         p->info.channels);
    memset(&t, 0, sizeof(t));
    t.level = t.progress = mono_ms();
    while (!stopping()) {
        enum rec_step st;
        int64_t now;

        read_commands(paused ? PAUSED_WAIT_MS : 0);
        if (stopping()) {
            break;
        }
        if (pause_wanted && !paused) {
            pause_wanted = 0;
            pocketaudio_close(s); /* amplifier off first, inside pocketaudio */
            s = NULL;
            paused = 1;
            emit("paused");
        }
        if (resume_wanted && paused) {
            resume_wanted = 0;
            s = open_audio(POCKETAUDIO_PLAYBACK, o, &b);
            if (s) {
                paused = 0;
                emit("resumed");
            }
        }
        if (paused) {
            continue;
        }
        st = rec_player_step(p, s);
        if (st == REC_STEP_END) {
            if (pocketaudio_drain(s, 1000) != POCKETAUDIO_OK) {
                fail_event(REC_ERR_AUDIO, pocketaudio_last_error(s));
                rc = REC_EXIT_AUDIO;
            } else {
                emit("progress %llu 0", (unsigned long long)rec_player_total_ms(p));
                emit("played");
            }
            break;
        }
        if (st == REC_STEP_AUDIO_ERROR) {
            fail_event(REC_ERR_AUDIO, p->stalls >= REC_STALL_WAITS
                                          ? "the speaker stopped accepting samples"
                                          : pocketaudio_last_error(s));
            rc = REC_EXIT_AUDIO;
            break;
        }
        if (st == REC_STEP_FILE_ERROR) {
            fail_event(REC_ERR_STORAGE, "the recording could not be read");
            rc = REC_EXIT_STORAGE;
            break;
        }
        now = mono_ms();
        if (now - t.level >= REC_LEVEL_EVERY_MS) {
            int peak;
            int rms;

            rec_meter_read(&p->meter, &peak, &rms);
            rec_meter_reset(&p->meter);
            emit("level %d %d", peak, rms);
            t.level = now;
        }
        if (now - t.progress >= REC_PROGRESS_EVERY_MS) {
            emit("progress %llu 0", (unsigned long long)rec_player_pos_ms(p));
            t.progress = now;
        }
    }
    pocketaudio_close(s);
    rec_player_close(p);
    free(p);
    if (rc == REC_EXIT_OK && stopping()) {
        emit("stopped");
    }
    return rc;
}

/* ---- recover ------------------------------------------------------------ */

static void on_recovered(enum rec_recover_kind kind, const char *name, void *user)
{
    (void)user;
    if (kind == REC_RECOVER_REPAIRED) {
        emit("repaired %s", name);
    } else if (kind == REC_RECOVER_DAMAGED) {
        emit("damaged %s", name);
    }
}

static int cmd_recover(const struct opts *o)
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
    /* A busy lock means a live owner: its audio state is its own, and the
     * recordings are still worth repairing. */
    if (o->dir) {
        int n = rec_file_recover_dir(o->dir, on_recovered, NULL);

        if (n < 0 && n != -ENOENT) {
            fail_event(REC_ERR_STORAGE, strerror(-n));
            return REC_EXIT_STORAGE;
        }
    }
    if (rc < 0 && rc != POCKETAUDIO_E_BUSY) {
        fail_event(audio_code(rc), report);
        return REC_EXIT_AUDIO;
    }
    return REC_EXIT_OK;
}

/* ---- test hooks --------------------------------------------------------- */

#ifdef POS_RECORD_TEST_HOOKS
static int64_t free_from_file(const char *dir)
{
    const char *path = getenv("POS_RECORD_FREE_FILE");
    long long v = -1;
    FILE *f;

    (void)dir;
    f = path ? fopen(path, "r") : NULL;
    if (!f) {
        return -EIO;
    }
    if (fscanf(f, "%lld", &v) != 1) {
        v = -EIO;
    }
    fclose(f);
    return v;
}

static void install_hooks(void)
{
    const char *fail = getenv("POS_RECORD_FAIL_AFTER");

    if (getenv("POS_RECORD_FREE_FILE")) {
        rec_free_hook = free_from_file;
    }
    if (fail && *fail) {
        rec_file_fail_after = strtoll(fail, NULL, 10);
    }
}
#endif

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
    /* Recordings are somebody's words: nothing this process creates is
     * readable by anyone else, whatever umask it was started with. */
    umask(077);
#ifdef POS_RECORD_TEST_HOOKS
    install_hooks();
#endif

    if (argc < 2) {
        usage();
        return REC_EXIT_USAGE;
    }
    if (strcmp(argv[1], "info") == 0) {
        return cmd_info();
    }
    if (parse_opts(argc, argv, &o) != 0) {
        return REC_EXIT_USAGE;
    }
    if (strcmp(argv[1], "record") == 0) {
        return cmd_record(&o);
    }
    if (strcmp(argv[1], "play") == 0) {
        return cmd_play(&o);
    }
    if (strcmp(argv[1], "recover") == 0) {
        return o.path ? (fail_event(REC_ERR_USAGE, "recover takes no file"), REC_EXIT_USAGE)
                      : cmd_recover(&o);
    }
    usage();
    return REC_EXIT_USAGE;
}
