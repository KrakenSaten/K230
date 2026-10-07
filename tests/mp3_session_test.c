/*
 * The MP3 app's helper client (apps/mp3/mp3_session.h), against real
 * processes: a scripted fake that floods, sends garbage and over-long lines,
 * ignores every stop or dies by a signal (tests/fake_pos_mp3.sh), and the
 * real pos-mp3 built with test hooks (its WAV decoder) over the file-backed
 * sound card (tests/fake_audio_backend.c) - playing to the end, pausing
 * (the device closed), resuming, seeking, starting part way, changing the
 * volume, stopping, being abandoned, and refusing every kind of bad file
 * before the device is touched: missing, empty, not audio, failing to
 * decode part way, the storage failing, and the device in use.
 *
 * Usage: mp3_session_test <fake_pos_mp3.sh> <pos-mp3-testhooks>
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_protocol.h"
#include "mp3_session.h"
#include "pocketwav/pocketwav.h"

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
static char tmp[] = "/tmp/mp3_session_test.XXXXXX";
static char audio[256];
static char run[256];

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

static char *at(const char *name)
{
    static char buf[4][300];
    static int k;

    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", tmp, name);
    return buf[k];
}

static void write_wav(const char *path, unsigned rate, unsigned frames)
{
    uint8_t h[POCKETWAV_HEADER_BYTES];
    FILE *f = fopen(path, "wb");
    unsigned i;

    pocketwav_header(h, rate, 1, frames * 2u);
    fwrite(h, 1, sizeof(h), f);
    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(12000.0 * sin(2.0 * M_PI * 440.0 * i / rate));

        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

/* The fake sound card's file, "" when there is none. */
static const char *card(const char *name)
{
    static char v[64];
    char path[300];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", audio, name);
    v[0] = '\0';
    f = fopen(path, "r");
    if (f) {
        if (!fgets(v, sizeof(v), f)) {
            v[0] = '\0';
        }
        fclose(f);
    }
    v[strcspn(v, "\n")] = '\0';
    return v;
}

static int card_log_has(const char *what)
{
    char path[300];
    char line[128];
    FILE *f;
    int found = 0;

    snprintf(path, sizeof(path), "%s/log", audio);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        found |= strncmp(line, what, strlen(what)) == 0;
    }
    if (f) {
        fclose(f);
    }
    return found;
}

static void card_reset(void)
{
    char path[300];
    FILE *f;

    snprintf(path, sizeof(path), "%s/log", audio);
    unlink(path);
    snprintf(path, sizeof(path), "%s/route", audio);
    f = fopen(path, "w");
    fputs("0\n", f);
    fclose(f);
}

/* What arrived while waiting. */
struct seen {
    int kinds[MP3_EV_EXITED + 1];
    int exited;
    int code;
    int64_t first_progress;
    int64_t last_progress;
    int64_t total;
    int seekable;
    int rate;
    int channels;
    char codec[MP3_EVENT_TEXT_MAX];
    char title[MP3_EVENT_TEXT_MAX];
    char error[MP3_EVENT_TEXT_MAX];
    int max_queue;
};

static void take(struct seen *w, const struct mp3_event *ev)
{
    w->kinds[ev->kind]++;
    switch (ev->kind) {
    case MP3_EV_PROGRESS:
        if (w->kinds[MP3_EV_PROGRESS] == 1) {
            w->first_progress = ev->a;
        }
        w->last_progress = ev->a;
        break;
    case MP3_EV_PLAYING:
        w->total = ev->a;
        w->seekable = (int)ev->b;
        w->rate = ev->c;
        w->channels = ev->d;
        snprintf(w->codec, sizeof(w->codec), "%s", ev->text);
        break;
    case MP3_EV_TITLE:
        snprintf(w->title, sizeof(w->title), "%s", ev->text);
        break;
    case MP3_EV_ERROR:
        snprintf(w->error, sizeof(w->error), "%s", ev->text);
        break;
    case MP3_EV_EXITED:
        w->exited = 1;
        w->code = ev->c;
        break;
    default:
        break;
    }
}

