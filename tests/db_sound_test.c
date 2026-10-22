/*
 * DeskBuddy's visit sound: the backend seam, the chime itself, and the
 * pos-record backend against a stand-in helper that writes down how it was
 * started (the real pos-record plays the same WAV on the device; ADR-010).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_sound.h"

#include "pocketwav/pocketwav.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the seam ---------------------------------------------------------------- */

static int fake_plays;
static int fake_volume;

static int fake_available(void) { return 1; }
static int fake_play(int volume_percent)
{
    fake_plays++;
    fake_volume = volume_percent;
    return 0;
}

static const struct db_sound_backend fake = {
    .name = "fake", .available = fake_available, .play = fake_play, .stop = NULL, .why = "counts",
};

static void test_seam(void)
{
    check("the default backend is pos-record", strcmp(db_sound_backend_name(), "pos-record") == 0);
    db_sound_set_backend(NULL);
    check("the silent backend has no sound, and says so",
          !db_sound_available() && strstr(db_sound_why(), "not heard") != NULL);
    check("asking it to play does nothing", db_sound_play(80) == -1);
    db_sound_stop();
    db_sound_set_backend(&fake);
    check("a registered backend is used", strcmp(db_sound_backend_name(), "fake") == 0 && db_sound_available());
    check("it plays at the volume asked", db_sound_play(70) == 0 && fake_plays == 1 && fake_volume == 70);
    check("never at volume 0 (muted)", db_sound_play(0) == -1 && fake_plays == 1);
    check("never above 100", db_sound_play(150) == 0 && fake_volume == 100);
    db_sound_stop(); /* a backend without stop */
    db_sound_set_backend(&db_sound_pos_record);
}

/* ---- the chime --------------------------------------------------------------- */

static int16_t tone[48000];

static void test_tone(void)
{
    size_t n = db_sound_tone_render(tone, 48000);
    int peak = 0;
    int lead = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        peak = abs(tone[i]) > peak ? abs(tone[i]) : peak;
    }
    for (i = 0; i < 48000 * 30 / 1000; i++) {
        lead += tone[i] != 0;
    }
    check("the chime is short: under 400 ms", n > 48000 / 10 && n < 48000 * 4 / 10);
    check("it starts with silence, for the amplifier", lead == 0);
    check("restrained: well under full scale", peak > 3000 && peak < 12000);
    check("and ends at rest, no click", abs(tone[n - 1]) < 200);
    check("not one of RIFT's lengths (150 and 222 ms)", n != 48000 * 150 / 1000 && n != 48000 * 222 / 1000);
    check("a buffer too small is refused", db_sound_tone_render(tone, 100) == 0);
    check("the length is asked for without a buffer", db_sound_tone_render(NULL, 0) == n);
}

/* ---- the helper -------------------------------------------------------------- */

static int file_has(const char *path, const char *text)
{
    char buf[2048] = "";
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

static int lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0;
    int c;

    if (!f) {
        return 0;
    }
    while ((c = fgetc(f)) != EOF) {
        n += c == '\n';
    }
    fclose(f);
    return n;
}

static void wait_for(const char *path, const char *text)
{
    int waited;

    for (waited = 0; waited < 3000 && !file_has(path, text); waited += 20) {
        usleep(20000);
    }
}

static void test_helper(void)
{
    char run[] = "/tmp/db-sound-XXXXXX";
    char fake_helper[600];
    char log[600];
    char cmd[700];
    struct pocketwav_info info;
    const char *wav;
    int fd;
    FILE *f;

    if (!mkdtemp(run)) {
        check("a temporary directory", 0);
        return;
    }
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    wav = db_sound_wav_path();
    check("the chime's file is written in DeskBuddy's runtime directory",
          wav && strncmp(wav, run, strlen(run)) == 0 && strstr(wav, "/deskbuddy/visit.wav") != NULL);
    fd = wav ? open(wav, O_RDONLY) : -1;
    check("as a WAV pos-record plays: 48 kHz mono 16-bit, the whole chime",
          fd >= 0 && pocketwav_probe_fd(fd, &info) == POCKETWAV_OK && info.rate == 48000 &&
              info.channels == 1 && info.frames == db_sound_tone_render(NULL, 0));
    if (fd >= 0) {
        close(fd);
    }

    /* A stand-in for pos-record: writes down how it was started and stays a
     * moment, as a short sound does. */
    snprintf(fake_helper, sizeof(fake_helper), "%s/pos-record", run);
    snprintf(log, sizeof(log), "%s/helper.log", run);
    f = fopen(fake_helper, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\necho \"$@\" >> %s\nsleep 0.3\n", log);
        fclose(f);
        chmod(fake_helper, 0755);
    }
    setenv("POCKETOS_RECORD_HELPER", "/nonexistent/pos-record", 1);
    check("with no helper installed the backend says it cannot play",
          !db_sound_available() && strstr(db_sound_why(), "pos-record") != NULL && db_sound_play(50) == -1);
    setenv("POCKETOS_RECORD_HELPER", fake_helper, 1);
    check("with the helper there, it can", db_sound_available());
    check("the chime starts the helper and returns at once", db_sound_play(40) == 0);
    check("a second while the first still sounds is refused, never stacked", db_sound_play(40) == -1);
    wait_for(log, "visit.wav");
    check("pos-record play, at the system volume, on the chime's file",
          file_has(log, "play --volume-percent 40 ") && file_has(log, "/deskbuddy/visit.wav"));
    usleep(400000);
    check("once it has finished the next is played", db_sound_play(65) == 0);
    wait_for(log, "--volume-percent 65 ");
    check("and only the two asked for were started", lines(log) == 2);
    db_sound_stop();
    check("stop ends it, and another can start straight after", db_sound_play(40) == 0);
    db_sound_stop();
    check("nothing is left running", waitpid(-1, NULL, WNOHANG) <= 0);
    unsetenv("POCKETOS_RECORD_HELPER");
    unsetenv("POCKETOS_RUNTIME_DIR");
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", run);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", run);
    }
}

int main(void)
{
    test_seam();
    test_tone();
    test_helper();
    printf("db_sound_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
