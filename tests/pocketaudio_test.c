/*
 * pocketaudio's policy against a fake backend: the gate, the lock, the route
 * and the amplifier, their order on open and on close, what every failure
 * leaves behind, the level clamp, the channel mapping and the bounds.
 *
 * Nothing here opens a sound device or a GPIO. The fake records every call it
 * receives as one short token in a log, so order is checked as a string.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketaudio/pocketaudio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- the fake ----------------------------------------------------------- */

static struct {
    char log[1024];
    int route;             /* the mixer switch */
    int route_get_err;
    int route_set_err;
    int open_err;          /* negative errno, or 0 */
    unsigned open_rate, open_channels, open_period, open_buffer;
    int open_capture;
    char open_name[64];
    int amp_err;
    int amp_value;         /* last value driven, -1 never requested */
    int amp_requests;
    unsigned amp_line;
    long write_script[8];  /* results for successive writes; 0 entries mean "all" */
    int write_calls;
    int16_t last_wire[POCKETAUDIO_PERIOD_FRAMES * POCKETAUDIO_MAX_CHANNELS];
    size_t last_frames;
    int last_timeout;
    long read_script[8];
    int read_calls;
    int drain_result;
    int drain_timeout;
    int pcm_open_now;
} fk;

static void logf_(const char *tok)
{
    size_t n = strlen(fk.log);

    snprintf(fk.log + n, sizeof(fk.log) - n, "%s%s", n ? " " : "", tok);
}

static void fake_reset(void)
{
    memset(&fk, 0, sizeof(fk));
    fk.amp_value = -1;
}

static int fake_handle = 42;

static void *f_open(const char *name, int capture, unsigned rate, unsigned channels,
                    unsigned period, unsigned buffer, int *err, char *msg, size_t msglen)
{
    logf_(capture ? "open-c" : "open-p");
    snprintf(fk.open_name, sizeof(fk.open_name), "%s", name);
    fk.open_capture = capture;
    fk.open_rate = rate;
    fk.open_channels = channels;
    fk.open_period = period;
    fk.open_buffer = buffer;
    if (fk.open_err) {
        *err = fk.open_err;
        snprintf(msg, msglen, "fake refused");
        return NULL;
    }
    fk.pcm_open_now = 1;
    return &fake_handle;
}

static long f_write(void *pcm, const int16_t *buf, size_t frames, int timeout_ms)
{
    long r;

    (void)pcm;
    logf_("write");
    memcpy(fk.last_wire, buf, frames * fk.open_channels * sizeof(int16_t));
    fk.last_frames = frames;
    fk.last_timeout = timeout_ms;
    r = fk.write_calls < 8 ? fk.write_script[fk.write_calls] : 0;
    fk.write_calls++;
    return r == 0 ? (long)frames : (r == 1 ? 0 : r);
}

static long f_read(void *pcm, int16_t *buf, size_t frames, int timeout_ms)
{
    size_t i;
    unsigned c;
    long r;

    (void)pcm;
    logf_("read");
    fk.last_timeout = timeout_ms;
    for (i = 0; i < frames; i++) {
        for (c = 0; c < fk.open_channels; c++) {
            /* channel c of frame i is (c + 1) * 1000 + i */
            buf[i * fk.open_channels + c] = (int16_t)((c + 1) * 1000 + i);
        }
    }
    r = fk.read_calls < 8 ? fk.read_script[fk.read_calls] : 0;
    fk.read_calls++;
    return r == 0 ? (long)frames : (r == 1 ? 0 : r);
}

static int f_drain(void *pcm, int timeout_ms)
{
    (void)pcm;
    logf_("drain");
    fk.drain_timeout = timeout_ms;
    return fk.drain_result;
}

static void f_close(void *pcm)
{
    (void)pcm;
    logf_("close");
    fk.pcm_open_now = 0;
}

static int f_get(const char *ctl, const char *name, int *value)
{
    (void)ctl;
    (void)name;
    logf_("route?");
    if (fk.route_get_err) {
        return fk.route_get_err;
    }
    *value = fk.route;
    return 0;
}

