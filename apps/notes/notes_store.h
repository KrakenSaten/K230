/*
 * PocketNotes persistence: one plain UTF-8 file per note.
 *
 * There is no container format and no index. A note is its text, and the
 * directory listing is the list of notes - so a note written by this app can
 * be read by anything, and a half-written one never exists, because every
 * write goes through a temporary file, fsync and rename.
 *
 * Kept small and local on purpose. The common state facility on the roadmap
 * will absorb this together with the Fleet, Radar and Timber stores; starting
 * that migration is not this milestone's work.
 *
 * This is the only file in the app that touches the filesystem
 * (tests/notes_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETNOTES_STORE_H
#define POCKETNOTES_STORE_H

#include "notes_view.h"

#include <stddef.h>
#include <stdint.h>

/* $POCKETOS_STATE_DIR/notes, default below. */
#define NOTES_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define NOTES_STORE_SUBDIR "notes"
/* Names are generated here, never taken from a note's text: a title is user
 * input and has no business in a path. */
#define NOTES_FILE_FMT "note-%08u.txt"

/* A note holds at most this many bytes. The editor caps input well below it
 * (NOTES_MAX_CHARS), and a file larger than this is refused rather than
 * silently truncated on the next save. */
#define NOTES_MAX_BYTES 4096
/* The keyboard produces ASCII and the three Latin-1 letters of C9, so no
 * character it can make is wider than two bytes: 2000 of them cannot exceed
 * the byte cap above. */
#define NOTES_MAX_CHARS 2000
#define NOTES_MAX_NOTES 64

struct notes_entry {
    uint32_t id;
    int64_t modified;             /* seconds, from the file's mtime */
    int readable;                 /* 0 when the file exists but could not be used */
    char title[NOTES_TITLE_MAX];  /* first non-blank line, truncated on a boundary */
};

const char *notes_store_dir(void);
/* Absolute path of a note. Returns 0, or -1 when it would not fit. */
int notes_store_path(uint32_t id, char *out, size_t out_len);

/* Newest first. Returns how many entries were written, or -1. An unreadable
 * note is listed with readable = 0 rather than hidden: a note that cannot be
 * read is exactly the one the owner most wants to see is still there. */
int notes_store_list(struct notes_entry *out, int max);

/* Read a note. Returns its length in bytes, 1 when there is no such note, or
 * -1 when it exists but cannot be used (too large, or unreadable). */
int notes_store_read(uint32_t id, char *out, size_t out_len);

/* Write a note atomically: temp, fsync, rename. Returns 0, or -1. */
int notes_store_write(uint32_t id, const char *text);

/* The lowest id not currently on disk, or 0 if the store is full or
 * unreadable. */
uint32_t notes_store_next_id(void);

/* Returns 0 when the note is gone afterwards, including when it was already
 * absent, and -1 otherwise. */
int notes_store_delete(uint32_t id);

#endif
