/*
 * pos-record's record and play steps (tools/recorder/rec_engine.h) against
 * the real pocketaudio over a scripted backend and a real folder.
 *
 * The backend is not a perfect device. A script says what each read does:
 * deliver a full period, a short one, nothing within the wait (a timeout),
 * an overrun (-EPIPE, which pocketaudio recovers from and counts), a device
 * failure (-EIO), a slow read (it sleeps first), or a malformed answer (more
 * frames than asked). The microphone's samples are a known signal on the
 * right slot of a stereo wire, as on the K230, so what reaches the file can
 * be counted and compared; a start-up transient at negative full scale can
 * be put in front of it. Playback records what was written, accepts less
 * than offered when told to, and times out on request.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketwav/pocketwav.h"
#include "rec_engine.h"
#include "rec_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;
static char dir[] = "/tmp/rec_engine_test.XXXXXX";

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* ---- the scripted device --------------------------------------------------- */

enum op { FULL, SHORT, TIMEOUT, OVERRUN, FAIL, SLOW, TOO_MANY, ACCEPT_SOME };

struct step {
    enum op op;
    int arg; /* SHORT, ACCEPT_SOME: frames; SLOW: ms */
};

static struct step script[64];
static int script_len;
static int script_at;
static int handle;
static unsigned long long mic_t;      /* frames the microphone has produced */
static unsigned long long delivered;  /* frames handed to pocketaudio */
static int transient_frames;          /* at negative full scale, before the signal */
static double mic_amp = 10000.0;
static int16_t played[200000];
static size_t played_n;

static void set_script(const struct step *s, int n)
{
    if (n > 0) {
        memcpy(script, s, (size_t)n * sizeof(*s));
    }
    script_len = n;
    script_at = 0;
}

static struct step next_step(void)
{
    struct step full = { FULL, 0 };

    return script_at < script_len ? script[script_at++] : full;
}

static void *m_open(const char *name, int capture, unsigned rate, unsigned channels, unsigned period,
                    unsigned buffer, int *err, char *msg, size_t msglen)
{
    (void)name;
    (void)capture;
    (void)rate;
    (void)channels;
    (void)period;
    (void)buffer;
    (void)err;
    (void)msg;
    (void)msglen;
    return &handle;
}

static void produce(int16_t *wire, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++, mic_t++) {
        int16_t v = mic_t < (unsigned long long)transient_frames
                        ? -32768
                        : (int16_t)lrint(mic_amp * sin(2.0 * M_PI * 440.0 * (double)mic_t / 48000.0));

        wire[2 * i] = 0;     /* the empty headset input */
        wire[2 * i + 1] = v; /* the on-board microphone */
    }
}

static long m_read(void *pcm, int16_t *wire, size_t frames, int timeout_ms)
{
    struct step s = next_step();

    (void)pcm;
    (void)timeout_ms;
    switch (s.op) {
    case SHORT:
        if ((size_t)s.arg < frames) {
            frames = (size_t)s.arg;
        }
        break;
    case TIMEOUT: return 0;
    case OVERRUN:
        mic_t += 4800; /* the audio the device lost */
        return -EPIPE;
    case FAIL: return -EIO;
    case SLOW: {
        struct timespec d = { 0, (long)s.arg * 1000000L };

        nanosleep(&d, NULL);
        break;
    }
    case TOO_MANY:
        produce(wire, frames);
        return (long)frames + 5;
    default: break;
    }
    produce(wire, frames);
    delivered += frames;
    return (long)frames;
}

static long m_write(void *pcm, const int16_t *wire, size_t frames, int timeout_ms)
{
    struct step s = next_step();
    size_t i;

    (void)pcm;
    (void)timeout_ms;
    if (s.op == TIMEOUT) {
        return 0;
    }
    if (s.op == FAIL) {
        return -EIO;
    }
    if (s.op == ACCEPT_SOME && (size_t)s.arg < frames) {
        frames = (size_t)s.arg;
    }
    for (i = 0; i < frames && played_n < sizeof(played) / sizeof(played[0]); i++) {
        played[played_n++] = wire[2 * i];
    }
    return (long)frames;
}

