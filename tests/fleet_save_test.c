/*
 * PocketFleet persistence test: the save codec (round trip, rejection of
 * damaged and impossible saves) and the save file (atomic write, absent
 * file, corruption, unwritable directory).
 *
 * The important one is "resuming continues the same match": a match saved
 * mid-game and reloaded must play out exactly as the one that was never
 * interrupted, including every AI decision.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "fleet_save.h"
#include "fleet_store.h"

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

static uint32_t fnv1a(const uint8_t *data, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* Re-checksum a tampered blob so the semantic checks are what reject it. */
static void reseal(uint8_t *buf)
{
    uint32_t h = fnv1a(buf, FLEET_SAVE_SIZE - 4);

    buf[FLEET_SAVE_SIZE - 4] = (uint8_t)(h & 0xFFu);
    buf[FLEET_SAVE_SIZE - 3] = (uint8_t)((h >> 8) & 0xFFu);
    buf[FLEET_SAVE_SIZE - 2] = (uint8_t)((h >> 16) & 0xFFu);
    buf[FLEET_SAVE_SIZE - 1] = (uint8_t)((h >> 24) & 0xFFu);
}

static uint32_t digest_game(const struct fleet_game *g)
{
    /* Encoding is field by field, so the blob is a faithful digest of every
     * field that matters and ignores padding. */
    uint8_t blob[FLEET_SAVE_SIZE];

    if (fleet_save_encode(g, blob, sizeof(blob)) < 0) {
        return 0;
    }
    return fnv1a(blob, sizeof(blob));
}

/* The player fires at the first cell not yet fired at; the opponent plays its
 * own AI. Both sides are deterministic, so the match replays exactly. */
static void play_rounds(struct fleet_game *g, int rounds)
{
    while (rounds-- > 0 && !fleet_game_is_over(g)) {
        int i;

        for (i = 0; i < FLEET_CELLS; i++) {
            if (fleet_game_fire(g, FLEET_SIDE_PLAYER, i / FLEET_GRID, i % FLEET_GRID,
                                NULL) != FLEET_SHOT_INVALID) {
                break;
            }
        }
        if (fleet_game_is_over(g)) {
            break;
        }
        fleet_game_opponent_turn(g, NULL, NULL, NULL);
    }
}

static void start_match(struct fleet_game *g, uint32_t seed, enum fleet_difficulty d)
{
    struct fleet_rng deploy;

    fleet_game_new(g, seed, d);
    fleet_rng_seed(&deploy, seed ^ 0x5A5A5A5Au);
    fleet_board_autoplace(&g->board[FLEET_SIDE_PLAYER], &deploy);
    fleet_game_start(g);
}

static void test_codec(void)
{
    struct fleet_game g;
    struct fleet_game back;
    struct fleet_game untouched;
    uint8_t blob[FLEET_SAVE_SIZE];
    uint8_t bad[FLEET_SAVE_SIZE];

    check("save size is the documented constant", fleet_save_size() == FLEET_SAVE_SIZE);

    /* A match still in deployment. */
    fleet_game_new(&g, 4321u, FLEET_ADMIRAL);
    check("deployment encodes", fleet_save_encode(&g, blob, sizeof(blob)) == FLEET_SAVE_SIZE);
    check("deployment round trips", fleet_save_decode(&back, blob, sizeof(blob)) == 0 &&
                                    digest_game(&back) == digest_game(&g));

    /* A match in progress. */
    start_match(&g, 4321u, FLEET_ADMIRAL);
    play_rounds(&g, 9);
    check("mid-game encodes", fleet_save_encode(&g, blob, sizeof(blob)) == FLEET_SAVE_SIZE);
    check("mid-game round trips", fleet_save_decode(&back, blob, sizeof(blob)) == 0 &&
                                  digest_game(&back) == digest_game(&g));
    check("the AI record survives", back.ai.shots == g.ai.shots &&
          memcmp(back.ai.shot, g.ai.shot, FLEET_CELLS) == 0 &&
          memcmp(back.ai.result, g.ai.result, FLEET_CELLS) == 0 &&
          memcmp(back.ai.resolved, g.ai.resolved, FLEET_CELLS) == 0 &&
          back.ai.queue_len == g.ai.queue_len);
    check("the streams survive", back.rng_ai.state == g.rng_ai.state &&
                                 back.rng_setup.state == g.rng_setup.state);

    /* A finished match. */
    play_rounds(&g, 200);
    check("the match finished", fleet_game_is_over(&g));
    check("a finished match round trips",
          fleet_save_encode(&g, blob, sizeof(blob)) == FLEET_SAVE_SIZE &&
          fleet_save_decode(&back, blob, sizeof(blob)) == 0 &&
          digest_game(&back) == digest_game(&g));

    check("a short buffer is refused", fleet_save_encode(&g, blob, FLEET_SAVE_SIZE - 1) == -1);
    check("NULL is refused", fleet_save_encode(NULL, blob, sizeof(blob)) == -1 &&
                             fleet_save_encode(&g, NULL, sizeof(blob)) == -1 &&
                             fleet_save_decode(&back, NULL, sizeof(blob)) == -1 &&
                             fleet_save_decode(NULL, blob, sizeof(blob)) == -1);

    /* Damaged blobs. The caller's match must survive every rejection. */
    start_match(&untouched, 99u, FLEET_OFFICER);
    play_rounds(&untouched, 4);
    fleet_save_encode(&g, blob, sizeof(blob));

    memcpy(bad, blob, sizeof(bad));
    bad[0] = 'X';
    {
        struct fleet_game victim = untouched;

        check("a foreign blob is refused", fleet_save_decode(&victim, bad, sizeof(bad)) == -1);
        check("a refused load leaves the match alone",
              digest_game(&victim) == digest_game(&untouched));
    }

    memcpy(bad, blob, sizeof(bad));
    bad[4] = FLEET_SAVE_VERSION + 1;
    reseal(bad);
    check("another save version is refused", fleet_save_decode(&back, bad, sizeof(bad)) == -1);

    memcpy(bad, blob, sizeof(bad));
    bad[40] ^= 0xFFu;
    check("a damaged blob is refused", fleet_save_decode(&back, bad, sizeof(bad)) == -1);

    memcpy(bad, blob, sizeof(bad));
    check("a truncated blob is refused",
          fleet_save_decode(&back, bad, FLEET_SAVE_SIZE - 1) == -1);
}

