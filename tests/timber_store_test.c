/*
 * PocketTimber record store test: the codec, its refusal of anything that
 * is not a record the game could have written, the bookkeeping of a
 * finished run, and the file behaviour the app depends on: atomic writes,
 * a missing file that is not an error, a damaged or truncated one that
 * never stops play, and a write failure that loses nothing already stored.
 *
 * The filesystem cases drive the real paths through POCKETOS_STATE_DIR in
 * a temporary directory made here; nothing touches PocketOS's own storage.
 * They are POSIX and run natively; nothing here has been exercised on the
 * K230.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "timber_store.h"

#include "timber_pull.h"
#include "timber_rules.h"
#include "timber_summit.h"
#include "timber_tower.h"

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

static struct timber_records sample(void)
{
    struct timber_records r;

    timber_records_init(&r);
    r.core.best_score = 10359u;
    r.core.best_height = 29u;
    r.core.best_streak = 7u;
    r.core.runs = 17u;
    r.core.pulls = 380u;
    r.best_pulls = 33u;
    r.collapses = 15u;
    r.summits = 2u;
    r.causes[TIMBER_CAUSE_TIP - 1] = 6u;
    r.causes[TIMBER_CAUSE_JOLT - 1] = 5u;
    r.causes[TIMBER_CAUSE_PLACEMENT - 1] = 3u;
    r.causes[TIMBER_CAUSE_SWAY - 1] = 1u;
    return r;
}

static int same(const struct timber_records *a, const struct timber_records *b)
{
    int i;

    if (a->core.best_score != b->core.best_score || a->core.best_height != b->core.best_height ||
        a->core.best_streak != b->core.best_streak || a->core.runs != b->core.runs ||
        a->core.pulls != b->core.pulls || a->best_pulls != b->best_pulls ||
        a->collapses != b->collapses || a->summits != b->summits) {
        return 0;
    }
    for (i = 0; i < TIMBER_RECORDS_CAUSES; i++) {
        if (a->causes[i] != b->causes[i]) {
            return 0;
        }
    }
    return 1;
}

/* Encode a record without validating it, so an impossible one can be
 * handed to the decoder with a checksum that is perfectly correct.
 * Otherwise the checksum would reject it first and the invariant checks
 * would never run. */
static void encode_raw(const struct timber_records *r, uint8_t *buf)
{
    if (timber_records_encode(r, buf, TIMBER_RECORDS_SIZE) != TIMBER_RECORDS_SIZE) {
        check("the fixture encodes", 0);
    }
}

static void test_codec(void)
{
    struct timber_records in = sample();
    struct timber_records out;
    uint8_t buf[TIMBER_RECORDS_SIZE + 4];
    uint8_t small[TIMBER_RECORDS_SIZE - 1];

    memset(&out, 0xAB, sizeof(out));
    check("a record encodes to its stated size",
          timber_records_encode(&in, buf, sizeof(buf)) == TIMBER_RECORDS_SIZE);
    check("a record is fifty-two bytes", TIMBER_RECORDS_SIZE == 52);
    check("the magic is PTR1",
          buf[0] == 'P' && buf[1] == 'T' && buf[2] == 'R' && buf[3] == '1');
    check("the version follows the magic, little-endian",
          buf[4] == TIMBER_RECORDS_VERSION && buf[5] == 0);
    check("a record decodes", timber_records_decode(&out, buf, TIMBER_RECORDS_SIZE) == 0);
    check("a record survives the round trip", same(&in, &out));

    check("a buffer that is too small is refused",
          timber_records_encode(&in, small, sizeof(small)) == -1);
    check("NULL is refused",
          timber_records_encode(NULL, buf, sizeof(buf)) == -1 &&
          timber_records_encode(&in, NULL, sizeof(buf)) == -1 &&
          timber_records_decode(NULL, buf, TIMBER_RECORDS_SIZE) == -1 &&
          timber_records_decode(&out, NULL, TIMBER_RECORDS_SIZE) == -1);

    /* An empty record is a legal one: it is what a first run starts from. */
    {
        struct timber_records empty;
        struct timber_records back;

        timber_records_init(&empty);
        timber_records_encode(&empty, buf, sizeof(buf));
        memset(&back, 0xCD, sizeof(back));
        check("an empty record round-trips",
              timber_records_decode(&back, buf, TIMBER_RECORDS_SIZE) == 0 &&
              same(&empty, &back));
    }
}