static int m_drain(void *pcm, int timeout_ms)
{
    (void)pcm;
    (void)timeout_ms;
    return 0;
}

static void m_close(void *pcm)
{
    (void)pcm;
}

static int m_get(const char *ctl, const char *name, int *v)
{
    (void)ctl;
    (void)name;
    *v = 0;
    return 0;
}

static int m_set(const char *ctl, const char *name, int v)
{
    (void)ctl;
    (void)name;
    (void)v;
    return 0;
}

static int m_gpio(const char *chip, unsigned line, int v)
{
    (void)chip;
    (void)line;
    (void)v;
    return 3;
}

static int m_gpio_set(int h, int v)
{
    (void)h;
    (void)v;
    return 0;
}

static void m_gpio_release(int h)
{
    (void)h;
}

static const struct pocketaudio_backend mock = {
    m_open, m_write, m_read, m_drain, m_close, m_get, m_set, m_gpio, m_gpio_set, m_gpio_release,
};

static struct pocketaudio_board board = {
    .name = "scripted",
    .pcm = "scripted",
    .channels = 2,
    .capture_channel = 1,
    .capture_verified = 1,
    .playback_verified = 1,
};

static struct pocketaudio_stream *open_stream(enum pocketaudio_dir d)
{
    struct pocketaudio_options o;
    struct pocketaudio_stream *s = NULL;
    char err[160];

    memset(&o, 0, sizeof(o));
    o.backend = &mock;
    o.board = &board;
    o.lock_dir = dir;
    if (pocketaudio_open(&s, d, &o, err, sizeof(err)) != POCKETAUDIO_OK) {
        printf("note open failed: %s\n", err);
    }
    return s;
}

static int probe(const char *name, struct pocketwav_info *i)
{
    char path[256];
    int fd;
    int rc;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_RDONLY);
    rc = fd >= 0 ? pocketwav_probe_fd(fd, i) : -100;
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}

/* The largest magnitude among a saved recording's samples. */
static int file_peak(const char *name)
{
    char path[256];
    unsigned char b[2];
    FILE *f;
    int peak = 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f || fseek(f, 44, SEEK_SET) != 0) {
        return -1;
    }
    while (fread(b, 1, 2, f) == 2) {
        int v = (int16_t)(b[0] | b[1] << 8);

        v = v < 0 ? -v : v;
        peak = v > peak ? v : peak;
    }
    fclose(f);
    return peak;
}

static void reset_device(void)
{
    mic_t = 0;
    delivered = 0;
    transient_frames = 0;
    board.capture_settle_frames = 0;
    played_n = 0;
    set_script(NULL, 0);
}

/* ---- recording ------------------------------------------------------------- */