/* Impossible matches are built as structs and encoded (encoding does not
 * validate), so the decoder's rules checks are what must reject them. */
static void test_impossible_saves(void)
{
    struct fleet_game base;
    struct fleet_game g;
    struct fleet_game back;
    uint8_t blob[FLEET_SAVE_SIZE];

    start_match(&base, 5150u, FLEET_COMMANDER);
    play_rounds(&base, 6);

#define REJECTS(name, mutation)                                            \
    do {                                                                   \
        g = base;                                                          \
        mutation;                                                          \
        fleet_save_encode(&g, blob, sizeof(blob));                         \
        check(name, fleet_save_decode(&back, blob, sizeof(blob)) == -1);   \
    } while (0)

    REJECTS("an unknown difficulty is refused", g.difficulty = FLEET_DIFFICULTY_COUNT);
    REJECTS("an unknown phase is refused", g.phase = FLEET_PHASE_OVER + 1);
    REJECTS("a winner without an ending is refused", g.winner = FLEET_SIDE_PLAYER);
    REJECTS("an ending without a winner is refused", g.phase = FLEET_PHASE_OVER);
    REJECTS("an impossible turn number is refused", g.turn = FLEET_CELLS + 2);
    REJECTS("an unknown ship on the board is refused",
            g.board[FLEET_SIDE_OPPONENT].ship_at[0] = FLEET_SHIP_COUNT);
    REJECTS("a ship claiming a cell it does not own is refused",
            g.board[FLEET_SIDE_OPPONENT].ships[FLEET_SHIP_CARRIER].row =
                (uint8_t)((g.board[FLEET_SIDE_OPPONENT].ships[FLEET_SHIP_CARRIER].row + 3) %
                          FLEET_GRID));
    REJECTS("a wrong ship count is refused", g.board[FLEET_SIDE_PLAYER].ships_placed = 4);
    REJECTS("a wrong afloat count is refused", g.board[FLEET_SIDE_PLAYER].ships_afloat = 1);
    REJECTS("more hits than hull is refused",
            g.board[FLEET_SIDE_OPPONENT].ships[FLEET_SHIP_DESTROYER].hits = 9);
    REJECTS("a placed flag out of range is refused",
            g.board[FLEET_SIDE_PLAYER].ships[FLEET_SHIP_CRUISER].placed = 2);
    REJECTS("more hits than shots is refused",
            g.stats[FLEET_SIDE_PLAYER].hits = (uint16_t)(g.stats[FLEET_SIDE_PLAYER].shots + 1));
    REJECTS("an AI difficulty that disagrees is refused",
            g.ai.difficulty = (uint8_t)(g.difficulty == FLEET_RECRUIT ? FLEET_ADMIRAL
                                                                     : FLEET_RECRUIT));
    REJECTS("an out-of-range queue entry is refused",
            (g.ai.queue_len = 1, g.ai.queue[0] = FLEET_CELLS));
    REJECTS("an AI shot nobody fired is refused",
            (g.ai.shot[FLEET_CELLS - 1] = 1,
             g.board[FLEET_SIDE_PLAYER].shot[FLEET_CELLS - 1] = 0));
    REJECTS("a result without a shot is refused",
            (g.ai.shot[FLEET_CELLS - 1] = 0, g.ai.result[FLEET_CELLS - 1] = FLEET_SHOT_HIT));
    REJECTS("an unknown shot result is refused",
            (g.ai.shot[0] = 1, g.board[FLEET_SIDE_PLAYER].shot[0] = 1,
             g.ai.result[0] = FLEET_SHOT_SUNK + 1));

#undef REJECTS

    /* The same base match, untouched, must still be accepted. */
    fleet_save_encode(&base, blob, sizeof(blob));
    check("the intact match is still accepted",
          fleet_save_decode(&back, blob, sizeof(blob)) == 0);
}

