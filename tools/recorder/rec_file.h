/*
 * A recording on disk, from its first byte to its final name - and the
 * repair of one that never got there. Used by pos-record only: the app never
 * writes audio.
 *
 * WHILE RECORDING the file is <name>.part in the recordings folder, 0600,
 * created with O_EXCL (nothing is ever overwritten) and held with an
 * exclusive flock for as long as the writer has it open. It starts with a
 * complete 44-byte header whose sizes say "no data yet", synced together
 * with the folder before the first sample, so from then on the file is
 * always a readable WAV. Samples are appended as they come; every
 * REC_FILE_CHECKPOINT_MS the header is rewritten with the sizes so far and
 * writeback of the new data is started (sync_file_range, which does not
 * wait), so what a power cut can take is bounded by the kernel's writeback
 * and never makes the header lie about more than the last checkpoint.
 *
 * FINISHING writes the final header, fsyncs the file, renames <name>.part to
 * <name> without replacing anything (RENAME_NOREPLACE; if <name> exists after
 * all, <name> gains "-2", "-3"...), and fsyncs the folder. A name without
 * .part is therefore always a complete file. If finishing fails the .part
 * stays, with every sample written so far, for the repair.
 *
 * REPAIR (rec_file_recover_dir) is for a .part whose writer is gone - it was
 * killed, crashed, or the power went. Its lock tells a live writer from a
 * dead one: a .part whose flock is held is skipped. A dead writer's .part
 * gets a header that matches the whole frames it holds (pocketwav_fix_fd),
 * a trailing partial frame cut, a sync, and the name <name>-recovered.wav.
 * One with no audio in it is removed. One whose header cannot be read at all
 * - which the synced header at creation makes a storage fault rather than an
 * interruption - is left as it is and reported as damaged, never renamed to
 * something that looks valid.
 *
 * LIMITS. The data never exceeds POCKETWAV_MAX_DATA_BYTES (rec_file_append
 * returns -EFBIG before it would). A full or failing filesystem is an errno
 * from append; nothing is retried behind the caller's back.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_FILE_H
#define POCKETREC_FILE_H

#include "rec_names.h"

#include <stddef.h>
#include <stdint.h>

#define REC_FILE_CHECKPOINT_MS 2000
#define REC_FILE_PATH_MAX 1024

struct rec_file {
    int fd;
    int dirfd;
    char dir[REC_FILE_PATH_MAX];
    char name[REC_NAME_MAX];                               /* the final name asked for */
    char part[REC_NAME_MAX + sizeof(REC_PART_SUFFIX)];     /* name + .part */
    unsigned rate;
    uint64_t data_bytes;
    uint64_t kicked_to;   /* data bytes whose writeback has been started */
};

/* Create dir/name.part for mono 16-bit audio at rate. name must be a
 * recording name (rec_name_parse). 0, or -errno: -EEXIST when name or its
 * .part exists, -EINVAL for a name that is not a recording's. */
int rec_file_create(struct rec_file *f, const char *dir, const char *name, unsigned rate);

/* Append frames (host-order samples; written little-endian). 0, or -errno:
 * -EFBIG when they would take the data past POCKETWAV_MAX_DATA_BYTES (nothing
 * is written), -ENOSPC when the filesystem is full (whole frames that did
 * reach the file are kept and counted). */
int rec_file_append(struct rec_file *f, const int16_t *samples, size_t frames);

/* The header's sizes brought up to date, and writeback started. 0 or -errno. */
int rec_file_checkpoint(struct rec_file *f);

/* The data bytes that still fit under the WAV limit. */
uint64_t rec_file_room(const struct rec_file *f);

/* Finish: final header, fsync, rename without replacing, folder fsync. The
 * name it ended up with is written to final (at least REC_NAME_MAX). 0, or
 * -errno with the .part kept (and closed). A recording with no audio is
 * removed instead, and -ENODATA returned. */
int rec_file_finish(struct rec_file *f, char *final, size_t final_len);

/* Close without finishing: an empty .part is removed, one with audio stays
 * for the repair. */
void rec_file_abandon(struct rec_file *f);

enum rec_recover_kind {
    REC_RECOVER_REPAIRED, /* name is the repaired recording's new name */
    REC_RECOVER_EMPTY,    /* a .part without audio, removed; name is the .part */
    REC_RECOVER_DAMAGED,  /* left in place; name is the .part */
    REC_RECOVER_LIVE      /* a writer still holds it; skipped */
};

typedef void (*rec_recover_fn)(enum rec_recover_kind kind, const char *name, void *user);

/* Repair every dead writer's .part in dir. The number of files acted on
 * (repaired, removed or reported damaged), or -errno when dir cannot be
 * read. */
int rec_file_recover_dir(const char *dir, rec_recover_fn fn, void *user);

/* Test seam, -1 in the shipped helper: when >= 0, appends fail with
 * ENOSPC once the data would pass this many bytes (tools/recorder/
 * pos_record.c sets it in its test-hooks build only). */
extern int64_t rec_file_fail_after;

#endif
