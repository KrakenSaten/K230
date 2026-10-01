/*
 * RIFT's preferences file: the reader's own choices about this app, and
 * nothing about the mesh.
 *
 * RIFT keeps no messages, nodes, keys or read marks: those are the
 * service's, or this session's, and docs/apps/RIFT.md says why. What it does
 * keep is what a reader chose and would be annoyed to choose again - today
 * one thing, whether a new direct message makes a sound.
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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
#define RIFT_STORE_TEXT_MAX 256

struct rift_prefs {
    int dm_sound; /* 0 or 1 */
};

void rift_prefs_defaults(struct rift_prefs *p);

/* Read the file's text into p, over whatever p held. Returns a mask of the
 * known keys whose value was unusable (1: dm_sound), which keep what p had;
 * 0 when everything present was usable. Pure: no I/O. */
int rift_prefs_parse(struct rift_prefs *p, const char *text);
/* The file's text. Returns the length, or -1 when out is too small. */
int rift_prefs_format(const struct rift_prefs *p, char *out, size_t out_len);

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