static void test_damaged_blobs(void)
{
    struct timber_records in = sample();
    struct timber_records probe;
    uint8_t good[TIMBER_RECORDS_SIZE];
    uint8_t bad[TIMBER_RECORDS_SIZE];
    struct timber_records before;

    timber_records_encode(&in, good, sizeof(good));
    before = sample();

#define REJECTS(name, mutation)                                              \
    do {                                                                     \
        memcpy(bad, good, sizeof(bad));                                       \
        mutation;                                                             \
        probe = before;                                                       \
        check(name, timber_records_decode(&probe, bad, sizeof(bad)) == -1 &&  \
                    same(&probe, &before));                                   \
    } while (0)

    REJECTS("a foreign magic is refused", bad[0] = 'X');
    REJECTS("PocketRadar's magic is refused", bad[1] = 'R');
    REJECTS("a newer version is refused", bad[4] = TIMBER_RECORDS_VERSION + 1);
    REJECTS("version zero is refused", bad[4] = 0);
    REJECTS("a flipped payload bit is refused", bad[8] ^= 0x01);
    REJECTS("a flipped counter bit is refused", bad[30] ^= 0x10);
    REJECTS("a flipped checksum bit is refused",
            bad[TIMBER_RECORDS_SIZE - 1] ^= 0x80);
    REJECTS("all zeros is refused", memset(bad, 0, sizeof(bad)));
    REJECTS("all ones is refused", memset(bad, 0xFF, sizeof(bad)));

#undef REJECTS

    /* Length is checked before anything else, so a truncated or padded
     * file is refused whatever its contents say. */
    probe = before;
    check("a truncated record is refused",
          timber_records_decode(&probe, good, TIMBER_RECORDS_SIZE - 1) == -1 &&
          same(&probe, &before));
    probe = before;
    check("a record cut in half is refused",
          timber_records_decode(&probe, good, TIMBER_RECORDS_SIZE / 2) == -1 &&
          same(&probe, &before));
    probe = before;
    check("an over-long record is refused",
          timber_records_decode(&probe, good, TIMBER_RECORDS_SIZE + 1) == -1 &&
          same(&probe, &before));
    probe = before;
    check("an empty buffer is refused",
          timber_records_decode(&probe, good, 0) == -1 && same(&probe, &before));
}

