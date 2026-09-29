/*
 * Where the MP3 app finds music (apps/mp3/mp3_library.h), against real
 * temporary folders: the places and their environment, one folder read
 * (what counts as audio, hidden names, links, the order, the sizes), the
 * list's two boundaries (entries kept, entries looked at), the path rules
 * that keep browsing inside a place, the remembered folder, and the scanner
 * thread - including storage that hangs, which must never hold up the
 * caller, and scans that are abandoned.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "mp3_library.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void (*mp3_library_read_hook)(const char *dir);

static int failed;
static int checks;
static char tmp[] = "/tmp/mp3_library_test.XXXXXX";

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

static char *at(const char *fmt, const char *a)
{
    static char buf[8][512];
    static int k;
    char rel[256];

    k = (k + 1) % 8;
    snprintf(rel, sizeof(rel), fmt, a ? a : "");
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", tmp, rel);
    return buf[k];
}

static void touch(const char *path, int bytes)
{
    FILE *f = fopen(path, "wb");
    int i;

    for (i = 0; f && i < bytes; i++) {
        fputc('x', f);
    }
    if (f) {
        fclose(f);
    }
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static const struct mp3_entry *find(const struct mp3_list *l, const char *name)
{
    int k;

    for (k = 0; k < l->n; k++) {
        if (strcmp(l->e[k].name, name) == 0) {
            return &l->e[k];
        }
    }
    return NULL;
}

/* ---- places ------------------------------------------------------------------ */