static int f_set(const char *ctl, const char *name, int value)
{
    char tok[16];

    (void)ctl;
    (void)name;
    snprintf(tok, sizeof(tok), "route=%d", value);
    logf_(tok);
    if (fk.route_set_err) {
        return fk.route_set_err;
    }
    fk.route = value;
    return 0;
}

static int f_gpio_req(const char *chip, unsigned line, int value)
{
    char tok[16];

    (void)chip;
    snprintf(tok, sizeof(tok), "amp+%d", value);
    logf_(tok);
    fk.amp_requests++;
    fk.amp_line = line;
    if (fk.amp_err) {
        return fk.amp_err;
    }
    fk.amp_value = value;
    return 7;
}

static int f_gpio_set(int h, int value)
{
    char tok[16];

    (void)h;
    snprintf(tok, sizeof(tok), "amp=%d", value);
    logf_(tok);
    fk.amp_value = value;
    return 0;
}

static void f_gpio_release(int h)
{
    (void)h;
    logf_("amp-");
}

static const struct pocketaudio_backend fake = {
    .pcm_open = f_open,
    .pcm_write = f_write,
    .pcm_read = f_read,
    .pcm_drain = f_drain,
    .pcm_close = f_close,
    .ctl_get_bool = f_get,
    .ctl_set_bool = f_set,
    .gpio_request_output = f_gpio_req,
    .gpio_set = f_gpio_set,
    .gpio_release = f_gpio_release,
};

static char lockdir[64];

static struct pocketaudio_options opts(const struct pocketaudio_board *b, int allow)
{
    struct pocketaudio_options o;

    memset(&o, 0, sizeof(o));
    o.backend = &fake;
    o.board = b;
    o.allow_unverified = allow;
    o.lock_dir = lockdir;
    return o;
}

/* ---- tests -------------------------------------------------------------- */

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_detect(void)
{
    char root[64] = "/tmp/pocketaudio-proc-XXXXXX";
    char path[128];
    struct pocketaudio_board b;

    if (!mkdtemp(root)) {
        check("detect: temp dir", 0);
        return;
    }
    unsetenv("POCKETOS_AUDIO_BOARD");
    unsetenv("POCKETOS_AUDIO_PCM");

    pocketaudio_board_detect(&b, root);
    check("detect: no card at all is the generic board", strcmp(b.name, "generic") == 0);

    snprintf(path, sizeof(path), "%s/asound", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/asound/card0", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/asound/card0/id", root);
    write_file(path, "K230I2SINNO\n");
    pocketaudio_board_detect(&b, root);
    check("detect: the K230 codec card is the K230 board", strcmp(b.name, "k230-t-display") == 0);
    check("detect: the K230 PCM is named by card id, not index",
          strcmp(b.pcm, "hw:CARD=K230I2SINNO,DEV=0") == 0);
    check("detect: K230 wire is stereo, mic on the right slot",
          b.channels == 2 && b.capture_channel == 1);
    check("detect: K230 speaker needs the external I2S route on, mic needs it off",
          b.route_control && strcmp(b.route_control, "External I2S Output Switch") == 0 &&
              b.route_playback == 1 && b.route_capture == 0);
    check("detect: K230 amplifier enable is gpiochip1 line 2, active high",
          b.amp_chip && strcmp(b.amp_chip, "/dev/gpiochip1") == 0 && b.amp_line == 2 &&
              b.amp_active_high == 1);
    check("detect: neither K230 path is marked validated yet",
          b.playback_verified == 0 && b.capture_verified == 0);

    setenv("POCKETOS_AUDIO_PCM", "null", 1);
    pocketaudio_board_detect(&b, root);
    check("detect: POCKETOS_AUDIO_PCM replaces the PCM name only",
          strcmp(b.pcm, "null") == 0 && strcmp(b.name, "k230-t-display") == 0);
    setenv("POCKETOS_AUDIO_BOARD", "generic", 1);
    pocketaudio_board_detect(&b, root);
    check("detect: POCKETOS_AUDIO_BOARD=generic forces the generic board",
          strcmp(b.name, "generic") == 0 && strcmp(b.pcm, "null") == 0);
    unsetenv("POCKETOS_AUDIO_BOARD");
    unsetenv("POCKETOS_AUDIO_PCM");

    write_file(path, "SomethingElse\n");
    pocketaudio_board_detect(&b, root);
    check("detect: another card is the generic board", strcmp(b.name, "generic") == 0);
    check("detect: generic has no route and no amplifier", !b.route_control && !b.amp_chip);

    unlink(path);
    snprintf(path, sizeof(path), "%s/asound/card0", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/asound", root);
    rmdir(path);
    rmdir(root);
}

static void test_gate(void)
{
    struct pocketaudio_stream *s = (struct pocketaudio_stream *)1;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 0);
    char err[160];
    char lock[128];
    int rc;

    fake_reset();
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("gate: unvalidated speaker playback is refused", rc == POCKETAUDIO_E_DISABLED);
    check("gate: and *out is NULL", s == NULL);
    check("gate: and nothing was touched", fk.log[0] == '\0');
    check("gate: the reason names the path", strstr(err, "speaker playback") != NULL);
    snprintf(lock, sizeof(lock), "%s/audio.lock", lockdir);
    check("gate: not even the lock file was created", access(lock, F_OK) != 0);

    fake_reset();
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("gate: unvalidated microphone capture is refused", rc == POCKETAUDIO_E_DISABLED);
    check("gate: capture touched nothing either", fk.log[0] == '\0');
    check("gate: the reason names the microphone", strstr(err, "microphone") != NULL);
}

