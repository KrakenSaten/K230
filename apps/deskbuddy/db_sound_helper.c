/*
 * DeskBuddy's visit sound through Doors's existing audio path. See
 * db_sound.h.
 *
 * One `pos-record play` per sound: the Recorder's helper, unchanged, on the
 * same core/pocketaudio (ADR-010 Amendment 2), started the way RIFT starts
 * it for its message sounds (apps/rift/rift_sound_helper.c). It takes the
 * one audio lock - so a sound while Wave, the Recorder or another player
 * holds the card is simply not heard, never mixed in - sets the route,
 * enables the amplifier, applies the level ceiling and the system volume it
 * is given, and undoes all of it when the file ends. DeskBuddy only writes
 * one short WAV file and starts the helper; it never opens audio itself.
 *
 * The helper's stdin and stdout are a socketpair held open until it is
 * reaped: pos-record treats a closed stdin as a stop. It leaves with the
 * shell (PR_SET_PDEATHSIG). One sound at a time: a play while the last is
 * still sounding is refused, never queued. Reaping is non-blocking, on the
 * next play, availability check or stop; stop is bounded (SIGTERM, then
 * SIGKILL after STOP_GRACE_MS), and a helper that had to be killed leaves
 * its route to `pos-record recover`, started detached, as the Recorder does.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_sound.h"

#include "pocketpaths.h"
#include "pocketwav/pocketwav.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-record"
#define RATE 48000
/* Room for the sound, with margin. */
#define TONE_FRAMES_MAX (RATE / 2)
#define STOP_GRACE_MS 300
#define CHILD_FD_SCAN_MAX 1024

static pid_t child = -1;
static int child_fd = -1;
static char wav_path[POCKETOS_PATH_MAX + 32];

/* The same override the Recorder and RIFT honour, so a bench or a test can
 * point all of them at another build of the helper. */