/* Poll until kind has been seen (or the helper exited), at most ms. */
static int wait_for(struct mp3_session *s, struct seen *w, enum mp3_event_kind kind, int ms)
{
    int64_t end = mono_ms() + ms;
    struct mp3_event ev;

    while (mono_ms() < end) {
        while (mp3_session_poll(s, &ev, mono_ms())) {
            take(w, &ev);
        }
        if (s->q_count > w->max_queue) {
            w->max_queue = s->q_count;
        }
        if (w->kinds[kind] || w->exited) {
            return w->kinds[kind] > 0;
        }
        nap(10);
    }
    return 0;
}

static int start(struct mp3_session *s, const char *h, const char *path, int64_t start_ms)
{
    char err[128];

    mp3_session_init(s);
    return mp3_session_start_play(s, h, path, start_ms, 70, err, sizeof(err));
}

static int recover_marks(void)
{
    FILE *f = fopen(at("recover.mark"), "r");
    char line[32];
    int n = 0;

    while (f && fgets(line, sizeof(line), f)) {
        n++;
    }
    if (f) {
        fclose(f);
    }
    return n;
}

/* ---- parsing -------------------------------------------------------------------- */

static void parsing(void)
{
    struct mp3_event ev;

    check("ready", mp3_session_parse_line("ready fake-k230", &ev) && ev.kind == MP3_EV_READY &&
                       strcmp(ev.text, "fake-k230") == 0);
    check("meta title keeps UTF-8 and spaces",
          mp3_session_parse_line("meta title Bj\xc3\xb6rk  Live", &ev) && ev.kind == MP3_EV_TITLE &&
              strcmp(ev.text, "Bj\xc3\xb6rk  Live") == 0);
    check("meta artist", mp3_session_parse_line("meta artist A", &ev) && ev.kind == MP3_EV_ARTIST);
    check("playing", mp3_session_parse_line("playing 124839 1 44100 2 mp3float", &ev) &&
                         ev.kind == MP3_EV_PLAYING && ev.a == 124839 && ev.b == 1 && ev.c == 44100 && ev.d == 2 &&
                         strcmp(ev.text, "mp3float") == 0);
    check("playing with an unknown length", mp3_session_parse_line("playing 0 0 8000 1 pcm_s16le", &ev) &&
                                                ev.a == 0 && ev.b == 0);
    check("progress", mp3_session_parse_line("progress 250", &ev) && ev.kind == MP3_EV_PROGRESS && ev.a == 250);
    check("the bare words", mp3_session_parse_line("paused", &ev) && ev.kind == MP3_EV_PAUSED &&
                                mp3_session_parse_line("resumed", &ev) && ev.kind == MP3_EV_RESUMED &&
                                mp3_session_parse_line("played", &ev) && ev.kind == MP3_EV_PLAYED &&
                                mp3_session_parse_line("stopped", &ev) && ev.kind == MP3_EV_STOPPED &&
                                mp3_session_parse_line("recovered", &ev) && ev.kind == MP3_EV_RECOVERED);
    check("error", mp3_session_parse_line("error format not an audio file", &ev) && ev.kind == MP3_EV_ERROR &&
                       strcmp(ev.text, "format not an audio file") == 0);
    check("malformed lines are refused",
          !mp3_session_parse_line("playing 1000 2 44100 2 mp3", &ev) &&
              !mp3_session_parse_line("playing 1000 1 44100 2 MP3", &ev) &&
              !mp3_session_parse_line("playing 1000 1 0 2 mp3", &ev) &&
              !mp3_session_parse_line("playing 1000 1 44100 0 mp3", &ev) &&
              !mp3_session_parse_line("playing 1000 1 44100 2", &ev) &&
              !mp3_session_parse_line("progress abc", &ev) && !mp3_session_parse_line("progress -1", &ev) &&
              !mp3_session_parse_line("progress 1234567890123456", &ev) &&
              !mp3_session_parse_line("meta album X", &ev) && !mp3_session_parse_line("meta title ", &ev) &&
              !mp3_session_parse_line("meta title a\x01" "b", &ev) && !mp3_session_parse_line("played now", &ev) &&
              !mp3_session_parse_line("error", &ev) && !mp3_session_parse_line("hello", &ev));
}

/* ---- the fake ---------------------------------------------------------------------- */

