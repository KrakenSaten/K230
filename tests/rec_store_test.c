/*
 * The Recorder's folder (apps/recorder/rec_store.h) in real temporary
 * directories: where it is, that it is private, what the list shows for our
 * recordings, foreign WAVs, broken files, unfinished ones, links and
 * folders, the order and the cap, the next name with and without a clock and
 * around collisions, what a delete may and may not remove, and the preset
 * surviving a restart.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketwav/pocketwav.h"
#include "rec_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int failed;
static int checks;
static char base[] = "/tmp/rec_store_test.XXXXXX";
static char dir[256];

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

static void wav(const char *name, unsigned rate, unsigned ch, uint32_t frames, long mtime)
{
    char path[512];
    uint8_t h[44];
    struct timeval tv[2];
    FILE *f;
    uint32_t i;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    pocketwav_header(h, rate, ch, frames * ch * 2);
    fwrite(h, 1, 44, f);
    for (i = 0; i < frames * ch; i++) {
        fputc(0, f);
        fputc(0, f);
    }
    fclose(f);
    tv[0].tv_sec = tv[1].tv_sec = mtime;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(path, tv);
}

static void raw(const char *name, const char *text, long mtime)
{
    char path[512];
    struct timeval tv[2];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
    tv[0].tv_sec = tv[1].tv_sec = mtime;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(path, tv);
}

static const struct rec_entry *find(const struct rec_list *l, const char *name)
{
    int i;

    for (i = 0; i < l->n; i++) {
        if (strcmp(l->e[i].name, name) == 0) {
            return &l->e[i];
        }
    }
    return NULL;
}

static int present(const char *name)
{
    char path[512];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return lstat(path, &st) == 0;
}

static void test_where(void)
{
    char out[256];
    struct stat st;

    unsetenv("POCKETOS_RECORDINGS_DIR");
    setenv("HOME", "/home/owner", 1);
    check("the folder is Recordings in the owner's home, where Files opens",
          rec_store_dir(out, sizeof(out)) == 0 && strcmp(out, "/home/owner/Recordings") == 0);
    unsetenv("HOME");
    check("with no home it is /root/Recordings", rec_store_dir(out, sizeof(out)) == 0 &&
                                                   strcmp(out, "/root/Recordings") == 0);
    setenv("HOME", "/", 1);
    check("a home of / is not used", rec_store_dir(out, sizeof(out)) == 0 && strcmp(out, "/root/Recordings") == 0);
    setenv("POCKETOS_RECORDINGS_DIR", "relative", 1);
    check("a relative override is ignored", rec_store_dir(out, sizeof(out)) == 0 &&
                                              strcmp(out, "/root/Recordings") == 0);
    snprintf(dir, sizeof(dir), "%s/home/Recordings", base);
    setenv("POCKETOS_RECORDINGS_DIR", dir, 1);
    check("the override is used", rec_store_dir(out, sizeof(out)) == 0 && strcmp(out, dir) == 0);
    check("a buffer too small is refused", rec_store_dir(out, 8) != 0);

    check("the folder is created with its parents", rec_store_ensure_dir(dir) == 0 && stat(dir, &st) == 0 &&
                                                        S_ISDIR(st.st_mode));
    check("private: 0700", (st.st_mode & 0777) == 0700);
    chmod(dir, 0777);
    check("a world-writable folder has group and other write taken off",
          rec_store_ensure_dir(dir) == 0 && stat(dir, &st) == 0 && (st.st_mode & 0777) == 0755);
    chmod(dir, 0700);
    {
        char file[512];

        snprintf(file, sizeof(file), "%s/afile", base);
        raw("../../afile", "x", 1);
        check("a path that is a file is an error", rec_store_ensure_dir(file) == -EEXIST ||
                                                       rec_store_ensure_dir(file) == -ENOTDIR);
    }
}

static void test_list(void)
{
    struct rec_list l;
    const struct rec_entry *e;
    char path[512];
    int fd;

    wav("REC-20260926-101500.wav", 16000, 1, 16000 * 42, 1000);
    wav("REC-0003-recovered.wav", 48000, 1, 4800, 3000);
    wav("Interview.WAV", 48000, 2, 48000, 2000);
    wav("cd.wav", 44100, 2, 100, 500);
    raw("broken.wav", "this is not a wav", 400);
    raw("notes.txt", "x", 300);
    raw(".hidden.wav", "x", 300);
    wav("REC-0009.wav.part", 16000, 1, 100, 5000);
    snprintf(path, sizeof(path), "%s/sub.wav", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/link.wav", dir);
    if (symlink("/etc/passwd", path) != 0) {
        printf("note symlink failed\n");
    }

    check("the folder lists", rec_store_list(dir, &l) == 0);
    check("six entries: the .wav files and the .part, not text, hidden files, folders or links",
          l.n == 6 && l.total == 6 && !find(&l, "notes.txt") && !find(&l, ".hidden.wav") &&
              !find(&l, "sub.wav") && !find(&l, "link.wav"));
    check("newest first", l.n == 6 && strcmp(l.e[0].name, "REC-0009.wav.part") == 0 &&
                              strcmp(l.e[1].name, "REC-0003-recovered.wav") == 0 &&
                              strcmp(l.e[5].name, "broken.wav") == 0);
    e = find(&l, "REC-20260926-101500.wav");
    check("a Voice recording: its length, size and rate from its header",
          e && e->status == REC_ENTRY_OK && e->ms == 42000 && e->rate == 16000 && e->channels == 1 &&
              e->bytes == 44 + 16000 * 42 * 2 && !e->recovered);
    e = find(&l, "REC-0003-recovered.wav");
    check("a repaired one says so", e && e->status == REC_ENTRY_OK && e->recovered && e->ms == 100);
    e = find(&l, "Interview.WAV");
    check("a renamed 48 kHz stereo file still plays", e && e->status == REC_ENTRY_OK && e->channels == 2 &&
                                                          e->ms == 1000);
    e = find(&l, "cd.wav");
    check("a 44.1 kHz WAV is shown as one this app does not play", e && e->status == REC_ENTRY_OTHER);
    e = find(&l, "broken.wav");
    check("a broken one is shown as unreadable", e && e->status == REC_ENTRY_UNREADABLE);
    e = find(&l, "REC-0009.wav.part");
    check("a .part is shown as unfinished", e && e->status == REC_ENTRY_PART);
    rec_store_list_free(&l);
    check("freeing leaves an empty list", l.n == 0 && l.e == NULL);

    /* Deleting. */
    snprintf(path, sizeof(path), "%s/REC-0009.wav.part", dir);
    fd = open(path, O_RDONLY);
    flock(fd, LOCK_EX);
    check("a .part a writer holds is not deleted", rec_store_delete(dir, "REC-0009.wav.part") == -EBUSY &&
                                                       present("REC-0009.wav.part"));
    close(fd);
    check("once the writer is gone it is", rec_store_delete(dir, "REC-0009.wav.part") == 0 &&
                                               !present("REC-0009.wav.part"));
    check("a recording is deleted", rec_store_delete(dir, "cd.wav") == 0 && !present("cd.wav"));
    check("a link is not followed or removed", rec_store_delete(dir, "link.wav") == -EINVAL &&
                                                   present("link.wav") && access("/etc/passwd", F_OK) == 0);
    check("a folder is not removed", rec_store_delete(dir, "sub.wav") == -EINVAL && present("sub.wav"));
    check("a name the list would not show is refused",
          rec_store_delete(dir, "notes.txt") == -EINVAL && rec_store_delete(dir, "../x.wav") == -EINVAL &&
              present("notes.txt"));
    check("a missing one is ENOENT", rec_store_delete(dir, "gone.wav") == -ENOENT);
    check("a missing folder lists as empty", rec_store_list("/nonexistent/Recordings", &l) == 0 && l.n == 0);
    rec_store_list_free(&l);
}

