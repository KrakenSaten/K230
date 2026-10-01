/*
 * The Video app's file list: the playable files in one folder, sorted by
 * name. v0.1 plays MP4 files with H.264 pictures, so the list shows *.mp4
 * files only (any case) - what else the helper might decode is not claimed
 * until it has been tested (docs/apps/VIDEO.md).
 *
 * The folder is $POCKETOS_VIDEOS_DIR when that is an absolute path, else
 * $HOME/Videos (/root/Videos when HOME is unset or "/"), the convention of
 * Recorder's $POCKETOS_RECORDINGS_DIR and $HOME/Recordings.
 *
 * Reading the folder is one bounded readdir of at most VIDEO_FILES_SCAN_MAX
 * entries and an fstatat per candidate, on the LVGL thread, once per visit
 * and on RESCAN - the same cost as Recorder's list. Nothing here opens a file.
 *
 * Pure C, no LVGL: tests/video_files_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VIDEO_FILES_H
#define POCKETOS_VIDEO_FILES_H

#include <stddef.h>
#include <stdint.h>

#define VIDEO_FILES_SUBDIR "Videos"
/* Shown at most; a folder with more says so. */
#define VIDEO_FILES_MAX 200
/* Directory entries looked at, at most (so a huge folder cannot stall the
 * screen). */
#define VIDEO_FILES_SCAN_MAX 2000
/* A name longer than this is skipped: it could not be opened through the
 * helper's line protocol together with the folder anyway. */
#define VIDEO_FILES_NAME_MAX 128

struct video_file {
    char name[VIDEO_FILES_NAME_MAX];
    uint64_t bytes;
};

struct video_files {
    struct video_file items[VIDEO_FILES_MAX];
    int count;
    int more;        /* 1: playable files beyond VIDEO_FILES_MAX were left out */
    int error;       /* 0, or the errno of opening the folder (ENOENT: no folder yet) */
};

/* The folder into out. 0, or -1 when it does not fit. */
int video_files_dir(char *out, size_t n);

/* Whether a name is one the list shows: not hidden, ends in .mp4 (any case),
 * shorter than VIDEO_FILES_NAME_MAX, and no control characters. */
int video_files_playable_name(const char *name);

/* Read dir into *list (regular files only, links followed, sorted by name
 * byte-wise). Returns the count, or -1 with list->error set. */
int video_files_scan(struct video_files *list, const char *dir);

/* "12.3 MB", "850 KB", "0 KB". */
void video_files_size_text(uint64_t bytes, char *out, size_t n);

/* "m:ss", or "h:mm:ss" from an hour; negative ms reads as 0. */
void video_files_time_text(int64_t ms, char *out, size_t n);

#endif
