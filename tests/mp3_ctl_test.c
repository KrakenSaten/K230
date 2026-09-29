/*
 * The MP3 app's controller and view model (apps/mp3/mp3_ctl.h, mp3_view.h),
 * driven the way the screen drives them, against the real pos-mp3 (test
 * hooks, its WAV decoder) over the file-backed sound card and, where the
 * real one cannot misbehave on cue, the scripted fake (tests/fake_pos_mp3.sh).
 *
 * What is checked, in the words of docs/apps/MP3.md: the first screen; the
 * places and folders; play, pause, resume and stop; next and previous while
 * playing and while stopped; a track's end starting the next, and the
 * queue's end stopping; a bad file said on screen, and skipped only when
 * the previous one's end reached it; the helper failing and the device in
 * use; bursts of NEXT and of PLAY/PAUSE settling on what was asked last with
 * one helper at a time; the system volume and mute; a track or a folder
 * disappearing; the list's boundary; the remembered folder; and closing -
 * no helper, thread or descriptor left behind, and a clean reopen.
 *
 * Usage: mp3_ctl_test <pos-mp3-testhooks> <fake_pos_mp3.sh>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "mp3_ctl.h"
#include "mp3_view.h"
#include "pocketwav/pocketwav.h"

#include <dirent.h>
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
static const char *helper;
static const char *fake;
static char tmp[] = "/tmp/mp3_ctl_test.XXXXXX";
static char music[300];

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

/* Poll c until cond holds or ms pass; the value of cond. */
#define WAIT(c, ms, cond)                                                                                     \
    ({                                                                                                        \
        int64_t end_ = mono_ms() + (ms);                                                                      \
        while (!(cond) && mono_ms() < end_) {                                                                 \
            mp3_ctl_poll((c), mono_ms());                                                                     \
            nap(10);                                                                                          \
        }                                                                                                     \
        (cond) ? 1 : 0;                                                                                       \
    })

static void pump(struct mp3_ctl *c, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        mp3_ctl_poll(c, mono_ms());
        nap(10);
    }
}

static char *at(const char *fmt, const char *a)
{
    static char buf[8][512];
    static int k;
    char rel[300];

    k = (k + 1) % 8;
    snprintf(rel, sizeof(rel), fmt, a ? a : "");
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", tmp, rel);
    return buf[k];
}

static void write_wav(const char *path, unsigned ms)
{
    uint8_t h[POCKETWAV_HEADER_BYTES];
    unsigned frames = 16000u * ms / 1000u;
    FILE *f = fopen(path, "wb");
    unsigned i;

    pocketwav_header(h, 16000, 1, frames * 2u);
    fwrite(h, 1, sizeof(h), f);
    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(9000.0 * sin(2.0 * M_PI * 440.0 * i / 16000.0));

        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

static void text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    fputs(text, f);
    fclose(f);
}

/* ---- the system volume, as the shell keeps it ------------------------------------ */

static int g_level = 70;
static int g_muted;
static int g_available = 1;
static int g_sets;

static int v_get(void) { return g_level; }
static int v_muted(void) { return g_muted; }
static int v_available(void) { return g_available; }

static int v_set(int pct)
{
    g_sets++;
    g_level = pct;
    return 0;
}

static int v_set_muted(int m)
{
    g_muted = m;
    return 0;
}

static const struct mp3_volume_ops vol = { v_get, v_muted, v_available, v_set, v_set_muted };

/* ---- looking ------------------------------------------------------------------------ */

static int row(const struct mp3_ctl *c, const char *name)
{
    int k;

    for (k = 0; c->list && k < c->list->n; k++) {
        if (strcmp(c->list->e[k].name, name) == 0) {
            return k;
        }
    }
    return -1;
}

static struct mp3_view view;

static struct mp3_view *seen(const struct mp3_ctl *c)
{
    mp3_view_refresh(c, &view);
    return &view;
}

static int listing(const struct mp3_ctl *c, const char *dir)
{
    return !c->loading && c->list && strcmp(c->list->dir, dir) == 0;
}