static void test_cap(void)
{
    char sub[128];
    char saved[256];
    struct rec_list l;
    int i;
    int newest_kept = 1;

    snprintf(saved, sizeof(saved), "%s", dir);
    snprintf(sub, sizeof(sub), "%s/many", base);
    mkdir(sub, 0700);
    snprintf(dir, sizeof(dir), "%s", sub);
    for (i = 0; i < REC_LIST_MAX + 20; i++) {
        char name[32];

        snprintf(name, sizeof(name), "REC-%04d.wav", i + 1);
        wav(name, 16000, 1, 10, 10000 + i);
    }
    rec_store_list(dir, &l);
    for (i = 0; i < l.n; i++) {
        newest_kept &= l.e[i].mtime >= 10000 + 20;
    }
    check("past the cap the newest are kept and the rest counted",
          l.n == REC_LIST_MAX && l.total == REC_LIST_MAX + 20 && newest_kept &&
              strcmp(l.e[0].name, "REC-0120.wav") == 0);
    rec_store_list_free(&l);
    snprintf(dir, sizeof(dir), "%s", saved);
}

static void test_names(void)
{
    char name[REC_NAME_MAX];
    int64_t t = 1790000000; /* 2026-09-21 14:13:20 UTC */

    check("with a clock the name is the local time",
          rec_store_next_name(dir, t, true, name, sizeof(name)) == 0 &&
              strcmp(name, "REC-20260921-141320.wav") == 0);
    wav("REC-20260921-141320.wav", 16000, 1, 1, 1);
    check("a second recording in the same second is -2",
          rec_store_next_name(dir, t, true, name, sizeof(name)) == 0 &&
              strcmp(name, "REC-20260921-141320-2.wav") == 0);
    wav("REC-20260921-141320-2.wav.part", 16000, 1, 1, 1);
    check("a name whose .part exists is taken too",
          rec_store_next_name(dir, t, true, name, sizeof(name)) == 0 &&
              strcmp(name, "REC-20260921-141320-3.wav") == 0);
    check("without a clock the name continues the sequence (REC-0003-recovered is 3)",
          rec_store_next_name(dir, 0, false, name, sizeof(name)) == 0 && strcmp(name, "REC-0004.wav") == 0);
    wav("REC-0011.wav.part", 16000, 1, 1, 1);
    check("an unfinished recording's number is spoken for",
          rec_store_next_name(dir, 0, false, name, sizeof(name)) == 0 && strcmp(name, "REC-0012.wav") == 0);
    check("an empty or missing folder starts at 0001",
          rec_store_next_name("/nonexistent/x", 0, false, name, sizeof(name)) == 0 &&
              strcmp(name, "REC-0001.wav") == 0);
}

