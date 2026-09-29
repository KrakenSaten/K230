/*
 * Where the MP3 app finds music: a handful of places, and one folder at a
 * time inside them. No media library, no database, no recursive scan.
 *
 * PLACES (mp3_library_places), in this order, each shown only when it is a
 * folder:
 *
 *   Music       $POCKETOS_MUSIC_DIR, else $HOME/Music - the Doors
 *               convention, like Recorder's $HOME/Recordings; created
 *               (0755) the first time the places are listed
 *   Home        $HOME (/root on the device)
 *   Recordings  $POCKETOS_RECORDINGS_DIR, else $HOME/Recordings: what
 *               Recorder saved plays here too
 *   /mnt, /media  removable storage; $POCKETOS_MP3_MEDIA_ROOTS
 *               (colon-separated) replaces the pair, "" removes it
 *
 * A FOLDER (mp3_library_read) is read once, non-recursively: at most
 * MP3_SCAN_MAX directory entries are looked at and at most MP3_LIST_MAX
 * kept - folders first, then audio files, each sorted by name - and
 * `truncated` says when there was more. Hidden names are skipped. An audio
 * file is one whose extension is one of mp3_library_is_audio()'s; what
 * really plays is pos-mp3's decoder's decision, not this list's.
 *
 * THE SCANNER runs mp3_library_read on its own thread, so a slow or dying
 * storage device never stalls the LVGL thread: start, poll for the result,
 * close. A scan that is abandoned (a newer scan, a closed app) finishes on
 * its own and frees itself; if storage hangs, at most MP3_SCAN_STUCK_MAX
 * such threads wait on it and further scans are refused.
 *
 * THE LAST FOLDER is remembered in $POCKETOS_STATE_DIR/mp3/folder: the place
 * it is in and the folder, one line each. It is only a suggestion: the app
 * uses it only when it is still inside one of the places.
 *
 * Pure C, no LVGL: tested against real temporary folders
 * (tests/mp3_library_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETMP3_LIBRARY_H
#define POCKETMP3_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#define MP3_PATH_MAX 1024
#define MP3_NAME_MAX 256
/* Entries kept from one folder (and so the longest play queue). */
#define MP3_LIST_MAX 200
/* Directory entries looked at in one folder. */
#define MP3_SCAN_MAX 4096
#define MP3_PLACES_MAX 6
/* Scan threads that may be waiting on storage at once. */
#define MP3_SCAN_STUCK_MAX 4

enum mp3_entry_kind {
    MP3_ENTRY_PLACE,
    MP3_ENTRY_FOLDER,
    MP3_ENTRY_TRACK
};

struct mp3_entry {
    uint8_t kind;               /* enum mp3_entry_kind */
    uint8_t place;              /* PLACE: its index in mp3_library_places() */
    char name[MP3_NAME_MAX];    /* the file or folder name; a place's label */
    int64_t bytes;              /* TRACK: the file's size */
};

struct mp3_list {
    char dir[MP3_PATH_MAX];     /* "" for the places */
    int err;                    /* 0, or the errno that kept the folder from being read */
    int n;
    int folders;
    int tracks;
    int truncated;              /* more entries than were kept or looked at */
    struct mp3_entry e[MP3_LIST_MAX];
};

struct mp3_place {
    char label[32];
    char path[MP3_PATH_MAX];
};

/* The places from the environment, whether they exist or not (nothing is
 * touched). The number, at most MP3_PLACES_MAX. */
int mp3_library_places(struct mp3_place out[MP3_PLACES_MAX]);

/* Read dir ("" for the places) into out. out->err is 0 or an errno. The Music
 * folder is created when the places are read. Blocking: the scanner's job. */
void mp3_library_read(const char *dir, struct mp3_list *out);

/* Whether the name has an audio file's extension (case-insensitive): mp3,
 * wav, flac, ogg, oga, opus, m4a, aac. */
int mp3_library_is_audio(const char *name);

/* Case-insensitive order by name, folders first: what mp3_library_read
 * sorts by. */
int mp3_entry_compare(const struct mp3_entry *a, const struct mp3_entry *b);

/* src into dst (n bytes with the terminator), cut if it must be, but never
 * inside a UTF-8 character: names and tags end up on screen. */
void mp3_copy(char *dst, size_t n, const char *src);

/* dir/name into out. 0, or -1 when it would not fit or name is not a
 * plain name ("", ".", "..", or containing '/'). */
int mp3_library_join(char *out, const char *dir, const char *name);

/* The parent of dir inside root: 1 with *out filled when dir is strictly
 * inside root, 0 when dir is root itself or outside it. */
int mp3_library_parent(char *out, const char *root, const char *dir);
/* Whether dir is root or inside it. */
int mp3_library_within(const char *root, const char *dir);

/* The remembered place root and folder. 0 with both filled (dir may equal
 * root), -1 when there is none. */
int mp3_library_load_last(char root[MP3_PATH_MAX], char dir[MP3_PATH_MAX]);
/* Remember them ("" and "" for the places). 0, or -1 (nothing changes). */
int mp3_library_save_last(const char *root, const char *dir);

/* ---- the scanner ---------------------------------------------------------- */

struct mp3_scan_job;

struct mp3_scanner {
    struct mp3_scan_job *job;
};

void mp3_scanner_init(struct mp3_scanner *sc);
/* Read dir on a new thread; a scan still running is abandoned. 0, or -1
 * when no thread could be started (too many stuck on storage, or no
 * resources); the running scan, if any, is abandoned either way. */
int mp3_scanner_start(struct mp3_scanner *sc, const char *dir);
/* 1 with *out set to the finished list (the caller frees it with free()),
 * 0 while it is still reading or when there is no scan. */
int mp3_scanner_poll(struct mp3_scanner *sc, struct mp3_list **out);
int mp3_scanner_busy(const struct mp3_scanner *sc);
/* Abandon whatever runs. Never waits. */
void mp3_scanner_close(struct mp3_scanner *sc);
/* Scan threads alive now (for tests). */
int mp3_scanner_threads(void);

#endif