static int playing(const struct mp3_ctl *c, const char *name)
{
    return c->player.state == MP3_PLAYER_PLAYING && !c->player.pending &&
           strcmp(mp3_ctl_current_name(c), name) == 0 && mp3_ctl_player_is_current(c);
}

static int no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n - 1; /* the directory's own */
}

static int card_playbacks(void)
{
    FILE *f = fopen(at("audio/log", NULL), "r");
    char line[128];
    int n = 0;

    while (f && fgets(line, sizeof(line), f)) {
        n += strncmp(line, "pcm playback", 12) == 0;
    }
    if (f) {
        fclose(f);
    }
    return n;
}

static void open_music(struct mp3_ctl *c, const char *h)
{
    mp3_ctl_open(c, &vol, h);
    WAIT(c, 3000, listing(c, "") || (c->list && !c->loading));
}

static void go(struct mp3_ctl *c, const char *sub)
{
    char dir[512];

    if (!listing(c, "")) {
        while (c->list && c->list->dir[0]) {
            mp3_ctl_up(c);
            WAIT(c, 2000, !c->loading);
        }
    }
    mp3_ctl_open_entry(c, row(c, "Music"), mono_ms());
    WAIT(c, 2000, listing(c, music));
    if (sub) {
        snprintf(dir, sizeof(dir), "%s/%s", music, sub);
        mp3_ctl_open_entry(c, row(c, sub), mono_ms());
        WAIT(c, 2000, listing(c, dir));
    }
}

/* ---- the journeys ----------------------------------------------------------------------- */

static void first_screen(void)
{
    struct mp3_ctl c;
    struct mp3_view *v;

    mp3_ctl_open(&c, &vol, helper);
    v = seen(&c);
    check("opening: the places are being read, nothing plays",
          c.loading && strcmp(v->chip, "READY") == 0 && strcmp(v->title, "Nothing playing") == 0 && !v->hint);
    WAIT(&c, 3000, listing(&c, ""));
    v = seen(&c);
    check("the first screen: the places (Music, Home), LIBRARY, UP off",
          c.list->n == 2 && row(&c, "Music") == 0 && row(&c, "Home") == 1 && strcmp(v->caption, "LIBRARY") == 0 &&
              !v->up_enabled && !v->note[0]);
    check("nothing to play yet: PLAY, PREV, NEXT, STOP and seeking off; it says what to do",
          !v->play_enabled && !v->prev_enabled && !v->next_enabled && !v->stop_enabled && !v->seek_enabled &&
              strcmp(v->status, "Choose a track in the list.") == 0 && strcmp(v->play_label, "PLAY") == 0);
    check("the time reads 0:00 of --:--, the volume 70 %, both steps on",
          strcmp(v->elapsed, "0:00") == 0 && strcmp(v->total, "--:--") == 0 && strcmp(v->volume, "70 %") == 0 &&
              v->vol_down_enabled && v->vol_up_enabled && v->progress == 0);
    {
        char title[64];
        char caption[600];

        mp3_view_row(&c, 0, title, sizeof(title), caption, sizeof(caption));
        check("a place's row: its name and its folder", strcmp(title, "Music") == 0 && strcmp(caption, music) == 0);
    }
    go(&c, NULL);
    v = seen(&c);
    check("Music opens: folders first; UP on; the caption names the place; PLAY off (no files here)",
          c.list->e[0].kind == MP3_ENTRY_FOLDER && strcmp(v->caption, "Music") == 0 && v->up_enabled &&
              !v->play_enabled);
    go(&c, "album");
    v = seen(&c);
    check("a folder in it: the caption follows", strcmp(v->caption, "Music/album") == 0 && c.list->tracks == 3);
    mp3_ctl_up(&c);
    WAIT(&c, 2000, listing(&c, music));
    mp3_ctl_up(&c);
    WAIT(&c, 2000, listing(&c, ""));
    check("UP climbs to the place, then to the places", listing(&c, ""));
    mp3_ctl_close(&c);
}