static void test_prefs(void)
{
    char state[512];
    char path[600];
    enum rec_preset p;
    FILE *f;
    struct stat st;

    snprintf(state, sizeof(state), "%s/state", base);
    setenv("POCKETOS_STATE_DIR", state, 1);
    check("nothing stored: Voice", rec_store_load_preset(&p) == 1 && p == REC_PRESET_VOICE);
    check("Standard is stored", rec_store_save_preset(REC_PRESET_STANDARD) == 0);
    check("and comes back after a restart", rec_store_load_preset(&p) == 0 && p == REC_PRESET_STANDARD);
    snprintf(path, sizeof(path), "%s/recorder/recorder.conf", state);
    check("in a private file", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    f = fopen(path, "w");
    fputs("garbage\npreset=standard\n", f);
    fclose(f);
    check("a file that is not ours gives Voice", rec_store_load_preset(&p) == -1 && p == REC_PRESET_VOICE);
    f = fopen(path, "w");
    fputs("recorder-prefs 1\nfuture=1\npreset=voice\n", f);
    fclose(f);
    check("unknown keys are skipped", rec_store_load_preset(&p) == 0 && p == REC_PRESET_VOICE);
    check("the presets are 16 and 48 kHz", rec_preset_rate(REC_PRESET_VOICE) == 16000 &&
                                              rec_preset_rate(REC_PRESET_STANDARD) == 48000);
}

int main(void)
{
    char cmd[80];

    umask(022);
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }
    test_where();
    test_list();
    test_cap();
    test_names();
    test_prefs();
    check("free space is reported for a real folder", rec_store_free(dir) > 0 &&
                                                          rec_store_free("/nonexistent/x") == -ENOENT);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", base);
    }
    printf("rec_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