static void test_playback_order(void)
{
    struct pocketaudio_stream *s = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 1);
    char err[160];
    int rc;

    fake_reset();
    fk.route = 0; /* the codec route: must be switched for the speaker */
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("playback: opens with allow_unverified", rc == POCKETAUDIO_OK && s != NULL);
    check("playback: route read, switched, PCM opened, amplifier enabled last",
          strcmp(fk.log, "route? route=1 open-p amp+1") == 0);
    check("playback: 48 kHz, 2 channels, 20 ms period, 80 ms buffer",
          fk.open_rate == 48000 && fk.open_channels == 2 && fk.open_period == 960 &&
              fk.open_buffer == 3840 && !fk.open_capture);
    check("playback: the amplifier line is line 2", fk.amp_line == 2 && fk.amp_value == 1);

    fk.log[0] = '\0';
    pocketaudio_close(s);
    check("close: amplifier off, released, PCM closed, then route restored",
          strcmp(fk.log, "amp=0 amp- close route=0") == 0);
    check("close: the route is back where it was", fk.route == 0);
    check("close: the amplifier is left off", fk.amp_value == 0);

    fake_reset();
    fk.route = 1; /* already the speaker route */
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("playback: a route already right is not written",
          rc == POCKETAUDIO_OK && strcmp(fk.log, "route? open-p amp+1") == 0);
    fk.log[0] = '\0';
    pocketaudio_close(s);
    check("close: and not restored either", strcmp(fk.log, "amp=0 amp- close") == 0);
}

static void test_capture_order(void)
{
    struct pocketaudio_stream *s = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 1);
    char err[160];
    int rc;

    fake_reset();
    fk.route = 1; /* the boot default on the K230 */
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("capture: opens", rc == POCKETAUDIO_OK && s != NULL);
    check("capture: route switched to the codec, PCM opened, no amplifier",
          strcmp(fk.log, "route? route=0 open-c") == 0 && fk.amp_requests == 0);
    check("capture: 500 ms buffer", fk.open_capture && fk.open_buffer == 24000);
    fk.log[0] = '\0';
    pocketaudio_close(s);
    check("capture close: PCM closed and the external route restored",
          strcmp(fk.log, "close route=1") == 0 && fk.route == 1);
}