static void places(void)
{
    struct mp3_place p[MP3_PLACES_MAX];
    struct mp3_list *l = malloc(sizeof(*l));
    struct stat st;
    int n;

    setenv("HOME", at("home", NULL), 1);
    unsetenv("POCKETOS_MUSIC_DIR");
    unsetenv("POCKETOS_RECORDINGS_DIR");
    {
        char roots[1200];

        snprintf(roots, sizeof(roots), "%s:%s::%s/", at("usb", NULL), at("sd", NULL), at("usb", NULL));
        setenv("POCKETOS_MP3_MEDIA_ROOTS", roots, 1);
    }
    mkdir(at("home", NULL), 0755);
    mkdir(at("usb", NULL), 0755);
    n = mp3_library_places(p);
    check("the places: Music, Home, Recordings, then each media root once (a trailing slash is the same folder)",
          n == 5 && strcmp(p[0].label, "Music") == 0 && strcmp(p[0].path, at("home/Music", NULL)) == 0 &&
              strcmp(p[1].label, "Home") == 0 && strcmp(p[1].path, at("home", NULL)) == 0 &&
              strcmp(p[2].label, "Recordings") == 0 && strcmp(p[2].path, at("home/Recordings", NULL)) == 0 &&
              strcmp(p[3].path, at("usb", NULL)) == 0 && strcmp(p[4].path, at("sd", NULL)) == 0);

    mp3_library_read("", l);
    check("reading the places makes the Music folder (0755)",
          stat(at("home/Music", NULL), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0755);
    check("and lists only the places that are folders: Music, Home, the stick",
          l->err == 0 && l->n == 3 && l->e[0].kind == MP3_ENTRY_PLACE && l->e[0].place == 0 &&
              l->e[1].place == 1 && l->e[2].place == 3 && l->dir[0] == '\0' && l->tracks == 0);
    mkdir(at("home/Recordings", NULL), 0700);
    mp3_library_read("", l);
    check("Recorder's folder shows once it exists", l->n == 4 && l->e[2].place == 2 &&
                                                    strcmp(l->e[2].name, "Recordings") == 0);

    setenv("POCKETOS_MUSIC_DIR", at("elsewhere", NULL), 1);
    setenv("POCKETOS_MP3_MEDIA_ROOTS", "", 1);
    n = mp3_library_places(p);
    check("POCKETOS_MUSIC_DIR moves Music; an empty media list has no media places",
          n == 3 && strcmp(p[0].path, at("elsewhere", NULL)) == 0);
    unsetenv("POCKETOS_MUSIC_DIR");
    unsetenv("POCKETOS_MP3_MEDIA_ROOTS");
    n = mp3_library_places(p);
    check("by default the media roots are /mnt and /media",
          n == 5 && strcmp(p[3].path, "/mnt") == 0 && strcmp(p[4].path, "/media") == 0);
    setenv("POCKETOS_MP3_MEDIA_ROOTS", "", 1);
    free(l);
}

/* ---- one folder ---------------------------------------------------------------- */

static void folder(void)
{
    struct mp3_list *l = malloc(sizeof(*l));
    const struct mp3_entry *e;
    char m[512];
    int k;
    int sorted = 1;

    snprintf(m, sizeof(m), "%s", at("home/Music", NULL));
    touch(at("home/Music/b song.mp3", NULL), 1234);
    touch(at("home/Music/A song.MP3", NULL), 10);
    touch(at("home/Music/c.flac", NULL), 5);
    touch(at("home/Music/d.WAV", NULL), 0);
    touch(at("home/Music/notes.txt", NULL), 5);
    touch(at("home/Music/mp3", NULL), 5);
    touch(at("home/Music/.hidden.mp3", NULL), 5);
    mkdir(at("home/Music/Zed", NULL), 0755);
    mkdir(at("home/Music/albums", NULL), 0755);
    mkdir(at("home/Music/.git", NULL), 0755);
    check("links made", symlink(at("home/Music/albums", NULL), at("home/Music/linked folder", NULL)) == 0 &&
                            symlink(at("home/Music/b song.mp3", NULL), at("home/Music/linked.mp3", NULL)) == 0 &&
                            symlink(at("home/Music/gone.mp3", NULL), at("home/Music/dangling.mp3", NULL)) == 0);

    mp3_library_read(m, l);
    check("a folder reads", l->err == 0 && strcmp(l->dir, m) == 0);
    check("three folders (one of them a link), five audio files (one a link)",
          l->folders == 3 && l->tracks == 5 && l->n == 8 && !l->truncated);
    check("text, a name without an extension, hidden names and a dangling link are left out",
          !find(l, "notes.txt") && !find(l, "mp3") && !find(l, ".hidden.mp3") && !find(l, ".git") &&
              !find(l, "dangling.mp3"));
    for (k = 1; k < l->n; k++) {
        sorted &= mp3_entry_compare(&l->e[k - 1], &l->e[k]) < 0;
    }
    check("folders first, then files, each by name without regard to case",
          sorted && l->e[0].kind == MP3_ENTRY_FOLDER && strcmp(l->e[0].name, "albums") == 0 &&
              strcmp(l->e[2].name, "Zed") == 0 && l->e[3].kind == MP3_ENTRY_TRACK &&
              strcmp(l->e[3].name, "A song.MP3") == 0 && strcmp(l->e[4].name, "b song.mp3") == 0);
    e = find(l, "b song.mp3");
    check("a file's size is kept", e && e->bytes == 1234);
    e = find(l, "d.WAV");
    check("an empty file is listed, with size 0 (playing it says why not)", e && e->bytes == 0);
    e = find(l, "linked.mp3");
    check("a link is judged by what it points to", e && e->kind == MP3_ENTRY_TRACK && e->bytes == 1234);

    mp3_library_read(at("home/Music/nothing", NULL), l);
    check("a missing folder says ENOENT and lists nothing", l->err == ENOENT && l->n == 0);
    mp3_library_read(at("home/Music/c.flac", NULL), l);
    check("a file is not a folder: ENOTDIR", l->err == ENOTDIR && l->n == 0);
    mp3_library_read("relative/path", l);
    check("a relative path is refused", l->err == EINVAL);

    for (k = 0; k < 10; k++) {
        check(k == 0 ? "the audio extensions, any case" : "  ...",
              mp3_library_is_audio((const char *[]){ "a.mp3", "a.Mp3", "a.wav", "a.flac", "a.ogg", "a.oga",
                                                      "a.opus", "a.m4a", "a.AAC", "x.y.mp3" }[k]));
    }
    check("and not these", !mp3_library_is_audio("a.txt") && !mp3_library_is_audio(".mp3") &&
                               !mp3_library_is_audio("mp3") && !mp3_library_is_audio("a.mp3.part") &&
                               !mp3_library_is_audio("a.mp4"));
    free(l);
}

static void boundaries(void)
{
    struct mp3_list *l = malloc(sizeof(*l));
    char name[64];
    int k;

    mkdir(at("big", NULL), 0755);
    for (k = 0; k < MP3_LIST_MAX + 5; k++) {
        snprintf(name, sizeof(name), "big/t%03d.mp3", k);
        touch(at("%s", name), 1);
    }
    mp3_library_read(at("big", NULL), l);
    check("205 audio files: the first 200 are kept, and it says there were more",
          l->n == MP3_LIST_MAX && l->tracks == MP3_LIST_MAX && l->truncated && l->err == 0);
    mkdir(at("huge", NULL), 0755);
    for (k = 0; k < MP3_SCAN_MAX + 10; k++) {
        snprintf(name, sizeof(name), "huge/n%05d.txt", k);
        touch(at("%s", name), 0);
    }
    touch(at("huge/zzz.mp3", NULL), 1);
    mp3_library_read(at("huge", NULL), l);
    check("a folder of more than 4096 entries is looked at no further than that, and says so",
          l->truncated && l->n <= 1 && l->err == 0);
    free(l);
}

/* ---- paths ---------------------------------------------------------------------- */

static void paths(void)
{
    char out[MP3_PATH_MAX];
    char longname[MP3_PATH_MAX];

    check("join", mp3_library_join(out, "/a/b", "c.mp3") == 0 && strcmp(out, "/a/b/c.mp3") == 0);
    check("join at the root", mp3_library_join(out, "/", "mnt") == 0 && strcmp(out, "/mnt") == 0);
    check("join refuses what is not a plain name",
          mp3_library_join(out, "/a", "..") != 0 && mp3_library_join(out, "/a", ".") != 0 &&
              mp3_library_join(out, "/a", "b/c") != 0 && mp3_library_join(out, "/a", "") != 0 &&
              mp3_library_join(out, "rel", "x") != 0);
    memset(longname, 'n', 255);
    longname[255] = '\0';
    {
        char deep[MP3_PATH_MAX];

        memset(deep, 'd', sizeof(deep) - 1);
        deep[0] = '/';
        deep[sizeof(deep) - 1] = '\0';
        deep[900] = '\0';
        check("join refuses a path that would not fit", mp3_library_join(out, deep, longname) != 0);
    }
    check("within: the root itself and below it",
          mp3_library_within("/root/Music", "/root/Music") && mp3_library_within("/root/Music", "/root/Music/a/b"));
    check("within: not a neighbour that shares a prefix, not above",
          !mp3_library_within("/root/Music", "/root/Music2") && !mp3_library_within("/root/Music", "/root"));
    check("within: everything is inside /", mp3_library_within("/", "/mnt/x"));
    check("parent inside the place", mp3_library_parent(out, "/root/Music", "/root/Music/a/b") &&
                                         strcmp(out, "/root/Music/a") == 0);
    check("parent reaches the place itself", mp3_library_parent(out, "/root/Music", "/root/Music/a") &&
                                                 strcmp(out, "/root/Music") == 0);
    check("but never above it", !mp3_library_parent(out, "/root/Music", "/root/Music") &&
                                    !mp3_library_parent(out, "/root/Music", "/etc"));
    check("a place at / goes to /", mp3_library_parent(out, "/", "/mnt") && strcmp(out, "/") == 0);
    mp3_copy(out, 5, "abc\xc3\xa9");
    check("copy never cuts a character", strcmp(out, "abc") == 0);
    mp3_copy(out, 5, "ab");
    check("copy keeps what fits", strcmp(out, "ab") == 0);
}

/* ---- the last folder --------------------------------------------------------------- */

static void last(void)
{
    char root[MP3_PATH_MAX];
    char dir[MP3_PATH_MAX];
    FILE *f;

    setenv("POCKETOS_STATE_DIR", at("state", NULL), 1);
    check("nothing remembered at first", mp3_library_load_last(root, dir) != 0);
    check("a folder is remembered", mp3_library_save_last("/root/Music", "/root/Music/Albums") == 0);
    check("and read back", mp3_library_load_last(root, dir) == 0 && strcmp(root, "/root/Music") == 0 &&
                               strcmp(dir, "/root/Music/Albums") == 0);
    check("no temporary file is left", access(at("state/mp3/folder.tmp", NULL), F_OK) != 0);
    mp3_library_save_last("", "");
    check("the places are remembered as nothing to restore", mp3_library_load_last(root, dir) != 0);
    f = fopen(at("state/mp3/folder", NULL), "w");
    fputs("/root/Music\n/etc\n", f);
    fclose(f);
    check("a folder outside its place is not believed", mp3_library_load_last(root, dir) != 0);
    f = fopen(at("state/mp3/folder", NULL), "w");
    fputs("/root/Music\n", f);
    fclose(f);
    check("half a file is not believed", mp3_library_load_last(root, dir) != 0);
    check("a newline in a name is refused", mp3_library_save_last("/root/Music", "/root/Music/a\nb") != 0);
}

/* ---- the scanner --------------------------------------------------------------------- */

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open;

/* Storage that hangs: any folder named "*hang*" blocks until the gate opens. */
static void hang_hook(const char *dir)
{
    if (!strstr(dir, "hang")) {
        return;
    }
    pthread_mutex_lock(&gate_mu);
    while (!gate_open) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
}

static struct mp3_list *wait_list(struct mp3_scanner *sc, int ms)
{
    struct mp3_list *l = NULL;
    int waited;

    for (waited = 0; waited < ms; waited += 5) {
        if (mp3_scanner_poll(sc, &l)) {
            return l;
        }
        nap(5);
    }
    return NULL;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void scanner(void)
{
    struct mp3_scanner sc;
    struct mp3_scanner stuck[MP3_SCAN_STUCK_MAX];
    struct mp3_list *l;
    int64_t t0;
    int k;
    int waited;

    mp3_library_read_hook = hang_hook;
    mp3_scanner_init(&sc);
    check("an idle scanner has nothing", !mp3_scanner_busy(&sc) && !wait_list(&sc, 20));
    check("a scan starts", mp3_scanner_start(&sc, at("home/Music", NULL)) == 0 && mp3_scanner_busy(&sc));
    l = wait_list(&sc, 2000);
    check("and delivers the folder, once", l && l->tracks == 5 && !mp3_scanner_busy(&sc) && !wait_list(&sc, 20));
    free(l);

    mp3_scanner_start(&sc, at("big", NULL));
    mp3_scanner_start(&sc, at("home/Music", NULL));
    l = wait_list(&sc, 2000);
    check("a second scan replaces the first: only its folder arrives",
          l && strcmp(l->dir, at("home/Music", NULL)) == 0 && !wait_list(&sc, 50));
    free(l);

    mkdir(at("hang", NULL), 0755);
    t0 = mono_ms();
    mp3_scanner_start(&sc, at("hang", NULL));
    check("hanging storage: starting the scan returns at once", mono_ms() - t0 < 100);
    check("  it is busy and has nothing", mp3_scanner_busy(&sc) && !wait_list(&sc, 100));
    t0 = mono_ms();
    mp3_scanner_close(&sc);
    check("  and closing it (the app goes) returns at once", mono_ms() - t0 < 50 && !mp3_scanner_busy(&sc));
    check("  one thread is left waiting on the storage", mp3_scanner_threads() == 1);
    for (k = 0; k < MP3_SCAN_STUCK_MAX; k++) {
        mp3_scanner_init(&stuck[k]);
    }
    for (k = 0; k < MP3_SCAN_STUCK_MAX - 1; k++) {
        mp3_scanner_start(&stuck[k], at("hang", NULL));
    }
    check("  four threads may wait on it", mp3_scanner_threads() == MP3_SCAN_STUCK_MAX);
    check("  a fifth scan is refused rather than piling up",
          mp3_scanner_start(&stuck[MP3_SCAN_STUCK_MAX - 1], at("home/Music", NULL)) != 0 &&
              !mp3_scanner_busy(&stuck[MP3_SCAN_STUCK_MAX - 1]));
    pthread_mutex_lock(&gate_mu);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
    l = wait_list(&stuck[0], 2000);
    check("when the storage answers, a scan still wanted delivers", l && l->err == 0 && strcmp(l->dir, at("hang", NULL)) == 0);
    free(l);
    for (k = 1; k < MP3_SCAN_STUCK_MAX; k++) {
        mp3_scanner_close(&stuck[k]);
    }
    for (waited = 0; waited < 2000 && mp3_scanner_threads() > 0; waited += 5) {
        nap(5);
    }
    check("  and every thread ends, the abandoned ones freeing themselves", mp3_scanner_threads() == 0);
    check("  scanning works again", mp3_scanner_start(&sc, at("home", NULL)) == 0 && (l = wait_list(&sc, 2000)) != NULL &&
                                        find(l, "Music") != NULL);
    free(l);
    mp3_scanner_close(&sc);
    mp3_library_read_hook = NULL;
}

int main(void)
{
    char cmd[128];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!mkdtemp(tmp)) {
        perror("mkdtemp");
        return 2;
    }
    places();
    folder();
    boundaries();
    paths();
    last();
    scanner();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", tmp);
    }
    printf("mp3_library_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