static void fakes(void)
{
    struct mp3_session s;
    struct seen w;
    int64_t t0;
    int marks;

    setenv("MP3_FAKE_RECOVER_MARK", at("recover.mark"), 1);

    memset(&w, 0, sizeof(w));
    setenv("MP3_FAKE", "flood", 1);
    start(&s, fake, "/music/a.mp3", 0);
    nap(400); /* let it write everything before the first poll */
    wait_for(&s, &w, MP3_EV_EXITED, 5000);
    check("a flood of progress: the queue never grows past its bound", w.max_queue <= MP3_EVENT_QUEUE);
    check("  progress readings were dropped, the state events were not",
          s.dropped > 0 && w.kinds[MP3_EV_PLAYING] == 1 && w.kinds[MP3_EV_PLAYED] == 1 && w.exited && w.code == 0);

    memset(&w, 0, sizeof(w));
    setenv("MP3_FAKE", "overlong", 1);
    start(&s, fake, "/music/a.mp3", 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("an over-long line is an error, and the line after it still arrives",
          strstr(w.error, "too long") && w.kinds[MP3_EV_STOPPED] == 1 && w.exited);

    memset(&w, 0, sizeof(w));
    setenv("MP3_FAKE", "garbage", 1);
    start(&s, fake, "/music/a.mp3", 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("garbage is ignored: only the one good line and the exit",
          w.kinds[MP3_EV_STOPPED] == 1 && w.kinds[MP3_EV_PLAYING] == 0 && w.kinds[MP3_EV_TITLE] == 0 &&
              w.kinds[MP3_EV_PROGRESS] == 0 && w.exited && w.code == 2);

    memset(&w, 0, sizeof(w));
    marks = recover_marks();
    setenv("MP3_FAKE", "deaf", 1);
    start(&s, fake, "/music/a.mp3", 0);
    wait_for(&s, &w, MP3_EV_PLAYING, 3000);
    t0 = mono_ms();
    mp3_session_stop(&s, mono_ms());
    check("stop: the session is STOPPING", s.state == MP3_SESSION_STOPPING);
    wait_for(&s, &w, MP3_EV_EXITED, MP3_STOP_GRACE_MS + 2000);
    check("a helper deaf to stop is killed when the grace runs out",
          w.exited && w.code == 128 + SIGKILL && mono_ms() - t0 >= MP3_STOP_GRACE_MS - 50 &&
              mono_ms() - t0 < MP3_STOP_GRACE_MS + 1500);
    nap(300);
    check("  and the audio route is recovered, by a detached `recover`", recover_marks() == marks + 1 &&
                                                                          s.recoveries >= 1);

    memset(&w, 0, sizeof(w));
    marks = recover_marks();
    setenv("MP3_FAKE", "crash", 1);
    start(&s, fake, "/music/a.mp3", 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    nap(300);
    check("a helper that dies by a signal: EXITED 137, and recover runs", w.exited && w.code == 137 &&
                                                                          recover_marks() == marks + 1);

    marks = recover_marks();
    setenv("MP3_FAKE", "deaf", 1);
    start(&s, fake, "/music/a.mp3", 0);
    memset(&w, 0, sizeof(w));
    wait_for(&s, &w, MP3_EV_PLAYING, 3000);
    t0 = mono_ms();
    mp3_session_abandon(&s, 300);
    check("abandon waits no longer than its grace and the reap", mono_ms() - t0 < 300 + MP3_KILL_REAP_MS + 150);
    nap(300);
    check("  the session is idle and a recover ran", s.state == MP3_SESSION_IDLE && s.fd < 0 &&
                                                     recover_marks() == marks + 1);
    check("  no child is left", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);

    check("commands to nothing fail rather than block", mp3_session_command(&s, "pause") != 0 &&
                                                        mp3_session_seek(&s, 10) != 0 &&
                                                        mp3_session_volume(&s, 50) != 0);
    check("starting with a bad request is refused, the session stays idle",
          start(&s, fake, "relative.mp3", 0) != 0 && start(&s, fake, "/a\nb.mp3", 0) != 0 &&
              s.state == MP3_SESSION_IDLE);
    {
        char err[64];

        mp3_session_init(&s);
        check("  and so is a volume out of range",
              mp3_session_start_play(&s, fake, "/a.mp3", 0, 0, err, sizeof(err)) != 0 &&
                  mp3_session_start_play(&s, fake, "/a.mp3", -1, 50, err, sizeof(err)) != 0);
    }
    unsetenv("MP3_FAKE");
}

/* ---- the real helper ------------------------------------------------------------------- */

static void real(void)
{
    struct mp3_session s;
    struct seen w;
    int64_t t0;

    write_wav(at("one.wav"), 22050, 22050);
    write_wav(at("three.wav"), 48000, 3 * 48000);

    card_reset();
    memset(&w, 0, sizeof(w));
    check("the real helper starts", start(&s, helper, at("one.wav"), 0) == 0);
    t0 = mono_ms();
    wait_for(&s, &w, MP3_EV_EXITED, 5000);
    check("  it says ready and what it plays: 1000 ms, seekable, 22050 Hz mono, pcm_s16le",
          w.kinds[MP3_EV_READY] == 1 && w.total == 1000 && w.seekable == 1 && w.rate == 22050 &&
              w.channels == 1 && strcmp(w.codec, "pcm_s16le") == 0);
    check("  progress arrives several times and ends at the length", w.kinds[MP3_EV_PROGRESS] >= 4 &&
                                                                     w.last_progress == 1000);
    check("  it plays in real time, then says played and exits 0",
          w.kinds[MP3_EV_PLAYED] == 1 && w.exited && w.code == 0 && mono_ms() - t0 >= 900 && mono_ms() - t0 < 3000);
    check("  the device was opened for playback and closed, the amplifier off",
          card_log_has("pcm playback") && strcmp(card("pcm"), "closed") == 0 && strcmp(card("amp"), "0") == 0);

    card_reset();
    memset(&w, 0, sizeof(w));
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_PLAYING, 3000);
    nap(300);
    check("pause", mp3_session_command(&s, "pause") == 0 && wait_for(&s, &w, MP3_EV_PAUSED, 1000));
    nap(100);
    check("  paused, the device is closed and the amplifier off",
          strcmp(card("pcm"), "closed") == 0 && strcmp(card("amp"), "0") == 0);
    {
        int64_t held = w.last_progress;

        nap(600);
        wait_for(&s, &w, MP3_EV_PLAYED, 50);
        check("  and the position does not move", w.last_progress == held && !w.kinds[MP3_EV_PLAYED]);
    }
    check("resume opens the device again", mp3_session_command(&s, "resume") == 0 &&
                                               wait_for(&s, &w, MP3_EV_RESUMED, 1000) &&
                                               strcmp(card("pcm"), "playback") == 0);
    check("the volume can change while it plays", mp3_session_volume(&s, 30) == 0);
    nap(200);
    memset(&w.kinds, 0, sizeof(w.kinds));
    {
        int64_t end = mono_ms() + 1000;
        int sent = mp3_session_seek(&s, 2500) == 0;

        /* Progress written before the seek may still be queued. */
        while (mono_ms() < end && w.last_progress < 2500 && !w.exited) {
            memset(&w.kinds, 0, sizeof(w.kinds));
            wait_for(&s, &w, MP3_EV_PROGRESS, 100);
        }
        printf("note position after the seek: %lld ms\n", (long long)w.last_progress);
        check("seek to 2500 ms: the position jumps there", sent && w.last_progress >= 2500 &&
                                                               w.last_progress < 2700);
    }
    t0 = mono_ms();
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("  and it ends half a second later", w.kinds[MP3_EV_PLAYED] == 1 && w.code == 0 &&
                                               mono_ms() - t0 < 1200);

    memset(&w, 0, sizeof(w));
    start(&s, helper, at("three.wav"), 2000);
    wait_for(&s, &w, MP3_EV_PROGRESS, 3000);
    check("starting part way: the first position is where it was asked to start", w.first_progress == 2000);
    t0 = mono_ms();
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("  and one second is left", w.kinds[MP3_EV_PLAYED] == 1 && mono_ms() - t0 >= 800 && mono_ms() - t0 < 1800);

    memset(&w, 0, sizeof(w));
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_PLAYING, 3000);
    t0 = mono_ms();
    mp3_session_stop(&s, mono_ms());
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("stop: stopped, exit 0, within one audio wait", w.kinds[MP3_EV_STOPPED] == 1 && w.code == 0 &&
                                                          mono_ms() - t0 < 600);
    check("  the device is closed", strcmp(card("pcm"), "closed") == 0);

    memset(&w, 0, sizeof(w));
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_PLAYING, 3000);
    {
        unsigned rec = s.recoveries;

        t0 = mono_ms();
        mp3_session_abandon(&s, 1000);
        check("abandoned (the app closed): the helper leaves by itself, quickly, and needs no recover",
              mono_ms() - t0 < 600 && s.recoveries == rec && s.state == MP3_SESSION_IDLE);
    }
    check("  the device is closed", strcmp(card("pcm"), "closed") == 0);
}