static void test_busy(void)
{
    struct pocketaudio_stream *a = NULL;
    struct pocketaudio_stream *b = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_generic(), 0);
    char err[160];
    int rc;

    fake_reset();
    rc = pocketaudio_open(&a, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("busy: the first stream opens", rc == POCKETAUDIO_OK);
    fk.log[0] = '\0';
    rc = pocketaudio_open(&b, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("busy: a second stream in the other direction is refused", rc == POCKETAUDIO_E_BUSY);
    check("busy: and never reached the device", fk.log[0] == '\0' && b == NULL);
    rc = pocketaudio_open(&b, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("busy: so is a second playback", rc == POCKETAUDIO_E_BUSY);
    check("busy: the first stream still works", pocketaudio_write(a, (int16_t[4]){ 0 }, 4) == 4);
    pocketaudio_close(a);
    rc = pocketaudio_open(&b, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("busy: after close the lock is free again", rc == POCKETAUDIO_OK);
    pocketaudio_close(b);
}

static void test_failures(void)
{
    struct pocketaudio_stream *s = (struct pocketaudio_stream *)1;
    struct pocketaudio_stream *t = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 1);
    struct pocketaudio_options g = opts(pocketaudio_board_generic(), 0);
    char err[160];
    int rc;

    fake_reset();
    fk.route = 0;
    fk.open_err = -ENOENT;
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("missing device: E_NODEV", rc == POCKETAUDIO_E_NODEV && s == NULL);
    check("missing device: amplifier never requested, route restored",
          fk.amp_requests == 0 && strcmp(fk.log, "route? route=1 open-p route=0") == 0 &&
              fk.route == 0);
    check("missing device: the reason is kept", strstr(err, "cannot open") != NULL);
    rc = pocketaudio_open(&t, POCKETAUDIO_PLAYBACK, &g, err, sizeof(err));
    check("missing device: the lock was released", rc == POCKETAUDIO_E_NODEV);

    fake_reset();
    fk.open_err = -EINVAL;
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &g, err, sizeof(err));
    check("unsupported format: E_FORMAT", rc == POCKETAUDIO_E_FORMAT);
    fake_reset();
    fk.open_err = -EBUSY;
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &g, err, sizeof(err));
    check("device held by another program: E_BUSY", rc == POCKETAUDIO_E_BUSY);
    fake_reset();
    fk.open_err = -EIO;
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &g, err, sizeof(err));
    check("other open failure: E_IO", rc == POCKETAUDIO_E_IO);

    fake_reset();
    fk.route_get_err = -EIO;
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("route unreadable: E_ROUTE, nothing opened",
          rc == POCKETAUDIO_E_ROUTE && strcmp(fk.log, "route?") == 0);

    fake_reset();
    fk.route_get_err = -ENODEV;
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("no card behind the route control: E_NODEV, nothing opened",
          rc == POCKETAUDIO_E_NODEV && strcmp(fk.log, "route?") == 0);

    fake_reset();
    fk.route = 1; /* capture needs 0, so a write is attempted */
    fk.route_set_err = -EPERM;
    rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
    check("route not settable: E_ROUTE, nothing opened, nothing to restore",
          rc == POCKETAUDIO_E_ROUTE && strcmp(fk.log, "route? route=0") == 0);

    fake_reset();
    fk.route = 1;
    fk.amp_err = -EBUSY;
    rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err));
    check("amplifier line unavailable: E_ROUTE", rc == POCKETAUDIO_E_ROUTE && s == NULL);
    check("amplifier line unavailable: the PCM is closed again",
          strcmp(fk.log, "route? open-p amp+1 close") == 0 && !fk.pcm_open_now);
    check("amplifier line unavailable: the reason names the line", strstr(err, "line 2") != NULL);

    fake_reset();
    {
        struct pocketaudio_board bad = *pocketaudio_board_k230();

        bad.capture_channel = 2;
        o = opts(&bad, 1);
        rc = pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err));
        check("a board whose mic channel is not on the wire is refused", rc == POCKETAUDIO_E_INVAL);
    }
    {
        struct pocketaudio_options lvl = opts(pocketaudio_board_generic(), 0);

        lvl.peak_limit = POCKETAUDIO_PEAK_CEILING + 1;
        rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &lvl, err, sizeof(err));
        check("a peak limit above the ceiling is refused", rc == POCKETAUDIO_E_INVAL);
        lvl.peak_limit = -1;
        rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &lvl, err, sizeof(err));
        check("a negative peak limit is refused", rc == POCKETAUDIO_E_INVAL);
    }
    {
        struct pocketaudio_options nolock = opts(pocketaudio_board_generic(), 0);

        nolock.lock_dir = "/proc/pocketaudio-cannot-exist";
        fake_reset();
        rc = pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &nolock, err, sizeof(err));
        check("an unusable lock directory is E_LOCK, before the device",
              rc == POCKETAUDIO_E_LOCK && fk.log[0] == '\0');
    }
    check("open with out NULL is refused", pocketaudio_open(NULL, POCKETAUDIO_PLAYBACK, &g, err,
                                                            sizeof(err)) == POCKETAUDIO_E_INVAL);
    pocketaudio_close(NULL);
    check("close(NULL) is harmless", 1);
}

