/*
 * pocketaudio: ownership, routing, amplifier and level policy over a backend.
 * See pocketaudio.h for what is guaranteed; this file is where it happens.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
#include <time.h>
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
 * (ui_hardware.c amp_gpio_request_line).
 *
 * K230 codec/capture startup transient: every capture on unit A began with
 * both slots at negative full scale for about 140 ms (right) and 210 ms
 * (left), settling within about a second; it is the codec's, not ggwave's.
 * 500 ms is discarded (AUDIO_HARDWARE_MAP §15).
 *
 * Both paths are VERIFIED on unit A (2026-09-13): the microphone decoded a
 * phone's ggwave message on the right slot, and the controlled first playback
 * (DOORS at -26 dBFS through the MAX98357A on the nRF52840 base board) was
 * heard, decoded by a phone, left the panel steady and IO34 low
 * (AUDIO_HARDWARE_MAP §16). Validation does not raise any limit: every
 * played sample is still clamped to POCKETAUDIO_PEAK_CEILING. */
#define K230_CAPTURE_SETTLE_FRAMES (POCKETAUDIO_RATE / 2)

static const struct pocketaudio_board board_k230 = {
    .name = "k230-t-display",
    .pcm = "hw:CARD=K230I2SINNO,DEV=0",
    .ctl = "hw:CARD=K230I2SINNO",
    .channels = 2,
    .capture_channel = 1,
    .capture_settle_frames = K230_CAPTURE_SETTLE_FRAMES,
    .route_control = "External I2S Output Switch",
    .route_playback = 1,
    .route_capture = 0,
    .amp_chip = "/dev/gpiochip1",
    .amp_line = 2,
    .amp_active_high = 1,
    .playback_verified = 1,
    .capture_verified = 1,
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
    .capture_settle_frames = 0,
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
#define RECOVERY_HEADER "pocketaudio-recovery 1"
#define RECOVERY_MAX_BYTES 1024
#define RECOVERY_NAME_MAX 128

struct pocketaudio_stream {
    const struct pocketaudio_backend *be;
    struct pocketaudio_board board;
    enum pocketaudio_dir dir;
    void *pcm;
    int lock_fd;
    char lock_dir[POCKETOS_PATH_MAX];
    int route_saved; /* the route found at open, or -1 when it was left alone */
    int amp;         /* the enable line's handle, or -1 */
    int record_written; /* a recovery record of ours is on disk */
    int record_route;   /* what that record says to restore the route to, or -1 */
    int recovered;      /* open found and undid a previous owner's leftovers */
    int peak_limit;
    int gain_q15;         /* playback: the system volume as a Q15 gain */
    unsigned settle_left; /* capture: startup frames still to be discarded */
    unsigned xruns;
    int16_t wire[POCKETAUDIO_PERIOD_FRAMES * POCKETAUDIO_MAX_CHANNELS];
    char error[160];
};

/* What a recovery record says (pocketaudio.h, "Recovery"). */
struct recovery {
    char ctl[RECOVERY_NAME_MAX];
    char route_control[RECOVERY_NAME_MAX];
    int route_restore; /* -1: the route was not changed */
    char amp_chip[64];
    unsigned amp_line;
    int amp_active_high;
    int amp_enabled;
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

int pocketaudio_volume_gain_q15(int percent)
{
    /* 10^(dB/20) x 32768 at 0, 10, ..., 100 percent, dB = 0.3 x percent - 30. */
    static const int table[11] = { 1036,  1464,  2068,  2920,  4125, 5827,
                                   8231, 11627, 16423, 23198, 32768 };
    int i;
    int frac;

    if (percent <= 0) {
        return table[0];
    }
    if (percent >= 100) {
        return table[10];
    }
    i = percent / 10;
    frac = percent % 10;
    return table[i] + (table[i + 1] - table[i]) * frac / 10;
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
           b->capture_channel < b->channels &&
           b->capture_settle_frames <= POCKETAUDIO_MAX_SETTLE_FRAMES &&
           (!b->route_control || b->ctl);
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
    if (snprintf(path, sizeof(path), "%s/audio.lock", dir) >= (int)sizeof(path) ||
        snprintf(s->lock_dir, sizeof(s->lock_dir), "%s", dir) >= (int)sizeof(s->lock_dir)) {
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

/* ---- the recovery record ----------------------------------------------- */

static int record_path(const char *dir, char *path, size_t len)
{
    return snprintf(path, len, "%s/audio.recovery", dir) < (int)len ? 0 : -1;
}

/* Written before the change it describes, replaced atomically (temp file and
 * rename, so a reader sees the old record or the new one and never half of
 * one). No fsync: it lives in the runtime directory, which a power cut
 * clears along with the hardware state it describes. */
static int record_write(struct pocketaudio_stream *s, int route_restore, int amp_enabled)
{
    char path[POCKETOS_PATH_MAX];
    char tmp[POCKETOS_PATH_MAX + 8];
    char text[RECOVERY_MAX_BYTES];
    int n;
    int fd;
    ssize_t w;

    if (record_path(s->lock_dir, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    n = snprintf(text, sizeof(text), "%s\npid %ld\ndirection %s\n", RECOVERY_HEADER,
                 (long)getpid(), s->dir == POCKETAUDIO_CAPTURE ? "capture" : "playback");
    if (route_restore >= 0) {
        n += snprintf(text + n, sizeof(text) - (size_t)n, "ctl %s\nroute_control %s\nroute_restore %d\n",
                      s->board.ctl, s->board.route_control, route_restore ? 1 : 0);
    }
    if (amp_enabled) {
        n += snprintf(text + n, sizeof(text) - (size_t)n,
                      "amp_chip %s\namp_line %u\namp_active_high %d\namp_enabled 1\n",
                      s->board.amp_chip, s->board.amp_line, s->board.amp_active_high ? 1 : 0);
    }
    if (n <= 0 || n >= (int)sizeof(text)) {
        return -EOVERFLOW;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -errno;
    }
    w = write(fd, text, (size_t)n);
    if (close(fd) != 0 || w != n || rename(tmp, path) != 0) {
        int e = errno ? errno : EIO;

        unlink(tmp);
        return -e;
    }
    s->record_written = 1;
    s->record_route = route_restore;
    return 0;
}

static int printable_name(const char *v, size_t max)
{
    size_t n = strlen(v);
    size_t i;

    if (n == 0 || n >= max) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e) {
            return 0;
        }
    }
    return 1;
}

static int parse_flag(const char *v, int *out)
{
    if (strcmp(v, "0") == 0 || strcmp(v, "1") == 0) {
        *out = v[0] - '0';
        return 1;
    }
    return 0;
}

/* 0 when there is no record, 1 with *r filled, -1 when a record is there
 * and is not one this code wrote. */
static int record_read(const char *dir, struct recovery *r)
{
    char path[POCKETOS_PATH_MAX];
    char text[RECOVERY_MAX_BYTES + 1];
    char *line;
    char *save = NULL;
    ssize_t n;
    int fd;
    int first = 1;

    memset(r, 0, sizeof(*r));
    r->route_restore = -1;
    if (record_path(dir, path, sizeof(path)) != 0) {
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    n = read(fd, text, sizeof(text));
    close(fd);
    if (n <= 0 || n > RECOVERY_MAX_BYTES) {
        return -1;
    }
    text[n] = '\0';
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *v = strchr(line, ' ');

        if (first) {
            if (strcmp(line, RECOVERY_HEADER) != 0) {
                return -1;
            }
            first = 0;
            continue;
        }
        if (!v) {
            return -1;
        }
        *v++ = '\0';
        if (strcmp(line, "ctl") == 0 && printable_name(v, sizeof(r->ctl))) {
            snprintf(r->ctl, sizeof(r->ctl), "%s", v);
        } else if (strcmp(line, "route_control") == 0 && printable_name(v, sizeof(r->route_control))) {
            snprintf(r->route_control, sizeof(r->route_control), "%s", v);
        } else if (strcmp(line, "route_restore") == 0) {
            if (!parse_flag(v, &r->route_restore)) {
                return -1;
            }
        } else if (strcmp(line, "amp_chip") == 0) {
            const char *d = v + strlen("/dev/gpiochip");

            if (strncmp(v, "/dev/gpiochip", strlen("/dev/gpiochip")) != 0 || !*d ||
                strspn(d, "0123456789") != strlen(d) || strlen(v) >= sizeof(r->amp_chip)) {
                return -1;
            }
            snprintf(r->amp_chip, sizeof(r->amp_chip), "%s", v);
        } else if (strcmp(line, "amp_line") == 0) {
            char *end;
            unsigned long l = strtoul(v, &end, 10);

            if (!*v || *end || l > 1023) {
                return -1;
            }
            r->amp_line = (unsigned)l;
        } else if (strcmp(line, "amp_active_high") == 0) {
            if (!parse_flag(v, &r->amp_active_high)) {
                return -1;
            }
        } else if (strcmp(line, "amp_enabled") == 0) {
            if (!parse_flag(v, &r->amp_enabled)) {
                return -1;
            }
        } else if (strcmp(line, "pid") != 0 && strcmp(line, "direction") != 0) {
            return -1;
        }
    }
    if (first || (r->route_restore >= 0 && (!r->ctl[0] || !r->route_control[0])) ||
        (r->amp_enabled && !r->amp_chip[0])) {
        return -1;
    }
    return 1;
}

/* Undo what a previous owner left behind. Called with the lock held, so the
 * owner that wrote the record is gone. The amplifier goes off before the
 * route is touched. 0 when there was nothing to do, 1 when something was
 * restored, POCKETAUDIO_E_ROUTE when a restore failed (the record stays, so
 * the next attempt tries again; every step is idempotent). */
static int reconcile(const struct pocketaudio_backend *be, const char *dir, char *report,
                     size_t len)
{
    char path[POCKETOS_PATH_MAX];
    struct recovery r;
    int v = record_read(dir, &r);
    int e;

    if (v == 0) {
        return 0;
    }
    if (record_path(dir, path, sizeof(path)) != 0) {
        return POCKETAUDIO_E_LOCK;
    }
    if (v < 0) {
        /* Not ours to act on, and left in place it would refuse every open. */
        unlink(path);
        say(report, len, "discarded an unreadable recovery record");
        return 0;
    }
    if (r.amp_enabled) {
        int h = be->gpio_request_output(r.amp_chip, r.amp_line, r.amp_active_high ? 0 : 1);

        if (h < 0) {
            say(report, len, "cannot switch the amplifier off (%s line %u): %s", r.amp_chip,
                r.amp_line, strerror(-h));
            return POCKETAUDIO_E_ROUTE;
        }
        be->gpio_release(h);
    }
    if (r.route_restore >= 0) {
        e = be->ctl_set_bool(r.ctl, r.route_control, r.route_restore);
        if (e < 0) {
            say(report, len, "cannot restore %s: %s", r.route_control, strerror(-e));
            return POCKETAUDIO_E_ROUTE;
        }
    }
    unlink(path);
    say(report, len, "restored after an owner that did not close:%s%s",
        r.amp_enabled ? " amplifier off" : "",
        r.route_restore >= 0 ? (r.route_restore ? " route 1" : " route 0") : "");
    return 1;
}

int pocketaudio_recover(const struct pocketaudio_options *options, char *report, size_t len)
{
    struct pocketaudio_stream *s;
    int rc;

    say(report, len, "%s", "");
    if (!options || !options->backend) {
        say(report, len, "invalid recovery request");
        return POCKETAUDIO_E_INVAL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        return POCKETAUDIO_E_IO;
    }
    s->lock_fd = -1;
    rc = take_lock(s, options->lock_dir, report, len);
    if (rc == POCKETAUDIO_OK) {
        rc = reconcile(options->backend, s->lock_dir, report, len);
        if (rc == 0 && report && len && !report[0]) {
            say(report, len, "nothing to recover");
        }
        close(s->lock_fd);
    }
    free(s);
    return rc;
}

static void release(struct pocketaudio_stream *s)
{
    int restored = 1;

    if (s->amp >= 0) {
        /* Silence first: the speaker goes quiet before the stream stops, so
         * whatever the PCM does as it closes is not amplified. */
        if (s->be->gpio_set(s->amp, s->board.amp_active_high ? 0 : 1) < 0) {
            restored = 0;
        }
        s->be->gpio_release(s->amp);
        s->amp = -1;
    }
    if (s->pcm) {
        s->be->pcm_close(s->pcm);
        s->pcm = NULL;
    }
    if (s->route_saved >= 0) {
        if (s->be->ctl_set_bool(s->board.ctl, s->board.route_control, s->route_saved) < 0) {
            restored = 0;
        }
        s->route_saved = -1;
    }
    if (s->record_written && restored) {
        char path[POCKETOS_PATH_MAX];

        /* Removed while the lock is still held, and only once everything it
         * describes has really been undone; otherwise the next owner does it. */
        if (record_path(s->lock_dir, path, sizeof(path)) == 0) {
            unlink(path);
        }
        s->record_written = 0;
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
        o->peak_limit > POCKETAUDIO_PEAK_CEILING || o->volume_percent < 0 || o->volume_percent > 100) {
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
    s->gain_q15 = pocketaudio_volume_gain_q15(o->volume_percent ? o->volume_percent : 100);
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

    s->settle_left = capture ? s->board.capture_settle_frames : 0;

    verified = capture ? s->board.capture_verified : s->board.playback_verified;
    if (!verified && !o->allow_unverified) {
        say(err, errlen, "%s %s is not validated on hardware", s->board.name,
            capture ? "microphone capture" : "speaker playback");
        free(s);
        return POCKETAUDIO_E_DISABLED;
    }

    s->record_route = -1;
    rc = take_lock(s, o->lock_dir, err, errlen);
    if (rc != POCKETAUDIO_OK) {
        free(s);
        return rc;
    }

    /* Before anything of ours: whatever a previous owner that died without
     * closing left switched on is switched back first. */
    rc = reconcile(s->be, s->lock_dir, msg, sizeof(msg));
    if (rc < 0) {
        say(err, errlen, "%s", msg);
        goto fail;
    }
    s->recovered = rc == 1;
    msg[0] = '\0';

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
            /* Write-ahead: the record says how to undo the switch before the
             * switch happens, so a death at any instant after this is
             * recoverable. */
            e = record_write(s, have, 0);
            if (e < 0) {
                say(err, errlen, "cannot write the audio recovery record: %s", strerror(-e));
                rc = POCKETAUDIO_E_LOCK;
                goto fail;
            }
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
        e = record_write(s, s->record_route, 1);
        if (e < 0) {
            say(err, errlen, "cannot write the audio recovery record: %s", strerror(-e));
            rc = POCKETAUDIO_E_LOCK;
            goto fail;
        }
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
        /* Volume first, then the ceiling: the clamp is the safety limit and
         * must hold whatever the gain is. Division, not a shift, so negative
         * samples round the same way as positive ones. */
        int v = (int)((int32_t)mono[i] * s->gain_q15 / 32768);

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

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* One device read of up to n wire frames, waiting at most timeout_ms. */
static long read_wire(struct pocketaudio_stream *s, size_t n, int timeout_ms)
{
    long r = s->be->pcm_read(s->pcm, s->wire, n, timeout_ms);

    if (r == -EPIPE) {
        /* An overrun: some audio was lost, the stream runs on. The decoder
         * sees a gap, which it survives like any other noise. */
        s->xruns++;
        r = s->be->pcm_read(s->pcm, s->wire, n, timeout_ms);
        if (r == -EPIPE) {
            s->xruns++;
            r = 0;
        }
    }
    r = io_result(s, r, "capture");
    if (r > 0 && (size_t)r > n) {
        say(s->error, sizeof(s->error), "capture returned more than asked");
        return POCKETAUDIO_E_IO;
    }
    return r;
}

long pocketaudio_read(struct pocketaudio_stream *s, int16_t *mono, size_t frames)
{
    int64_t start;
    int timeout = POCKETAUDIO_MAX_WAIT_MS;
    size_t skip = 0;
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
    start = now_ms();
    for (;;) {
        r = read_wire(s, n, timeout);
        if (r <= 0) {
            return r;
        }
        if (s->settle_left == 0) {
            break;
        }
        /* The startup transient: dropped, not returned. A read that crosses
         * its end keeps only the frames after it. */
        if ((unsigned long)r > s->settle_left) {
            skip = s->settle_left;
            s->settle_left = 0;
            break;
        }
        s->settle_left -= (unsigned)r;
        /* The whole read was transient. Read on, but only within this call's
         * one wait: the caller's stop flag must not wait on the discard. */
        timeout = POCKETAUDIO_MAX_WAIT_MS - (int)(now_ms() - start);
        if (timeout <= 0) {
            return 0;
        }
    }
    for (i = skip; i < (size_t)r; i++) {
        mono[i - skip] = s->wire[i * s->board.channels + s->board.capture_channel];
    }
    return r - (long)skip;
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

int pocketaudio_recovered(const struct pocketaudio_stream *s)
{
    return s ? s->recovered : 0;
}

unsigned pocketaudio_xruns(const struct pocketaudio_stream *s)
{
    return s ? s->xruns : 0;
}

const char *pocketaudio_last_error(const struct pocketaudio_stream *s)
{
    return s ? s->error : "";
}
