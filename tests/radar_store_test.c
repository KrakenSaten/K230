/*
 * PocketRadar record store test: the codec, its refusal of anything that is
 * not a record the game could have written, and the file behaviour the app
 * depends on - atomic writes, a missing file that is not an error, and a
 * damaged one that never stops play.
 *
 * The filesystem cases drive the real paths through POCKETOS_STATE_DIR in a
 * temporary directory. They are POSIX and are run natively; nothing here
 * has been exercised on the K230.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "radar_store.h"

#include "radar_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static struct radar_record sample(void)
{
    struct radar_record r;

    radar_record_init(&r);
    r.best_score = 4820u;
    r.best_streak = 9u;
    r.best_level = 6u;
    r.runs = 17u;
    r.engaged = 214u;
    r.mistakes = 31u;
    return r;
}

static int same(const struct radar_record *a, const struct radar_record *b)
{
    return a->best_score == b->best_score && a->best_streak == b->best_streak &&
           a->best_level == b->best_level && a->runs == b->runs &&
           a->engaged == b->engaged && a->mistakes == b->mistakes;
}

/* Encode a record without validating it, so an impossible one can be handed
 * to the decoder with a checksum that is perfectly correct. Otherwise the
 * checksum would reject it first and the invariant checks below would never
 * run. */
static void encode_raw(const struct radar_record *r, uint8_t *buf)
{
    if (radar_record_encode(r, buf, RADAR_RECORD_SIZE) != RADAR_RECORD_SIZE) {
        check("the fixture encodes", 0);
    }
}

static void test_codec(void)
{
    struct radar_record in = sample();
    struct radar_record out;
    uint8_t buf[RADAR_RECORD_SIZE + 4];
    uint8_t small[RADAR_RECORD_SIZE - 1];

    memset(&out, 0xAB, sizeof(out));
    check("a record encodes to its stated size",
          radar_record_encode(&in, buf, sizeof(buf)) == RADAR_RECORD_SIZE);
    check("a record is thirty bytes", RADAR_RECORD_SIZE == 30);
    check("a record decodes", radar_record_decode(&out, buf, RADAR_RECORD_SIZE) == 0);
    check("a record survives the round trip", same(&in, &out));

    check("a buffer that is too small is refused",
          radar_record_encode(&in, small, sizeof(small)) == -1);
    check("NULL is refused",
          radar_record_encode(NULL, buf, sizeof(buf)) == -1 &&
          radar_record_encode(&in, NULL, sizeof(buf)) == -1 &&
          radar_record_decode(NULL, buf, RADAR_RECORD_SIZE) == -1 &&
          radar_record_decode(&out, NULL, RADAR_RECORD_SIZE) == -1);

    /* An empty record is a legal one: it is what a first run starts from. */
    {
        struct radar_record empty;
        struct radar_record back;

        radar_record_init(&empty);
        radar_record_encode(&empty, buf, sizeof(buf));
        memset(&back, 0xCD, sizeof(back));
        check("an empty record round-trips",
              radar_record_decode(&back, buf, RADAR_RECORD_SIZE) == 0 &&
              same(&empty, &back));
    }
}

static void test_damaged_blobs(void)
{
    struct radar_record in = sample();
    struct radar_record probe;
    uint8_t good[RADAR_RECORD_SIZE];
    uint8_t bad[RADAR_RECORD_SIZE];
    struct radar_record before;

    radar_record_encode(&in, good, sizeof(good));
    before = sample();

#define REJECTS(name, mutation)                                              \
    do {                                                                     \
        memcpy(bad, good, sizeof(bad));                                       \
        mutation;                                                             \
        probe = before;                                                       \
        check(name, radar_record_decode(&probe, bad, sizeof(bad)) == -1 &&    \
                    same(&probe, &before));                                   \
    } while (0)

    REJECTS("a foreign magic is refused", bad[0] = 'X');
    REJECTS("a foreign version is refused", bad[4] = 9);
    REJECTS("a flipped payload bit is refused", bad[8] ^= 0x01);
    REJECTS("a flipped checksum bit is refused",
            bad[RADAR_RECORD_SIZE - 1] ^= 0x80);

#undef REJECTS

    /* Length is checked before anything else, so a truncated or padded file
     * is refused whatever its contents say. */
    probe = before;
    check("a truncated record is refused",
          radar_record_decode(&probe, good, RADAR_RECORD_SIZE - 1) == -1 &&
          same(&probe, &before));
    probe = before;
    check("an over-long record is refused",
          radar_record_decode(&probe, good, RADAR_RECORD_SIZE + 1) == -1 &&
          same(&probe, &before));
    probe = before;
    check("an empty buffer is refused",
          radar_record_decode(&probe, good, 0) == -1 && same(&probe, &before));
}

