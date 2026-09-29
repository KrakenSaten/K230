/*
 * DeskBuddy's files: the only part of DeskBuddy that touches the filesystem.
 *
 *   $POCKETOS_STATE_DIR/deskbuddy/prefs.v1   preferences (db_prefs.h)
 *   $POCKETOS_STATE_DIR/deskbuddy/guard.v1   the guard log (db_guard.h)
 *
 * default /var/lib/pocketos/deskbuddy: app-owned storage in the pattern
 * PocketFleet established and RIFT and the games follow. The directory is
 * 0700 and the files 0600: a visitor log says when the desk was empty.
 * Writes are atomic (temp file, fsync, rename), so a power cut leaves the old
 * file or the new one. Persistence never blocks the app: a failure is
 * reported to the caller and DeskBuddy carries on with what it has.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DB_STORE_H
#define DB_STORE_H

#include "db_guard.h"
#include "db_prefs.h"

#define DB_STORE_SUBDIR "deskbuddy"
#define DB_STORE_PREFS "prefs.v1"
#define DB_STORE_GUARD "guard.v1"

const char *db_store_dir(void);

/* 0 read, 1 no file (defaults), -1 unreadable (defaults). */
int db_store_load_prefs(struct db_prefs *p);
int db_store_save_prefs(const struct db_prefs *p);
/* 0 read (lines this build could not have written skipped), 1 no file
 * (empty log), -1 unreadable (empty log). */
int db_store_load_guard(struct db_guard_log *log);
int db_store_save_guard(const struct db_guard_log *log);

#endif
