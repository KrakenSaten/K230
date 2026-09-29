/*
 * pos-video's player engine, in process, against the fake backend and the
 * file-backed sound card (tests/fake_audio_backend.c, which paces like a
 * device): opening valid, damaged, missing and unsupported files; the first
 * picture while paused; play, pause, stop, seek (paused and playing),
 * the end and playing again; the picture's size following `view`; a decoder
 * that errs, and a slow one (dropped pictures); rapid commands; a session
 * that holds its slots (never overwritten); the sound card opened on play and
 * closed on pause, end and destroy, a busy card, muted, an undecodable sound
 * track; and closing while playing. The fitting rule is checked on its own.
 *
 * The session side is simulated here: every `frame` is checked against the
 * slots the "session" still holds, its first pixel read (the fake writes the
 * picture number there), and released after the step, as the app would.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "fake_audio_backend.h"
#include "video_player.h"
#include "video_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 20000

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
    fflush(stdout);
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

/* ---- the simulated session ------------------------------------------------------ */

static struct {
    struct video_player *p;
    uint8_t *shm;
    char (*lines)[VIDEO_LINE_MAX];
    int n;
    int held[VIDEO_SLOTS];
    int to_release[64];
    int nrel;
    int hold;            /* 1: keep every slot (a session that stopped taking pictures) */
    int frames;
    int reused;          /* a slot announced while the session still held it */
    int size_mismatch;   /* a frame whose pixel 0 is not its picture number's */
    int64_t last_pts;
    uint32_t fw;
    uint32_t fh;
    int last_pic;
    char dir[128];
    char audio_dir[160];
} R;

static void emit(void *user, const char *line)
{
    int slot;
    unsigned seq;
    unsigned w;
    unsigned h;
    long long pts;

    (void)user;
    if (R.n < MAX_LINES) {
        snprintf(R.lines[R.n++], VIDEO_LINE_MAX, "%s", line);
    }
    if (sscanf(line, "frame %d %u %u %u %lld", &slot, &seq, &w, &h, &pts) == 5) {
        if (slot < 0 || slot >= VIDEO_SLOTS || R.held[slot]) {
            R.reused++;
            return;
        }
        R.held[slot] = 1;
        R.frames++;
        R.last_pts = pts;
        R.fw = w;
        R.fh = h;
        R.last_pic = ((const uint16_t *)(R.shm + video_slot_offset((uint32_t)slot)))[0];
        if (!R.hold && R.nrel < 64) {
            R.to_release[R.nrel++] = slot;
        }
    }
}

static void release_all(void)
{
    int i;

    for (i = 0; i < R.nrel; i++) {
        char cmd[32];

        R.held[R.to_release[i]] = 0;
        snprintf(cmd, sizeof(cmd), "release %d", R.to_release[i]);
        video_player_command(R.p, cmd);
    }
    R.nrel = 0;
}

static void pump(int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        int w = video_player_step(R.p);

        release_all();
        sleep_ms(w < 1 ? 1 : (w > 5 ? 5 : w));
    }
}

/* Pump until a line starting with prefix appears at or after index from.
 * Returns its index, or -1 on timeout. */
static int wait_for(const char *prefix, int from, int ms)
{
    int64_t end = mono_ms() + ms;
    size_t n = strlen(prefix);

    for (;;) {
        int i;

        for (i = from; i < R.n; i++) {
            if (strncmp(R.lines[i], prefix, n) == 0) {
                return i;
            }
        }
        if (mono_ms() >= end) {
            return -1;
        }
        {
            int w = video_player_step(R.p);

            release_all();
            sleep_ms(w < 1 ? 1 : (w > 5 ? 5 : w));
        }
    }
}

static int count_from(const char *prefix, int from)
{
    int i;
    int c = 0;
    size_t n = strlen(prefix);

    for (i = from; i < R.n; i++) {
        c += strncmp(R.lines[i], prefix, n) == 0;
    }
    return c;
}

static void cmd(const char *c)
{
    video_player_command(R.p, c);
}

static void file(const char *name, const char *line)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", R.dir, name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static void open_file(const char *name)
{
    char c[300];

    snprintf(c, sizeof(c), "open %s/%s", R.dir, name);
    cmd(c);
}

static void audio_state(const char *what, char *out, size_t n)
{
    char path[256];
    FILE *f;

    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", R.audio_dir, what);
    f = fopen(path, "r");
    if (f) {
        if (!fgets(out, (int)n, f)) {
            out[0] = '\0';
        }
        fclose(f);
    }
    out[strcspn(out, "\n")] = '\0';
}