static void test_impossible_records(void)
{
    uint8_t buf[RADAR_RECORD_SIZE];
    struct radar_record probe;
    struct radar_record r;

    /* Each of these is a correctly checksummed record of the right version
     * and length that the game could never have written. The checksum
     * cannot tell them apart from a real one; the invariants can. */
#define REFUSES(name, setup)                                                 \
    do {                                                                     \
        r = sample();                                                         \
        setup;                                                                \
        encode_raw(&r, buf);                                                  \
        memset(&probe, 0, sizeof(probe));                                     \
        check(name, radar_record_decode(&probe, buf, sizeof(buf)) == -1);     \
    } while (0)

    REFUSES("a level above the ceiling is refused",
            r.best_level = RADAR_LEVEL_MAX + 1);
    REFUSES("a streak longer than every engagement is refused",
            r.best_streak = (uint16_t)(r.engaged + 1));
    REFUSES("a score with no engagements behind it is refused",
            r.engaged = 0; r.best_streak = 0);
    REFUSES("counters without a run behind them are refused", r.runs = 0);

#undef REFUSES

    /* And the boundary cases that are legal, so the checks are not simply
     * rejecting everything. */
    r = sample();
    r.best_level = RADAR_LEVEL_MAX;
    encode_raw(&r, buf);
    check("the highest level is accepted",
          radar_record_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.best_streak = (uint16_t)r.engaged;
    encode_raw(&r, buf);
    check("a streak of every engagement is accepted",
          radar_record_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.best_score = 0;
    r.best_streak = 0;
    r.engaged = 0;
    encode_raw(&r, buf);
    check("a run that engaged nothing is accepted",
          radar_record_decode(&probe, buf, sizeof(buf)) == 0);
}

static void test_file(const char *dir)
{
    struct radar_record in = sample();
    struct radar_record out;
    struct stat st;
    char tmp[512];

    check("the path is under the state directory",
          strstr(radar_store_path(), dir) == radar_store_path() &&
          strstr(radar_store_path(), RADAR_STORE_SUBDIR) != NULL &&
          strstr(radar_store_path(), RADAR_STORE_FILE) != NULL);

    /* Nothing stored yet: not an error, and the caller keeps the empty
     * record it came in with. */
    out = sample();
    check("a missing record is not an error", radar_store_load(&out) == 1);
    check("a missing record leaves the caller alone", same(&out, &in));
    check("clearing nothing succeeds", radar_store_clear() == 0);

    /* The directory does not exist yet either; saving creates it. */
    check("a record is stored", radar_store_save(&in) == 0);
    check("the file is exactly one record",
          stat(radar_store_path(), &st) == 0 && st.st_size == RADAR_RECORD_SIZE);
    snprintf(tmp, sizeof(tmp), "%s.tmp", radar_store_path());
    check("the temporary file is gone", stat(tmp, &st) != 0);

    memset(&out, 0, sizeof(out));
    check("a record is read back", radar_store_load(&out) == 0);
    check("the record survived the file", same(&in, &out));

    /* A better run overwrites in place. */
    in.best_score = 9999u;
    in.runs = 18u;
    check("a record is replaced", radar_store_save(&in) == 0);
    check("the replacement is what comes back",
          radar_store_load(&out) == 0 && same(&in, &out));
    check("replacing leaves one file",
          stat(radar_store_path(), &st) == 0 && st.st_size == RADAR_RECORD_SIZE);

    /* A damaged file is refused and left where it is, so a person can look
     * at it; the app simply starts with no best score. */
    {
        FILE *f = fopen(radar_store_path(), "r+b");

        check("the record can be opened for damage", f != NULL);
        if (f) {
            fseek(f, 8, SEEK_SET);
            fputc(0x00, f);
            fputc(0xFF, f);
            fclose(f);
        }
    }
    {
        struct radar_record untouched = sample();

        out = untouched;
        check("a damaged record is refused", radar_store_load(&out) == -1);
        check("a damaged record leaves the caller alone", same(&out, &untouched));
        check("a damaged record is left in place",
              stat(radar_store_path(), &st) == 0);
    }

    /* A file of exactly the right size that is not ours at all. */
    {
        FILE *f = fopen(radar_store_path(), "wb");
        int i;

        if (f) {
            for (i = 0; i < RADAR_RECORD_SIZE; i++) {
                fputc('Z', f);
            }
            fclose(f);
        }
        check("a foreign file of the right size is refused",
              radar_store_load(&out) == -1);
    }

    check("a record can be cleared", radar_store_clear() == 0);
    check("a cleared record is gone", stat(radar_store_path(), &st) != 0);
    check("clearing twice still succeeds", radar_store_clear() == 0);
    out = sample();
    check("after clearing there is nothing to read", radar_store_load(&out) == 1);

    check("NULL is refused",
          radar_store_save(NULL) == -1 && radar_store_load(NULL) == -1);
}

/* The whole persistence path as the app will use it: play a run to its end,
 * fold it into the record, store it, and read it back the way the next
 * launch would. The point is that a record written by a real run always
 * decodes. An invariant that quietly rejected a legitimate record would
 * lose the best score of everybody who reached it, and that failure would
 * only ever be seen by the players it happened to. */
static void test_a_real_run(void)
{
    struct radar_record record;
    struct radar_record loaded;
    struct radar_run run;
    uint32_t seed;
    int all_stored = 1;
    int improved = 0;
    int fumbles = 0;

    radar_record_init(&record);
    radar_store_clear();

    for (seed = 1u; seed <= 12u; seed++) {
        int guard;

        radar_run_new(&run, seed * 101u);
        radar_run_start(&run);
        for (guard = 0; guard < 40000 && !radar_run_is_over(&run); guard++) {
            const struct radar_contact *sel = radar_run_selected(&run);

            if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
                if (radar_class_is_target((enum radar_class)sel->cls)) {
                    radar_run_engage(&run);
                } else if (fumbles++ % 4 == 0) {
                    /* Every fourth identified decoy is engaged anyway. A
                     * player makes that mistake and the record has to carry
                     * it, so the fixture makes it too. */
                    radar_run_engage(&run);
                } else {
                    radar_run_deselect(&run);
                }
            } else if (!sel) {
                int i;

                for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                    const struct radar_contact *t = radar_run_slot(&run, i);

                    if (t->active && !t->classified) {
                        radar_run_select(&run, t->id);
                        break;
                    }
                }
            }
            radar_run_tick(&run);
            radar_run_clear_events(&run);
        }
        all_stored &= radar_run_is_over(&run);
        improved |= radar_record_note_run(&record, &run.score,
                                          radar_run_level(&run));
        all_stored &= radar_store_save(&record) == 0;
        memset(&loaded, 0, sizeof(loaded));
        all_stored &= radar_store_load(&loaded) == 0 && same(&record, &loaded);
    }
    check("twelve runs were played to the end", record.runs == 12u);
    check("they engaged and mis-engaged things",
          record.engaged > 0 && record.mistakes > 0);
    check("one of them set a best score", improved && record.best_score > 0);
    check("every record a real run produced stored and reloaded", all_stored);
    check("the record stayed inside its own invariants",
          record.best_streak <= record.engaged &&
          record.best_level <= RADAR_LEVEL_MAX && record.best_level > 0);
    radar_store_clear();
}