static void test_record_standard(void)
{
    static const struct step s[] = {
        { FULL, 0 },     { SHORT, 100 }, { TIMEOUT, 0 }, { FULL, 0 }, { OVERRUN, 0 },
        { SHORT, 1 },    { SLOW, 30 },   { SHORT, 959 }, { FULL, 0 }, { TIMEOUT, 0 },
    };
    struct rec_recorder r;
    struct pocketaudio_stream *st;
    struct pocketwav_info i;
    char final[REC_NAME_MAX];
    int waits = 0;
    int k;
    int peak;
    int rms;

    reset_device();
    set_script(s, (int)(sizeof(s) / sizeof(s[0])));
    check("a Standard recording opens its file", rec_recorder_open(&r, dir, "REC-0001.wav", 48000) == 0);
    st = open_stream(POCKETAUDIO_CAPTURE);
    for (k = 0; k < 20; k++) {
        enum rec_step x = rec_recorder_step(&r, st);

        waits += x == REC_STEP_WAIT;
        if (x != REC_STEP_OK && x != REC_STEP_WAIT) {
            printf("note unexpected step %d\n", x);
        }
    }
    r.gaps += pocketaudio_xruns(st);
    rec_meter_read(&r.meter, &peak, &rms);
    pocketaudio_close(st);
    check("timeouts are waits, not errors, and the stall count resets", waits == 2 && r.stalls < 2);
    check("the overrun is counted as a gap", r.gaps == 1);
    check("every frame delivered - short, slow or after an overrun - is in the file, once",
          r.file.data_bytes == delivered * 2);
    printf("note meter peak %d rms %d, ms %llu\n", peak, rms, (unsigned long long)rec_recorder_ms(&r));
    /* The overrun's gap is a step in the sine; the DC blocker lets a
     * little of it through, so the peak may sit a few percent over. */
    check("the meter read the microphone's level (a 10000 sine)", peak > 9500 && peak <= 11000 && rms > 6500 &&
                                                                     rms < 7300);
    check("the elapsed time is the audio written", rec_recorder_ms(&r) == delivered * 1000 / 48000);
    check("the file finishes", rec_file_finish(&r.file, final, sizeof(final)) == 0 &&
                                   probe(final, &i) == POCKETWAV_OK && i.rate == 48000 &&
                                   i.frames == delivered);
}