static void test_resume_continues_the_match(void)
{
    struct fleet_game live;
    struct fleet_game resumed;
    uint8_t blob[FLEET_SAVE_SIZE];
    int seed;
    int identical = 1;

    for (seed = 1; seed <= 25 && identical; seed++) {
        start_match(&live, (uint32_t)(seed * 101u), (enum fleet_difficulty)(seed % 4));
        play_rounds(&live, 11);
        fleet_save_encode(&live, blob, sizeof(blob));
        if (fleet_save_decode(&resumed, blob, sizeof(blob)) != 0) {
            identical = 0;
            break;
        }
        play_rounds(&live, 200);
        play_rounds(&resumed, 200);
        if (digest_game(&live) != digest_game(&resumed)) {
            printf("     seed %d: the resumed match diverged\n", seed);
            identical = 0;
        }
    }
    check("a resumed match plays out identically", identical);
}

static void test_store(void)
{
    char dir[] = "/tmp/pos_fleet.XXXXXX";
    char path[512];
    char tmp[560];
    char cmd[700];
    struct fleet_game g;
    struct fleet_game back;
    struct fleet_game untouched;
    struct stat st;
    FILE *f;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        failed++;
        return;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/fleet/save.v1", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    check("path is under the state directory", strcmp(fleet_store_path(), path) == 0);
    check("no save to begin with", fleet_store_load(&back) == 1);
    check("no save is reported", !fleet_store_has_save());
    check("clearing nothing succeeds", fleet_store_clear() == 0);

    start_match(&g, 8080u, FLEET_ADMIRAL);
    play_rounds(&g, 7);
    check("saving succeeds", fleet_store_save(&g) == 0);
    check("the file is there with the right size",
          stat(path, &st) == 0 && st.st_size == (off_t)FLEET_SAVE_SIZE);
    check("no temporary file is left", stat(tmp, &st) != 0);
    check("a save is reported", fleet_store_has_save());
    check("loading round trips", fleet_store_load(&back) == 0 &&
                                 digest_game(&back) == digest_game(&g));

    play_rounds(&g, 3);
    check("saving again succeeds", fleet_store_save(&g) == 0);
    check("the newer match is loaded", fleet_store_load(&back) == 0 &&
                                       digest_game(&back) == digest_game(&g));

    /* A damaged file must be refused without disturbing the caller. */
    untouched = g;
    f = fopen(path, "wb");
    if (f) {
        fputs("not a PocketFleet save", f);
        fclose(f);
    }
    check("a damaged file is refused", fleet_store_load(&back) == -1);
    check("a damaged file reports no save", !fleet_store_has_save());
    check("a refused load leaves the match alone", digest_game(&g) == digest_game(&untouched));

    check("clearing removes the file", fleet_store_clear() == 0 && stat(path, &st) != 0);
    check("nothing to load afterwards", fleet_store_load(&back) == 1);

    /* Persistence must never block play. */
    if (geteuid() != 0) {
        char subdir[560];

        snprintf(subdir, sizeof(subdir), "%s/fleet", dir);
        check("saving works before the directory is locked", fleet_store_save(&g) == 0);
        chmod(subdir, 0555);
        play_rounds(&g, 2);
        check("an unwritable directory reports failure", fleet_store_save(&g) == -1);
        check("no temporary file is left behind", stat(tmp, &st) != 0);
        check("the previous save is still readable", fleet_store_load(&back) == 0);
        chmod(subdir, 0755);
        check("saving recovers", fleet_store_save(&g) == 0 &&
                                 fleet_store_load(&back) == 0 &&
                                 digest_game(&back) == digest_game(&g));
    } else {
        printf("skip unwritable-directory checks (running as root)\n");
    }

    /* The directory is created when it does not exist yet. */
    snprintf(cmd, sizeof(cmd), "rm -rf %s/fleet", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    check("a missing directory is created", fleet_store_save(&g) == 0 &&
                                            fleet_store_load(&back) == 0);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    unsetenv("POCKETOS_STATE_DIR");
}

int main(void)
{
    test_codec();
    test_impossible_saves();
    test_resume_continues_the_match();
    test_store();
    printf("fleet_save_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
