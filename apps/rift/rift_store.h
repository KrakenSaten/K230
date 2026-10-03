/*
 * RIFT's preferences file: the reader's own choices about this app, and
 * nothing about the mesh.
 *
 * RIFT keeps no messages, nodes, keys or read marks: those are the
 * service's, or this session's, and docs/apps/RIFT.md says why. What it does
 * keep is what a reader chose and would be annoyed to choose again: whether
 * a new direct message makes a sound, whether a new channel message does,
 * and which channels are muted.
 *
 * Location: $POCKETOS_STATE_DIR/rift/prefs.v1, default
 * /var/lib/pocketos/rift/prefs.v1 - app-owned storage, the pattern
 * PocketFleet, PocketRadar and PocketTimber use. The shell's settings.conf is
 * the shell's; no app writes it, and there is no platform API for an app's
 * preference (docs/apps/RIFT.md lists that as a shared requirement). That
 * /var/lib is writable on the K230 is DOCUMENTED from the Buildroot
 * defconfig and ASSUMED until hardware confirms it, as for those apps.
 *
 * The format is settings.conf's: one key=value per line, '#' comments. A
 * value this build could not have written is ignored - the default stands -
 * and the file is left as it is until the reader changes a setting, which
 * rewrites it whole. An unknown key is ignored on read and not carried over,
 * as settings.conf does. Writes are atomic (temp file, fsync, rename), so a
 * power loss leaves the old file or the new one.
 *
 * Persistence never blocks the app: every call reports failure and does
 * nothing else, and RIFT then runs with the default for this session.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_STORE_H
#define RIFT_STORE_H

#include <stddef.h>

#define RIFT_STORE_SUBDIR "rift"
#define RIFT_STORE_FILE "prefs.v1"
/* The keys, and what each takes. */
#define RIFT_PREF_DM_SOUND "dm_sound" /* 0|1 */
/* A new direct message makes a sound unless the reader turned it off. */
#define RIFT_PREF_DM_SOUND_DEFAULT 1
/* A new channel message makes its own, different sound unless the reader
 * turned channel sounds off, or muted that one channel. */
#define RIFT_PREF_CH_SOUND "channel_sound" /* 0|1 */
#define RIFT_PREF_CH_SOUND_DEFAULT 1
/* One line per muted channel, its conversation key as COMMS keys it
 * ("#<slot>:<hash>:<name fingerprint>", rift_model.h): that names the
 * channel and not merely its slot, so a different channel later joined into
 * the same slot is not muted by it. At most RIFT_PREF_MUTE_MAX are kept. */
#define RIFT_PREF_CH_MUTE "channel_mute"
#define RIFT_PREF_MUTE_MAX 16
#define RIFT_PREF_MUTE_KEY_MAX 65
/* The longest line this build writes, and the whole file. */
#define RIFT_STORE_TEXT_MAX 256
#define RIFT_STORE_FILE_MAX 2048

struct rift_prefs {
    int dm_sound; /* 0 or 1 */
    int ch_sound; /* 0 or 1 */
    char mute[RIFT_PREF_MUTE_MAX][RIFT_PREF_MUTE_KEY_MAX];
    int mute_count;
};

void rift_prefs_defaults(struct rift_prefs *p);

/* Read the file's text into p, over whatever p held. Returns a mask of the
 * known keys whose value was unusable (1: dm_sound, 2: channel_sound, 4: a
 * channel_mute line), which keep what p had; 0 when everything present was
 * usable. Pure: no I/O. */
int rift_prefs_parse(struct rift_prefs *p, const char *text);
/* The file's text. Returns the length, or -1 when out is too small. */
int rift_prefs_format(const struct rift_prefs *p, char *out, size_t out_len);

/* Whether this channel (a conversation key) is muted, and muting it or not.
 * set returns 0, or -1 when the key is not a channel's or the list is full. */
int rift_prefs_channel_muted(const struct rift_prefs *p, const char *conv_key);
int rift_prefs_set_channel_muted(struct rift_prefs *p, const char *conv_key, int muted);

const char *rift_store_dir(void);
const char *rift_store_path(void);

/* 0 when read (p filled, defaults for anything absent or unusable), 1 when
 * there is no file (p is the defaults), -1 when there is one that could not
 * be read (p is the defaults). */
int rift_store_load(struct rift_prefs *p);
/* 0, or -1 when it could not be written; the running value is the caller's
 * and is not affected. */
int rift_store_save(const struct rift_prefs *p);

#endif
