/*
 * DeskBuddy's guard log, preferences and files (apps/deskbuddy/db_guard.h,
 * db_prefs.h, db_store.h): the ring stays bounded, the text forms read back
 * exactly, a damaged file costs only its damaged lines, and the files are
 * private and written atomically under $POCKETOS_STATE_DIR.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_guard.h"
#include "db_prefs.h"
#include "db_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void test_ring(void)
{
    struct db_guard_log log;
    int i;

    db_guard_init(&log);
    check("an empty log", db_guard_count(&log) == 0 && !db_guard_at(&log, 0) && db_guard_unacknowledged(&log) == 0);
    for (i = 1; i <= 100; i++) {
        db_guard_add(&log, 1790000000 + i, i % 3 == 0 ? DB_SUBJECT_OWNER : DB_SUBJECT_UNKNOWN, 700, i % 3 == 0);
    }
    check("a hundred events keep DB_GUARD_CAP", db_guard_count(&log) == DB_GUARD_CAP);
    check("newest first", db_guard_at(&log, 0)->id == 100 && db_guard_at(&log, DB_GUARD_CAP - 1)->id == 69);
    check("an event that left the ring is not found", !db_guard_find(&log, 68) && db_guard_find(&log, 69));
    check("owners are not visitors to report", db_guard_unacknowledged(&log) == 21);
    check("acknowledging marks every one", db_guard_acknowledge_all(&log) == 21 && db_guard_unacknowledged(&log) == 0);
    check("a second time changes nothing", db_guard_acknowledge_all(&log) == 0);
    check("a bad subject is refused", !db_guard_add(&log, 0, DB_SUBJECT_COUNT, 0, false));
    check("a bad confidence is stored as none, a bad time as unset",
          db_guard_add(&log, -5, DB_SUBJECT_PERSON, 4000, false)->confidence_pm == -1 &&
              db_guard_at(&log, 0)->wall_s == 0);
    check("v0.1 never keeps a snapshot", db_guard_at(&log, 0)->snapshot_id == 0);
}

static void test_text(void)
{
    struct db_guard_log log;
    struct db_guard_log back;
    char text[DB_GUARD_TEXT_MAX];
    int same = 1;
    unsigned i;
    int n;

    db_guard_init(&log);
    for (i = 0; i < DB_GUARD_CAP + 5; i++) {
        db_guard_add(&log, i ? 1790000000 + (int64_t)i * 60 : 0, (enum db_subject)(i % DB_SUBJECT_COUNT),
                     i % 2 ? (int16_t)(i * 10) : -1, i % 4 == 0);
    }
    n = db_guard_format(&log, text, sizeof(text));
    check("a full log fits DB_GUARD_TEXT_MAX", n > 0 && n < (int)sizeof(text));
    check("the text is too long for a small buffer", db_guard_format(&log, text, 64) == -1);
    db_guard_format(&log, text, sizeof(text));
    check("it reads back with nothing skipped", db_guard_parse(&back, text) == 0);
    same = db_guard_count(&back) == db_guard_count(&log) && back.next_id == log.next_id;
    for (i = 0; same && i < db_guard_count(&log); i++) {
        const struct db_guard_event *a = db_guard_at(&log, i);
        const struct db_guard_event *b = db_guard_at(&back, i);

        same = a->id == b->id && a->wall_s == b->wall_s && a->subject == b->subject &&
               a->confidence_pm == b->confidence_pm && a->acknowledged == b->acknowledged &&
               a->snapshot_id == b->snapshot_id;
    }
    check("exactly", same);

    n = db_guard_parse(&back, "# note\n"
                              "next=9\n"
                              "e 1 1790000000 unknown 800 0 0\n"
                              "e 2 1790000060 alien 800 0 0\n"
                              "e 3 1790000120 unknown 1800 0 0\n"
                              "e 4 1790000180 unknown 800 2 0\n"
                              "e 5 1790000240 person -1 1 0 extra\n"
                              "e 1 1790000300 owner -1 1 0\n"
                              "garbage\r\n"
                              "e 6 1790000360 owner -1 1 0\r\n");
    check("a damaged file costs only its bad lines", n == 6 && db_guard_count(&back) == 2);
    check("and the good ones are exact", db_guard_at(&back, 1)->id == 1 && db_guard_at(&back, 0)->id == 6 &&
                                             db_guard_at(&back, 0)->subject == DB_SUBJECT_OWNER);
    check("ids carry on after the highest, whatever next= said", back.next_id == 9);
    db_guard_parse(&back, "next=2\ne 7 0 person -1 0 0\n");
    check("next= below the ids is ignored", back.next_id == 8);
    {
        char *many = malloc(64 * 48);
        size_t used = 0;

        for (i = 1; many && i <= 64; i++) {
            used += (size_t)snprintf(many + used, 64 * 48 - used, "e %u %u unknown 500 0 0\n", i, 1790000000u + i);
        }
        check("a file with more lines than the ring keeps the newest",
              many && db_guard_parse(&back, many) == 0 && db_guard_count(&back) == DB_GUARD_CAP &&
                  db_guard_at(&back, 0)->id == 64 && db_guard_at(&back, DB_GUARD_CAP - 1)->id == 33);
        free(many);
    }
    check("an empty text is an empty log", db_guard_parse(&back, "") == 0 && db_guard_count(&back) == 0 &&
                                               back.next_id == 1);
}

static void test_prefs(void)
{
    struct db_prefs p;
    struct db_prefs back;
    char text[DB_PREFS_TEXT_MAX];
    int all_on = 1;
    int k;

    db_prefs_defaults(&p);
    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        all_on &= p.on[k];
    }
    check("everything on by default, disarmed, in Companion", all_on && !p.guard_armed && p.mode == DB_MODE_COMPANION);
    p.on[DB_PREF_GREETING] = false;
    p.guard_armed = true;
    p.mode = DB_MODE_NIGHT;
    check("the preferences format", db_prefs_format(&p, text, sizeof(text)) > 0);
    db_prefs_defaults(&back);
    check("and read back exactly", db_prefs_parse(&back, text) == 0 && !back.on[DB_PREF_GREETING] && back.guard_armed &&
                                       back.mode == DB_MODE_NIGHT && back.on[DB_PREF_COMPANION]);
    db_prefs_defaults(&back);
    check("values this build could not have written keep the default",
          db_prefs_parse(&back, "greeting=yes\nmode=party\n  night = 0 \nfuture_key=1\n# x=0\n") == 2 &&
              back.on[DB_PREF_GREETING] && back.mode == DB_MODE_COMPANION && !back.on[DB_PREF_NIGHT]);
    check("a small buffer is refused", db_prefs_format(&p, text, 16) == -1);
    db_prefs_defaults(&p);
    p.on[DB_PREF_COMPANION] = false;
    check("a mode that is off resolves to the next that is on",
          db_prefs_resolve_mode(&p, DB_MODE_COMPANION) == DB_MODE_GUARD &&
              db_prefs_resolve_mode(&p, DB_MODE_NIGHT) == DB_MODE_NIGHT);
    p.on[DB_PREF_GUARD] = false;
    p.on[DB_PREF_NIGHT] = false;
    check("all off resolves to Companion, resting", db_prefs_resolve_mode(&p, DB_MODE_NIGHT) == DB_MODE_COMPANION &&
                                                        !db_prefs_mode_allowed(&p, DB_MODE_COMPANION));
}

static int mode_of(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

static void test_store(void)
{
    char root[] = "/tmp/db_store_testXXXXXX";
    char path[512];
    struct db_prefs p;
    struct db_prefs back;
    struct db_guard_log log;
    struct db_guard_log lback;
    FILE *f;

    if (!mkdtemp(root)) {
        check("a temporary directory", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", root, 1);
    check("nothing stored yet: defaults", db_store_load_prefs(&back) == 1 && back.on[DB_PREF_GUARD]);
    check("and an empty log", db_store_load_guard(&lback) == 1 && db_guard_count(&lback) == 0);
    db_prefs_defaults(&p);
    p.guard_armed = true;
    p.on[DB_PREF_IDLE_ANIMATION] = false;
    db_guard_init(&log);
    db_guard_add(&log, 1790000000, DB_SUBJECT_UNKNOWN, 810, false);
    check("the preferences save", db_store_save_prefs(&p) == 0);
    check("the log saves", db_store_save_guard(&log) == 0);
    check("they load back", db_store_load_prefs(&back) == 0 && back.guard_armed && !back.on[DB_PREF_IDLE_ANIMATION] &&
                                db_store_load_guard(&lback) == 0 && db_guard_count(&lback) == 1 &&
                                db_guard_at(&lback, 0)->confidence_pm == 810);
    snprintf(path, sizeof(path), "%s/deskbuddy", root);
    check("the directory is private (0700)", mode_of(path) == 0700);
    snprintf(path, sizeof(path), "%s/deskbuddy/guard.v1", root);
    check("the log is private (0600)", mode_of(path) == 0600);
    snprintf(path, sizeof(path), "%s/deskbuddy/prefs.v1", root);
    check("the preferences are private (0600)", mode_of(path) == 0600);
    snprintf(path, sizeof(path), "%s/deskbuddy/guard.v1.tmp", root);
    check("no temporary file is left behind", access(path, F_OK) != 0);

    snprintf(path, sizeof(path), "%s/deskbuddy/guard.v1", root);
    f = fopen(path, "w");
    if (f) {
        fputs("\x01\x02 binary junk\ne 1 1790000000 unknown 810 0 0\n", f);
        fclose(f);
    }
    check("a damaged log still loads what it can", db_store_load_guard(&lback) == 0 && db_guard_count(&lback) == 1);

    snprintf(path, sizeof(path), "%s/deskbuddy", root);
    chmod(path, 0500);
    if (geteuid() != 0) {
        check("a save that cannot write fails, and says so", db_store_save_prefs(&p) == -1);
    }
    chmod(path, 0700);
    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", root);
        }
    }
    unsetenv("POCKETOS_STATE_DIR");
    check("without the override: /var/lib/pocketos/deskbuddy", strcmp(db_store_dir(), "/var/lib/pocketos/deskbuddy") == 0);
}

int main(void)
{
    test_ring();
    test_text();
    test_prefs();
    test_store();
    printf("db_guard_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