static int start(int volume, int with_audio)
{
    struct video_player_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    memset(R.held, 0, sizeof(R.held));
    R.n = 0;
    R.nrel = 0;
    R.hold = 0;
    R.frames = 0;
    R.reused = 0;
    R.last_pts = -1;
    cfg.backend = video_backend_fake();
    if (with_audio) {
        cfg.audio_backend = fake_audio_backend(R.audio_dir);
        cfg.audio_board = fake_audio_board();
        cfg.allow_unverified = 1;
        cfg.audio_lock_dir = R.audio_dir;
    }
    cfg.volume_percent = volume;
    cfg.shm = R.shm;
    cfg.emit = emit;
    return video_player_create(&R.p, &cfg);
}

static void stop_player(void)
{
    video_player_destroy(R.p);
    R.p = NULL;
}

int main(void)
{
    char tmpl[] = "/tmp/video_player_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    char s[64];
    int i;
    int at;
    long long v1 = 0;
    long long v2 = 0;

    R.lines = calloc(MAX_LINES, VIDEO_LINE_MAX);
    R.shm = calloc(1, VIDEO_SHM_BYTES);
    if (!dir || !R.lines || !R.shm) {
        perror("setup");
        return 1;
    }
    snprintf(R.dir, sizeof(R.dir), "%s", dir);
    snprintf(R.audio_dir, sizeof(R.audio_dir), "%s/card", dir);
    mkdir(R.audio_dir, 0755);
    {
        char p[256];
        FILE *f;

        snprintf(p, sizeof(p), "%s/route", R.audio_dir);
        f = fopen(p, "w");
        if (f) {
            fputs("0\n", f);
            fclose(f);
        }
    }

    /* ---- fitting ---- */
    {
        uint32_t w;
        uint32_t h;

        check("720p into a 818x460 box: 816x460, the width a multiple of 8",
              video_fit(1280, 720, 818, 460, &w, &h) == 0 && w == 816 && h == 460);
        check("... and a height that would be odd is made even",
              video_fit(1280, 720, 1232, 461, &w, &h) == 0 && h == 460 && w % 8 == 0);
        check("never enlarged", video_fit(320, 180, 1232, 568, &w, &h) == 0 && w == 320 && h == 180);
        check("an odd source is aligned down", video_fit(321, 181, 1232, 568, &w, &h) == 0 && w == 320 &&
                                                   h == 180);
        check("portrait into landscape keeps the shape",
              video_fit(720, 1320, 1232, 568, &w, &h) == 0 && h == 568 && w == 304);
        check("1080p fullscreen landscape", video_fit(1920, 1080, 1232, 568, &w, &h) == 0 &&
                                                w == 1008 && h == 568);
        check("within a slot", video_fit(4000, 4000, 1280, 1280, &w, &h) == 0 &&
                                   (uint64_t)w * h <= VIDEO_VIEW_MAX_PIXELS);
        check("nothing fits a 4x1 box", video_fit(1280, 720, 4, 1, &w, &h) != 0);
        check("zero sizes are refused", video_fit(0, 720, 100, 100, &w, &h) != 0 &&
                                            video_fit(1280, 720, 0, 100, &w, &h) != 0);
    }

    file("good.mp4", "DOORS-FAKE-VIDEO w=320 h=180 fps=25 ms=3000 gop=25");
    file("short.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=600");
    file("sound.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=2000 audio=1");
    file("badsound.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=2000 audio=bad");
    file("broken.mp4", "this is not a video");
    file("damagedhdr.mp4", "DOORS-FAKE-VIDEO w=abc");
    file("hevc.mp4", "DOORS-FAKE-VIDEO codec=hevc");
    file("nodecoder.mp4", "DOORS-FAKE-VIDEO fail=device");
    file("errs.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=4000 error_at=12");
    file("slow.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=4000 slow_ms=70");
    file("nosize.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=2000 output_fail=1");

    /* ---- opening ---- */
    check("a player starts", start(100, 0) == 0);
    cmd("view 640 360");
    open_file("good.mp4");
    at = wait_for("opened", 0, 1000);
    check("opened: length, size, rate, no sound track, codec",
          at >= 0 && strcmp(R.lines[at], "opened 3000 320 180 2500 none h264") == 0);
    at = wait_for("frame", 0, 1000);
    check("the first picture comes while paused, at the start",
          at >= 0 && R.last_pts == 0 && R.fw == 320 && R.fh == 180 && R.last_pic == 0);
    i = R.frames;
    pump(300);
    check("nothing more while paused", R.frames == i && count_from("pos", 0) == 0);

    /* ---- playing ---- */
    at = R.n;
    cmd("play");
    check("play is answered", strcmp(R.lines[at], "state playing 0") == 0);
    i = R.frames;
    pump(1200);
    check("about 25 pictures a second", R.frames - i >= 22 && R.frames - i <= 32);
    check("in order, no slot overwritten while held", R.reused == 0);
    check("progress while playing", count_from("pos ", at) >= 3);
    check("statistics once a second", count_from("stats ", at) >= 1);
    {
        int k = wait_for("stats ", at, 10);
        unsigned long long fps = 0;

        check("the statistics say about 25 fps",
              k >= 0 && sscanf(R.lines[k], "stats %llu", &fps) == 1 && fps >= 200 && fps <= 300);
    }

    /* ---- pausing ---- */
    at = R.n;
    cmd("pause");
    check("pause is answered with the position",
          sscanf(R.lines[at], "state paused %lld", &v1) == 1 && v1 >= 900 && v1 <= 1500);
    i = R.frames;
    pump(300);
    check("nothing while paused", R.frames == i && count_from("pos", at) == 0);
    at = R.n;
    cmd("play");
    pump(400);
    check("resumed from where it paused", count_from("frame", at) >= 7 && R.last_pts > v1);

    /* ---- seeking ---- */
    at = R.n;
    cmd("seek 2000");
    i = wait_for("seeked", at, 1000);
    check("a seek while playing is answered at or just before the target",
          i >= 0 && sscanf(R.lines[i], "seeked %lld", &v1) == 1 && v1 >= 1960 && v1 <= 2000);
    pump(200);
    check("and pictures carry on from there", R.last_pts >= 2000 && R.last_pts < 2400);
    cmd("pause");
    at = R.n;
    cmd("seek 400");
    i = wait_for("seeked", at, 1000);
    check("a seek while paused shows the picture there",
          i >= 0 && sscanf(R.lines[i], "seeked %lld", &v1) == 1 && v1 == 400 &&
              wait_for("frame", at, 500) >= 0 && R.last_pts == 400 && R.last_pic == 10);
    i = R.frames;
    pump(200);
    check("and stays paused", R.frames == i);
    at = R.n;
    cmd("seek 999999");
    i = wait_for("seeked", at, 1000);
    check("a seek past the end lands on the last picture",
          i >= 0 && sscanf(R.lines[i], "seeked %lld", &v1) == 1 && v1 >= 2900 && v1 < 3000);

    /* ---- stop ---- */
    cmd("play");
    pump(100);
    at = R.n;
    cmd("stop");
    check("stop is answered at 0", count_from("state stopped 0", at) == 1);
    i = wait_for("frame", at, 500);
    check("and shows the first picture", i >= 0 && R.last_pts == 0);
    i = R.frames;
    pump(200);
    check("and plays nothing", R.frames == i);

    /* ---- the picture's size ---- */
    cmd("play");
    pump(100);
    at = R.n;
    cmd("view 200 200");
    i = wait_for("frame", at, 500);
    check("a new view: pictures at the new size, from where it was", i >= 0 && R.fw == 200 &&
                                                                         R.fh == 112 && R.last_pts > 0);
    pump(200);
    check("still playing after the resize", count_from("frame", at) >= 3);
    at = R.n;
    cmd("view 0 100");
    cmd("view 99999 100");
    cmd("view banana");
    pump(100);
    check("a bad view is ignored", R.fw == 200 && count_from("error", at) == 0);

    /* ---- the end ---- */
    open_file("short.mp4");
    at = wait_for("opened", 0, 1000);
    cmd("play");
    at = R.n;
    i = wait_for("state ended", at, 2000);
    check("a short file ends at its length",
          i >= 0 && sscanf(R.lines[i], "state ended %lld", &v1) == 1 && v1 == 600 &&
              count_from("frame", at) >= 12);
    at = R.n;
    cmd("play");
    i = wait_for("frame", at, 500);
    check("play after the end starts over", i >= 0 && R.last_pts < 200);
    i = wait_for("state ended", at, 2000);
    check("... and ends again", i >= 0);
    at = R.n;
    cmd("seek 100");
    i = wait_for("seeked", at, 500);
    check("a seek after the end is answered, paused", i >= 0 && count_from("state playing", at) == 0);

    /* ---- files that cannot be played ---- */
    at = R.n;
    open_file("broken.mp4");
    check("a file that is not a video", count_from("openfail corrupt", at) == 1);
    open_file("damagedhdr.mp4");
    check("a damaged header", count_from("openfail corrupt", at) == 2);
    open_file("missing.mp4");
    check("a missing file", count_from("openfail missing", at) == 1);
    open_file("hevc.mp4");
    check("a codec v0.1 does not play", count_from("openfail unsupported", at) == 1);
    open_file("nodecoder.mp4");
    check("no decoder", count_from("openfail device", at) == 1);
    open_file("nosize.mp4");
    check("a decoder that cannot take the size", count_from("openfail device", at) == 2);
    cmd("open");
    check("an empty path", count_from("openfail missing", at) == 2);
    i = R.frames;
    cmd("play");
    cmd("seek 100");
    cmd("stop");
    pump(100);
    check("transport without a file does nothing", R.frames == i && count_from("state", at) == 0);

    /* A decode error in the middle. */
    at = R.n;
    open_file("errs.mp4");
    cmd("play");
    i = wait_for("error decode", at, 2000);
    check("a decode error mid-file ends playback and says so",
          i >= 0 && count_from("frame", at) >= 11 && strstr(R.lines[i], "picture 12") != NULL);
    i = R.frames;
    pump(200);
    check("nothing after it", R.frames == i);
    at = R.n;
    open_file("good.mp4");
    check("the helper can open another file after an error",
          wait_for("opened", at, 500) >= 0 && wait_for("frame", at, 500) >= 0);

    /* A decoder slower than the file: pictures are dropped or late, the clock
     * keeps real time, something is always shown. */
    at = R.n;
    open_file("slow.mp4");
    wait_for("frame", at, 1000);
    at = R.n;
    cmd("play");
    pump(1500);
    i = wait_for("stats", at, 1500);
    {
        unsigned long long shown = 0;
        unsigned long long dropped = 0;
        unsigned long long late = 0;
        unsigned long long fps = 0;
        int k;

        for (k = R.n - 1; k >= at; k--) {
            if (sscanf(R.lines[k], "stats %llu %llu %llu %llu", &fps, &shown, &dropped, &late) == 4) {
                break;
            }
        }
        check("a slow decoder: pictures still shown", shown >= 8);
        check("... and the lag is reported as dropped or late pictures", dropped + late >= 3);
        for (k = R.n - 1; k >= at; k--) {
            if (sscanf(R.lines[k], "pos %lld", &v2) == 1) {
                break;
            }
        }
        check("... while the position keeps real time", count_from("pos ", at) >= 4 && v2 >= 1000);
    }

    /* Rapid commands, in every order. */
    at = R.n;
    open_file("good.mp4");
    wait_for("frame", at, 1000);
    srand(7);
    for (i = 0; i < 300; i++) {
        char c[48];

        switch (rand() % 6) {
        case 0:
            cmd("play");
            break;
        case 1:
            cmd("pause");
            break;
        case 2:
            snprintf(c, sizeof(c), "seek %d", rand() % 3500);
            cmd(c);
            break;
        case 3:
            cmd("stop");
            break;
        case 4:
            snprintf(c, sizeof(c), "view %d %d", 100 + rand() % 400, 100 + rand() % 300);
            cmd(c);
            break;
        default:
            video_player_step(R.p);
            release_all();
            break;
        }
    }
    cmd("view 320 180");
    cmd("seek 0");
    at = R.n;
    cmd("play");
    pump(600);
    check("after 300 rapid commands: still playing, pictures flowing",
          count_from("frame", at) >= 10 && R.reused == 0 && count_from("error", 0) == 1);

    /* A session that stops taking pictures: its slots are never written. */
    R.hold = 1;
    at = R.n;
    pump(600);
    check("a session holding every slot: at most four announced, none overwritten",
          count_from("frame", at) <= VIDEO_SLOTS && R.reused == 0);
    R.hold = 0;
    for (i = 0; i < VIDEO_SLOTS; i++) {
        if (R.held[i]) {
            char c[32];

            snprintf(c, sizeof(c), "release %d", i);
            R.held[i] = 0;
            cmd(c);
        }
    }
    at = R.n;
    pump(400);
    check("... and it goes on once they come back", count_from("frame", at) >= 3);
    cmd("release 9");
    cmd("release x");
    check("releasing a slot that is not the session's is ignored", R.reused == 0);

    {
        int64_t t0 = mono_ms();

        stop_player();
        check("closing while playing is quick", mono_ms() - t0 < 300);
    }

    /* ---- the sound ---- */
    check("a player with a sound card", start(60, 1) == 0);
    cmd("view 160 90");
    open_file("sound.mp4");
    at = wait_for("opened", 0, 1000);
    check("the file's sound is on", at >= 0 && strstr(R.lines[at], " on h264") != NULL);
    audio_state("pcm", s, sizeof(s));
    check("the card is not opened by opening a file", strcmp(s, "playback") != 0);
    at = R.n;
    cmd("play");
    pump(700);
    audio_state("pcm", s, sizeof(s));
    check("playing opens the card", strcmp(s, "playback") == 0);
    audio_state("amp", s, sizeof(s));
    check("... with the amplifier on", strcmp(s, "1") == 0);
    check("pictures follow the sound's clock", count_from("frame", at) >= 10 &&
                                                   count_from("frame", at) <= 22);
    at = R.n;
    cmd("pause");
    pump(300);
    audio_state("pcm", s, sizeof(s));
    check("pausing closes the card", strcmp(s, "closed") == 0);
    audio_state("amp", s, sizeof(s));
    check("... and turns the amplifier off", strcmp(s, "0") == 0);
    cmd("play");
    at = R.n;
    i = wait_for("state ended", at, 3000);
    check("with sound, a file still ends", i >= 0);
    pump(200);
    audio_state("pcm", s, sizeof(s));
    check("the end closes the card", strcmp(s, "closed") == 0);

    /* The card taken by someone else: said once, the picture plays on. */
    {
        char lock[256];
        int fd;

        snprintf(lock, sizeof(lock), "%s/audio.lock", R.audio_dir);
        fd = open(lock, O_RDWR | O_CREAT, 0600);
        check("another program holds the card", fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
        at = R.n;
        cmd("seek 0");
        cmd("play");
        i = wait_for("audio busy", at, 1500);
        check("audio busy is said", i >= 0);
        pump(400);
        check("... and the picture plays on without it", count_from("frame", i) >= 6 &&
                                                             count_from("audio busy", at) == 1);
        if (fd >= 0) {
            close(fd);
        }
    }
    stop_player();
    audio_state("pcm", s, sizeof(s));
    check("after the player: the card is closed", strcmp(s, "closed") == 0);
    audio_state("amp", s, sizeof(s));
    check("... and the amplifier off", strcmp(s, "0") == 0);

    /* Closing while the sound plays. */
    start(60, 1);
    cmd("view 160 90");
    open_file("sound.mp4");
    wait_for("opened", 0, 1000);
    cmd("play");
    pump(400);
    audio_state("pcm", s, sizeof(s));
    check("sound playing before the close", strcmp(s, "playback") == 0);
    {
        int64_t t0 = mono_ms();

        stop_player();
        check("closing while the sound plays is quick", mono_ms() - t0 < 500);
    }
    audio_state("pcm", s, sizeof(s));
    check("... and closes the card", strcmp(s, "closed") == 0);

    /* Muted, and a sound track this build cannot decode. */
    start(0, 1);
    cmd("view 160 90");
    open_file("sound.mp4");
    at = wait_for("opened", 0, 1000);
    check("volume 0: muted", at >= 0 && strstr(R.lines[at], " muted h264") != NULL);
    cmd("play");
    pump(300);
    audio_state("pcm", s, sizeof(s));
    check("... and the card is never opened", strcmp(s, "playback") != 0 && count_from("frame", at) >= 4);
    stop_player();
    start(60, 1);
    cmd("view 160 90");
    open_file("badsound.mp4");
    at = wait_for("opened", 0, 1000);
    check("an undecodable sound track: said, played without",
          at >= 0 && strstr(R.lines[at], " unsupported h264") != NULL);
    cmd("play");
    pump(300);
    check("... pictures regardless", count_from("frame", at) >= 4);
    stop_player();
    start(60, 0);
    cmd("view 160 90");
    open_file("sound.mp4");
    at = wait_for("opened", 0, 1000);
    check("no sound card at all: a sounded file is muted", at >= 0 && strstr(R.lines[at], " muted h264") != NULL);
    open_file("good.mp4");
    at = wait_for("opened", at + 1, 1000);
    check("a file without sound says none, card or no card", at >= 0 && strstr(R.lines[at], " none h264") != NULL);
    check("quit is recognised", video_player_command(R.p, "quit") == 1 &&
                                    video_player_command(R.p, "quitting") == 0);
    stop_player();

    {
        char c[256];

        snprintf(c, sizeof(c), "rm -rf '%s'", dir);
        if (system(c) != 0) {
            fprintf(stderr, "cleanup failed\n");
        }
    }
    free(R.lines);
    free(R.shm);
    printf("video_player_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
