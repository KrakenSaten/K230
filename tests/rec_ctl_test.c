/*
 * The Recorder's controller (apps/recorder/rec_ctl.h) end to end, with no
 * LVGL: the real pos-record (test hooks, file-backed sound card) behind it
 * and a real Recordings folder in front. Open repairs; RECORD, PAUSE, RESUME
 * and STOP give a saved, selected file; PLAY plays it and RECORD during a
 * playback waits for it to end; a double-tap delete; a muted system does not
 * play; a full disk and a busy device are said and create nothing; a helper
 * killed mid-recording is repaired at once; closing mid-recording saves; the
 * preset survives a reopen; the name follows the clock when there is one.
 *
 * Usage: rec_ctl_test <pos-record-testhooks> <fake_pos_record.sh>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rec_ctl.h"
#include "rec_protocol.h"
#include "rec_view.h"

#include <errno.h>
#include <fcntl.h>
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
static char tmp[] = "/tmp/rec_ctl_test.XXXXXX";
static char recs[256];
static char audio[256];
static char run[256];
static char freefile[256];
static int volume = 60;

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

static int the_volume(void)
{
    return volume;
}

static struct rec_ctl c;

static void pump(int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        struct timespec d = { 0, 20 * 1000000L };

        rec_ctl_poll(&c, mono_ms());
        nanosleep(&d, NULL);
    }
}

static int until_state(enum rec_state s, int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        struct timespec d = { 0, 20 * 1000000L };

        rec_ctl_poll(&c, mono_ms());
        if (c.m.state == s) {
            return 1;
        }
        nanosleep(&d, NULL);
    }
    return c.m.state == s;
}

static int until_idle(int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        struct timespec d = { 0, 20 * 1000000L };

        rec_ctl_poll(&c, mono_ms());
        if (!rec_machine_busy(&c.m)) {
            return 1;
        }
        nanosleep(&d, NULL);
    }
    return !rec_machine_busy(&c.m);
}

static int exists(const char *name)
{
    char p[512];
    struct stat st;

    snprintf(p, sizeof(p), "%s/%s", recs, name);
    return stat(p, &st) == 0;
}

static int select_name(const char *name)
{
    int i;

    for (i = 0; i < c.list.n; i++) {
        if (strcmp(c.list.e[i].name, name) == 0) {
            rec_ctl_select(&c, i);
            return 1;
        }
    }
    return 0;
}

static void set_free(unsigned long long bytes)
{
    FILE *f = fopen(freefile, "w");

    if (f) {
        fprintf(f, "%llu\n", bytes);
        fclose(f);
    }
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int no_children(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static void test_open_record_play(void)
{
    struct rec_view v;
    char part[512];
    int fd;

    /* An interrupted recording from "last time". */
    snprintf(part, sizeof(part), "%s/REC-0007.wav.part", recs);
    {
        unsigned char h[44] = { 'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0,
                                'd', 'a', 't', 'a', 0, 0, 0, 0 };
        char zeros[3200];

        memset(zeros, 1, sizeof(zeros));
        fd = open(part, O_WRONLY | O_CREAT, 0600);
        if (write(fd, h, 44) != 44 || write(fd, zeros, sizeof(zeros)) != (ssize_t)sizeof(zeros)) {
            printf("note short write\n");
        }
        close(fd);
    }

    rec_ctl_open(&c, the_volume);
    check("opening starts in CHECKING, with the repair running", c.m.state == REC_ST_CHECKING &&
                                                                     rec_session_active(&c.session));
    rec_view_refresh(&c, mono_ms(), &v);
    check("and RECORD waits for it", !v.main_enabled);
    check("the repair ends in IDLE", until_state(REC_ST_IDLE, 5000));
    check("the interrupted recording was repaired and is listed",
          exists("REC-0007-recovered.wav") && !exists("REC-0007.wav.part") && c.list.n == 1 &&
              strstr(c.message, "Recovered 1 interrupted recording") != NULL);
    {
        struct stat st;

        /* It was created 0777 for this test: open takes the others' write off. */
        check("the folder is not left writable by others", stat(recs, &st) == 0 && (st.st_mode & 022) == 0);
    }

    rec_ctl_record(&c, mono_ms(), 0, false);
    check("RECORD starts a recording", c.m.state == REC_ST_STARTING && c.m.op == REC_OP_RECORD);
    check("which is RECORDING once the helper says so", until_state(REC_ST_RECORDING, 3000));
    check("named by sequence without a clock, after the repaired one",
          strcmp(c.current, "REC-0008.wav") == 0 && exists("REC-0008.wav.part"));
    pump(1400);
    rec_view_refresh(&c, mono_ms(), &v);
    check("the view shows STOP, MIC ON and a real level",
          strcmp(v.main_label, "STOP") == 0 && v.mic_on && v.meter_pct > 50 && c.elapsed_ms > 500);
    rec_ctl_pause(&c, mono_ms());
    check("PAUSE pauses", until_state(REC_ST_PAUSED, 2000));
    rec_view_refresh(&c, mono_ms(), &v);
    check("paused: RESUME, no level, microphone off", strcmp(v.pause_label, "RESUME") == 0 && v.meter_pct == 0 &&
                                                         !v.mic_on);
    rec_ctl_pause(&c, mono_ms());
    check("and resumes", until_state(REC_ST_RECORDING, 2000));
    pump(700);
    rec_ctl_record(&c, mono_ms(), 0, false); /* the big button: STOP */
    check("the big button stops it", c.m.state == REC_ST_STOPPING);
    check("and it is saved", until_state(REC_ST_IDLE, 5000) && exists("REC-0008.wav") &&
                                 !exists("REC-0008.wav.part"));
    check("the message says saved, and the new file is selected",
          strncmp(c.message, "Saved REC-0008.wav", 18) == 0 && c.tone == REC_TONE_OK &&
              strcmp(c.selected, "REC-0008.wav") == 0 && c.list.n == 2);
    check("newest first", strcmp(c.list.e[0].name, "REC-0008.wav") == 0 ||
                              strcmp(c.list.e[1].name, "REC-0008.wav") == 0);

    rec_ctl_play(&c, mono_ms());
    check("PLAY plays the selection", until_state(REC_ST_PLAYING, 3000) && c.total_ms > 500);
    rec_view_refresh(&c, mono_ms(), &v);
    check("PLAY reads STOP while it plays", strcmp(v.play_label, "STOP") == 0);
    rec_ctl_pause(&c, mono_ms());
    check("a playback pauses", until_state(REC_ST_PLAY_PAUSED, 2000));
    rec_ctl_pause(&c, mono_ms());
    check("and resumes", until_state(REC_ST_PLAYING, 2000));
    check("and plays to the end", until_state(REC_ST_IDLE, 8000) && c.elapsed_ms == c.total_ms);

    rec_ctl_play(&c, mono_ms());
    until_state(REC_ST_PLAYING, 3000);
    rec_ctl_record(&c, mono_ms(), 0, false);
    check("RECORD during a playback stops the playback first", c.m.state == REC_ST_STOPPING &&
                                                                  c.m.next == REC_OP_RECORD);
    check("and then records", until_state(REC_ST_RECORDING, 4000) && c.m.op == REC_OP_RECORD);
    pump(1000);
    rec_ctl_stop(&c, mono_ms());
    until_state(REC_ST_IDLE, 5000);
    check("that one is saved too", exists("REC-0009.wav") && c.list.n == 3);
    check("no helper is left", !rec_session_active(&c.session) && no_children());
}

