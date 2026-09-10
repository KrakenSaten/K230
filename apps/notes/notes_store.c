/*
 * PocketNotes persistence. See notes_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "notes_store.h"

#include "notes_view.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STORE_PATH_MAX 256

static char dir_buf[STORE_PATH_MAX];

const char *notes_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : NOTES_STORE_DEFAULT_DIR, NOTES_STORE_SUBDIR);
    return dir_buf;
}

int notes_store_path(uint32_t id, char *out, size_t out_len)
{
    char name[32];
    int n;

    snprintf(name, sizeof(name), NOTES_FILE_FMT, (unsigned)id);
    n = snprintf(out, out_len, "%s/%s", notes_store_dir(), name);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

/* Create every missing component of the store directory. Best effort: a
 * failure surfaces as a failed write, which every caller already handles. */
static void make_dirs(const char *dir)
{
    char work[STORE_PATH_MAX];
    size_t i;

    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        mkdir(work, 0755);
        work[i] = '/';
    }
    mkdir(work, 0755);
}

/* A name this store made, or nothing. Anything else in the directory is
 * someone else's and is left alone. */
static int id_from_name(const char *name, uint32_t *id)
{
    unsigned long v;
    char *end;
    size_t i;

    if (strncmp(name, "note-", 5) != 0) {
        return -1;
    }
    for (i = 5; i < 13; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return -1;
        }
    }
    if (strcmp(name + 13, ".txt") != 0) {
        return -1;
    }
    errno = 0;
    v = strtoul(name + 5, &end, 10);
    if (errno != 0 || end != name + 13 || v == 0 || v > 0xFFFFFFFFul) {
        return -1;
    }
    *id = (uint32_t)v;
    return 0;
}

int notes_store_read(uint32_t id, char *out, size_t out_len)
{
    char path[STORE_PATH_MAX];
    FILE *f;
    size_t got;

    if (!out || out_len == 0 || notes_store_path(id, path, sizeof(path)) != 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    /* One byte more than the cap, so a file that is too large is detected
     * rather than read short and then written back truncated. */
    got = fread(out, 1, out_len - 1, f);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (got >= out_len - 1 && out_len - 1 <= NOTES_MAX_BYTES) {
        /* Filled the buffer: either exactly at the cap or beyond it. Check
         * the file's real size before trusting it. */
        struct stat st;

        if (stat(path, &st) == 0 && (size_t)st.st_size > got) {
            return -1;
        }
    }
    out[got] = '\0';
    /* A note is text. Embedded NULs, or bytes that are not UTF-8, mean the
     * file is not one - refuse it rather than paint mojibake and then save
     * it back. */
    if (strlen(out) != got || !notes_text_is_utf8(out)) {
        return -1;
    }
    return (int)got;
}

int notes_store_list(struct notes_entry *out, int max)
{
    DIR *d;
    struct dirent *e;
    int n = 0;

    if (!out || max <= 0) {
        return -1;
    }
    d = opendir(notes_store_dir());
    if (!d) {
        /* No directory yet is an empty store, not a failure. */
        return errno == ENOENT ? 0 : -1;
    }
    while ((e = readdir(d)) != NULL && n < max) {
        static char text[NOTES_MAX_BYTES + 1];
        char path[STORE_PATH_MAX];
        struct notes_entry *entry = &out[n];
        struct stat st;
        uint32_t id;
        int len;

        if (id_from_name(e->d_name, &id) != 0) {
            continue;
        }
        if (notes_store_path(id, path, sizeof(path)) != 0 || stat(path, &st) != 0) {
            continue;
        }
        memset(entry, 0, sizeof(*entry));
        entry->id = id;
        entry->modified = (int64_t)st.st_mtime;
        len = notes_store_read(id, text, sizeof(text));
        if (len < 0) {
            entry->readable = 0;
            notes_title_unreadable(entry->title, sizeof(entry->title));
        } else {
            entry->readable = 1;
            notes_title_from_text(len == 1 && text[0] == '\0' ? "" : text, entry->title,
                                  sizeof(entry->title));
        }
        n++;
    }
    closedir(d);

    /* Newest first. n is at most NOTES_MAX_NOTES, so an insertion sort costs
     * nothing and keeps the comparison in one place. */
    {
        int i;

        for (i = 1; i < n; i++) {
            struct notes_entry key = out[i];
            int j = i - 1;

            while (j >= 0 && (out[j].modified < key.modified ||
                              (out[j].modified == key.modified && out[j].id < key.id))) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = key;
        }
    }
    return n;
}

int notes_store_write(uint32_t id, const char *text)
{
    char path[STORE_PATH_MAX];
    char tmp[STORE_PATH_MAX + 8];
    size_t len;
    FILE *f;

    if (!text || notes_store_path(id, path, sizeof(path)) != 0) {
        return -1;
    }
    len = strlen(text);
    if (len > NOTES_MAX_BYTES) {
        return -1;
    }
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        return -1;
    }
    make_dirs(notes_store_dir());
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if ((len > 0 && fwrite(text, 1, len, f) != len) || fflush(f) != 0 ||
        fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    /* rename() is atomic on the same filesystem, so a reader sees either the
     * previous note or this one and never a partial file. An interrupted
     * write leaves only the temporary, which the next write overwrites and
     * which the listing ignores because its name is not a note's. */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

uint32_t notes_store_next_id(void)
{
    struct notes_entry list[NOTES_MAX_NOTES];
    uint32_t candidate;
    int n;
    int i;

    n = notes_store_list(list, NOTES_MAX_NOTES);
    if (n < 0) {
        return 0;
    }
    if (n >= NOTES_MAX_NOTES) {
        return 0;
    }
    for (candidate = 1; candidate <= NOTES_MAX_NOTES + 1; candidate++) {
        int taken = 0;

        for (i = 0; i < n; i++) {
            if (list[i].id == candidate) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
    return 0;
}

int notes_store_delete(uint32_t id)
{
    char path[STORE_PATH_MAX];

    if (notes_store_path(id, path, sizeof(path)) != 0) {
        return -1;
    }
    if (unlink(path) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}