static void transport(void)
{
    struct mp3_ctl c;
    struct mp3_view *v;
    int64_t held;

    open_music(&c, helper);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    v = seen(&c);
    check("a track tapped: OPENING at once, its name and folder shown, 1 / 3",
          strcmp(v->chip, "OPENING") == 0 && strcmp(v->title, "a1.wav") == 0 && strcmp(v->subtitle, "album") == 0 &&
              strcmp(v->counter, "1 / 3") == 0 && strcmp(v->play_label, "PAUSE") == 0 && v->stop_enabled);
    check("then PLAYING, with the header hint", WAIT(&c, 3000, playing(&c, "a1.wav")) &&
                                                   strcmp(seen(&c)->chip, "PLAYING") == 0 &&
                                                   seen(&c)->hint && strcmp(seen(&c)->hint, "PLAYING") == 0);
    v = seen(&c);
    check("  its length is known and it can seek; the row says NOW",
          strcmp(v->total, "0:01") == 0 && v->seek_enabled && mp3_ctl_row_is_current(&c, row(&c, "a1.wav")));
    pump(&c, 400);
    check("  the position moves", c.player.pos_ms >= 200 && seen(&c)->progress > 100);

    mp3_ctl_play_pause(&c, mono_ms());
    check("PAUSE: the button says PLAY at once", strcmp(seen(&c)->play_label, "PLAY") == 0);
    check("  and the chip PAUSED when the helper has closed the device",
          WAIT(&c, 1000, c.player.state == MP3_PLAYER_PAUSED) && strcmp(seen(&c)->chip, "PAUSED") == 0 &&
              !seen(&c)->hint);
    held = c.player.pos_ms;
    pump(&c, 500);
    check("  paused, the position holds", c.player.pos_ms == held);
    mp3_ctl_play_pause(&c, mono_ms());
    check("PLAY resumes", WAIT(&c, 1000, c.player.state == MP3_PLAYER_PLAYING) &&
                              strcmp(seen(&c)->play_label, "PAUSE") == 0);

    mp3_ctl_stop(&c, mono_ms());
    check("STOP: stopped, the time back to 0:00, STOP off",
          WAIT(&c, 1000, !mp3_player_running(&c.player)) && strcmp(seen(&c)->chip, "STOPPED") == 0 &&
              strcmp(seen(&c)->elapsed, "0:00") == 0 && !seen(&c)->stop_enabled && seen(&c)->play_enabled);
    pump(&c, 300);
    check("  and it stays stopped: the next track does not start", !mp3_player_running(&c.player) &&
                                                                   c.q_index == 0);
    mp3_ctl_next(&c, mono_ms());
    check("NEXT while stopped chooses the next track and plays nothing",
          c.q_index == 1 && !mp3_player_running(&c.player) && strcmp(seen(&c)->title, "a2.wav") == 0 &&
              strcmp(seen(&c)->counter, "2 / 3") == 0);
    mp3_ctl_prev(&c, mono_ms());
    check("PREV while stopped goes back", c.q_index == 0 && !mp3_player_running(&c.player));
    mp3_ctl_prev(&c, mono_ms());
    check("  and stays on the first track", c.q_index == 0);
    mp3_ctl_play_pause(&c, mono_ms());
    check("PLAY after STOP plays the chosen track from the start", WAIT(&c, 3000, playing(&c, "a1.wav")) &&
                                                                   c.player.pos_ms < 400);
    mp3_ctl_next(&c, mono_ms());
    check("NEXT while playing plays the next track",
          WAIT(&c, 3000, playing(&c, "a2.wav")) && c.q_index == 1 && !c.message[0]);
    mp3_ctl_prev(&c, mono_ms());
    check("PREV early in a track plays the one before", WAIT(&c, 3000, playing(&c, "a1.wav")) && !c.message[0]);

    /* The end: a1 -> a2 -> a3 -> the end of the folder. */
    check("a track's end starts the next", WAIT(&c, 3000, playing(&c, "a2.wav")));
    check("and the next", WAIT(&c, 3000, playing(&c, "a3.wav")) && strcmp(seen(&c)->counter, "3 / 3") == 0 &&
                              !seen(&c)->next_enabled);
    mp3_ctl_next(&c, mono_ms());
    check("NEXT on the last track says so and carries on", strstr(c.message, "last track") &&
                                                            playing(&c, "a3.wav"));
    check("the queue's end stops, and says so",
          WAIT(&c, 3000, !mp3_player_running(&c.player)) && strcmp(c.message, "End of album.") == 0 &&
              c.q_index == 2 && strcmp(seen(&c)->chip, "STOPPED") == 0);
    check("  each track had its own helper, one after another, never two at once (no device-busy)",
          card_playbacks() >= 7 && !strstr(c.message, "in use"));
    mp3_ctl_close(&c);
    check("closing leaves no helper", no_child());
}