static void test_delete_and_mute(void)
{
    int n = c.list.n;

    check("select a recording", select_name("REC-0009.wav"));
    rec_ctl_delete(&c, mono_ms());
    check("the first DELETE only asks", exists("REC-0009.wav") && rec_ctl_delete_armed(&c, mono_ms()) &&
                                            strstr(c.message, "Tap DELETE again") != NULL);
    rec_ctl_select(&c, 0);
    check("selecting something else disarms it", !rec_ctl_delete_armed(&c, mono_ms()));
    select_name("REC-0009.wav");
    rec_ctl_delete(&c, mono_ms());
    rec_ctl_delete(&c, mono_ms());
    check("the second deletes it", !exists("REC-0009.wav") && c.list.n == n - 1 && c.selected[0] == '\0' &&
                                       strncmp(c.message, "Deleted", 7) == 0);

    select_name("REC-0008.wav");
    volume = 0;
    rec_ctl_play(&c, mono_ms());
    check("muted, nothing plays, and the screen says why",
          !rec_session_active(&c.session) && strstr(c.message, "muted") != NULL && !rec_machine_busy(&c.m));
    volume = 60;
}

static void test_refusals(void)
{
    char lock[300];
    int lfd;
    int n;

    set_free(REC_RESERVE_BYTES + 1000);
    n = c.list.total;
    rec_ctl_record(&c, mono_ms(), 0, false);
    /* The app checks the real filesystem first; the helper checks again,
     * and it is the helper's view (the test seam) that is full here. */
    check("with the storage full RECORD says so and creates nothing",
          until_idle(3000) && strstr(c.message, "Storage is full") != NULL && c.m.state == REC_ST_ERROR &&
              c.list.total == n);
    set_free(10000000000ull);

    snprintf(lock, sizeof(lock), "%s/audio.lock", run);
    lfd = open(lock, O_RDWR | O_CREAT, 0644);
    flock(lfd, LOCK_EX);
    rec_ctl_record(&c, mono_ms(), 0, false);
    check("with the audio in use: 'Audio device in use', nothing created",
          until_idle(3000) && strcmp(c.message, "Audio device in use") == 0 && c.m.state == REC_ST_ERROR &&
              c.list.total == n);
    close(lfd);
}

