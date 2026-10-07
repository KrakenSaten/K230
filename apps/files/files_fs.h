/*
 * Files: the filesystem side of the Files app - listing, the write policy
 * and every operation that changes something on disk.
 *
 * LVGL-free and UI-free, so it is tested on its own (tests/files_fs_test.c)
 * and so nothing here can ever run inside a layout or draw pass. The app
 * lists directories and reads text through it on the LVGL thread (both are
 * bounded); the operations that can take long - copy, move, delete - run on
 * the worker in files_job.h.
 *
 * Errors are negative numbers: -errno from the system, or one of the
 * FILES_E* codes below for what this layer refuses on its own. Nothing here
 * ever leaves a half-made result under the name the owner asked for: a copy
 * is built under a temporary name and renamed into place only when complete,
 * and a failed one is removed again. The source of a copy is never written
 * to, and the source of a move is removed only once its copy is complete
 * and synced.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FILES_FS_H
#define FILES_FS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The longest path this app builds or accepts, terminator included, and the
 * longest name (NAME_MAX on every filesystem the board mounts). */
#define FILES_PATH_MAX 1024
#define FILES_NAME_MAX 255

/* A directory is read up to this many entries; the rest are counted, not
 * kept. The app shows fewer still (files_app.c). */
#define FILES_LIST_MAX 2048

/* The text viewer reads at most this much of a file. */
#define FILES_TEXT_MAX (32 * 1024)

/* How deep a copy or a delete goes into a tree before it refuses. */
#define FILES_DEPTH_MAX 32

/* Refusals of this layer's own, beyond errno. */
#define FILES_EBASE 10000
enum {
    FILES_EPOLICY = FILES_EBASE + 1, /* the policy protects this path */
    FILES_EBINARY,                   /* not text: the viewer will not show it */
    FILES_EINSIDE,                   /* a folder into itself or its own subfolder */
    FILES_EMOUNT,                    /* the tree holds another filesystem */
    FILES_ESPECIAL,                  /* a device, pipe or socket */
    FILES_ECANCELED,                 /* stopped on request */
    FILES_EDEPTH,                    /* deeper than FILES_DEPTH_MAX */
    FILES_ESAME,                     /* moved to where it already is */
    FILES_ENAME,                     /* not a valid name */
    FILES_ESOURCE_KEPT,              /* moved, but the original could not all be removed */
};

/* A short, plain sentence for any error this layer returns. */
const char *files_strerror(int err);

/* ---- paths ---------------------------------------------------------------- */

/* dir + "/" + name into out. Returns 0, or -ENAMETOOLONG. */
int files_path_join(char *out, size_t out_len, const char *dir, const char *name);
/* The parent of an absolute path ("/a/b" -> "/a", "/a" -> "/"). Returns 0,
 * or -1 when path is "/" and has none. */
int files_path_parent(char *out, size_t out_len, const char *path);
/* The last component ("/a/b" -> "b", "/" -> "/"). */
const char *files_path_base(const char *path);
/* An absolute path with repeated and trailing slashes and "." removed and
 * ".." resolved textually. Returns 0, or -EINVAL for a relative path, or
 * -ENAMETOOLONG. */
int files_path_clean(char *out, size_t out_len, const char *path);
/* Whether path is at or below dir (textually; both clean). */
bool files_path_within(const char *path, const char *dir);

/* NULL when name may be given to a file or folder; otherwise why not.
 * Not empty, not "." or "..", no "/", no control characters, no leading or
 * trailing space, at most FILES_NAME_MAX bytes of valid UTF-8. */
const char *files_name_problem(const char *name);

/* ---- listing -------------------------------------------------------------- */

enum files_kind {
    FILES_KIND_DIR = 0,
    FILES_KIND_FILE,
    FILES_KIND_LINK,  /* a symbolic link; link_dir says whether it leads to a folder */
    FILES_KIND_OTHER, /* device, pipe, socket */
};

struct files_entry {
    char name[FILES_NAME_MAX + 1];
    enum files_kind kind;
    bool link_dir;    /* a link that resolves to a directory: it opens like one */
    bool link_broken; /* a link whose target is missing */
    int64_t size;     /* bytes; -1 when unknown */
    int64_t mtime;    /* seconds since the epoch; 0 when unknown */
    uint32_t mode;    /* st_mode of the entry itself */
};

struct files_dir {
    struct files_entry *entries; /* heap, n of them */
    int n;
    int total;                   /* entries in the directory; > n when capped */
};