static void prev_restarts(void)
{
    struct mp3_ctl c;

    open_music(&c, helper);
    go(&c, "long");
    mp3_ctl_open_entry(&c, row(&c, "l2.wav"), mono_ms());
    WAIT(&c, 3000, playing(&c, "l2.wav"));
    WAIT(&c, 5000, c.player.pos_ms > MP3_PREV_RESTART_MS + 200);
    mp3_ctl_prev(&c, mono_ms());
    check("PREV more than 3 s into a track starts it again (a seek, the same helper)",
          WAIT(&c, 1000, c.player.pos_ms < 1000) && playing(&c, "l2.wav") && c.q_index == 1);

    /* Seeking. */
    mp3_ctl_seek_permille(&c, 800);
    check("a seek to 80 %", WAIT(&c, 1000, c.player.pos_ms >= 4000) && c.player.pos_ms < 4500);
    mp3_ctl_play_pause(&c, mono_ms());
    WAIT(&c, 1000, c.player.state == MP3_PLAYER_PAUSED);
    mp3_ctl_seek_permille(&c, 100);
    check("a seek while paused moves the position and stays paused",
          WAIT(&c, 1000, c.player.pos_ms <= 600) && c.player.state == MP3_PLAYER_PAUSED);
    mp3_ctl_seek_permille(&c, 5000);
    check("a seek past the end is the end", WAIT(&c, 1000, c.player.pos_ms == 5000));
    mp3_ctl_close(&c);
}

static void bad_tracks(void)
{
    struct mp3_ctl c;

    open_music(&c, helper);
    go(&c, "mixed");
    mp3_ctl_open_entry(&c, row(&c, "m2 bad.wav"), mono_ms());
    check("a damaged file tapped: said on screen, nothing plays, no skip",
          WAIT(&c, 3000, !mp3_player_running(&c.player) && c.message[0]) &&
              strstr(c.message, "m2 bad.wav cannot be played") && c.tone == MP3_TONE_ERROR && c.q_index == 1);
    mp3_ctl_open_entry(&c, row(&c, "m3 empty.wav"), mono_ms());
    check("an empty file: said so", WAIT(&c, 3000, !mp3_player_running(&c.player) && strstr(c.message, "empty")));
    check("  the row says it is empty", ({
              char t[300];
              char cap[300];
              mp3_view_row(&c, row(&c, "m3 empty.wav"), t, sizeof(t), cap, sizeof(cap));
              strstr(cap, "empty file") != NULL;
          }));

    mp3_ctl_open_entry(&c, row(&c, "m1.wav"), mono_ms());
    WAIT(&c, 3000, playing(&c, "m1.wav"));
    check("reached by the end of the one before, bad files are skipped and said",
          WAIT(&c, 5000, playing(&c, "m4.wav")) && strstr(c.message, "Skipped") && c.skipped == 2);
    check("and the folder then ends as usual", WAIT(&c, 3000, strcmp(c.message, "End of mixed.") == 0));

    go(&c, "allbad");
    mp3_ctl_open_entry(&c, row(&c, "b1.wav"), mono_ms());
    WAIT(&c, 3000, playing(&c, "b1.wav"));
    check("a folder of nothing but bad files after the first: skipping stops at the end, no loop",
          WAIT(&c, 6000, !mp3_player_running(&c.player) && c.q_index == c.q_n - 1) &&
              c.tone == MP3_TONE_ERROR && c.skipped <= c.q_n);

    /* Decoding failing part way. */
    setenv("POS_MP3_FAIL_AT_MS", "200", 1);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    check("a file that stops decoding: said, and playback stops",
          WAIT(&c, 3000, !mp3_player_running(&c.player) && c.message[0]) && strstr(c.message, "damaged") &&
              c.q_index == 0);
    unsetenv("POS_MP3_FAIL_AT_MS");

    /* A track deleted from under the queue. */
    write_wav(at("home/Music/gone/g1.wav", NULL), 600);
    write_wav(at("home/Music/gone/g2.wav", NULL), 600);
    write_wav(at("home/Music/gone/g3.wav", NULL), 600);
    go(&c, "gone");
    mp3_ctl_open_entry(&c, row(&c, "g1.wav"), mono_ms());
    WAIT(&c, 3000, playing(&c, "g1.wav"));
    unlink(at("home/Music/gone/g2.wav", NULL));
    check("a queued track that disappeared is skipped with a word, the next one plays",
          WAIT(&c, 4000, playing(&c, "g3.wav")) && strstr(c.message, "g2.wav is not there any more"));
    mp3_ctl_stop(&c, mono_ms());
    WAIT(&c, 1000, !mp3_player_running(&c.player));
    unlink(at("home/Music/gone/g1.wav", NULL));
    unlink(at("home/Music/gone/g3.wav", NULL));
    rmdir(at("home/Music/gone", NULL));
    mp3_ctl_open_entry(&c, row(&c, "g1.wav"), mono_ms());
    check("its whole folder gone (storage removed): playing says so, nothing crashes",
          WAIT(&c, 3000, !mp3_player_running(&c.player) && c.message[0]) && strstr(c.message, "not there any more"));
    mp3_ctl_up(&c);
    WAIT(&c, 2000, !c.loading);
    check("the list above still reads", listing(&c, music) && row(&c, "gone") < 0);
    mp3_ctl_close(&c);
}