static void test_unwritable(const char *dir)
{
    struct radar_record in = sample();
    char sub[512];

    /* A filesystem that says no must be reported, never fatal. Running as
     * root would defeat the permission, so the case is skipped rather than
     * failed. */
    if (geteuid() == 0) {
        printf("skip unwritable-directory checks (running as root)\n");
        return;
    }
    snprintf(sub, sizeof(sub), "%s/locked", dir);
    if (mkdir(sub, 0755) != 0) {
        check("the locked directory was created", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", sub, 1);
    chmod(sub, 0555);
    check("an unwritable directory reports failure", radar_store_save(&in) == -1);
    check("an unwritable directory has nothing to load",
          radar_store_load(&in) == 1);
    chmod(sub, 0755);
    check("the same store works once it is writable again",
          radar_store_save(&in) == 0);
    setenv("POCKETOS_STATE_DIR", dir, 1);
}

int main(void)
{
    char dir[] = "/tmp/pos_radar.XXXXXX";
    char cmd[512];

    test_codec();
    test_damaged_blobs();
    test_impossible_records();

    if (!mkdtemp(dir)) {
        printf("FAIL could not make a temporary directory\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    test_file(dir);
    test_a_real_run();
    test_unwritable(dir);
    unsetenv("POCKETOS_STATE_DIR");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        printf("     could not remove %s\n", dir);
    }

    printf("radar_store_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