/* Read a directory: every entry but "." and "..", unsorted. Returns 0 with
 * d filled (release it with files_dir_free), or -errno with d empty. */
int files_dir_read(struct files_dir *d, const char *path);
void files_dir_free(struct files_dir *d);

/* True for anything that opens as a folder: a directory or a link to one. */
bool files_entry_is_dir(const struct files_entry *e);

enum files_sort {
    FILES_SORT_NAME = 0, /* A to Z, case-insensitive */
    FILES_SORT_TYPE,     /* by extension, then name */
    FILES_SORT_SIZE,     /* largest first */
    FILES_SORT_DATE,     /* newest first */
    FILES_SORT_COUNT
};

/* Folders first, then by key; ties by name, so the order is total. */
void files_sort(struct files_entry *e, int n, enum files_sort key);

/* One entry's facts, by path (lstat, then the target for a link). */
int files_stat(const char *path, struct files_entry *out);

/* ---- the write policy ------------------------------------------------------ *
 *
 * Browsing and reading are allowed everywhere. Changing something is allowed
 * only strictly inside a writable root (the root itself cannot be renamed,
 * moved or deleted), and never at, inside or around a protected path - so a
 * folder that contains Doors' own data cannot be deleted or moved away
 * either. Paths are resolved through symbolic links before they are judged,
 * so a link inside a writable root does not open a way into /etc.
 *
 * The default roots are /root, /home, /tmp, /mnt and /media. The default
 * protected paths are Doors' four directories (pocketpaths.h, including any
 * environment override) and /root/.ssh.
 */
#define FILES_POLICY_MAX 8

struct files_policy {
    const char *writable[FILES_POLICY_MAX]; /* NULL-terminated when shorter */
    const char *protect[FILES_POLICY_MAX];
};

enum files_access {
    FILES_ACCESS_OK = 0,
    FILES_ACCESS_SYSTEM,  /* outside every writable root */
    FILES_ACCESS_ROOT,    /* a writable root itself */
    FILES_ACCESS_DOORS,   /* at, inside or around a protected path */
    FILES_ACCESS_MISSING, /* could not be resolved: it is not there */
};

/* The default policy, built from the environment when first asked for and
 * fixed from then on. A NULL policy anywhere below means this one. */
const struct files_policy *files_policy_default(void);

/* May the entry at path be renamed, moved or deleted? */
enum files_access files_policy_entry(const struct files_policy *pol, const char *path);
/* May something be created in (or copied or moved into) the directory dir? */
enum files_access files_policy_dir(const struct files_policy *pol, const char *dir);
/* A caption for an access answer: "" for OK. */
const char *files_access_text(enum files_access a);

/* ---- operations ------------------------------------------------------------ *
 *
 * Each checks the policy itself, whatever the caller already checked. cancel
 * may be NULL; when it becomes nonzero a copy stops at the next chunk or
 * entry and removes what it made. */

/* A new, empty folder called name in dir. */
int files_mkdir(const struct files_policy *pol, const char *dir, const char *name);
/* Rename the entry at path to name, in the same folder. Never replaces. */
int files_rename(const struct files_policy *pol, const char *path, const char *name);
/* Copy the entry at src into dst_dir. When its name is taken there, the
 * copy is called "name (2)", "name (3)" and so on; the name it got is
 * written to out_name. */
int files_copy(const struct files_policy *pol, const char *src, const char *dst_dir,
               char *out_name, size_t out_name_len, atomic_int *cancel);
/* Move the entry at src into dst_dir under the same name. Never replaces:
 * a taken name is -EEXIST. */
int files_move(const struct files_policy *pol, const char *src, const char *dst_dir,
               atomic_int *cancel);
/* Delete the entry at path; a folder with everything in it. A link is
 * removed, never followed. Refuses a tree that holds another filesystem
 * before removing anything. */
int files_delete(const struct files_policy *pol, const char *path, atomic_int *cancel);

/* Up to FILES_TEXT_MAX bytes of a regular file as displayable UTF-8 into
 * buf (terminated). Returns the length, or -FILES_EBINARY for a file with
 * NUL bytes, -FILES_ESPECIAL for anything but a regular file, or -errno.
 * *truncated says whether there was more. */
int files_read_text(const char *path, char *buf, size_t buf_len, bool *truncated);

/* The size of the filesystem path is on and what an unprivileged writer may
 * still use of it, in bytes. Returns 0, or -errno with both set to -1. */
int files_space(const char *path, int64_t *total, int64_t *avail);

#endif