static void failures(void)
{
    struct mp3_ctl c;
    int fd;

    /* The device in use: Wave or Recorder holds the audio lock. */
    fd = open(at("run/audio.lock", NULL), O_CREAT | O_RDWR, 0600);
    flock(fd, LOCK_EX);
    open_music(&c, helper);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    check("the device in use: it says so and nothing plays",
          WAIT(&c, 3000, !mp3_player_running(&c.player) && c.message[0]) &&
              strstr(c.message, "audio device is in use") && strcmp(seen(&c)->chip, "STOPPED") == 0);
    close(fd);
    mp3_ctl_play_pause(&c, mono_ms());
    check("  and once it is free, PLAY plays", WAIT(&c, 3000, playing(&c, "a1.wav")) && !c.message[0]);
    mp3_ctl_close(&c);

    /* No helper at all. */
    mp3_ctl_open(&c, &vol, at("no-such-helper", NULL));
    WAIT(&c, 2000, listing(&c, ""));
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    check("the helper missing: a failure said on screen, the app carries on",
          WAIT(&c, 3000, !mp3_player_running(&c.player) && c.message[0]) && strstr(c.message, "Playback failed") &&
              no_child());
    mp3_ctl_close(&c);

    /* A resume the device refuses (fake helper). */
    setenv("MP3_FAKE", "echo", 1);
    setenv("MP3_FAKE_RESUME_BUSY", "1", 1);
    open_music(&c, fake);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    WAIT(&c, 3000, c.player.state == MP3_PLAYER_PLAYING);
    check("tags from the file: the title and the artist replace the file name",
          strcmp(seen(&c)->title, "Fake Song") == 0 && strcmp(seen(&c)->subtitle, "Fake Artist") == 0);
    mp3_ctl_play_pause(&c, mono_ms());
    WAIT(&c, 1000, c.player.state == MP3_PLAYER_PAUSED);
    mp3_ctl_play_pause(&c, mono_ms());
    check("a refused resume: still paused, PLAY offered again, and the reason shown",
          WAIT(&c, 1000, c.message[0] != '\0') && c.player.state == MP3_PLAYER_PAUSED &&
              strcmp(seen(&c)->play_label, "PLAY") == 0 && strstr(c.message, "in use"));
    unsetenv("MP3_FAKE_RESUME_BUSY");
    mp3_ctl_close(&c);

    setenv("MP3_FAKE", "decode", 1);
    open_music(&c, fake);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    check("the helper's own error text reaches the screen", WAIT(&c, 3000, !mp3_player_running(&c.player) &&
                                                                          strstr(c.message, "damaged")));
    mp3_ctl_close(&c);
    unsetenv("MP3_FAKE");
}

