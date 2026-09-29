/*
 * Places, one folder at a time, and the scanner thread. See mp3_library.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "mp3_library.h"

#include "pocketpaths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* A test seam, NULL in use: run by the scan thread before it reads, so a
 * test can make storage hang (tests/mp3_library_test.c). */
void (*mp3_library_read_hook)(const char *dir);

/* ---- places --------------------------------------------------------------- */

static const char *home_dir(void)
{
    const char *home = getenv("HOME");

    return home && home[0] == '/' && strcmp(home, "/") != 0 ? home : "/root";
}

static int add_place(struct mp3_place *out, int n, const char *label, const char *path)
{
    char clean[MP3_PATH_MAX];
    size_t l;
    int k;

    if (n >= MP3_PLACES_MAX || !path || path[0] != '/' || strlen(path) >= MP3_PATH_MAX) {
        return n;
    }
    /* No trailing slash, so "within" and "parent" compare plainly. */
    snprintf(clean, sizeof(clean), "%s", path);
    l = strlen(clean);
    while (l > 1 && clean[l - 1] == '/') {
        clean[--l] = '\0';
    }
    for (k = 0; k < n; k++) {
        if (strcmp(out[k].path, clean) == 0) {
            return n; /* the same folder twice is shown once */
        }
    }
    mp3_copy(out[n].label, sizeof(out[n].label), label);
    memcpy(out[n].path, clean, l + 1);
    return n + 1;
}

int mp3_library_places(struct mp3_place out[MP3_PLACES_MAX])
{
    char buf[MP3_PATH_MAX];
    const char *env;
    const char *media;
    int n = 0;

    env = getenv("POCKETOS_MUSIC_DIR");
    if (env && env[0] == '/') {
        n = add_place(out, n, "Music", env);
    } else if (snprintf(buf, sizeof(buf), "%s/Music", home_dir()) < (int)sizeof(buf)) {
        n = add_place(out, n, "Music", buf);
    }
    n = add_place(out, n, "Home", home_dir());
    env = getenv("POCKETOS_RECORDINGS_DIR");
    if (env && env[0] == '/') {
        n = add_place(out, n, "Recordings", env);
    } else if (snprintf(buf, sizeof(buf), "%s/Recordings", home_dir()) < (int)sizeof(buf)) {
        n = add_place(out, n, "Recordings", buf);
    }
    media = getenv("POCKETOS_MP3_MEDIA_ROOTS");
    if (!media) {
        media = "/mnt:/media";
    }
    while (*media) {
        const char *end = strchr(media, ':');
        size_t len = end ? (size_t)(end - media) : strlen(media);

        if (len > 0 && len < sizeof(buf)) {
            memcpy(buf, media, len);
            buf[len] = '\0';
            n = add_place(out, n, buf, buf);
        }
        media = end ? end + 1 : media + len;
    }
    return n;
}

/* ---- names and paths ------------------------------------------------------ */

