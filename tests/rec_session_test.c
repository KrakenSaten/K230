/*
 * The Recorder's helper client (apps/recorder/rec_session.h), against real
 * processes: a scripted fake that floods, sends garbage, over-long lines,
 * ignores every stop or dies by a signal (tests/fake_pos_record.sh), and the
 * real pos-record built with test hooks over the file-backed sound card
 * (tests/fake_audio_backend.c), recording, pausing, resuming, stopping,
 * playing, being killed and repaired, running out of space before and
 * during a recording, finding the audio in use, and being abandoned by a
 * closing app.
 *
 * Usage: rec_session_test <fake_pos_record.sh> <pos-record-testhooks>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketwav/pocketwav.h"
#include "rec_protocol.h"
#include "rec_session.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;
static const char *fake;
static const char *helper;
static char tmp[] = "/tmp/rec_session_test.XXXXXX";
static char recs[256];
static char audio[256];
static char run[256];
static char freefile[256];

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

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

/* What arrived while waiting. */
struct seen {
    int levels;
    int max_peak;
    int progress;
    int64_t last_ms;
    int exited;
    int code;
    int kinds[REC_EV_EXITED + 1];
    char saved[REC_EVENT_TEXT_MAX];
    char error[REC_EVENT_TEXT_MAX];
    char limit[16];
    char repaired[REC_EVENT_TEXT_MAX];
};

static void take(struct seen *s, const struct rec_event *ev)
{
    s->kinds[ev->kind]++;
    switch (ev->kind) {
    case REC_EV_LEVEL:
        s->levels++;
        if (ev->a > s->max_peak) {
            s->max_peak = (int)ev->a;
        }
        break;
    case REC_EV_PROGRESS:
        s->progress++;
        s->last_ms = ev->a;
        break;
    case REC_EV_SAVED: snprintf(s->saved, sizeof(s->saved), "%s", ev->text); break;
    case REC_EV_ERROR: snprintf(s->error, sizeof(s->error), "%s", ev->text); break;
    case REC_EV_LIMIT: snprintf(s->limit, sizeof(s->limit), "%.15s", ev->text); break;
    case REC_EV_REPAIRED: snprintf(s->repaired, sizeof(s->repaired), "%s", ev->text); break;
    case REC_EV_EXITED:
        s->exited = 1;
        s->code = ev->c;
        break;
    default: break;
    }
}

/* Poll until an event of kind arrives (1) or ms pass (0). */
static int wait_for(struct rec_session *ss, struct seen *s, enum rec_event_kind kind, int ms)
{
    int64_t end = mono_ms() + ms;
    struct rec_event ev;

    while (mono_ms() < end) {
        while (rec_session_poll(ss, &ev, mono_ms())) {
            take(s, &ev);
            if (ev.kind == kind) {
                return 1;
            }
        }
        nap(10);
    }
    return 0;
}

static void drain_for(struct rec_session *ss, struct seen *s, int ms)
{
    int64_t end = mono_ms() + ms;
    struct rec_event ev;

    while (mono_ms() < end) {
        while (rec_session_poll(ss, &ev, mono_ms())) {
            take(s, &ev);
        }
        nap(10);
    }
}