static void rapid(void)
{
    struct mp3_ctl c;
    int k;
    int before;

    open_music(&c, helper);
    go(&c, "many");
    setenv("POS_MP3_OPEN_DELAY_MS", "250", 1);
    before = card_playbacks();
    mp3_ctl_open_entry(&c, row(&c, "n01.wav"), mono_ms());
    for (k = 0; k < 5; k++) {
        mp3_ctl_next(&c, mono_ms());
        mp3_ctl_poll(&c, mono_ms());
        nap(15);
    }
    check("five quick NEXTs: the choice moves at once", c.q_index == 5);
    check("  and what plays is the last one chosen", WAIT(&c, 4000, playing(&c, "n06.wav")));
    check("  one helper at a time, and not one per press: at most three were started",
          card_playbacks() - before <= 3 && !c.message[0]);
    unsetenv("POS_MP3_OPEN_DELAY_MS");

    for (k = 0; k < 7; k++) {
        mp3_ctl_play_pause(&c, mono_ms());
        mp3_ctl_poll(&c, mono_ms());
    }
    check("seven quick PLAY/PAUSEs settle on PAUSED",
          WAIT(&c, 2000, c.player.state == MP3_PLAYER_PAUSED) && (pump(&c, 300), c.player.state == MP3_PLAYER_PAUSED));
    for (k = 0; k < 8; k++) {
        mp3_ctl_play_pause(&c, mono_ms());
    }
    check("eight more settle where they began (PAUSED)", (pump(&c, 400), c.player.state == MP3_PLAYER_PAUSED) &&
                                                         c.player.want_paused);
    mp3_ctl_play_pause(&c, mono_ms());
    check("  one more plays", WAIT(&c, 1000, c.player.state == MP3_PLAYER_PLAYING));
    for (k = 0; k < 4; k++) {
        mp3_ctl_stop(&c, mono_ms());
        mp3_ctl_play_pause(&c, mono_ms());
    }
    check("STOP and PLAY in a burst end playing, the device never found busy", WAIT(&c, 3000, playing(&c, "n06.wav")) &&
                                                                   !c.message[0]);
    mp3_ctl_close(&c);
    check("closed: no helper left", no_child());
}

static void volume(void)
{
    struct mp3_ctl c;
    FILE *f;
    char line[128];
    int got80 = 0;

    g_level = 70;
    g_muted = 0;
    setenv("MP3_FAKE", "echo", 1);
    setenv("MP3_FAKE_LOG", at("fake.log", NULL), 1);
    open_music(&c, fake);
    go(&c, "album");
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    WAIT(&c, 3000, c.player.state == MP3_PLAYER_PLAYING);
    mp3_ctl_volume_step(&c, 1, mono_ms());
    check("VOL +: the system volume goes up a step", g_level == 80 && strcmp(seen(&c)->volume, "80 %") == 0);
    pump(&c, 300);
    f = fopen(at("fake.log", NULL), "r");
    while (f && fgets(line, sizeof(line), f)) {
        got80 |= strncmp(line, "volume 80", 9) == 0;
    }
    if (f) {
        fclose(f);
    }
    check("  and the helper hears it at once", got80);
    check("  the helper was started at the system volume",
          (f = fopen(at("fake.log", NULL), "r")) && fgets(line, sizeof(line), f) &&
              strstr(line, "--volume-percent 70") != NULL);
    if (f) {
        fclose(f);
    }
    g_level = 100;
    g_sets = 0;
    mp3_ctl_volume_step(&c, 1, mono_ms());
    check("at 100 %, VOL + is off and changes nothing", g_sets == 0 && !seen(&c)->vol_up_enabled);
    g_level = 10;
    mp3_ctl_volume_step(&c, -1, mono_ms());
    check("at 10 %, VOL - is off and changes nothing", g_sets == 0 && !seen(&c)->vol_down_enabled && g_level == 10);
    g_level = 50;

    g_muted = 1;
    check("muted in Controls while playing: playback pauses and it says why",
          WAIT(&c, 1000, c.player.state == MP3_PLAYER_PAUSED) && strstr(c.message, "muted") &&
              strcmp(seen(&c)->volume, "MUTED") == 0 && !seen(&c)->vol_down_enabled && seen(&c)->vol_up_enabled);
    mp3_ctl_play_pause(&c, mono_ms());
    pump(&c, 200);
    check("  PLAY while muted plays nothing", c.player.state == MP3_PLAYER_PAUSED);
    mp3_ctl_volume_step(&c, 1, mono_ms());
    check("  VOL + unmutes (the level is kept)", g_muted == 0 && g_level == 50);
    mp3_ctl_play_pause(&c, mono_ms());
    check("  and PLAY resumes", WAIT(&c, 1000, c.player.state == MP3_PLAYER_PLAYING));
    mp3_ctl_stop(&c, mono_ms());
    WAIT(&c, 1000, !mp3_player_running(&c.player));
    g_muted = 1;
    mp3_ctl_play_pause(&c, mono_ms());
    check("muted before PLAY: nothing starts", !mp3_player_running(&c.player) && strstr(c.message, "muted"));
    g_muted = 0;
    g_available = 0;
    check("no sound card: NO AUDIO, both steps off", strcmp(seen(&c)->volume, "NO AUDIO") == 0 &&
                                                        !seen(&c)->vol_up_enabled && !seen(&c)->vol_down_enabled);
    g_available = 1;
    g_level = 70;
    mp3_ctl_close(&c);
    unsetenv("MP3_FAKE");
    unsetenv("MP3_FAKE_LOG");
}