int mp3_library_is_audio(const char *name)
{
    static const char *const ext[] = { "mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac" };
    const char *dot = strrchr(name, '.');
    size_t i;

    if (!dot || dot == name) {
        return 0;
    }
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        if (strcasecmp(dot + 1, ext[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int mp3_entry_compare(const struct mp3_entry *a, const struct mp3_entry *b)
{
    int c;

    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->kind == MP3_ENTRY_PLACE) {
        return (int)a->place - (int)b->place;
    }
    c = strcasecmp(a->name, b->name);
    return c ? c : strcmp(a->name, b->name);
}

static int cmp_entries(const void *a, const void *b)
{
    return mp3_entry_compare(a, b);
}

void mp3_copy(char *dst, size_t n, const char *src)
{
    size_t len;

    if (!dst || n == 0) {
        return;
    }
    len = src ? strnlen(src, n) : 0;
    if (len >= n) {
        len = n - 1;
        /* Back off continuation bytes so a character is kept whole or not
         * at all. */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    if (len) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static int plain_name(const char *name)
{
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !strchr(name, '/');
}

int mp3_library_join(char *out, const char *dir, const char *name)
{
    int w;

    if (!plain_name(name) || !dir || dir[0] != '/') {
        return -1;
    }
    w = snprintf(out, MP3_PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", name);
    return w < 0 || w >= MP3_PATH_MAX ? -1 : 0;
}

int mp3_library_within(const char *root, const char *dir)
{
    size_t n = strlen(root);

    if (!root[0] || !dir) {
        return 0;
    }
    if (strcmp(root, "/") == 0) {
        return dir[0] == '/';
    }
    return strncmp(root, dir, n) == 0 && (dir[n] == '\0' || dir[n] == '/');
}

int mp3_library_parent(char *out, const char *root, const char *dir)
{
    const char *slash;

    if (!mp3_library_within(root, dir) || strcmp(root, dir) == 0) {
        return 0;
    }
    slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    if (slash == dir) {
        snprintf(out, MP3_PATH_MAX, "/");
    } else {
        snprintf(out, MP3_PATH_MAX, "%.*s", (int)(slash - dir), dir);
    }
    return mp3_library_within(root, out);
}

/* ---- reading ---------------------------------------------------------------- */

static void read_places(struct mp3_list *out)
{
    struct mp3_place p[MP3_PLACES_MAX];
    int n = mp3_library_places(p);
    int k;

    /* The Music folder is the one place Doors makes for music. */
    if (n > 0 && strcmp(p[0].label, "Music") == 0) {
        pocketos_mkdir_p(p[0].path, 0755);
    }
    for (k = 0; k < n && out->n < MP3_LIST_MAX; k++) {
        struct stat st;

        if (stat(p[k].path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        out->e[out->n].kind = MP3_ENTRY_PLACE;
        out->e[out->n].place = (uint8_t)k;
        mp3_copy(out->e[out->n].name, sizeof(out->e[out->n].name), p[k].label);
        out->n++;
        out->folders++;
    }
}

void mp3_library_read(const char *dir, struct mp3_list *out)
{
    DIR *d;
    struct dirent *de;
    int looked = 0;

    memset(out, 0, sizeof(*out));
    snprintf(out->dir, sizeof(out->dir), "%s", dir ? dir : "");
    if (!dir || !dir[0]) {
        read_places(out);
        return;
    }
    if (dir[0] != '/' || strlen(dir) >= MP3_PATH_MAX) {
        out->err = EINVAL;
        return;
    }
    d = opendir(dir);
    if (!d) {
        out->err = errno ? errno : EIO;
        return;
    }
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        struct mp3_entry *e;
        int is_dir;
        int is_reg;
        struct stat st;

        if (++looked > MP3_SCAN_MAX) {
            out->truncated = 1;
            break;
        }
        if (de->d_name[0] == '.' || strlen(de->d_name) >= MP3_NAME_MAX) {
            continue;
        }
        is_dir = de->d_type == DT_DIR;
        is_reg = de->d_type == DT_REG;
        if (!is_dir && !is_reg) {
            if (de->d_type != DT_LNK && de->d_type != DT_UNKNOWN) {
                continue;
            }
            /* A link, or a filesystem that does not say: follow it. */
            if (fstatat(dirfd(d), de->d_name, &st, 0) != 0) {
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
            is_reg = S_ISREG(st.st_mode);
        }
        if (!is_dir && !(is_reg && mp3_library_is_audio(de->d_name))) {
            continue;
        }
        if (out->n >= MP3_LIST_MAX) {
            out->truncated = 1;
            continue;
        }
        e = &out->e[out->n];
        e->kind = is_dir ? MP3_ENTRY_FOLDER : MP3_ENTRY_TRACK;
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->bytes = -1;
        if (!is_dir && fstatat(dirfd(d), de->d_name, &st, 0) == 0) {
            e->bytes = (int64_t)st.st_size;
        }
        out->n++;
        if (is_dir) {
            out->folders++;
        } else {
            out->tracks++;
        }
        errno = 0;
    }
    if (!de && errno != 0 && out->n == 0) {
        out->err = errno;
    }
    closedir(d);
    qsort(out->e, (size_t)out->n, sizeof(out->e[0]), cmp_entries);
}

/* ---- the last folder ---------------------------------------------------------- */

static int last_path(char *out, size_t n, int tmp)
{
    int w = snprintf(out, n, "%s/mp3/folder%s", pocketos_state_dir(), tmp ? ".tmp" : "");

    return w < 0 || (size_t)w >= n ? -1 : 0;
}

static void chomp(char *s)
{
    size_t l = strlen(s);

    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) {
        s[--l] = '\0';
    }
}

int mp3_library_load_last(char root[MP3_PATH_MAX], char dir[MP3_PATH_MAX])
{
    char path[MP3_PATH_MAX + 32];
    FILE *f;
    int ok;

    if (last_path(path, sizeof(path), 0) != 0 || !(f = fopen(path, "r"))) {
        return -1;
    }
    ok = fgets(root, MP3_PATH_MAX, f) != NULL && fgets(dir, MP3_PATH_MAX, f) != NULL;
    fclose(f);
    if (!ok) {
        return -1;
    }
    chomp(root);
    chomp(dir);
    if (!root[0] && !dir[0]) {
        return -1; /* the places: nothing to restore */
    }
    return root[0] == '/' && mp3_library_within(root, dir) ? 0 : -1;
}

int mp3_library_save_last(const char *root, const char *dir)
{
    char path[MP3_PATH_MAX + 32];
    char tmp[MP3_PATH_MAX + 32];
    char folder[MP3_PATH_MAX + 32];
    FILE *f;
    int ok;

    if (strchr(root, '\n') || strchr(dir, '\n') || last_path(path, sizeof(path), 0) != 0 ||
        last_path(tmp, sizeof(tmp), 1) != 0) {
        return -1;
    }
    snprintf(folder, sizeof(folder), "%s/mp3", pocketos_state_dir());
    if (pocketos_mkdir_p(folder, 0700) != 0) {
        return -1;
    }
    f = fopen(tmp, "w");
    if (!f) {
        return -1;
    }
    ok = fprintf(f, "%s\n%s\n", root, dir) > 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- the scanner --------------------------------------------------------------- */

struct mp3_scan_job {
    pthread_mutex_t mu;
    int refs;               /* the thread's and the scanner's */
    int done;
    struct mp3_list *list;  /* the result, until taken */
    char dir[MP3_PATH_MAX];
};

static int threads_alive;

int mp3_scanner_threads(void)
{
    return __atomic_load_n(&threads_alive, __ATOMIC_SEQ_CST);
}

static void job_release(struct mp3_scan_job *j)
{
    int left;

    pthread_mutex_lock(&j->mu);
    left = --j->refs;
    pthread_mutex_unlock(&j->mu);
    if (left == 0) {
        pthread_mutex_destroy(&j->mu);
        free(j->list);
        free(j);
    }
}

static void *scan_main(void *arg)
{
    struct mp3_scan_job *j = arg;
    struct mp3_list *l = malloc(sizeof(*l));

    if (mp3_library_read_hook) {
        mp3_library_read_hook(j->dir);
    }
    if (l) {
        mp3_library_read(j->dir, l);
    }
    pthread_mutex_lock(&j->mu);
    j->list = l;
    j->done = 1;
    pthread_mutex_unlock(&j->mu);
    job_release(j);
    __atomic_sub_fetch(&threads_alive, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

void mp3_scanner_init(struct mp3_scanner *sc)
{
    sc->job = NULL;
}

void mp3_scanner_close(struct mp3_scanner *sc)
{
    if (sc->job) {
        job_release(sc->job);
        sc->job = NULL;
    }
}

int mp3_scanner_start(struct mp3_scanner *sc, const char *dir)
{
    struct mp3_scan_job *j;
    pthread_attr_t attr;
    pthread_t t;
    int rc;

    mp3_scanner_close(sc);
    if (mp3_scanner_threads() >= MP3_SCAN_STUCK_MAX) {
        return -1;
    }
    j = calloc(1, sizeof(*j));
    if (!j) {
        return -1;
    }
    pthread_mutex_init(&j->mu, NULL);
    j->refs = 2;
    snprintf(j->dir, sizeof(j->dir), "%s", dir ? dir : "");
    __atomic_add_fetch(&threads_alive, 1, __ATOMIC_SEQ_CST);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* A folder listing needs little stack; the list itself is on the heap. */
    pthread_attr_setstacksize(&attr, 64 * 1024);
    rc = pthread_create(&t, &attr, scan_main, j);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        __atomic_sub_fetch(&threads_alive, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_destroy(&j->mu);
        free(j);
        return -1;
    }
    sc->job = j;
    return 0;
}

int mp3_scanner_poll(struct mp3_scanner *sc, struct mp3_list **out)
{
    struct mp3_scan_job *j = sc->job;
    int done;

    *out = NULL;
    if (!j) {
        return 0;
    }
    pthread_mutex_lock(&j->mu);
    done = j->done;
    if (done) {
        *out = j->list;
        j->list = NULL;
    }
    pthread_mutex_unlock(&j->mu);
    if (!done) {
        return 0;
    }
    job_release(j);
    sc->job = NULL;
    if (!*out) {
        /* The thread could not allocate: an empty, failed list. */
        *out = calloc(1, sizeof(**out));
        if (*out) {
            (*out)->err = ENOMEM;
        }
    }
    return *out != NULL;
}

int mp3_scanner_busy(const struct mp3_scanner *sc)
{
    return sc->job != NULL;
}
