/*
 * pocketaudio: ownership, routing, amplifier and level policy over a backend.
 * See pocketaudio.h for what is guaranteed; this file is where it happens.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketaudio.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* The T-Display K230 (docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md).
 *
 * The card is named by its id rather than its index so a second card (a USB
 * dongle) cannot silently take hw:0. Its I2S link is stereo only
 * (canaan-dwc-i2s.c accepts 2/4/6/8 channels), S16_LE is the one format the
 * vendor's own test path uses, and the capture's right slot is the codec's
 * right ADC, where the schematic puts the on-board microphone (MIC_PR).
 *
 * The route: "External I2S Output Switch" on sends the SoC's I2S to the pads
 * (IO32 BCLK, IO33 WS, IO35 data) that the vendor's device tree names
 * amp_i2s_pins, and bypasses the codec; off feeds the codec, which is the only
 * way to capture from the analog microphones. The amplifier enable is IO34,
 * gpiochip1 line 2, active high, as the vendor launcher drives it
 * (ui_hardware.c amp_gpio_request_line). Both paths are DOCUMENTED from
 * vendor sources and not yet VERIFIED, so both are gated. */
static const struct pocketaudio_board board_k230 = {
    .name = "k230-t-display",
    .pcm = "hw:CARD=K230I2SINNO,DEV=0",
    .ctl = "hw:CARD=K230I2SINNO",
    .channels = 2,
    .capture_channel = 1,
    .route_control = "External I2S Output Switch",
    .route_playback = 1,
    .route_capture = 0,
    .amp_chip = "/dev/gpiochip1",
    .amp_line = 2,
    .amp_active_high = 1,
    .playback_verified = 0,
    .capture_verified = 0,
};

/* Any other Linux machine: ALSA's default device converts mono for us, and
 * there is nothing to route or switch. This is what host tests run against,
 * with POCKETOS_AUDIO_PCM=null. */
static const struct pocketaudio_board board_generic = {
    .name = "generic",
    .pcm = "default",
    .ctl = NULL,
    .channels = 1,
    .capture_channel = 0,
    .route_control = NULL,
    .route_playback = 0,
    .route_capture = 0,
    .amp_chip = NULL,
    .amp_line = 0,
    .amp_active_high = 1,
    .playback_verified = 1,
    .capture_verified = 1,
};

#define K230_CARD_ID "K230I2SINNO"
#define DRAIN_MAX_MS 10000

struct pocketaudio_stream {
    const struct pocketaudio_backend *be;
    struct pocketaudio_board board;
    enum pocketaudio_dir dir;
    void *pcm;
    int lock_fd;
    int route_saved; /* the route found at open, or -1 when it was left alone */
    int amp;         /* the enable line's handle, or -1 */
    int peak_limit;
    unsigned xruns;
    int16_t wire[POCKETAUDIO_PERIOD_FRAMES * POCKETAUDIO_MAX_CHANNELS];
    char error[160];
};

const struct pocketaudio_board *pocketaudio_board_k230(void) { return &board_k230; }
const struct pocketaudio_board *pocketaudio_board_generic(void) { return &board_generic; }

void pocketaudio_board_detect(struct pocketaudio_board *board, const char *proc_root)
{
    const char *force = getenv("POCKETOS_AUDIO_BOARD");
    const char *pcm = getenv("POCKETOS_AUDIO_PCM");
    char path[POCKETOS_PATH_MAX];
    char id[64] = "";
    FILE *f;

    *board = board_generic;
    if (!(force && strcmp(force, "generic") == 0)) {
        snprintf(path, sizeof(path), "%s/asound/card0/id", proc_root ? proc_root : "/proc");
        f = fopen(path, "re");
        if (f) {
            if (fgets(id, sizeof(id), f)) {
                id[strcspn(id, "\r\n")] = '\0';
            }
            fclose(f);
        }
        if (strcmp(id, K230_CARD_ID) == 0) {
            *board = board_k230;
        }
    }
    if (pcm && *pcm) {
        board->pcm = pcm;
    }
}