static void boundary(void)
{
    struct mp3_ctl c;

    open_music(&c, helper);
    go(&c, "big");
    check("a folder of 205 files: 200 listed, and the list says there are more",
          c.list->n == MP3_LIST_MAX && strstr(seen(&c)->note, "first 200"));
    mp3_ctl_open_entry(&c, MP3_LIST_MAX - 1, mono_ms());
    check("the last listed is the last in the queue: 200 / 200",
          c.q_n == MP3_LIST_MAX && strcmp(seen(&c)->counter, "200 / 200") == 0 && !seen(&c)->next_enabled);
    mp3_ctl_close(&c);

    open_music(&c, helper);
    go(&c, "empty");
    check("an empty folder says what to do", strstr(seen(&c)->note, "No audio files") && !seen(&c)->play_enabled);
    mp3_ctl_close(&c);
}

static void remembered(void)
{
    struct mp3_ctl c;
    char dir[512];

    open_music(&c, helper);
    go(&c, "album");
    mp3_ctl_close(&c);
    snprintf(dir, sizeof(dir), "%s/album", music);
    mp3_ctl_open(&c, &vol, helper);
    check("reopening: the folder last shown comes back", WAIT(&c, 3000, listing(&c, dir)) &&
                                                         strcmp(seen(&c)->caption, "Music/album") == 0);
    mp3_ctl_open_entry(&c, row(&c, "a1.wav"), mono_ms());
    WAIT(&c, 3000, playing(&c, "a1.wav"));
    mp3_ctl_close(&c);

    rename(at("home/Music/album", NULL), at("home/Music/album-moved", NULL));
    mp3_ctl_open(&c, &vol, helper);
    check("the remembered folder gone: the places, quietly", WAIT(&c, 3000, listing(&c, "")) && !c.message[0]);
    mp3_ctl_close(&c);
    rename(at("home/Music/album-moved", NULL), at("home/Music/album", NULL));
}