static void test_write(void)
{
    struct pocketaudio_stream *s = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 1);
    int16_t big[POCKETAUDIO_PERIOD_FRAMES + 100];
    char err[160];
    long r;
    size_t i;

    fake_reset();
    fk.route = 1;
    o.peak_limit = 1000;
    if (pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err)) != POCKETAUDIO_OK) {
        check("write: open", 0);
        return;
    }
    for (i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
        big[i] = (int16_t)((i % 2) ? -30000 : 500);
    }
    r = pocketaudio_write(s, big, sizeof(big) / sizeof(big[0]));
    check("write: one call takes at most one period", r == POCKETAUDIO_PERIOD_FRAMES &&
                                                      fk.last_frames == POCKETAUDIO_PERIOD_FRAMES);
    check("write: the wait is bounded", fk.last_timeout == POCKETAUDIO_MAX_WAIT_MS);
    check("write: mono is copied to both wire channels",
          fk.last_wire[0] == 500 && fk.last_wire[1] == 500);
    check("write: samples beyond the limit are clamped, both signs",
          fk.last_wire[2] == -1000 && fk.last_wire[3] == -1000 && fk.last_wire[4] == 500);

    fk.write_script[fk.write_calls] = 1; /* the device stays busy */
    check("write: a busy device returns 0 after the bound", pocketaudio_write(s, big, 10) == 0);

    fk.write_script[fk.write_calls] = -EPIPE; /* underrun, then fine */
    r = pocketaudio_write(s, big, 10);
    check("write: an underrun is counted and retried", r == 10 && pocketaudio_xruns(s) == 1);

    fk.write_script[fk.write_calls] = -EPIPE;
    fk.write_script[fk.write_calls + 1] = -EPIPE;
    r = pocketaudio_write(s, big, 10);
    check("write: two underruns in a row give 0, not an error", r == 0 && pocketaudio_xruns(s) == 3);

    fk.write_script[fk.write_calls] = -EIO;
    r = pocketaudio_write(s, big, 10);
    check("write: a device error is E_IO with a reason",
          r == POCKETAUDIO_E_IO && strstr(pocketaudio_last_error(s), "playback") != NULL);

    check("write: zero frames is zero", pocketaudio_write(s, big, 0) == 0);
    check("write: NULL samples is refused", pocketaudio_write(s, NULL, 4) == POCKETAUDIO_E_INVAL);
    check("read on a playback stream is refused",
          pocketaudio_read(s, big, 4) == POCKETAUDIO_E_INVAL);

    fk.drain_result = -ETIMEDOUT;
    check("drain: a timeout is E_TIMEOUT", pocketaudio_drain(s, 300) == POCKETAUDIO_E_TIMEOUT &&
                                             fk.drain_timeout == 300);
    fk.drain_result = 0;
    check("drain: capped at 10 s", pocketaudio_drain(s, 999999) == POCKETAUDIO_OK &&
                                   fk.drain_timeout == 10000);
    check("drain: a negative bound is refused", pocketaudio_drain(s, -1) == POCKETAUDIO_E_INVAL);
    pocketaudio_close(s);

    fake_reset();
    o = opts(pocketaudio_board_generic(), 0);
    if (pocketaudio_open(&s, POCKETAUDIO_PLAYBACK, &o, err, sizeof(err)) == POCKETAUDIO_OK) {
        int16_t loud[2] = { 32767, -32768 };

        pocketaudio_write(s, loud, 2);
        check("write: no limit given means the -12 dBFS ceiling",
              fk.last_wire[0] == POCKETAUDIO_PEAK_CEILING &&
                  fk.last_wire[1] == -POCKETAUDIO_PEAK_CEILING);
        pocketaudio_close(s);
    }
}