static int alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int path_exists(const char *d, const char *name)
{
    char p[512];
    struct stat st;

    snprintf(p, sizeof(p), "%s/%s", d, name);
    return stat(p, &st) == 0;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void get(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");

    out[0] = '\0';
    if (f) {
        if (!fgets(out, (int)n, f)) {
            out[0] = '\0';
        }
        fclose(f);
    }
    out[strcspn(out, "\n")] = '\0';
}

static void set_free(unsigned long long bytes)
{
    char v[32];

    snprintf(v, sizeof(v), "%llu\n", bytes);
    put(freefile, v);
}

/* ---- parsing -------------------------------------------------------------- */

static void test_parse(void)
{
    struct rec_event ev;

    check("recording: rate and name", rec_session_parse_line("recording 16000 REC-0001.wav", &ev) &&
                                          ev.kind == REC_EV_RECORDING && ev.a == 16000 &&
                                          strcmp(ev.text, "REC-0001.wav") == 0);
    check("level: peak and rms", rec_session_parse_line("level 1200 300", &ev) && ev.a == 1200 && ev.b == 300);
    check("a level above full scale is refused", !rec_session_parse_line("level 32768 1", &ev));
    check("progress: ms and bytes, 64-bit",
          rec_session_parse_line("progress 21600000 2073600044", &ev) && ev.a == 21600000 &&
              ev.b == 2073600044LL);
    check("saved: ms, bytes, gaps, name", rec_session_parse_line("saved 42000 1344044 2 REC-0001.wav", &ev) &&
                                              ev.kind == REC_EV_SAVED && ev.c == 2 &&
                                              strcmp(ev.text, "REC-0001.wav") == 0);
    check("a saved name with a slash is refused", !rec_session_parse_line("saved 1 2 0 ../x.wav", &ev));
    check("playing needs 1 or 2 channels", rec_session_parse_line("playing 1000 16000 1", &ev) &&
                                               !rec_session_parse_line("playing 1000 16000 3", &ev));
    check("limit words are exact", rec_session_parse_line("limit space", &ev) &&
                                       !rec_session_parse_line("limit spacious", &ev));
    check("bare words take nothing after them", rec_session_parse_line("paused", &ev) &&
                                                    !rec_session_parse_line("paused now", &ev) &&
                                                    !rec_session_parse_line("pausedx", &ev));
    check("an error keeps its text", rec_session_parse_line("error audio_busy another audio stream is open", &ev) &&
                                         strcmp(ev.text, "audio_busy another audio stream is open") == 0);
    check("an empty error is refused", !rec_session_parse_line("error", &ev));
    check("repaired and damaged carry names", rec_session_parse_line("repaired REC-0001-recovered.wav", &ev) &&
                                                  ev.kind == REC_EV_REPAIRED &&
                                                  rec_session_parse_line("damaged REC-0002.wav.part", &ev) &&
                                                  ev.kind == REC_EV_DAMAGED);
    check("an unknown word is not an event", !rec_session_parse_line("novel 1", &ev));
    check("an over-long number is refused", !rec_session_parse_line("progress 1234567890123456 1", &ev));
}

/* ---- the misbehaving fake ------------------------------------------------- */

static void test_fake(void)
{
    struct rec_session s;
    struct seen seen;
    char err[160];
    char mark[300];
    char pidf[300];
    char pidtext[32];
    int64_t t0;

    snprintf(mark, sizeof(mark), "%s/recover.mark", tmp);
    snprintf(pidf, sizeof(pidf), "%s/fake.pid", tmp);
    setenv("REC_FAKE_RECOVER_MARK", mark, 1);
    setenv("REC_FAKE_PIDFILE", pidf, 1);

    rec_session_init(&s);
    setenv("REC_FAKE", "flood", 1);
    memset(&seen, 0, sizeof(seen));
    check("a helper starts", rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err)) == 0);
    check("a second start while one runs is refused",
          rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err)) != 0);
    nap(600); /* let it write thousands of lines before anyone reads */
    check("a flood of meter readings is bounded, and the state words survive it",
          wait_for(&s, &seen, REC_EV_EXITED, 5000) && seen.kinds[REC_EV_RECORDING] == 1 &&
              seen.kinds[REC_EV_SAVED] == 1 && strcmp(seen.saved, "REC-0001.wav") == 0 &&
              seen.code == 0);
    printf("note %d of 3000 level readings kept, %u dropped\n", seen.levels, s.dropped);
    check("readings were dropped rather than queued without bound", s.dropped > 0 && seen.levels < 3000);
    check("the session is idle after the exit", !rec_session_active(&s) && s.fd < 0);

    setenv("REC_FAKE", "overlong", 1);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    check("an over-long line is one protocol error, and the next line still reads",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.kinds[REC_EV_ERROR] == 1 &&
              strstr(seen.error, "too long") && seen.kinds[REC_EV_STOPPED] == 1);

    setenv("REC_FAKE", "garbage", 1);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    wait_for(&s, &seen, REC_EV_EXITED, 3000);
    check("malformed lines are ignored, not half-believed",
          seen.levels == 0 && seen.kinds[REC_EV_SAVED] == 0 && seen.kinds[REC_EV_RECORDING] == 0 &&
              seen.kinds[REC_EV_PLAYING] == 0 && seen.kinds[REC_EV_LIMIT] == 0 &&
              seen.kinds[REC_EV_STOPPED] == 1 && seen.code == 2);

    setenv("REC_FAKE", "echo", 1);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    check("pause reaches the helper", rec_session_command(&s, "pause") == 0 &&
                                          wait_for(&s, &seen, REC_EV_PAUSED, 3000));
    check("resume reaches the helper", rec_session_command(&s, "resume") == 0 &&
                                           wait_for(&s, &seen, REC_EV_RESUMED, 3000));
    check("anything else is not sent", rec_session_command(&s, "rm -rf") != 0);
    rec_session_stop(&s, mono_ms());
    check("stop is a line and a signal; the helper saves and exits 0",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.kinds[REC_EV_SAVED] == 1 && seen.code == 0);
    check("a command to no helper fails", rec_session_command(&s, "pause") != 0);
    rec_session_stop(&s, mono_ms());
    check("stop with no helper is harmless", !rec_session_active(&s));

    setenv("REC_FAKE", "deaf", 1);
    unlink(mark);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    t0 = mono_ms();
    rec_session_stop(&s, t0);
    /* Pretend the grace has passed. */
    {
        struct rec_event ev;

        while (rec_session_poll(&s, &ev, t0 + REC_STOP_GRACE_MS + 1)) {
            take(&seen, &ev);
        }
    }
    check("a helper that ignores stop is killed after the grace",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == 128 + SIGKILL);
    nap(300);
    get(mark, pidtext, sizeof(pidtext));
    check("and the audio route recovery was started, detached", strcmp(pidtext, "recover") == 0 &&
                                                                  s.recoveries == 1);

    setenv("REC_FAKE", "deaf", 1);
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    memset(&seen, 0, sizeof(seen));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    get(pidf, pidtext, sizeof(pidtext));
    t0 = mono_ms();
    rec_session_abandon(&s, 300);
    check("abandoning a deaf helper takes the grace and a reap, no more",
          mono_ms() - t0 < 300 + REC_KILL_REAP_MS + 300);
    check("and it is gone", !alive((pid_t)atoi(pidtext)) && !rec_session_active(&s));

    setenv("REC_FAKE", "crash", 1);
    unlink(mark);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, fake, 16000, "/tmp/x/REC-0001.wav", err, sizeof(err));
    check("a helper killed by a signal is reported as such",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == 128 + SIGKILL);
    nap(300);
    get(mark, pidtext, sizeof(pidtext));
    check("and its audio route is recovered", strcmp(pidtext, "recover") == 0);

    memset(&seen, 0, sizeof(seen));
    check("a helper that does not exist starts (the fork does)",
          rec_session_start_record(&s, "/nonexistent/pos-record", 16000, "/tmp/x/REC-0001.wav", err,
                                   sizeof(err)) == 0);
    check("and says so, exiting 127", wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == 127 &&
                                         seen.kinds[REC_EV_ERROR] == 1);
    check("requests that are not ours are refused before anything starts",
          rec_session_start_record(&s, fake, 44100, "/tmp/x/REC-0001.wav", err, sizeof(err)) != 0 &&
              rec_session_start_record(&s, fake, 16000, "relative.wav", err, sizeof(err)) != 0 &&
              rec_session_start_play(&s, fake, "/tmp/x.wav", -1, 100, err, sizeof(err)) != 0 &&
              rec_session_start_play(&s, fake, "/tmp/x.wav", 0, 0, err, sizeof(err)) != 0 &&
              rec_session_start_recover(&s, fake, "-rf", err, sizeof(err)) != 0 && !rec_session_active(&s));
}

