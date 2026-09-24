/*
 * pocketcam's photo store: where a capture goes, what it is called, how it
 * is written so that a photo is either all there or not there at all, and
 * when the camera stops taking photos rather than filling the card.
 *
 * WHERE. $POCKETOS_STATE_DIR/camera, so /var/lib/pocketos/camera on the unit:
 * the state convention every app follows (docs/ARCHITECTURE.md). On today's
 * images that directory is on the root filesystem - a 600 MB partition with
 * about 120 MB free on unit A - because the data partition of
 * docs/STORAGE_PLAN_v0.0.3.md does not exist yet. Hence the limits below.
 *
 * NAMES. IMG_<yyyymmdd>_<hhmmss>_<nnnn>.<ext> when the wall clock is valid,
 * IMG_<nnnn>.<ext> when it is not (this board has no RTC, so after a cold boot
 * without a network it usually is not; a made-up 1970 date would sort wrong
 * and say something false). <nnnn> is one more than the highest number in
 * the folder, at least four digits, so names are unique and sort in the
 * order the photos were taken whatever the clock did.
 *
 * ATOMIC. A photo is written to .<name>.tmp beside its final name, flushed,
 * fsync'd, closed, renamed into place, and then the directory is fsync'd.
 * Every failure removes the temporary file. A name that exists is therefore a
 * complete file; a power cut leaves at worst a .tmp, which the next open
 * removes.
 *
 * LIMITS. At most POCKETCAM_STORE_MAX_FILES photos and
 * POCKETCAM_STORE_MAX_BYTES in the folder (-EDQUOT past either), and never
 * leave the filesystem with less than POCKETCAM_STORE_RESERVE_BYTES free
 * (-ENOSPC): the root filesystem also holds the settings, the logs and every
 * other app's state, and a camera must not be the thing that fills it. Nothing
 * is ever deleted to make room; the owner decides what goes.
 *
 * LVGL-free, no threads. Used by pos-camera only: every write to the photo
 * folder happens in the helper, never on the LVGL thread.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_STORE_H
#define POCKETOS_POCKETCAM_STORE_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define POCKETCAM_STORE_SUBDIR "camera"
#define POCKETCAM_STORE_MAX_FILES 500u
#define POCKETCAM_STORE_MAX_BYTES (64ull << 20)
#define POCKETCAM_STORE_RESERVE_BYTES (48ull << 20)
/* "IMG_20260924_235959_000001.jpg" and its terminator, with room. */
#define POCKETCAM_STORE_NAME_MAX 48
/* The wall clock is taken as set from here on: 2024-01-01T00:00:00Z, the
 * same floor as PocketClock's CLOCK_WALL_VALID_FROM (apps/clock/clock_engine.h;
 * tests/camera_lint.sh keeps the two equal). */
#define POCKETCAM_WALL_VALID_FROM 1704067200LL

struct pocketcam_store {
    char dir[PATH_MAX];
    uint32_t max_files;
    uint64_t max_bytes;
    uint64_t reserve_bytes;
    /* From the last scan. */
    uint32_t files;
    uint64_t bytes;
    uint32_t next_seq;
    char last[POCKETCAM_STORE_NAME_MAX]; /* the newest photo, "" when none */
};

/* Use dir (created with its parents when missing), remove what an
 * interrupted write left there, and scan it. 0 or a negative errno. */
int pocketcam_store_open(struct pocketcam_store *s, const char *dir);

/* The default folder: $POCKETOS_STATE_DIR/camera. */
void pocketcam_store_default_dir(char *out, size_t out_len);

/* Count the photos again. 0 or a negative errno. */
int pocketcam_store_scan(struct pocketcam_store *s);

/* Whether a photo of about estimate bytes may be taken now: 0, -EDQUOT (the
 * folder is at its limit), -ENOSPC (the filesystem would drop below the
 * reserve), or another negative errno when the space cannot be read. */
int pocketcam_store_room(const struct pocketcam_store *s, uint64_t estimate);

/* The next photo's name for the wall clock now (epoch seconds; below
 * POCKETCAM_WALL_VALID_FROM means unset), with extension ext ("jpg"). */
void pocketcam_store_next_name(const struct pocketcam_store *s, int64_t now, const char *ext,
                               char *out, size_t out_len);

/* Write a photo: writer is handed a stream on the temporary file and returns
 * 0 or a negative errno. On success the photo is in place under name, the
 * store is rescanned and *bytes_out (may be NULL) holds its size. On failure
 * nothing is left behind: -ENOSPC when the storage ran out, -EEXIST when the
 * name is taken, -EINVAL for a name that is not a photo name, or another
 * negative errno. */
typedef int (*pocketcam_store_writer)(FILE *fp, void *user);
int pocketcam_store_write(struct pocketcam_store *s, const char *name,
                          pocketcam_store_writer writer, void *user, uint64_t *bytes_out);

/* Delete one photo by name. Only a photo name is accepted, so nothing
 * outside the folder and nothing but a photo can be named. */
int pocketcam_store_delete(struct pocketcam_store *s, const char *name);

/* IMG_<digits>.<ext> or IMG_<8 digits>_<6 digits>_<digits>.<ext>, with 4 to
 * 6 digits of sequence and ext jpg or ppm. */
bool pocketcam_store_valid_name(const char *name);

/* ---- test seams ----------------------------------------------------------- *
 * NULL in the shipped helper. pos-camera-testhooks sets them from its
 * environment (tools/camera/pos_camera.c) so the session tests can run a disk
 * that is full, or fills up in the middle of a photo. */
extern int64_t (*pocketcam_store_free_hook)(const char *dir);
/* When >= 0, a write stream refuses with ENOSPC once this many bytes are in. */
extern int64_t pocketcam_store_fail_after;

#endif
