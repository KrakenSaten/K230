/*
 * DeskBuddy's preferences: the owner's choices about this app.
 *
 * Kept the way RIFT keeps its own (apps/rift/rift_store.h): an app-owned
 * file in settings.conf's key=value format, $POCKETOS_STATE_DIR/deskbuddy/
 * prefs.v1. settings.conf is the shell's and no app writes it; there is no
 * platform API for an app preference, so there is no second framework here
 * either - one struct, one parser, one formatter (db_store.c does the file).
 *
 *   companion=0|1          Companion mode is offered and reacts to people
 *   guard=0|1              Desk Guard mode is offered and can be armed
 *   night=0|1              Night mode is offered
 *   owner_recognition=0|1  identity events are used; off: everyone is "a person"
 *   greeting=0|1           the owner is greeted when recognised
 *   idle_animation=0|1     random blinks and glances (reduced motion also stops them)
 *   guard_armed=0|1        the guard was armed when the app last saved
 *   mode=companion|guard|night   the mode last shown
 *
 * A value this build could not have written leaves the default standing,
 * and an unknown key is ignored and not carried over, as settings.conf and
 * RIFT do.
 *
 * Pure C, no I/O (tests/db_store_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_PREFS_H
#define DB_PREFS_H

#include <stdbool.h>
#include <stddef.h>

enum db_mode {
    DB_MODE_COMPANION = 0,
    DB_MODE_GUARD,
    DB_MODE_NIGHT,
    DB_MODE_COUNT
};

enum db_pref {
    DB_PREF_COMPANION = 0,
    DB_PREF_GUARD,
    DB_PREF_NIGHT,
    DB_PREF_RECOGNITION,
    DB_PREF_GREETING,
    DB_PREF_IDLE_ANIMATION,
    DB_PREF_TOGGLE_COUNT /* the owner-facing switches end here */
};

struct db_prefs {
    bool on[DB_PREF_TOGGLE_COUNT];
    bool guard_armed;
    enum db_mode mode;
};

#define DB_PREFS_TEXT_MAX 512

void db_prefs_defaults(struct db_prefs *p);
const char *db_pref_key(enum db_pref pref);   /* the file key */
const char *db_pref_label(enum db_pref pref); /* the screen's label */
const char *db_mode_name(enum db_mode mode);  /* "companion", "guard", "night" */

/* Whether the preferences offer this mode. */
bool db_prefs_mode_allowed(const struct db_prefs *p, enum db_mode mode);
/* The mode to show for a wish: the wish when allowed, else the first allowed
 * one in Companion, Guard, Night order, else Companion (resting: it does not
 * react, because companion is off). */
enum db_mode db_prefs_resolve_mode(const struct db_prefs *p, enum db_mode wish);

/* Read text over p. Returns the number of known keys whose value was
 * refused (they keep what p had). */
int db_prefs_parse(struct db_prefs *p, const char *text);
/* Returns the length, or -1 when out is too small. */
int db_prefs_format(const struct db_prefs *p, char *out, size_t out_len);

#endif