static void test_crash_and_close(void)
{
    pid_t pid;

    rec_ctl_record(&c, mono_ms(), 1790000000, true);
    until_state(REC_ST_RECORDING, 3000);
    check("with a clock the name is the date and time", strcmp(c.current, "REC-20260921-141320.wav") == 0);
    pump(2500); /* past a checkpoint */
    pid = c.session.pid;
    kill(pid, SIGKILL);
    check("a helper killed mid-recording sends the app to repair it", until_state(REC_ST_CHECKING, 3000));
    check("and the repair saves what it had", until_idle(5000) && c.m.state == REC_ST_ERROR &&
                                                  exists("REC-20260921-141320-recovered.wav"));
    check("the screen says what happened", strstr(c.message, "stopped unexpectedly") != NULL ||
                                               strstr(c.message, "Recovered") != NULL);

    rec_ctl_record(&c, mono_ms(), 0, false);
    until_state(REC_ST_RECORDING, 3000);
    pump(800);
    {
        char name[REC_FILE_NAME_MAX];
        int64_t t0 = mono_ms();

        snprintf(name, sizeof(name), "%s", c.current);
        rec_ctl_close(&c);
        printf("note close took %lld ms\n", (long long)(mono_ms() - t0));
        check("closing the app mid-recording saves it within the grace",
              exists(name) && mono_ms() - t0 <= REC_DESTROY_GRACE_MS + 300);
    }
    check("and leaves no helper", no_children());
}

static void test_reopen(void)
{
    struct rec_view v;
    int k;
    int ok = 1;

    rec_ctl_open(&c, the_volume);
    until_idle(5000);
    rec_ctl_next_preset(&c);
    check("the preset changes to Standard", c.preset == REC_PRESET_STANDARD);
    rec_ctl_close(&c);
    rec_ctl_open(&c, the_volume);
    until_idle(5000);
    check("and is remembered after a reopen", c.preset == REC_PRESET_STANDARD);
    rec_ctl_record(&c, mono_ms(), 0, false);
    until_state(REC_ST_RECORDING, 3000);
    check("a Standard recording is 48 kHz", c.rate == 48000);
    rec_view_refresh(&c, mono_ms(), &v);
    check("and the preset cannot change while recording", !v.preset_enabled);
    rec_ctl_next_preset(&c);
    check("even when asked", c.preset == REC_PRESET_STANDARD);
    rec_ctl_stop(&c, mono_ms());
    until_idle(5000);
    rec_ctl_close(&c);

    /* Twenty opens and closes, some mid-recording: nothing leaks. */
    for (k = 0; k < 20; k++) {
        rec_ctl_open(&c, the_volume);
        until_idle(5000);
        if (k % 2) {
            rec_ctl_record(&c, mono_ms(), 0, false);
            until_state(REC_ST_RECORDING, 3000);
            rec_ctl_record(&c, mono_ms(), 0, false); /* rapid stop */
        }
        rec_ctl_close(&c);
        ok &= no_children() && !rec_session_active(&c.session);
    }
    check("twenty opens and closes, half of them mid-recording, leave no helper", ok);
    {
        char cmd[400];
        FILE *p;
        int parts = -1;

        snprintf(cmd, sizeof(cmd), "ls %s | grep -c '\\.part$'", recs);
        p = popen(cmd, "r");
        if (p && fscanf(p, "%d", &parts) != 1) {
            parts = -1;
        }
        if (p) {
            pclose(p);
        }
        check("and no .part", parts == 0);
    }
}