static void test_record_voice(void)
{
    struct rec_recorder r;
    struct pocketaudio_stream *st;
    struct pocketwav_info i;
    char final[REC_NAME_MAX];
    int k;

    reset_device();
    board.capture_settle_frames = 480;
    transient_frames = 480; /* the codec's start-up transient, at -32768 */
    rec_recorder_open(&r, dir, "REC-0002.wav", 16000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    for (k = 0; k < 51; k++) {
        rec_recorder_step(&r, st);
    }
    pocketaudio_close(st);
    rec_file_finish(&r.file, final, sizeof(final));
    check("Voice writes one sample for every three the device delivers after the discard",
          probe(final, &i) == POCKETAUDIO_OK && i.rate == 16000 && i.frames == (delivered - 480) / 3);
    check("the start-up transient never reaches the file", file_peak(final) < 11000);
    check("and the signal does, at its level", file_peak(final) > 9000);
}

static void test_record_failures(void)
{
    static const struct step stall[] = {
        { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 },
        { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 }, { TIMEOUT, 0 },
    };
    static const struct step fail[] = { { FULL, 0 }, { FULL, 0 }, { FAIL, 0 } };
    static const struct step bad[] = { { FULL, 0 }, { TOO_MANY, 0 } };
    struct rec_recorder r;
    struct pocketaudio_stream *st;
    struct pocketwav_info i;
    char final[REC_NAME_MAX];
    int k;
    enum rec_step x = REC_STEP_OK;

    reset_device();
    set_script(stall, 10);
    rec_recorder_open(&r, dir, "REC-0003.wav", 48000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    for (k = 0; k < 9; k++) {
        x = rec_recorder_step(&r, st);
    }
    check("nine empty waits are still waiting", x == REC_STEP_WAIT);
    check("the tenth is a device that stopped delivering", rec_recorder_step(&r, st) == REC_STEP_AUDIO_ERROR);
    pocketaudio_close(st);
    check("a recording with no audio is not saved", rec_file_finish(&r.file, final, sizeof(final)) == -ENODATA);

    reset_device();
    set_script(fail, 3);
    rec_recorder_open(&r, dir, "REC-0004.wav", 48000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    rec_recorder_step(&r, st);
    rec_recorder_step(&r, st);
    check("a failing device is an audio error", rec_recorder_step(&r, st) == REC_STEP_AUDIO_ERROR);
    pocketaudio_close(st);
    check("and what came before it is saved", rec_file_finish(&r.file, final, sizeof(final)) == 0 &&
                                                  probe(final, &i) == 0 && i.frames == 1920);

    reset_device();
    set_script(bad, 2);
    rec_recorder_open(&r, dir, "REC-0005.wav", 48000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    rec_recorder_step(&r, st);
    check("a device that returns more than asked is refused, not trusted",
          rec_recorder_step(&r, st) == REC_STEP_AUDIO_ERROR && r.file.data_bytes == 1920);
    pocketaudio_close(st);
    rec_file_finish(&r.file, final, sizeof(final));

    reset_device();
    rec_recorder_open(&r, dir, "REC-0006.wav", 48000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    rec_file_fail_after = 5000;
    rec_recorder_step(&r, st);
    rec_recorder_step(&r, st);
    check("a disk that fills up is a full disk", rec_recorder_step(&r, st) == REC_STEP_STORAGE_FULL &&
                                                    r.last_errno == ENOSPC);
    rec_file_fail_after = -1;
    pocketaudio_close(st);
    check("and the audio that fit is saved", rec_file_finish(&r.file, final, sizeof(final)) == 0 &&
                                                 probe(final, &i) == 0 && i.frames == 2500);

    reset_device();
    rec_recorder_open(&r, dir, "REC-0007.wav", 48000);
    st = open_stream(POCKETAUDIO_CAPTURE);
    r.file.data_bytes = POCKETWAV_MAX_DATA_BYTES - 1000; /* six hours in */
    check("at the WAV limit the step writes what fits and says so",
          rec_recorder_step(&r, st) == REC_STEP_LIMIT_LENGTH && r.file.data_bytes == POCKETWAV_MAX_DATA_BYTES);
    pocketaudio_close(st);
    r.file.data_bytes = 1000;
    rec_file_finish(&r.file, final, sizeof(final));
    check("a rate other than the two presets is refused", rec_recorder_open(&r, dir, "REC-0008.wav", 44100) == -EINVAL);
}

/* ---- playback -------------------------------------------------------------- */

static void make_wav(const char *name, unsigned rate, unsigned ch, size_t frames, double amp, int16_t right)
{
    char path[256];
    uint8_t h[44];
    FILE *f;
    size_t i;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    pocketwav_header(h, rate, ch, (uint32_t)(frames * ch * 2));
    fwrite(h, 1, 44, f);
    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(amp * sin(2.0 * M_PI * 440.0 * (double)i / rate));

        fwrite(&v, 2, 1, f);
        if (ch == 2) {
            fwrite(&right, 2, 1, f);
        }
    }
    fclose(f);
}

static void test_play(void)
{
    static const struct step flaky[] = {
        { ACCEPT_SOME, 100 }, { TIMEOUT, 0 }, { ACCEPT_SOME, 1 }, { FULL, 0 }, { TIMEOUT, 0 },
    };
    struct rec_player p;
    struct pocketaudio_stream *st;
    char path[256];
    enum rec_step x;
    size_t i;
    int at_ceiling = 0;
    int over = 0;
    int steps = 0;

    reset_device();
    make_wav("loud16.wav", 16000, 1, 16000, 30000.0, 0);
    snprintf(path, sizeof(path), "%s/loud16.wav", dir);
    check("a 16 kHz recording opens for playback", rec_player_open(&p, path, 0) == 0 &&
                                                        rec_player_total_ms(&p) == 1000);
    set_script(flaky, 5);
    st = open_stream(POCKETAUDIO_PLAYBACK);
    while ((x = rec_player_step(&p, st)) == REC_STEP_OK || x == REC_STEP_WAIT) {
        steps++;
    }
    pocketaudio_close(st);
    rec_player_close(&p);
    check("it plays to the end, through partial accepts and timeouts", x == REC_STEP_END);
    check("as exactly three device samples per file sample", played_n == 48000);
    {
        int run = 0;

        for (i = 0; i < played_n; i++) {
            run = played[i] == 8192 || played[i] == -8192 ? run + 1 : 0;
            at_ceiling = run > at_ceiling ? run : at_ceiling;
            over |= played[i] > 8192 || played[i] < -8192;
        }
    }
    printf("note longest run at the ceiling: %d samples\n", at_ceiling);
    /* Clipping a 440 Hz sine at a quarter of its amplitude would flatten
     * about 70 samples of every half cycle; turning it down touches the
     * ceiling at the crest only. */
    check("a loud recording is turned down, not clipped: never over the ceiling, no flat tops",
          !over && at_ceiling <= 3);

    reset_device();
    make_wav("stereo48.wav", 48000, 2, 4800, 0.0, 3000);
    snprintf(path, sizeof(path), "%s/stereo48.wav", dir);
    rec_player_open(&p, path, 0);
    st = open_stream(POCKETAUDIO_PLAYBACK);
    while (rec_player_step(&p, st) == REC_STEP_OK) {
    }
    pocketaudio_close(st);
    rec_player_close(&p);
    check("a stereo file is mixed to mono", played_n == 4800 && played[100] == 1500);

    reset_device();
    make_wav("seek.wav", 48000, 1, 48000, 1000.0, 0);
    snprintf(path, sizeof(path), "%s/seek.wav", dir);
    rec_player_open(&p, path, 750);
    check("playback can start part-way", rec_player_pos_ms(&p) == 750);
    st = open_stream(POCKETAUDIO_PLAYBACK);
    while (rec_player_step(&p, st) == REC_STEP_OK) {
    }
    pocketaudio_close(st);
    rec_player_close(&p);
    check("and plays only the rest", played_n == 12000 && rec_player_pos_ms(&p) == 1000);

    reset_device();
    make_wav("cd.wav", 44100, 2, 100, 0.0, 0);
    snprintf(path, sizeof(path), "%s/cd.wav", dir);
    check("a 44.1 kHz file is refused before any device is opened",
          rec_player_open(&p, path, 0) == POCKETWAV_E_UNSUPPORTED);
    snprintf(path, sizeof(path), "%s/missing.wav", dir);
    check("a missing file is an I/O error", rec_player_open(&p, path, 0) == POCKETWAV_E_IO);

    /* A file whose header promises more than it holds. */
    reset_device();
    make_wav("cut.wav", 48000, 1, 4800, 1000.0, 0);
    snprintf(path, sizeof(path), "%s/cut.wav", dir);
    if (truncate(path, 44 + 2000) != 0) {
        printf("note truncate failed\n");
    }
    rec_player_open(&p, path, 0);
    st = open_stream(POCKETAUDIO_PLAYBACK);
    while (rec_player_step(&p, st) == REC_STEP_OK) {
    }
    pocketaudio_close(st);
    rec_player_close(&p);
    check("a file shorter than its header plays what it has, and ends", played_n == 1000);

    {
        static const struct step dead[] = { { FAIL, 0 } };

        reset_device();
        make_wav("fail.wav", 48000, 1, 4800, 1000.0, 0);
        snprintf(path, sizeof(path), "%s/fail.wav", dir);
        rec_player_open(&p, path, 0);
        set_script(dead, 1);
        st = open_stream(POCKETAUDIO_PLAYBACK);
        check("a speaker that fails is an audio error", rec_player_step(&p, st) == REC_STEP_AUDIO_ERROR);
        pocketaudio_close(st);
        rec_player_close(&p);
    }
}

static int64_t fake_free(const char *d)
{
    (void)d;
    return 12345;
}

int main(void)
{
    char cmd[80];

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_record_standard();
    test_record_voice();
    test_record_failures();
    test_play();
    check("the free space is statvfs's", rec_free_bytes(dir) > 0 && rec_free_bytes("/nonexistent/x") == -ENOENT);
    rec_free_hook = fake_free;
    check("or the test seam's", rec_free_bytes(dir) == 12345);
    rec_free_hook = NULL;
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", dir);
    }
    printf("rec_engine_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