static void cleanup(int fds_before)
{
    struct mp3_ctl c;
    int k;
    int64_t t0;
    int64_t worst = 0;

    for (k = 0; k < 10; k++) {
        open_music(&c, helper);
        go(&c, "long");
        mp3_ctl_open_entry(&c, row(&c, "l1.wav"), mono_ms());
        if (k % 2) {
            WAIT(&c, 3000, playing(&c, "l1.wav"));
        }
        if (k % 3 == 2) {
            mp3_ctl_play_pause(&c, mono_ms());
        }
        t0 = mono_ms();
        mp3_ctl_close(&c);
        if (mono_ms() - t0 > worst) {
            worst = mono_ms() - t0;
        }
    }
    printf("note longest close while starting, playing or paused: %lld ms\n", (long long)worst);
    check("ten opens and closes while starting, playing or paused: each close within the grace",
          worst < MP3_DESTROY_GRACE_MS);
    check("  no helper is left", no_child());
    check("  the device is closed and the amplifier off",
          ({
              FILE *f = fopen(at("audio/pcm", NULL), "r");
              char v[32] = "";
              if (f) {
                  if (!fgets(v, sizeof(v), f)) {
                      v[0] = '\0';
                  }
                  fclose(f);
              }
              strncmp(v, "closed", 6) == 0;
          }));
    for (k = 0; k < 200 && mp3_scanner_threads() > 0; k++) {
        nap(10);
    }
    check("  no scan thread is left", mp3_scanner_threads() == 0);
    check("  and no descriptor is left open", open_fds() == fds_before);
}

int main(int argc, char **argv)
{
    char cmd[600];
    int fds;
    int k;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 3 || access(argv[1], X_OK) != 0 || access(argv[2], X_OK) != 0) {
        printf("usage: mp3_ctl_test <pos-mp3-testhooks> <fake_pos_mp3.sh>\n");
        return 2;
    }
    helper = argv[1];
    fake = argv[2];
    signal(SIGPIPE, SIG_IGN);
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(music, sizeof(music), "%s/home/Music", tmp);
    mkdir(at("home", NULL), 0755);
    mkdir(at("audio", NULL), 0700);
    mkdir(at("run", NULL), 0700);
    mkdir(at("state", NULL), 0700);
    text_file(at("audio/route", NULL), "0\n");
    setenv("HOME", at("home", NULL), 1);
    setenv("POCKETOS_MP3_MEDIA_ROOTS", "", 1);
    unsetenv("POCKETOS_MUSIC_DIR");
    unsetenv("POCKETOS_RECORDINGS_DIR");
    setenv("POCKETOS_STATE_DIR", at("state", NULL), 1);
    setenv("POCKETOS_RUNTIME_DIR", at("run", NULL), 1);
    setenv("POS_MP3_FAKE_AUDIO", at("audio", NULL), 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "playback", 1);

    mkdir(music, 0755);
    const char *dirs[] = { "album", "long", "mixed", "allbad", "many", "big", "empty", "gone" };
    for (k = 0; k < (int)(sizeof(dirs) / sizeof(dirs[0])); k++) {
        mkdir(at("home/Music/%s", dirs[k]), 0755);
    }
    write_wav(at("home/Music/album/a1.wav", NULL), 1200);
    write_wav(at("home/Music/album/a2.wav", NULL), 1200);
    write_wav(at("home/Music/album/a3.wav", NULL), 1200);
    write_wav(at("home/Music/long/l1.wav", NULL), 5000);
    write_wav(at("home/Music/long/l2.wav", NULL), 5000);
    write_wav(at("home/Music/mixed/m1.wav", NULL), 600);
    text_file(at("home/Music/mixed/m2 bad.wav", NULL), "this is not audio\n");
    text_file(at("home/Music/mixed/m3 empty.wav", NULL), "");
    write_wav(at("home/Music/mixed/m4.wav", NULL), 600);
    write_wav(at("home/Music/allbad/b1.wav", NULL), 400);
    for (k = 2; k <= 5; k++) {
        char name[64];

        snprintf(name, sizeof(name), "home/Music/allbad/b%d.wav", k);
        text_file(at("%s", name), "junk\n");
    }
    for (k = 1; k <= 8; k++) {
        char name[64];

        snprintf(name, sizeof(name), "home/Music/many/n%02d.wav", k);
        write_wav(at("%s", name), 3000);
    }
    for (k = 0; k < MP3_LIST_MAX + 5; k++) {
        char name[64];

        snprintf(name, sizeof(name), "home/Music/big/t%03d.mp3", k);
        text_file(at("%s", name), "x");
    }
    fds = open_fds();

    first_screen();
    transport();
    prev_restarts();
    bad_tracks();
    failures();
    rapid();
    volume();
    boundary();
    remembered();
    cleanup(fds);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", tmp);
    }
    printf("mp3_ctl_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