static void test_read(void)
{
    struct pocketaudio_stream *s = NULL;
    struct pocketaudio_options o = opts(pocketaudio_board_k230(), 1);
    int16_t buf[POCKETAUDIO_PERIOD_FRAMES + 10];
    char err[160];
    long r;

    fake_reset();
    fk.route = 1;
    if (pocketaudio_open(&s, POCKETAUDIO_CAPTURE, &o, err, sizeof(err)) != POCKETAUDIO_OK) {
        check("read: open", 0);
        return;
    }
    r = pocketaudio_read(s, buf, sizeof(buf) / sizeof(buf[0]));
    check("read: one call gives at most one period", r == POCKETAUDIO_PERIOD_FRAMES);
    check("read: the mic is taken from the right slot",
          buf[0] == 2000 && buf[5] == 2005 && buf[r - 1] == (int16_t)(2000 + r - 1));
    fk.read_script[fk.read_calls] = -EPIPE;
    r = pocketaudio_read(s, buf, 8);
    check("read: an overrun is counted and the stream runs on", r == 8 && pocketaudio_xruns(s) == 1);
    fk.read_script[fk.read_calls] = 1;
    check("read: nothing within the bound is 0", pocketaudio_read(s, buf, 8) == 0);
    fk.read_script[fk.read_calls] = -ENODEV;
    check("read: a vanished device is E_IO", pocketaudio_read(s, buf, 8) == POCKETAUDIO_E_IO);
    check("write on a capture stream is refused",
          pocketaudio_write(s, buf, 4) == POCKETAUDIO_E_INVAL);
    check("drain on a capture stream is refused",
          pocketaudio_drain(s, 10) == POCKETAUDIO_E_INVAL);
    pocketaudio_close(s);
}

static void test_misc(void)
{
    int16_t v[] = { 3, -32768, 100 };
    int e;
    int named = 1;

    check("peak: -32768 is 32768", pocketaudio_peak(v, 3) == 32768);
    check("peak: empty is 0", pocketaudio_peak(v, 0) == 0 && pocketaudio_peak(NULL, 3) == 0);
    for (e = POCKETAUDIO_E_LOCK; e <= POCKETAUDIO_OK; e++) {
        if (strcmp(pocketaudio_strerror(e), "unknown audio error") == 0) {
            named = 0;
        }
    }
    check("every error has words", named);
    check("the ceiling is -12 dBFS", POCKETAUDIO_PEAK_CEILING == 8192);
}

int main(void)
{
    snprintf(lockdir, sizeof(lockdir), "/tmp/pocketaudio-lock-XXXXXX");
    if (!mkdtemp(lockdir)) {
        perror("mkdtemp");
        return 1;
    }
    test_detect();
    test_gate();
    test_playback_order();
    test_capture_order();
    test_busy();
    test_failures();
    test_write();
    test_read();
    test_misc();
    {
        char lock[128];

        snprintf(lock, sizeof(lock), "%s/audio.lock", lockdir);
        unlink(lock);
        rmdir(lockdir);
    }
    printf("pocketaudio_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