static void test_bad_folder(const char *fake)
{
    char file[300];
    struct rec_view v;

    snprintf(file, sizeof(file), "%s/afile", tmp);
    put(file, "x");
    snprintf(file, sizeof(file), "%s/afile/Recordings", tmp);
    setenv("POCKETOS_RECORDINGS_DIR", file, 1);
    setenv("POCKETOS_RECORD_HELPER", fake, 1);
    rec_ctl_open(&c, the_volume);
    until_idle(3000);
    rec_view_refresh(&c, mono_ms(), &v);
    check("a folder that cannot be made is said, and RECORD stays off",
          c.dir_error != 0 && !v.main_enabled && strstr(c.message, "Recordings folder") != NULL);
    rec_ctl_record(&c, mono_ms(), 0, false);
    check("and pressing it anyway starts nothing", !rec_session_active(&c.session));
    rec_ctl_close(&c);
}

int main(int argc, char **argv)
{
    char cmd[300];
    char state[300];
    char route[300];

    if (argc < 3) {
        fprintf(stderr, "usage: rec_ctl_test <pos-record-testhooks> <fake_pos_record.sh>\n");
        return 2;
    }
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(recs, sizeof(recs), "%s/home/Recordings", tmp);
    snprintf(audio, sizeof(audio), "%s/audio", tmp);
    snprintf(run, sizeof(run), "%s/run", tmp);
    snprintf(state, sizeof(state), "%s/state", tmp);
    snprintf(freefile, sizeof(freefile), "%s/free", tmp);
    mkdir(audio, 0700);
    mkdir(run, 0700);
    snprintf(cmd, sizeof(cmd), "%s/home", tmp);
    mkdir(cmd, 0700);
    mkdir(recs, 0777);
    chmod(recs, 0777);
    snprintf(route, sizeof(route), "%s/route", audio);
    put(route, "1\n");
    set_free(10000000000ull);
    {
        /* The microphone hears a loud tone. */
        char raw[300];
        FILE *f;
        int i;

        snprintf(raw, sizeof(raw), "%s/capture.raw", audio);
        f = fopen(raw, "wb");
        for (i = 0; i < 48000 * 30; i++) {
            int16_t v = (int16_t)((i / 24) % 2 ? 12000 : -12000);

            fwrite(&v, 2, 1, f);
        }
        fclose(f);
    }
    setenv("POCKETOS_RECORDINGS_DIR", recs, 1);
    setenv("POCKETOS_STATE_DIR", state, 1);
    setenv("POCKETOS_RECORD_HELPER", argv[1], 1);
    setenv("POS_RECORD_FAKE_AUDIO", audio, 1);
    setenv("POS_RECORD_FREE_FILE", freefile, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    setenv("POCKETOS_AUDIO_ALLOW_UNVERIFIED", "capture,playback", 1);
    signal(SIGPIPE, SIG_IGN);

    test_open_record_play();
    test_delete_and_mute();
    test_refusals();
    test_crash_and_close();
    test_reopen();
    test_bad_folder(argv[2]);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", tmp);
    }
    printf("rec_ctl_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