/* ---- the real helper ------------------------------------------------------ */

static void make_capture(void)
{
    char path[300];
    FILE *f;
    int i;

    snprintf(path, sizeof(path), "%s/capture.raw", audio);
    f = fopen(path, "wb");
    for (i = 0; i < 48000 * 20; i++) {
        int16_t v = (int16_t)lrint(10000.0 * sin(2.0 * M_PI * 440.0 * i / 48000.0));

        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

static void pcm_state(char *out, size_t n)
{
    char p[300];

    snprintf(p, sizeof(p), "%s/pcm", audio);
    get(p, out, n);
}

static void test_real(void)
{
    struct rec_session s;
    struct seen seen;
    struct pocketwav_info info;
    char err[160];
    char path[512];
    char state[32];
    char route[300];
    int fd;

    rec_session_init(&s);
    snprintf(path, sizeof(path), "%s/REC-0001.wav", recs);
    memset(&seen, 0, sizeof(seen));
    check("the real helper starts a Voice recording",
          rec_session_start_record(&s, helper, 16000, path, err, sizeof(err)) == 0 &&
              wait_for(&s, &seen, REC_EV_RECORDING, 3000));
    check("its .part exists while it records", path_exists(recs, "REC-0001.wav.part") &&
                                                  !path_exists(recs, "REC-0001.wav"));
    drain_for(&s, &seen, 1500);
    pcm_state(state, sizeof(state));
    check("the microphone is open", strcmp(state, "capture") == 0);
    check("the meter follows the real samples (a 10000 sine after the discard)",
          seen.levels >= 5 && seen.max_peak > 9000 && seen.max_peak < 11500);
    check("progress counts the audio written", seen.progress >= 3 && seen.last_ms > 500);
    check("pause", rec_session_command(&s, "pause") == 0 && wait_for(&s, &seen, REC_EV_PAUSED, 2000));
    pcm_state(state, sizeof(state));
    check("closes the microphone while paused", strcmp(state, "closed") == 0);
    check("resume", rec_session_command(&s, "resume") == 0 && wait_for(&s, &seen, REC_EV_RESUMED, 2000));
    drain_for(&s, &seen, 900);
    rec_session_stop(&s, mono_ms());
    check("stop saves it and the helper exits 0",
          wait_for(&s, &seen, REC_EV_EXITED, 5000) && seen.code == 0 && strcmp(seen.saved, "REC-0001.wav") == 0);
    pcm_state(state, sizeof(state));
    check("nothing is left open, nothing left behind", strcmp(state, "closed") == 0 &&
                                                        !path_exists(recs, "REC-0001.wav.part"));
    fd = open(path, O_RDONLY);
    check("the file is a complete 16 kHz mono WAV of one to three seconds",
          pocketwav_probe_fd(fd, &info) == 0 && info.rate == 16000 && info.channels == 1 &&
              !info.truncated && !info.trailing && info.frames > 16000 && info.frames < 16000 * 3);
    printf("note recorded %llu frames\n", (unsigned long long)info.frames);
    close(fd);
    {
        struct stat st;

        check("and private (0600)", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    }

    /* Playback of what was recorded. */
    memset(&seen, 0, sizeof(seen));
    check("the recording plays", rec_session_start_play(&s, helper, path, 0, 100, err, sizeof(err)) == 0 &&
                                     wait_for(&s, &seen, REC_EV_PLAYING, 3000));
    pcm_state(state, sizeof(state));
    check("through the speaker path", strcmp(state, "playback") == 0);
    check("to its end", wait_for(&s, &seen, REC_EV_EXITED, 6000) && seen.kinds[REC_EV_PLAYED] == 1 &&
                            seen.code == 0 && seen.levels > 3);
    snprintf(route, sizeof(route), "%s/amp", audio);
    get(route, state, sizeof(state));
    check("the amplifier is off after it", strcmp(state, "0") == 0);

    /* Killed mid-recording: the .part stays, the repair saves it. */
    snprintf(path, sizeof(path), "%s/REC-0002.wav", recs);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, helper, 48000, path, err, sizeof(err));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    drain_for(&s, &seen, 2600); /* past a checkpoint */
    kill(s.pid, SIGKILL);
    check("a helper killed while recording is seen as killed",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == 128 + SIGKILL);
    check("its .part is left, with its audio", path_exists(recs, "REC-0002.wav.part") &&
                                                 !path_exists(recs, "REC-0002.wav"));
    nap(300);
    snprintf(route, sizeof(route), "%s/route", audio);
    get(route, state, sizeof(state));
    check("the detached recovery put the route back", strcmp(state, "1") == 0);
    memset(&seen, 0, sizeof(seen));
    check("the repair runs", rec_session_start_recover(&s, helper, recs, err, sizeof(err)) == 0 &&
                                 wait_for(&s, &seen, REC_EV_EXITED, 5000) && seen.code == 0);
    check("and saves it as -recovered", strcmp(seen.repaired, "REC-0002-recovered.wav") == 0 &&
                                            path_exists(recs, "REC-0002-recovered.wav") &&
                                            !path_exists(recs, "REC-0002.wav.part"));
    snprintf(path, sizeof(path), "%s/REC-0002-recovered.wav", recs);
    fd = open(path, O_RDONLY);
    check("with at least the audio up to the last checkpoint",
          pocketwav_probe_fd(fd, &info) == 0 && info.rate == 48000 && info.frames >= 48000 && !info.truncated);
    close(fd);

    /* Space. */
    set_free(REC_RESERVE_BYTES + 100000); /* under the reserve plus ten seconds */
    snprintf(path, sizeof(path), "%s/REC-0003.wav", recs);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, helper, 16000, path, err, sizeof(err));
    check("with too little free space, a recording does not start",
          wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == REC_EXIT_STORAGE &&
              strncmp(seen.error, REC_ERR_STORAGE_FULL, strlen(REC_ERR_STORAGE_FULL)) == 0 &&
              seen.kinds[REC_EV_READY] == 0 && !path_exists(recs, "REC-0003.wav.part"));
    set_free(REC_RESERVE_BYTES + 100000000ull);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, helper, 16000, path, err, sizeof(err));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    drain_for(&s, &seen, 1200);
    set_free(REC_RESERVE_BYTES - 1);
    check("reaching the reserve stops and saves the recording",
          wait_for(&s, &seen, REC_EV_EXITED, 4000) && strcmp(seen.limit, "space") == 0 &&
              strcmp(seen.saved, "REC-0003.wav") == 0 && seen.code == 0);
    set_free(10000000000ull);

    setenv("POS_RECORD_FAIL_AFTER", "20000", 1);
    snprintf(path, sizeof(path), "%s/REC-0004.wav", recs);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, helper, 48000, path, err, sizeof(err));
    check("a disk that fills during a recording stops it and saves what fit",
          wait_for(&s, &seen, REC_EV_EXITED, 5000) && strcmp(seen.limit, "space") == 0 &&
              strcmp(seen.saved, "REC-0004.wav") == 0);
    unsetenv("POS_RECORD_FAIL_AFTER");
    snprintf(path, sizeof(path), "%s/REC-0004.wav", recs);
    fd = open(path, O_RDONLY);
    check("and that file is complete and truthful", pocketwav_probe_fd(fd, &info) == 0 &&
                                                        info.data_present == 20000 && !info.truncated);
    close(fd);

    /* The audio is somebody else's. */
    {
        char lock[300];
        int lfd;

        snprintf(lock, sizeof(lock), "%s/audio.lock", run);
        lfd = open(lock, O_RDWR | O_CREAT, 0644);
        flock(lfd, LOCK_EX);
        snprintf(path, sizeof(path), "%s/REC-0005.wav", recs);
        memset(&seen, 0, sizeof(seen));
        rec_session_start_record(&s, helper, 16000, path, err, sizeof(err));
        check("audio in use is said as such, and nothing is created",
              wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == REC_EXIT_AUDIO &&
                  strncmp(seen.error, REC_ERR_AUDIO_BUSY, strlen(REC_ERR_AUDIO_BUSY)) == 0 &&
                  !path_exists(recs, "REC-0005.wav.part"));
        close(lfd);
    }

    /* The app closes while recording. */
    snprintf(path, sizeof(path), "%s/REC-0006.wav", recs);
    memset(&seen, 0, sizeof(seen));
    rec_session_start_record(&s, helper, 16000, path, err, sizeof(err));
    wait_for(&s, &seen, REC_EV_RECORDING, 3000);
    drain_for(&s, &seen, 700);
    {
        int64_t t0 = mono_ms();

        rec_session_abandon(&s, 1500);
        printf("note abandon took %lld ms\n", (long long)(mono_ms() - t0));
    }
    check("closing the app mid-recording saves it: no .part, a complete file",
          !rec_session_active(&s) && path_exists(recs, "REC-0006.wav") && !path_exists(recs, "REC-0006.wav.part"));
    pcm_state(state, sizeof(state));
    check("and the microphone is closed", strcmp(state, "closed") == 0);

    /* Repeated open, record, stop, close leaks no process. */
    {
        int k;
        int ok = 1;

        for (k = 0; k < 10; k++) {
            snprintf(path, sizeof(path), "%s/REC-%04d.wav", recs, 100 + k);
            memset(&seen, 0, sizeof(seen));
            ok &= rec_session_start_record(&s, helper, 16000, path, err, sizeof(err)) == 0;
            ok &= wait_for(&s, &seen, REC_EV_RECORDING, 3000);
            rec_session_stop(&s, mono_ms());
            ok &= wait_for(&s, &seen, REC_EV_EXITED, 3000) && seen.code == 0;
        }
        check("ten record/stop cycles: each saves or reports empty, each helper exits", ok);
        check("and no child is left", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    }
}

int main(int argc, char **argv)
{
    char cmd[300];
    char route[300];

    if (argc < 3) {
        fprintf(stderr, "usage: rec_session_test <fake_pos_record.sh> <pos-record-testhooks>\n");
        return 2;
    }
    fake = argv[1];
    helper = argv[2];
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(recs, sizeof(recs), "%s/Recordings", tmp);
    snprintf(audio, sizeof(audio), "%s/audio", tmp);
    snprintf(run, sizeof(run), "%s/run", tmp);
    snprintf(freefile, sizeof(freefile), "%s/free", tmp);
    mkdir(recs, 0700);
    mkdir(audio, 0700);
    mkdir(run, 0700);
    snprintf(route, sizeof(route), "%s/route", audio);
    put(route, "1\n");
    make_capture();
    set_free(10000000000ull);
    setenv("POS_RECORD_FAKE_AUDIO", audio, 1);
    setenv("POS_RECORD_FREE_FILE", freefile, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "capture,playback", 1);
    signal(SIGPIPE, SIG_IGN);

    test_parse();
    test_fake();
    test_real();

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", tmp);
    }
    printf("rec_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