static void test_impossible_records(void)
{
    uint8_t buf[TIMBER_RECORDS_SIZE];
    struct timber_records probe;
    struct timber_records r;

    /* Each of these is a correctly checksummed record of the right version
     * and length that the game could never have written. The checksum
     * cannot tell them apart from a real one; the invariants can. */
#define REFUSES(name, setup)                                                 \
    do {                                                                     \
        r = sample();                                                         \
        setup;                                                                \
        encode_raw(&r, buf);                                                  \
        memset(&probe, 0, sizeof(probe));                                     \
        check(name, timber_records_decode(&probe, buf, sizeof(buf)) == -1);   \
    } while (0)

    REFUSES("a tower above the summit is refused",
            r.core.best_height = TIMBER_LAYERS_MAX + 1);
    REFUSES("more pulls in a run than the tower could take is refused",
            r.best_pulls = TIMBER_SLOTS * TIMBER_LAYERS_MAX + 1;
            r.core.pulls = 100000u);
    REFUSES("a streak longer than the best run's pulls is refused",
            r.core.best_streak = (uint16_t)(r.best_pulls + 1));
    REFUSES("a lifetime of fewer pulls than the best run is refused",
            r.core.pulls = r.best_pulls - 1);
    REFUSES("a score with no pull behind it is refused",
            r.core.pulls = 0; r.best_pulls = 0; r.core.best_streak = 0);
    REFUSES("a score no run could reach is refused",
            r.core.best_score = timber_records_score_ceiling() + 1);
    REFUSES("an absurd score is refused", r.core.best_score = 0xFFFFFFFFu);
    REFUSES("runs that neither fell nor stood are refused", r.core.runs++);
    REFUSES("more falls than runs are refused", r.collapses++);
    REFUSES("more causes than falls are refused", r.causes[0]++);
    REFUSES("counters without a run behind them are refused",
            r.core.runs = 0; r.collapses = 0; r.summits = 0;
            memset(r.causes, 0, sizeof(r.causes)));
    REFUSES("wrapped outcome counters are refused",
            r.collapses = 0xFFFFFFFFu; r.summits = 2u; r.core.runs = 1u);
    REFUSES("wrapped cause counters are refused",
            r.causes[0] = 0xFFFFFFFFu; r.causes[1] = 2u; r.collapses = 1u; r.summits = 16u);

#undef REFUSES

    /* And the boundary cases that are legal, so the checks are not simply
     * rejecting everything. */
    r = sample();
    r.core.best_height = TIMBER_LAYERS_MAX;
    encode_raw(&r, buf);
    check("the summit's height is accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.best_pulls = TIMBER_SLOTS * TIMBER_LAYERS_MAX;
    r.core.pulls = 1000u;
    encode_raw(&r, buf);
    check("the most pulls a run can hold is accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.core.best_streak = r.best_pulls;
    encode_raw(&r, buf);
    check("a streak of every pull is accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.core.best_score = timber_records_score_ceiling();
    encode_raw(&r, buf);
    check("the score ceiling itself is accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);
    check("the ceiling is far above a real run and far below absurd",
          timber_records_score_ceiling() > 50000u && timber_records_score_ceiling() < 10000000u);

    r = sample();
    r.summits = 0;
    r.collapses = r.core.runs;
    r.causes[0] = r.core.runs - 9u;
    encode_raw(&r, buf);
    check("a life with no summit is accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    memset(r.causes, 0, sizeof(r.causes));
    encode_raw(&r, buf);
    check("falls with no cause counted are accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);

    r = sample();
    r.core.best_score = 0;
    r.core.pulls = 0;
    r.best_pulls = 0;
    r.core.best_streak = 0;
    encode_raw(&r, buf);
    check("runs that pulled nothing are accepted",
          timber_records_decode(&probe, buf, sizeof(buf)) == 0);
}

/* The bookkeeping of a finished run: the engine's fold is reused, the
 * outcome is counted, and the best never regresses. */
static void test_note_run(void)
{
    struct timber_records r;
    struct timber_score s;
    uint8_t buf[TIMBER_RECORDS_SIZE];
    struct timber_records back;
    int i;

    timber_records_init(&r);
    timber_score_init(&s);
    s.points = 4200;
    s.pulls = 12;
    s.best_streak = 4;
    s.height = 22;
    check("the first run is a new best", timber_records_note_run(&r, &s, TIMBER_CAUSE_JOLT) == 1);
    check("one run, one fall, one jolt",
          r.core.runs == 1 && r.collapses == 1 && r.summits == 0 &&
          r.causes[TIMBER_CAUSE_JOLT - 1] == 1 && r.causes[TIMBER_CAUSE_TIP - 1] == 0);
    check("the run's numbers were kept",
          r.core.best_score == 4200 && r.best_pulls == 12 && r.core.pulls == 12 &&
          r.core.best_streak == 4 && r.core.best_height == 22);

    s.points = 3100;
    s.pulls = 15;
    s.best_streak = 2;
    s.height = 23;
    check("a lesser run is not a new best", timber_records_note_run(&r, &s, TIMBER_CAUSE_TIP) == 0);
    check("the best score did not regress", r.core.best_score == 4200);
    check("but the other bests move on their own",
          r.best_pulls == 15 && r.core.best_height == 23 && r.core.best_streak == 4);
    check("the lifetime pulls add up", r.core.pulls == 27);
    check("two runs, two falls", r.core.runs == 2 && r.collapses == 2 &&
                                   r.causes[TIMBER_CAUSE_TIP - 1] == 1);

    s.points = 4200;
    check("an equal score is not a new best", timber_records_note_run(&r, &s, TIMBER_CAUSE_SWAY) == 0);
    s.points = 4201;
    check("one point more is", timber_records_note_run(&r, &s, TIMBER_CAUSE_PLACEMENT) == 1);
    check("every cause has its counter",
          r.causes[TIMBER_CAUSE_TIP - 1] == 1 && r.causes[TIMBER_CAUSE_JOLT - 1] == 1 &&
          r.causes[TIMBER_CAUSE_PLACEMENT - 1] == 1 && r.causes[TIMBER_CAUSE_SWAY - 1] == 1);

    s.points = 9000;
    s.pulls = 54;
    s.height = TIMBER_LAYERS_MAX;
    check("a run that stood at the summit is a summit",
          timber_records_note_run(&r, &s, TIMBER_CAUSE_NONE) == 1 && r.summits == 1 &&
          r.collapses == 4 && r.core.runs == 5);

    /* A cause the engine does not have is still a fall, just not a counted
     * one, so the counters keep their invariant. */
    s.points = 1;
    timber_records_note_run(&r, &s, TIMBER_CAUSE_COUNT + 3);
    timber_records_note_run(&r, &s, -1);
    check("an unknown cause is a fall without a counter",
          r.collapses == 6 && r.core.runs == 7 &&
          r.causes[0] + r.causes[1] + r.causes[2] + r.causes[3] == 4);

    /* Everything the bookkeeping produces is something the codec accepts:
     * a hundred runs of every outcome, and the record always decodes. */
    for (i = 0; i < 100; i++) {
        s.points = (i * 977) % 12000;
        s.pulls = (uint16_t)(1 + i % 40);
        s.best_streak = (uint16_t)(i % 5);
        s.height = (uint8_t)(18 + i % 19);
        timber_records_note_run(&r, &s, i % (TIMBER_CAUSE_COUNT + 1));
    }
    encode_raw(&r, buf);
    memset(&back, 0, sizeof(back));
    check("a long life of bookkeeping still decodes",
          timber_records_decode(&back, buf, sizeof(buf)) == 0 && same(&r, &back));
    check("it counted every run", r.core.runs == 107 && r.collapses + r.summits == 107);

    check("NULL is ignored",
          timber_records_note_run(NULL, &s, 0) == 0 && timber_records_note_run(&r, NULL, 0) == 0 &&
          r.core.runs == 107);
}

static void write_bytes(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void test_file(const char *dir)
{
    struct timber_records in = sample();
    struct timber_records out;
    struct stat st;
    char tmp[600];
    uint8_t blob[TIMBER_RECORDS_SIZE];

    check("the path is under the state directory",
          strstr(timber_store_path(), dir) == timber_store_path() &&
          strstr(timber_store_path(), "/" TIMBER_STORE_SUBDIR "/") != NULL &&
          strstr(timber_store_path(), TIMBER_STORE_FILE) != NULL);
    check("the path is not PocketOS's own",
          strstr(timber_store_path(), TIMBER_STORE_DEFAULT_DIR) == NULL);

    /* Nothing stored yet: not an error, and the caller keeps the defaults
     * it came in with. */
    out = sample();
    check("a missing record is not an error", timber_store_load(&out) == 1);
    check("a missing record leaves the caller alone", same(&out, &in));
    check("clearing nothing succeeds", timber_store_clear() == 0);

    /* The directory does not exist yet either; saving creates it. */
    check("a record is stored", timber_store_save(&in) == 0);
    check("the file is exactly one record",
          stat(timber_store_path(), &st) == 0 && st.st_size == TIMBER_RECORDS_SIZE);
    snprintf(tmp, sizeof(tmp), "%s.tmp", timber_store_path());
    check("the temporary file is gone", stat(tmp, &st) != 0);

    memset(&out, 0, sizeof(out));
    check("a record is read back", timber_store_load(&out) == 0);
    check("the record survived the file", same(&in, &out));

    /* A better run overwrites in place, and repeated updates keep one file. */
    in.core.best_score = 12000u;
    in.core.runs = 18u;
    in.collapses = 16u;
    in.core.pulls += 20u;
    check("a record is replaced", timber_store_save(&in) == 0);
    check("the replacement is what comes back",
          timber_store_load(&out) == 0 && same(&in, &out));
    in.core.runs = 19u;
    in.summits = 3u;
    check("and replaced again", timber_store_save(&in) == 0 && timber_store_load(&out) == 0 &&
                                    same(&in, &out));
    check("replacing leaves one file",
          stat(timber_store_path(), &st) == 0 && st.st_size == TIMBER_RECORDS_SIZE);

    /* A temporary file an interrupted write left behind neither hides the
     * record nor survives the next save. */
    write_bytes(tmp, "half a record", 13);
    check("a stale temporary file does not hide the record",
          timber_store_load(&out) == 0 && same(&in, &out));
    check("the next save replaces it", timber_store_save(&in) == 0 && stat(tmp, &st) != 0);

    /* A damaged file is refused and left where it is, so a person can look
     * at it; the app simply starts with no best score. */
    {
        FILE *f = fopen(timber_store_path(), "r+b");

        check("the record can be opened for damage", f != NULL);
        if (f) {
            fseek(f, 8, SEEK_SET);
            fputc(0x00, f);
            fputc(0xFF, f);
            fclose(f);
        }
    }
    {
        struct timber_records untouched = sample();

        out = untouched;
        check("a damaged record is refused", timber_store_load(&out) == -1);
        check("a damaged record leaves the caller alone", same(&out, &untouched));
        check("a damaged record is left in place",
              stat(timber_store_path(), &st) == 0 && st.st_size == TIMBER_RECORDS_SIZE);
    }

    /* A truncated file: the record with its tail cut off. */
    timber_records_encode(&in, blob, sizeof(blob));
    write_bytes(timber_store_path(), blob, TIMBER_RECORDS_SIZE - 7);
    out = in;
    check("a truncated record is refused", timber_store_load(&out) == -1);
    check("a truncated record is left in place",
          stat(timber_store_path(), &st) == 0 && st.st_size == TIMBER_RECORDS_SIZE - 7);
    write_bytes(timber_store_path(), blob, 0);
    check("an empty file is refused", timber_store_load(&out) == -1);

    /* A padded file: the record with something after it. */
    {
        uint8_t padded[TIMBER_RECORDS_SIZE + 3];

        memcpy(padded, blob, sizeof(blob));
        memset(padded + TIMBER_RECORDS_SIZE, 0, 3);
        write_bytes(timber_store_path(), padded, sizeof(padded));
        check("a padded record is refused", timber_store_load(&out) == -1);
    }

    /* A file of exactly the right size that is not ours at all, and one
     * from a version this build does not know. */
    {
        uint8_t foreign[TIMBER_RECORDS_SIZE];

        memset(foreign, 'Z', sizeof(foreign));
        write_bytes(timber_store_path(), foreign, sizeof(foreign));
        check("a foreign file of the right size is refused",
              timber_store_load(&out) == -1);
        memcpy(foreign, blob, sizeof(blob));
        foreign[4] = TIMBER_RECORDS_VERSION + 1;
        write_bytes(timber_store_path(), foreign, sizeof(foreign));
        check("a record from a later version is refused",
              timber_store_load(&out) == -1);
    }
    check("every refusal left the caller's defaults alone", same(&out, &in));

    /* And after all that, a good save still works and reads back. */
    check("a record is stored over the wreckage", timber_store_save(&in) == 0);
    check("and read back", timber_store_load(&out) == 0 && same(&in, &out));

    check("a record can be cleared", timber_store_clear() == 0);
    check("a cleared record is gone", stat(timber_store_path(), &st) != 0);
    check("clearing twice still succeeds", timber_store_clear() == 0);
    out = sample();
    check("after clearing there is nothing to read", timber_store_load(&out) == 1);

    check("NULL is refused",
          timber_store_save(NULL) == -1 && timber_store_load(NULL) == -1);
}

/* Pull the loosest pullable centre block slowly until it is in hand;
 * failing that, any pullable block. Returns 0 when a block is in hand. */
static int pull_one(struct timber_run *run)
{
    int best = -1;
    int best_seat = -1;
    int id;
    int guard;

    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &run->tower.blocks[id];

        if (timber_tower_pullable(&run->tower, id) &&
            (b->slot == 1 ? b->seat + 1000 : b->seat) > best_seat) {
            best_seat = b->slot == 1 ? b->seat + 1000 : b->seat;
            best = id;
        }
    }
    if (best < 0 || timber_run_select(run, best) != 0) {
        return -1;
    }
    for (guard = 0; guard < 200 && run->turn != TIMBER_TURN_PLACING &&
                    run->state == TIMBER_RUN_ACTIVE; guard++) {
        timber_run_tick(run);
        timber_run_pull(run, timber_pull_limit(TIMBER_CLASS_STUCK) * 4 / 5);
        timber_run_clear_events(run);
    }
    return run->turn == TIMBER_TURN_PLACING ? 0 : -1;
}

/* The whole persistence path as the app will use it: play runs to their
 * end, fold each into the records, store them, and read them back the way
 * the next launch would. The point is that a record written by a real run
 * always decodes. An invariant that quietly rejected a legitimate record
 * would lose the best score of everybody who reached it, and that failure
 * would only ever be seen by the players it happened to. */
static void test_real_runs(void)
{
    struct timber_records records;
    struct timber_records loaded;
    struct timber_run run;
    uint32_t seed;
    int all_stored = 1;
    int all_over = 1;
    int improved = 0;
    int turn;

    timber_records_init(&records);
    timber_store_clear();

    for (seed = 1u; seed <= 6u; seed++) {
        timber_run_new(&run, seed * 7919u);
        timber_run_start(&run);
        for (turn = 0; turn < 400 && run.state == TIMBER_RUN_ACTIVE; turn++) {
            int i;

            if (pull_one(&run) == 0) {
                timber_run_place(&run, (turn + (int)seed) % TIMBER_SLOTS);
            }
            for (i = 0; i < 30 && !timber_run_is_over(&run); i++) {
                timber_run_tick(&run);
                timber_run_clear_events(&run);
            }
        }
        for (turn = 0; turn < TIMBER_COLLAPSE_TICKS_MAX + 2 && !timber_run_is_over(&run); turn++) {
            timber_run_tick(&run);
            timber_run_clear_events(&run);
        }
        all_over &= timber_run_is_over(&run);
        improved |= timber_records_note_run(&records, &run.score, timber_run_cause(&run));
        all_stored &= timber_store_save(&records) == 0;
        memset(&loaded, 0, sizeof(loaded));
        all_stored &= timber_store_load(&loaded) == 0 && same(&records, &loaded);
    }
    /* And one run that ends standing: the summit fixture, its last block
     * placed, is the best standing result there is. */
    summit_build(&run, 4242u);
    timber_run_place(&run, 1);
    for (turn = 0; turn < 8 && !timber_run_is_over(&run); turn++) {
        timber_run_tick(&run);
        timber_run_clear_events(&run);
    }
    check("the summit run ended standing",
          timber_run_is_over(&run) && timber_run_cause(&run) == TIMBER_CAUSE_NONE);
    all_over &= timber_run_is_over(&run);
    timber_records_note_run(&records, &run.score, timber_run_cause(&run));
    all_stored &= timber_store_save(&records) == 0;
    memset(&loaded, 0, sizeof(loaded));
    all_stored &= timber_store_load(&loaded) == 0 && same(&records, &loaded);
    check("the summit was counted as a summit at the summit's height",
          records.summits == 1u && records.core.best_height == TIMBER_LAYERS_MAX);

    check("seven runs were played to the end", all_over && records.core.runs == 7u);
    check("they pulled blocks and the towers fell or stood",
          records.core.pulls > 0 && records.best_pulls > 0 &&
          records.collapses + records.summits == 7u);
    check("one of them set a best score", improved && records.core.best_score > 0);
    check("every record a real run produced stored and reloaded", all_stored);
    check("the records stayed inside their own invariants",
          records.core.best_streak <= records.best_pulls &&
          records.core.best_height <= TIMBER_LAYERS_MAX &&
          records.core.best_height >= TIMBER_LAYERS_BASE);
    printf("     %u runs: best %u, %u fell (tip %u, jolt %u, placement %u, sway %u), %u stood\n",
           (unsigned)records.core.runs, (unsigned)records.core.best_score,
           (unsigned)records.collapses, (unsigned)records.causes[0], (unsigned)records.causes[1],
           (unsigned)records.causes[2], (unsigned)records.causes[3], (unsigned)records.summits);
    timber_store_clear();
}

static void test_unwritable(const char *dir)
{
    struct timber_records in = sample();
    struct timber_records newer = sample();
    struct timber_records out;
    char sub[600];
    char locked[700];            /* sub plus a subdirectory, so the compiler sees room */

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
    check("an unwritable directory reports failure", timber_store_save(&in) == -1);
    check("an unwritable directory has nothing to load",
          timber_store_load(&in) == 1);
    chmod(sub, 0755);
    check("the same store works once it is writable again",
          timber_store_save(&in) == 0);

    /* A write that fails after a record exists loses nothing: the previous
     * record is what the next launch reads. */
    snprintf(locked, sizeof(locked), "%s/%s", sub, TIMBER_STORE_SUBDIR);
    chmod(locked, 0555);
    newer.core.best_score = 20000u;
    newer.core.runs = 18u;
    newer.collapses = 16u;
    check("a failed replacement reports failure", timber_store_save(&newer) == -1);
    memset(&out, 0, sizeof(out));
    check("the previous record is intact",
          timber_store_load(&out) == 0 && same(&out, &in));
    chmod(locked, 0755);
    check("the replacement lands once the directory allows it",
          timber_store_save(&newer) == 0 && timber_store_load(&out) == 0 && same(&out, &newer));
    setenv("POCKETOS_STATE_DIR", dir, 1);
}

int main(void)
{
    char dir[] = "/tmp/pos_timber.XXXXXX";
    char cmd[512];

    test_codec();
    test_damaged_blobs();
    test_impossible_records();
    test_note_run();

    if (!mkdtemp(dir)) {
        printf("FAIL could not make a temporary directory\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    test_file(dir);
    test_real_runs();
    test_unwritable(dir);
    unsetenv("POCKETOS_STATE_DIR");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        printf("     could not remove %s\n", dir);
    }

    printf("timber_store_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