static void bad_files(void)
{
    struct mp3_session s;
    struct seen w;
    FILE *f;
    int fd;

    card_reset();
    memset(&w, 0, sizeof(w));
    start(&s, helper, at("missing.mp3"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("a missing file: error missing, exit 4", strncmp(w.error, MP3_ERR_MISSING " ", 8) == 0 &&
                                                   w.code == MP3_EXIT_FILE);
    check("  and the device was never touched", !card_log_has("pcm") && !card_log_has("amp"));

    fd = open(at("empty.mp3"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    close(fd);
    memset(&w, 0, sizeof(w));
    start(&s, helper, at("empty.mp3"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("a zero-length file: error format, exit 5, device untouched",
          strncmp(w.error, "format ", 7) == 0 && strstr(w.error, "empty") && w.code == MP3_EXIT_FORMAT &&
              !card_log_has("pcm"));

    f = fopen(at("text.wav"), "w");
    fputs("RIFF but not really a wave file\n", f);
    fclose(f);
    memset(&w, 0, sizeof(w));
    start(&s, helper, at("text.wav"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("a damaged or unsupported file: error format, exit 5, device untouched",
          strncmp(w.error, "format ", 7) == 0 && w.code == MP3_EXIT_FORMAT && !card_log_has("pcm"));

    memset(&w, 0, sizeof(w));
    setenv("POS_MP3_FAIL_AT_MS", "300", 1);
    setenv("POS_MP3_FAIL_KIND", "decode", 1);
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("decoding failing part way: error decode, exit 5, device closed",
          w.kinds[MP3_EV_PLAYING] == 1 && strncmp(w.error, "decode ", 7) == 0 && w.code == MP3_EXIT_FORMAT &&
              strcmp(card("pcm"), "closed") == 0 && strcmp(card("amp"), "0") == 0);

    memset(&w, 0, sizeof(w));
    setenv("POS_MP3_FAIL_KIND", "storage", 1);
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("the storage failing part way: error storage, exit 4, device closed",
          strncmp(w.error, "storage ", 8) == 0 && w.code == MP3_EXIT_FILE && strcmp(card("pcm"), "closed") == 0);
    unsetenv("POS_MP3_FAIL_AT_MS");
    unsetenv("POS_MP3_FAIL_KIND");

    /* Wave or Recorder holds the audio. */
    card_reset();
    fd = open(at("run/audio.lock"), O_CREAT | O_RDWR, 0600);
    check("(the audio lock is taken by someone else)", fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    memset(&w, 0, sizeof(w));
    start(&s, helper, at("three.wav"), 0);
    wait_for(&s, &w, MP3_EV_EXITED, 3000);
    check("the device in use: error audio_busy, exit 3, nothing switched",
          strncmp(w.error, "audio_busy ", 11) == 0 && w.code == MP3_EXIT_AUDIO && !card_log_has("pcm") &&
              !card_log_has("route"));
    close(fd);

    check("no child is left", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
}

int main(int argc, char **argv)
{
    char cmd[128];
    FILE *f;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 3 || access(argv[1], X_OK) != 0 || access(argv[2], X_OK) != 0) {
        printf("usage: mp3_session_test <fake_pos_mp3.sh> <pos-mp3-testhooks>\n");
        return 2;
    }
    fake = argv[1];
    helper = argv[2];
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(audio, sizeof(audio), "%s/audio", tmp);
    snprintf(run, sizeof(run), "%s/run", tmp);
    mkdir(audio, 0700);
    mkdir(run, 0700);
    f = fopen(at("audio/route"), "w");
    fputs("0\n", f);
    fclose(f);
    setenv("POS_MP3_FAKE_AUDIO", audio, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "playback", 1);
    signal(SIGPIPE, SIG_IGN);

    parsing();
    fakes();
    real();
    bad_files();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", tmp);
    }
    printf("mp3_session_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