const char *pocketaudio_strerror(int err)
{
    switch (err) {
    case POCKETAUDIO_OK: return "ok";
    case POCKETAUDIO_E_INVAL: return "invalid argument";
    case POCKETAUDIO_E_BUSY: return "audio is in use";
    case POCKETAUDIO_E_NODEV: return "no audio device";
    case POCKETAUDIO_E_FORMAT: return "audio format not supported";
    case POCKETAUDIO_E_ROUTE: return "audio route could not be set";
    case POCKETAUDIO_E_IO: return "audio device error";
    case POCKETAUDIO_E_TIMEOUT: return "audio device timed out";
    case POCKETAUDIO_E_DISABLED: return "audio path not validated on this hardware";
    case POCKETAUDIO_E_LOCK: return "audio lock unavailable";
    default: return "unknown audio error";
    }
}

int pocketaudio_peak(const int16_t *samples, size_t n)
{
    int peak = 0;
    size_t i;

    for (i = 0; samples && i < n; i++) {
        int v = samples[i] < 0 ? -(int)samples[i] : samples[i];

        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static void say(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;

    if (!buf || len == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

static int map_open_errno(int e)
{
    switch (e) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
        return POCKETAUDIO_E_NODEV;
    case EBUSY:
        return POCKETAUDIO_E_BUSY;
    case EINVAL:
        return POCKETAUDIO_E_FORMAT;
    default:
        return POCKETAUDIO_E_IO;
    }
}

static int board_valid(const struct pocketaudio_board *b)
{
    return b && b->pcm && b->channels >= 1 && b->channels <= POCKETAUDIO_MAX_CHANNELS &&
           b->capture_channel < b->channels && (!b->route_control || b->ctl);
}

static int take_lock(struct pocketaudio_stream *s, const char *dir, char *err, size_t errlen)
{
    char path[POCKETOS_PATH_MAX];

    if (!dir) {
        dir = pocketos_runtime_dir();
    }
    if (pocketos_mkdir_p(dir, 0755) != 0) {
        say(err, errlen, "cannot create %s: %s", dir, strerror(errno));
        return POCKETAUDIO_E_LOCK;
    }
    if (snprintf(path, sizeof(path), "%s/audio.lock", dir) >= (int)sizeof(path)) {
        say(err, errlen, "lock path too long");
        return POCKETAUDIO_E_LOCK;
    }
    s->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (s->lock_fd < 0) {
        say(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return POCKETAUDIO_E_LOCK;
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;

        close(s->lock_fd);
        s->lock_fd = -1;
        if (e == EWOULDBLOCK) {
            say(err, errlen, "another audio stream is open");
            return POCKETAUDIO_E_BUSY;
        }
        say(err, errlen, "cannot lock %s: %s", path, strerror(e));
        return POCKETAUDIO_E_LOCK;
    }
    return POCKETAUDIO_OK;
}

static void release(struct pocketaudio_stream *s)
{
    if (s->amp >= 0) {
        /* Silence first: the speaker goes quiet before the stream stops, so
         * whatever the PCM does as it closes is not amplified. */
        s->be->gpio_set(s->amp, s->board.amp_active_high ? 0 : 1);
        s->be->gpio_release(s->amp);
        s->amp = -1;
    }
    if (s->pcm) {
        s->be->pcm_close(s->pcm);
        s->pcm = NULL;
    }
    if (s->route_saved >= 0) {
        s->be->ctl_set_bool(s->board.ctl, s->board.route_control, s->route_saved);
        s->route_saved = -1;
    }
    if (s->lock_fd >= 0) {
        close(s->lock_fd); /* releases the flock */
        s->lock_fd = -1;
    }
}

int pocketaudio_open(struct pocketaudio_stream **out, enum pocketaudio_dir dir,
                     const struct pocketaudio_options *options, char *err, size_t errlen)
{
    const struct pocketaudio_options none = { 0 };
    const struct pocketaudio_options *o = options ? options : &none;
    struct pocketaudio_stream *s;
    int capture = dir == POCKETAUDIO_CAPTURE;
    int rc;
    int e = 0;
    int verified;
    char msg[128] = "";

    say(err, errlen, "%s", "");
    if (!out || (dir != POCKETAUDIO_PLAYBACK && dir != POCKETAUDIO_CAPTURE) || o->peak_limit < 0 ||
        o->peak_limit > POCKETAUDIO_PEAK_CEILING) {
        if (out) {
            *out = NULL;
        }
        say(err, errlen, "invalid audio request");
        return POCKETAUDIO_E_INVAL;
    }
    *out = NULL;

    s = calloc(1, sizeof(*s));
    if (!s) {
        say(err, errlen, "out of memory");
        return POCKETAUDIO_E_IO;
    }
    s->be = o->backend;
    s->dir = dir;
    s->lock_fd = -1;
    s->route_saved = -1;
    s->amp = -1;
    s->peak_limit = o->peak_limit ? o->peak_limit : POCKETAUDIO_PEAK_CEILING;
    if (o->board) {
        s->board = *o->board;
    } else {
        pocketaudio_board_detect(&s->board, NULL);
    }
    if (!s->be || !board_valid(&s->board)) {
        free(s);
        say(err, errlen, "invalid audio board description");
        return POCKETAUDIO_E_INVAL;
    }

    verified = capture ? s->board.capture_verified : s->board.playback_verified;
    if (!verified && !o->allow_unverified) {
        say(err, errlen, "%s %s is not validated on hardware", s->board.name,
            capture ? "microphone capture" : "speaker playback");
        free(s);
        return POCKETAUDIO_E_DISABLED;
    }

    rc = take_lock(s, o->lock_dir, err, errlen);
    if (rc != POCKETAUDIO_OK) {
        free(s);
        return rc;
    }

    if (s->board.route_control) {
        int want = capture ? s->board.route_capture : s->board.route_playback;
        int have = 0;

        e = s->be->ctl_get_bool(s->board.ctl, s->board.route_control, &have);
        if (e < 0) {
            say(err, errlen, "cannot read %s: %s", s->board.route_control, strerror(-e));
            /* No card at all reads as a missing device, not a routing fault. */
            rc = (e == -ENODEV || e == -ENOENT) ? POCKETAUDIO_E_NODEV : POCKETAUDIO_E_ROUTE;
            goto fail;
        }
        have = have ? 1 : 0;
        if (have != (want ? 1 : 0)) {
            e = s->be->ctl_set_bool(s->board.ctl, s->board.route_control, want ? 1 : 0);
            if (e < 0) {
                say(err, errlen, "cannot set %s: %s", s->board.route_control, strerror(-e));
                rc = POCKETAUDIO_E_ROUTE;
                goto fail;
            }
            s->route_saved = have;
        }
    }

    s->pcm = s->be->pcm_open(s->board.pcm, capture, POCKETAUDIO_RATE, s->board.channels,
                             POCKETAUDIO_PERIOD_FRAMES,
                             capture ? POCKETAUDIO_CAPTURE_BUFFER_FRAMES
                                     : POCKETAUDIO_PLAYBACK_BUFFER_FRAMES,
                             &e, msg, sizeof(msg));
    if (!s->pcm) {
        rc = map_open_errno(e < 0 ? -e : e);
        say(err, errlen, "cannot open %s: %s", s->board.pcm, msg[0] ? msg : strerror(e < 0 ? -e : e));
        goto fail;
    }

    /* The amplifier last, after the PCM is prepared: nothing reaches the
     * speaker before there is a stream to play. */
    if (!capture && s->board.amp_chip) {
        s->amp = s->be->gpio_request_output(s->board.amp_chip, s->board.amp_line,
                                            s->board.amp_active_high ? 1 : 0);
        if (s->amp < 0) {
            say(err, errlen, "cannot enable the amplifier (%s line %u): %s", s->board.amp_chip,
                s->board.amp_line, strerror(-s->amp));
            s->amp = -1;
            rc = POCKETAUDIO_E_ROUTE;
            goto fail;
        }
    }

    *out = s;
    return POCKETAUDIO_OK;

fail:
    release(s);
    free(s);
    return rc;
}

static long io_result(struct pocketaudio_stream *s, long r, const char *what)
{
    if (r >= 0) {
        return r;
    }
    say(s->error, sizeof(s->error), "%s failed: %s", what, strerror((int)-r));
    return POCKETAUDIO_E_IO;
}

long pocketaudio_write(struct pocketaudio_stream *s, const int16_t *mono, size_t frames)
{
    size_t n;
    size_t i;
    unsigned c;
    long r;

    if (!s || s->dir != POCKETAUDIO_PLAYBACK || (!mono && frames)) {
        return POCKETAUDIO_E_INVAL;
    }
    n = frames < POCKETAUDIO_PERIOD_FRAMES ? frames : POCKETAUDIO_PERIOD_FRAMES;
    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int v = mono[i];

        if (v > s->peak_limit) {
            v = s->peak_limit;
        } else if (v < -s->peak_limit) {
            v = -s->peak_limit;
        }
        for (c = 0; c < s->board.channels; c++) {
            s->wire[i * s->board.channels + c] = (int16_t)v;
        }
    }
    r = s->be->pcm_write(s->pcm, s->wire, n, POCKETAUDIO_MAX_WAIT_MS);
    if (r == -EPIPE) {
        /* An underrun, already recovered by the backend. Try once more, in
         * the same bounded wait. */
        s->xruns++;
        r = s->be->pcm_write(s->pcm, s->wire, n, POCKETAUDIO_MAX_WAIT_MS);
        if (r == -EPIPE) {
            s->xruns++;
            r = 0;
        }
    }
    return io_result(s, r, "playback");
}

long pocketaudio_read(struct pocketaudio_stream *s, int16_t *mono, size_t frames)
{
    size_t n;
    size_t i;
    long r;

    if (!s || s->dir != POCKETAUDIO_CAPTURE || (!mono && frames)) {
        return POCKETAUDIO_E_INVAL;
    }
    n = frames < POCKETAUDIO_PERIOD_FRAMES ? frames : POCKETAUDIO_PERIOD_FRAMES;
    if (n == 0) {
        return 0;
    }
    r = s->be->pcm_read(s->pcm, s->wire, n, POCKETAUDIO_MAX_WAIT_MS);
    if (r == -EPIPE) {
        /* An overrun: some audio was lost, the stream runs on. The decoder
         * sees a gap, which it survives like any other noise. */
        s->xruns++;
        r = s->be->pcm_read(s->pcm, s->wire, n, POCKETAUDIO_MAX_WAIT_MS);
        if (r == -EPIPE) {
            s->xruns++;
            r = 0;
        }
    }
    r = io_result(s, r, "capture");
    if (r <= 0) {
        return r;
    }
    if ((size_t)r > n) {
        say(s->error, sizeof(s->error), "capture returned more than asked");
        return POCKETAUDIO_E_IO;
    }
    for (i = 0; i < (size_t)r; i++) {
        mono[i] = s->wire[i * s->board.channels + s->board.capture_channel];
    }
    return r;
}

int pocketaudio_drain(struct pocketaudio_stream *s, int timeout_ms)
{
    int r;

    if (!s || s->dir != POCKETAUDIO_PLAYBACK || timeout_ms < 0) {
        return POCKETAUDIO_E_INVAL;
    }
    if (timeout_ms > DRAIN_MAX_MS) {
        timeout_ms = DRAIN_MAX_MS;
    }
    r = s->be->pcm_drain(s->pcm, timeout_ms);
    if (r == -ETIMEDOUT) {
        say(s->error, sizeof(s->error), "drain did not finish in %d ms", timeout_ms);
        return POCKETAUDIO_E_TIMEOUT;
    }
    if (r < 0) {
        say(s->error, sizeof(s->error), "drain failed: %s", strerror(-r));
        return POCKETAUDIO_E_IO;
    }
    return POCKETAUDIO_OK;
}

void pocketaudio_close(struct pocketaudio_stream *s)
{
    if (!s) {
        return;
    }
    release(s);
    free(s);
}

unsigned pocketaudio_xruns(const struct pocketaudio_stream *s)
{
    return s ? s->xruns : 0;
}

const char *pocketaudio_last_error(const struct pocketaudio_stream *s)
{
    return s ? s->error : "";
}
