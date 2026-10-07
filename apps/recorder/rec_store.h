/*
 * The Recorder's side of the filesystem: where recordings live, what the
 * next one is called, the list, deleting one, and the preset. The app never
 * writes audio - pos-record does (tools/recorder/rec_file.h) - so every
 * write here is small: a delete, a folder, a preferences file.
 *
 * WHERE. $POCKETOS_RECORDINGS_DIR when set (tests), else $HOME/Recordings,
 * else /root/Recordings: the owner's home, which is where the Files app
 * opens (docs/apps/FILES.md), so recordings are visible there and can be
 * copied, renamed or deleted like any other file. Not the state directory:
 * Files treats /var/lib/pocketos as Doors' own and will not change anything
 * in it. The folder is created 0700 and each recording is 0600 - they are
 * somebody's words - and a folder found group- or world-writable has that
 * write permission removed.
 *
 * THE LIST is the folder itself: every .wav in it (any name, so one renamed
 * in Files still shows), and every recording's .part. Each file's header is
 * read (one page) for its format and length; no index or database is kept.
 * Newest first. At most REC_LIST_MAX entries are read; the count of the rest
 * is kept.
 *
 * PREFERENCES. $POCKETOS_STATE_DIR/recorder/recorder.conf: the preset.
 * "recorder-prefs 1" then key=value lines; written temporary file, fsync,
 * rename, 0600 in a 0700 directory, like Wave's.
 *
 * Tested against temporary folders in tests/rec_store_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETREC_STORE_H
#define POCKETREC_STORE_H

#include "rec_names.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_STORE_SUBDIR "Recordings"
#define REC_STORE_PATH_MAX 1024
#define REC_LIST_MAX 100
#define REC_PREFS_SUBDIR "recorder"
#define REC_PREFS_FILE "recorder.conf"
#define REC_PREFS_MAGIC "recorder-prefs 1"

enum rec_entry_status {
    REC_ENTRY_OK,          /* a WAV the Recorder can play */
    REC_ENTRY_OTHER,       /* a valid WAV in a rate or format it does not play */
    REC_ENTRY_UNREADABLE,  /* not a WAV, or a broken one */
    REC_ENTRY_PART         /* a recording's .part: in progress, or awaiting repair */
};

struct rec_entry {
    char name[REC_FILE_NAME_MAX];
    enum rec_entry_status status;
    int64_t mtime;
    uint64_t bytes;
    uint64_t ms;
    unsigned rate;
    unsigned channels;
    bool truncated;        /* the header claims more than the file holds */
    bool recovered;        /* repaired after an interruption (by its name) */
};

struct rec_list {
    struct rec_entry *e;   /* heap, n of them, newest first */
    int n;
    int total;             /* listable entries in the folder; > n when capped */
};

enum rec_preset {
    REC_PRESET_VOICE,      /* 16 kHz mono */
    REC_PRESET_STANDARD    /* 48 kHz mono, the device rate */
};

/* The recordings folder (not created). 0, or -1 when it would not fit. */
int rec_store_dir(char *out, size_t n);
/* Create it (0700, parents as needed) and take group/other write off it. 0 or
 * -errno. */
int rec_store_ensure_dir(const char *dir);

/* 0 with l filled (free it), or -errno with l empty. A missing folder is an
 * empty list. */
int rec_store_list(const char *dir, struct rec_list *l);
void rec_store_list_free(struct rec_list *l);

/* The next recording's name: the local wall-clock time when wall_now is a
 * valid time (the shell's rule, app.h), else the next sequence number. A
 * name that exists, or whose .part does, gets -2, -3... 0, or -1 when no
 * free name could be made. */
int rec_store_next_name(const char *dir, int64_t wall_now, bool wall_valid, char *out, size_t n);

/* Delete one listed recording: a listable name, a regular file (not
 * followed if it is a link), and not a .part that a writer still holds.
 * 0 or -errno (-EBUSY for a live .part, -EINVAL for a name that is not one
 * the list could show). */
int rec_store_delete(const char *dir, const char *name);

/* Free bytes for recordings, or -errno. */
int64_t rec_store_free(const char *dir);

/* 0 loaded, 1 nothing stored (the default, Voice), -1 unusable (default). */
int rec_store_load_preset(enum rec_preset *p);
int rec_store_save_preset(enum rec_preset p);

int rec_preset_rate(enum rec_preset p);
const char *rec_preset_label(enum rec_preset p);

#endif
