/*
 * Wave's files, against a temporary $POCKETOS_STATE_DIR and
 * $POCKETOS_RUNTIME_DIR: the preset and the history survive a restart; the
 * history file stays bounded however much is added; a clear removes it;
 * damaged, foreign and oversized files are refused or trimmed rather than
 * trusted; files are private; a write leaves no temporary behind; and the
 * capture lives only in the runtime directory and is removed.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static char state[64];
static char run[64];

static void path_in(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/wave/%s", state, name);
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int mode_of(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

static void put(const char *name, const char *data, size_t len)
{
    char path[256];
    FILE *f;

    path_in(path, sizeof(path), name);
    f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}
/* A string literal's bytes without its terminator, counted by the compiler. */
#define PUT_TEXT(name, lit) put((name), (lit), sizeof(lit) - 1)

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void remove_tree(const char *top)
{
    DIR *d = opendir(top);
    struct dirent *e;

    while (d && (e = readdir(d)) != NULL) {
        char p[512];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        snprintf(p, sizeof(p), "%s/%s", top, e->d_name);
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p);
        } else {
            unlink(p);
        }
    }
    if (d) {
        closedir(d);
    }
    rmdir(top);
}

int main(void)
{
    struct wave_prefs p;
    struct wave_history h;
    struct wave_history back;
    char path[256];
    char dir[256];
    int skipped;
    int i;

    snprintf(state, sizeof(state), "/tmp/wave-store-XXXXXX");
    snprintf(run, sizeof(run), "/tmp/wave-run-XXXXXX");
    if (!mkdtemp(state) || !mkdtemp(run)) {
        return 2;
    }
    setenv("POCKETOS_STATE_DIR", state, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);

    /* ---- nothing stored yet ------------------------------------------------ */
    check("no preferences yet is 1, not an error", wave_store_load_prefs(&p) == 1 && !p.preset[0]);
    check("no history yet is 1, not an error",
          wave_store_load_history(&h, &skipped) == 1 && wave_history_count(&h) == 0);
    check("reading creates nothing", count_entries(state) == 0);

    /* ---- preferences ------------------------------------------------------- */
    snprintf(p.preset, sizeof(p.preset), "robust");
    check("the preset is saved", wave_store_save_prefs(&p) == 0);
    memset(&p, 0, sizeof(p));
    check("and read back after a restart", wave_store_load_prefs(&p) == 0 &&
                                               strcmp(p.preset, "robust") == 0);
    path_in(path, sizeof(path), WAVE_PREFS_FILE);
    check("the preferences file is private (0600)", mode_of(path) == 0600);
    wave_store_dir(dir, sizeof(dir));
    check("in a private directory (0700)", mode_of(dir) == 0700);
    PUT_TEXT(WAVE_PREFS_FILE, "wave-prefs 1\nfuture=1\npreset=quick\n");
    check("unknown keys are ignored", wave_store_load_prefs(&p) == 0 && strcmp(p.preset, "quick") == 0);
    PUT_TEXT(WAVE_PREFS_FILE, "clock 1\npreset=quick\n");
    check("a foreign file is refused", wave_store_load_prefs(&p) == -1 && !p.preset[0]);
    PUT_TEXT(WAVE_PREFS_FILE, "wave-prefs 1\npreset=aaaaaaaaaaaaaaaaaaaaaaaa\n");
    check("an overlong preset id is ignored", wave_store_load_prefs(&p) == 0 && !p.preset[0]);

    /* ---- history ------------------------------------------------------------ */
    wave_history_init(&h);
    wave_history_add_tx(&h, "standard", "HELLO", 5, WAVE_RESULT_OK, 1, 1758900000);
    wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1758900005, 1, 10000);
    wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1758900006, 2, 10000);
    check("the history is saved", wave_store_save_history(&h) == 0);
    path_in(path, sizeof(path), WAVE_HISTORY_FILE);
    check("the history file is private (0600)", mode_of(path) == 0600);
    check("no temporary file is left", count_entries(dir) == 2);
    check("it survives a restart", wave_store_load_history(&back, &skipped) == 0 &&
                                       wave_history_count(&back) == 2 && skipped == 0 &&
                                       strcmp(wave_history_at(&back, 0)->data, "DOORS") == 0 &&
                                       wave_history_at(&back, 0)->count == 2 &&
                                       wave_history_at(&back, 1)->dir == WAVE_DIR_TX);

    for (i = 0; i < 500; i++) {
        char msg[64];

        snprintf(msg, sizeof(msg), "message number %d with some length to it", i);
        wave_history_add_tx(&h, "robust", msg, strlen(msg), WAVE_RESULT_OK, 2, 1758900000 + i);
        wave_store_save_history(&h);
    }
    check("500 more sends keep the file bounded", file_size(path) > 0 &&
                                                     file_size(path) < WAVE_HISTORY_TEXT_MAX);
    check("to WAVE_HISTORY_MAX entries", wave_store_load_history(&back, NULL) == 0 &&
                                             wave_history_count(&back) == WAVE_HISTORY_MAX);
    check("the newest", strcmp(wave_history_at(&back, 0)->data,
                               "message number 499 with some length to it") == 0);

    wave_history_clear(&h);
    check("a cleared history is saved by removing the file",
          wave_store_save_history(&h) == 0 && file_size(path) == -1);
    check("and reads back empty", wave_store_load_history(&back, NULL) == 1 &&
                                      wave_history_count(&back) == 0);
    check("clearing an absent file is fine", wave_store_save_history(&h) == 0);

    PUT_TEXT(WAVE_HISTORY_FILE, "wave-history 1\n1 5 T ok 1 - standard 41\ngarbage line\n");
    check("a damaged line costs that line only", wave_store_load_history(&back, &skipped) == 0 &&
                                                     wave_history_count(&back) == 1 && skipped == 1);
    PUT_TEXT(WAVE_HISTORY_FILE, "PK\x03\x04 not ours");
    check("a foreign file is refused, empty history", wave_store_load_history(&back, NULL) == -1 &&
                                                          wave_history_count(&back) == 0);
    {
        size_t big = WAVE_STORE_FILE_MAX + 10;
        char *junk = malloc(big);

        memset(junk, 'x', big);
        memcpy(junk, "wave-history 1\n", 15);
        put(WAVE_HISTORY_FILE, junk, big);
        free(junk);
        check("a file larger than any history is refused",
              wave_store_load_history(&back, NULL) == -1);
    }

    /* ---- the capture ------------------------------------------------------- */
    check("the capture path is in the runtime directory",
          wave_store_capture_path(path, sizeof(path)) == 0 && strncmp(path, run, strlen(run)) == 0 &&
              strstr(path, "/wave/" WAVE_CAPTURE_FILE));
    snprintf(dir, sizeof(dir), "%s/wave", run);
    check("its directory is private (0700)", mode_of(dir) == 0700);
    {
        FILE *f = fopen(path, "wb");

        if (f) {
            fputs("RIFF", f);
            fclose(f);
        }
    }
    wave_store_capture_remove();
    check("remove removes it", file_size(path) == -1);
    wave_store_capture_remove();
    check("removing again is harmless", file_size(path) == -1);
    check("no audio ever lands in the state directory", ({
              char w[256];
              snprintf(w, sizeof(w), "%s/wave/" WAVE_CAPTURE_FILE, state);
              file_size(w) == -1;
          }));

    /* ---- failure ------------------------------------------------------------ */
    setenv("POCKETOS_STATE_DIR", "/proc/wave-cannot-write", 1);
    snprintf(p.preset, sizeof(p.preset), "quick");
    check("a store that cannot be written says so", wave_store_save_prefs(&p) == -1);
    wave_history_add_tx(&h, "standard", "x", 1, WAVE_RESULT_OK, 1, 1);
    check("for the history too", wave_store_save_history(&h) == -1);

    remove_tree(state);
    remove_tree(run);
    printf("wave_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