static const char *helper(void)
{
    const char *p = getenv("POCKETOS_RECORD_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

/* ---- the sound ------------------------------------------------------------- */

/* sin(x) for any x, to about 1e-6, without libm: reduced to [-pi, pi] and a
 * Taylor polynomial. Good enough for a 16-bit tone. */
static double sine(double x)
{
    const double pi = 3.14159265358979323846;
    double x2;

    x -= (double)(long)(x / (2 * pi)) * 2 * pi;
    if (x > pi) {
        x -= 2 * pi;
    } else if (x < -pi) {
        x += 2 * pi;
    }
    if (x > pi / 2) {
        x = pi - x;
    } else if (x < -pi / 2) {
        x = -pi - x;
    }
    x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

struct note {
    double hz;
    int ms;
    double level;  /* of full scale */
    double tau_ms; /* exponential decay */
};

/* Short, and not one of RIFT's: three notes falling (E6, C6, A5), a door
 * chime rather than a message ping, in the 1 - 2 kHz band a small speaker
 * carries best. */
static const struct note visit_notes[] = {
    { 1318.5, 80, 0.30, 50.0 },
    { 0.0, 15, 0.0, 1.0 },
    { 1046.5, 80, 0.30, 50.0 },
    { 0.0, 15, 0.0, 1.0 },
    { 880.0, 140, 0.28, 60.0 },
};
/* Silence first: the amplifier is enabled as the stream opens, and the
 * first milliseconds are where a pop or a cut would be. */
#define LEAD_MS 30
#define ATTACK_MS 4
#define RELEASE_MS 8

size_t db_sound_tone_render(int16_t *out, size_t max_frames)
{
    size_t count = sizeof(visit_notes) / sizeof(visit_notes[0]);
    size_t total = (size_t)LEAD_MS * RATE / 1000;
    size_t at = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        total += (size_t)visit_notes[i].ms * RATE / 1000;
    }
    if (!out) {
        return total;
    }
    if (total > max_frames) {
        return 0;
    }
    for (; at < (size_t)LEAD_MS * RATE / 1000; at++) {
        out[at] = 0;
    }
    for (i = 0; i < count; i++) {
        const struct note *n = &visit_notes[i];
        size_t len = (size_t)n->ms * RATE / 1000;
        size_t attack = (size_t)ATTACK_MS * RATE / 1000;
        size_t release = (size_t)RELEASE_MS * RATE / 1000;
        double decay = 1.0 - 1000.0 / (n->tau_ms * RATE); /* per sample */
        double env = 1.0;
        size_t k;

        for (k = 0; k < len; k++, at++) {
            double gain = env * n->level;

            if (k < attack) {
                gain *= (double)k / (double)attack;
            }
            if (k + release > len) {
                gain *= (double)(len - k) / (double)release;
            }
            out[at] = n->hz > 0 ? (int16_t)(32767.0 * gain *
                                            sine(2 * 3.14159265358979323846 * n->hz * (double)k / RATE))
                                : 0;
            env *= decay;
        }
    }
    return at;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t w = write(fd, p, len);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

const char *db_sound_wav_path(void)
{
    static int16_t pcm[TONE_FRAMES_MAX];
    uint8_t header[POCKETWAV_HEADER_BYTES];
    char dir[POCKETOS_PATH_MAX];
    char tmp[sizeof(wav_path) + 8];
    size_t frames;
    size_t i;
    int fd;

    snprintf(dir, sizeof(dir), "%s/deskbuddy", pocketos_runtime_dir());
    snprintf(wav_path, sizeof(wav_path), "%s/visit.wav", dir);
    /* The runtime directory is a tmpfs: written once per boot, again if it
     * was cleared. */
    if (access(wav_path, R_OK) == 0) {
        return wav_path;
    }
    frames = db_sound_tone_render(pcm, TONE_FRAMES_MAX);
    if (frames == 0 || pocketos_mkdir_p(dir, 0755) != 0) {
        return NULL;
    }
    /* Little-endian samples, as the WAV container is, whatever the host. */
    for (i = 0; i < frames; i++) {
        uint16_t v = (uint16_t)pcm[i];
        uint8_t *b = (uint8_t *)&pcm[i];

        b[0] = (uint8_t)(v & 0xFF);
        b[1] = (uint8_t)(v >> 8);
    }
    pocketwav_header(header, RATE, 1, (uint32_t)(frames * 2));
    snprintf(tmp, sizeof(tmp), "%s.tmp", wav_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return NULL;
    }
    if (write_all(fd, header, sizeof(header)) != 0 || write_all(fd, pcm, frames * 2) != 0) {
        close(fd);
        unlink(tmp);
        return NULL;
    }
    if (close(fd) != 0 || rename(tmp, wav_path) != 0) {
        unlink(tmp);
        return NULL;
    }
    return wav_path;
}

/* ---- the helper ------------------------------------------------------------ */

static void close_inherited(void)
{
    int fd;

    for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
        close(fd);
    }
}

/* Not waited for: a helper that had to be killed did not restore the audio
 * route, and the next audio user would - this just does it now. */
static void start_recover(void)
{
    pid_t mid = fork();
    int status;

    if (mid == 0) {
        if (fork() == 0) {
            char *argv[] = { (char *)helper(), "recover", NULL };
            int null = open("/dev/null", O_RDWR);

            setsid();
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
            }
            close_inherited();
            execv(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (mid > 0) {
        while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

static void reap(void)
{
    int status;

    if (child > 0 && waitpid(child, &status, WNOHANG) == child) {
        child = -1;
    }
    if (child <= 0 && child_fd >= 0) {
        close(child_fd);
        child_fd = -1;
    }
}

static int helper_available(void)
{
    reap();
    return access(helper(), X_OK) == 0;
}

static int helper_play(int volume_percent)
{
    char volume[16];
    const char *path;
    char *argv[6];
    pid_t parent = getpid();
    int sv[2];
    pid_t pid;

    reap();
    if (child > 0) {
        return -1; /* the last one is still sounding: never stacked */
    }
    path = db_sound_wav_path();
    if (!path) {
        return -1;
    }
    snprintf(volume, sizeof(volume), "%d", volume_percent);
    argv[0] = (char *)helper();
    argv[1] = "play";
    argv[2] = "--volume-percent";
    argv[3] = volume;
    argv[4] = (char *)path;
    argv[5] = NULL;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;

        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(sv[1], 0) < 0 || dup2(sv[1], 1) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        close_inherited();
        execv(argv[0], argv);
        _exit(127);
    }
    close(sv[1]);
    child = pid;
    child_fd = sv[0];
    return 0;
}

static void helper_stop(void)
{
    struct timespec nap = { 0, 10 * 1000 * 1000 };
    int status;
    int waited;

    reap();
    if (child <= 0) {
        return;
    }
    kill(child, SIGTERM);
    for (waited = 0; waited < STOP_GRACE_MS; waited += 10) {
        if (waitpid(child, &status, WNOHANG) == child) {
            child = -1;
            reap();
            return;
        }
        nanosleep(&nap, NULL);
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    child = -1;
    reap();
    start_recover();
}

const struct db_sound_backend db_sound_pos_record = {
    .name = "pos-record",
    .available = helper_available,
    .play = helper_play,
    .stop = helper_stop,
    .why = "pos-record, Doors's audio helper, is not installed: a visit is shown and logged, not heard.",
};
