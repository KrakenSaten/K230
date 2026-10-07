/*
 * PocketFleet AI test: the no-cheat property, legality, termination and the
 * difficulty progression.
 *
 * The central assertion is replay_from_tape(): a game played against a real
 * hidden board is reproduced exactly by replaying nothing but the announced
 * results. If the AI ever consulted the layout, the replay (which has no
 * board at all) would diverge.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_ai.h"
#include "fleet_rules.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

struct run {
    uint8_t cell[FLEET_CELLS];   /* cells fired at, in order */
    uint8_t result[FLEET_CELLS]; /* what was announced for each */
    int8_t sunk[FLEET_CELLS];    /* ship index on a sinking shot, else -1 */
    int n;
    int legal;                   /* in bounds and never repeated */
    int won;                     /* the fleet was sunk */
};

/* Play a full game against a real, hidden board. The AI is given the board
 * only through fleet_board_fire()'s announced result. */
static void play_vs_board(enum fleet_difficulty difficulty, uint32_t rng_seed,
                          uint32_t board_seed, struct run *out)
{
    struct fleet_board board;
    struct fleet_ai ai;
    struct fleet_rng rng;
    struct fleet_rng deploy;
    uint8_t seen[FLEET_CELLS];

    memset(out, 0, sizeof(*out));
    out->legal = 1;
    memset(seen, 0, sizeof(seen));
    fleet_board_clear(&board);
    fleet_rng_seed(&deploy, board_seed);
    fleet_board_autoplace(&board, &deploy);
    fleet_ai_init(&ai, difficulty);
    fleet_rng_seed(&rng, rng_seed);

    while (out->n < FLEET_CELLS) {
        int row = -1;
        int col = -1;
        int sunk = -1;
        int idx;
        enum fleet_shot_result result;

        if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
            break;
        }
        if (!fleet_in_bounds(row, col)) {
            out->legal = 0;
            break;
        }
        idx = fleet_index(row, col);
        if (seen[idx]) {
            out->legal = 0;
            break;
        }
        seen[idx] = 1;
        result = fleet_board_fire(&board, row, col, &sunk);
        if (result == FLEET_SHOT_INVALID) {
            out->legal = 0;
            break;
        }
        out->cell[out->n] = (uint8_t)idx;
        out->result[out->n] = (uint8_t)result;
        out->sunk[out->n] = (int8_t)sunk;
        out->n++;
        fleet_ai_observe(&ai, row, col, result, sunk);
        if (board.ships_afloat == 0) {
            out->won = 1;
            break;
        }
    }
}

/* Replay the same AI with no board whatsoever: every shot is answered from
 * the recorded tape. Returns the number of shots reproduced. */
static int replay_from_tape(enum fleet_difficulty difficulty, uint32_t rng_seed,
                            const struct run *tape, uint8_t *cells)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int k;

    fleet_ai_init(&ai, difficulty);
    fleet_rng_seed(&rng, rng_seed);
    for (k = 0; k < tape->n; k++) {
        int row = -1;
        int col = -1;

        if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
            break;
        }
        cells[k] = (uint8_t)fleet_index(row, col);
        fleet_ai_observe(&ai, row, col, (enum fleet_shot_result)tape->result[k],
                         tape->sunk[k]);
    }
    return k;
}

static void test_basics(void)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int row = -1;
    int col = -1;
    int i;

    fleet_ai_init(&ai, FLEET_OFFICER);
    check("fresh AI has fired nothing", ai.shots == 0 && ai.queue_len == 0);
    check("fresh AI sees a full board", fleet_ai_remaining(&ai) == FLEET_CELLS);
    check("unknown difficulty falls back",
          (fleet_ai_init(&ai, (enum fleet_difficulty)42), ai.difficulty == FLEET_OFFICER));

    fleet_ai_init(&ai, FLEET_RECRUIT);
    fleet_rng_seed(&rng, 5u);
    for (i = 0; i < FLEET_CELLS; i++) {
        if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
            break;
        }
        fleet_ai_observe(&ai, row, col, FLEET_SHOT_MISS, -1);
    }
    check("a blind sweep fires at every cell once", i == FLEET_CELLS && ai.shots == FLEET_CELLS);
    check("nothing is left", fleet_ai_remaining(&ai) == 0);
    check("an exhausted board reports no shot",
          fleet_ai_next_shot(&ai, &rng, &row, &col) == -1);

    /* Bad input is ignored rather than corrupting the state. */
    fleet_ai_init(&ai, FLEET_OFFICER);
    fleet_ai_observe(&ai, -1, 0, FLEET_SHOT_HIT, -1);
    fleet_ai_observe(&ai, 0, 0, FLEET_SHOT_INVALID, -1);
    check("invalid observations ignored", ai.shots == 0 && ai.queue_len == 0);
    fleet_ai_observe(&ai, 0, 0, FLEET_SHOT_MISS, -1);
    fleet_ai_observe(&ai, 0, 0, FLEET_SHOT_HIT, -1);
    check("repeated observation ignored", ai.shots == 1);
    check("NULL is safe",
          fleet_ai_next_shot(NULL, &rng, &row, &col) == -1 && fleet_ai_remaining(NULL) == 0);
}

static void test_no_cheating(void)
{
    static const enum fleet_difficulty levels[FLEET_DIFFICULTY_COUNT] = {
        FLEET_RECRUIT, FLEET_OFFICER, FLEET_COMMANDER, FLEET_ADMIRAL
    };
    uint8_t cells[FLEET_CELLS];
    int level;
    int seed;
    int all_match = 1;

    /* Every level, over many hidden layouts: the shots that came out of a
     * real game are reproduced exactly from the announcements alone. */
    for (level = 0; level < FLEET_DIFFICULTY_COUNT && all_match; level++) {
        for (seed = 1; seed <= 60 && all_match; seed++) {
            struct run game;
            int n;

            play_vs_board(levels[level], (uint32_t)(seed * 7 + 1), (uint32_t)seed, &game);
            memset(cells, 0xFF, sizeof(cells));
            n = replay_from_tape(levels[level], (uint32_t)(seed * 7 + 1), &game, cells);
            if (n != game.n || memcmp(cells, game.cell, (size_t)game.n) != 0) {
                printf("     %s seed %d: replay diverged at %d of %d shots\n",
                       fleet_difficulty_name(levels[level]), seed, n, game.n);
                all_match = 0;
            }
        }
    }
    check("a boardless replay of the announcements reproduces every game", all_match);

    /* The results are used (or ignored) only through the tape: feeding a
     * different tape must change what a hunting level does. */
    {
        struct run tape_a;
        struct run tape_b;
        uint8_t recruit_a[FLEET_CELLS];
        uint8_t recruit_b[FLEET_CELLS];
        uint8_t officer_a[FLEET_CELLS];
        uint8_t officer_b[FLEET_CELLS];
        int i;

        memset(&tape_a, 0, sizeof(tape_a));
        memset(&tape_b, 0, sizeof(tape_b));
        tape_a.n = FLEET_CELLS;
        tape_b.n = FLEET_CELLS;
        for (i = 0; i < FLEET_CELLS; i++) {
            tape_a.result[i] = FLEET_SHOT_MISS;
            tape_a.sunk[i] = -1;
            tape_b.result[i] = (uint8_t)(i % 3 == 0 ? FLEET_SHOT_HIT : FLEET_SHOT_MISS);
            tape_b.sunk[i] = -1;
        }
        replay_from_tape(FLEET_RECRUIT, 31u, &tape_a, recruit_a);
        replay_from_tape(FLEET_RECRUIT, 31u, &tape_b, recruit_b);
        check("Recruit ignores the results entirely",
              memcmp(recruit_a, recruit_b, FLEET_CELLS) == 0);
        replay_from_tape(FLEET_OFFICER, 31u, &tape_a, officer_a);
        replay_from_tape(FLEET_OFFICER, 31u, &tape_b, officer_b);
        check("Officer reacts to the results",
              memcmp(officer_a, officer_b, FLEET_CELLS) != 0);
        check("with no hits Officer searches like Recruit",
              memcmp(recruit_a, officer_a, FLEET_CELLS) == 0);
    }
}

static void test_legality_and_termination(double *mean_shots)
{
    static const enum fleet_difficulty levels[FLEET_DIFFICULTY_COUNT] = {
        FLEET_RECRUIT, FLEET_OFFICER, FLEET_COMMANDER, FLEET_ADMIRAL
    };
    const int games = 300;
    int level;

    for (level = 0; level < FLEET_DIFFICULTY_COUNT; level++) {
        int all_legal = 1;
        int all_won = 1;
        long total = 0;
        int seed;

        for (seed = 1; seed <= games; seed++) {
            struct run game;

            play_vs_board(levels[level], (uint32_t)(seed * 31 + 5), (uint32_t)seed, &game);
            all_legal &= game.legal;
            all_won &= game.won;
            total += game.n;
        }
        mean_shots[level] = (double)total / games;
        printf("     %-9s mean %.1f shots over %d games\n",
               fleet_difficulty_name(levels[level]), mean_shots[level], games);
        check(levels[level] == FLEET_RECRUIT ? "Recruit fires only legal shots" :
              levels[level] == FLEET_OFFICER ? "Officer fires only legal shots" :
              levels[level] == FLEET_COMMANDER ? "Commander fires only legal shots" :
                                                 "Admiral fires only legal shots", all_legal);
        check(levels[level] == FLEET_RECRUIT ? "Recruit always sinks the fleet" :
              levels[level] == FLEET_OFFICER ? "Officer always sinks the fleet" :
              levels[level] == FLEET_COMMANDER ? "Commander always sinks the fleet" :
                                                 "Admiral always sinks the fleet", all_won);
    }
}

/* While an unresolved hit still has an unfired orthogonal neighbour, the
 * next shot must be one of those neighbours. When every neighbour has
 * already been fired at the hunt has nothing left to offer and returning to
 * the search is correct. */
static void test_officer_hunts(void)
{
    int seed;
    int hunted = 1;
    int hunts_seen = 0;

    for (seed = 1; seed <= 40 && hunted; seed++) {
        struct fleet_board board;
        struct fleet_ai ai;
        struct fleet_rng rng;
        struct fleet_rng deploy;
        uint8_t open_hit[FLEET_CELLS];
        int shots;

        memset(open_hit, 0, sizeof(open_hit));
        fleet_board_clear(&board);
        fleet_rng_seed(&deploy, (uint32_t)seed);
        fleet_board_autoplace(&board, &deploy);
        fleet_ai_init(&ai, FLEET_OFFICER);
        fleet_rng_seed(&rng, (uint32_t)(seed * 13 + 2));

        for (shots = 0; shots < FLEET_CELLS && board.ships_afloat > 0; shots++) {
            int row = -1;
            int col = -1;
            int sunk = -1;
            enum fleet_shot_result result;
            int huntable = 0;
            int i;
            int d;

            /* Is there an unresolved hit with somewhere left to go? */
            for (i = 0; i < FLEET_CELLS; i++) {
                static const int dr[4] = { -1, 1, 0, 0 };
                static const int dc[4] = { 0, 0, -1, 1 };

                if (!open_hit[i]) {
                    continue;
                }
                for (d = 0; d < 4; d++) {
                    int nr = i / FLEET_GRID + dr[d];
                    int nc = i % FLEET_GRID + dc[d];
                    int nidx = fleet_index(nr, nc);

                    if (nidx >= 0 && !board.shot[nidx]) {
                        huntable = 1;
                    }
                }
            }
            if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
                break;
            }
            if (huntable) {
                int adjacent = 0;

                hunts_seen++;
                for (i = 0; i < FLEET_CELLS; i++) {
                    int hr = i / FLEET_GRID;
                    int hc = i % FLEET_GRID;

                    if (open_hit[i] && ((hr == row && (hc == col - 1 || hc == col + 1)) ||
                                        (hc == col && (hr == row - 1 || hr == row + 1)))) {
                        adjacent = 1;
                    }
                }
                if (!adjacent) {
                    printf("     seed %d: shot %d,%d ignored an open hit\n", seed, row, col);
                    hunted = 0;
                    break;
                }
            }
            result = fleet_board_fire(&board, row, col, &sunk);
            fleet_ai_observe(&ai, row, col, result, sunk);
            if (result == FLEET_SHOT_HIT) {
                open_hit[fleet_index(row, col)] = 1;
            } else if (result == FLEET_SHOT_SUNK) {
                memset(open_hit, 0, sizeof(open_hit)); /* Officer forgets on a sinking */
            }
        }
    }
    check("Officer follows up every unresolved hit", hunted);
    check("the hunt was actually exercised", hunts_seen > 100);
}

/* Commander searches on a diagonal lattice spaced by the shortest ship still
 * afloat, which cannot skip over it. Checked only while nothing is being
 * hunted, since a hunt legitimately leaves the lattice. */
static void test_commander_parity(void)
{
    int seed;
    int on_lattice = 1;
    int searches = 0;

    for (seed = 1; seed <= 40 && on_lattice; seed++) {
        struct fleet_board board;
        struct fleet_ai ai;
        struct fleet_rng rng;
        struct fleet_rng deploy;
        int shots;

        fleet_board_clear(&board);
        fleet_rng_seed(&deploy, (uint32_t)seed);
        fleet_board_autoplace(&board, &deploy);
        fleet_ai_init(&ai, FLEET_COMMANDER);
        fleet_rng_seed(&rng, (uint32_t)(seed * 17 + 3));

        for (shots = 0; shots < FLEET_CELLS && board.ships_afloat > 0; shots++) {
            int row = -1;
            int col = -1;
            int sunk = -1;
            int hunting = 0;
            int step = 0;
            int i;
            enum fleet_shot_result result;

            for (i = 0; i < FLEET_SHIP_COUNT; i++) {
                int length = fleet_ship_length((enum fleet_ship)i);

                if (!ai.sunk[i] && (step == 0 || length < step)) {
                    step = length;
                }
            }
            for (i = 0; i < FLEET_CELLS; i++) {
                if (ai.shot[i] && !ai.resolved[i] &&
                    (ai.result[i] == FLEET_SHOT_HIT || ai.result[i] == FLEET_SHOT_SUNK)) {
                    hunting = 1;
                }
            }
            if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
                break;
            }
            if (!hunting && step > 1) {
                int lattice_left = 0;

                for (i = 0; i < FLEET_CELLS; i++) {
                    if (!ai.shot[i] && ((i / FLEET_GRID) + (i % FLEET_GRID)) % step == 0) {
                        lattice_left = 1;
                    }
                }
                if (lattice_left) {
                    searches++;
                    if ((row + col) % step != 0) {
                        printf("     seed %d: search shot %d,%d is off the %d lattice\n",
                               seed, row, col, step);
                        on_lattice = 0;
                        break;
                    }
                }
            }
            result = fleet_board_fire(&board, row, col, &sunk);
            fleet_ai_observe(&ai, row, col, result, sunk);
        }
    }
    check("Commander searches on the parity lattice", on_lattice);
    check("the parity search was actually exercised", searches > 200);
}

/* Admiral must never fire where no ship still afloat could fit given the
 * misses it has seen. The oracle is rebuilt here from the announcements. */
static void test_admiral_density(void)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int row = -1;
    int col = -1;
    int seed;
    int always_possible = 1;

    /* With nothing known, the densest cells are the four in the middle. */
    fleet_ai_init(&ai, FLEET_ADMIRAL);
    fleet_rng_seed(&rng, 1u);
    check("Admiral opens in the centre",
          fleet_ai_next_shot(&ai, &rng, &row, &col) == 0 &&
          row >= 4 && row <= 5 && col >= 4 && col <= 5);

    /* A single unexplained hit pulls the next shot next to it. */
    fleet_ai_init(&ai, FLEET_ADMIRAL);
    fleet_rng_seed(&rng, 1u);
    fleet_ai_observe(&ai, 4, 4, FLEET_SHOT_HIT, -1);
    check("Admiral follows up an unresolved hit",
          fleet_ai_next_shot(&ai, &rng, &row, &col) == 0 &&
          ((row == 4 && (col == 3 || col == 5)) || (col == 4 && (row == 3 || row == 5))));

    for (seed = 1; seed <= 60 && always_possible; seed++) {
        struct fleet_board board;
        struct fleet_rng deploy;
        int shots;

        fleet_board_clear(&board);
        fleet_rng_seed(&deploy, (uint32_t)seed);
        fleet_board_autoplace(&board, &deploy);
        fleet_ai_init(&ai, FLEET_ADMIRAL);
        fleet_rng_seed(&rng, (uint32_t)(seed * 23 + 9));

        for (shots = 0; shots < FLEET_CELLS && board.ships_afloat > 0; shots++) {
            int sunk = -1;
            int possible = 0;
            int ship;
            enum fleet_shot_result result;

            if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
                break;
            }
            for (ship = 0; ship < FLEET_SHIP_COUNT && !possible; ship++) {
                int length = fleet_ship_length((enum fleet_ship)ship);
                int orient;

                if (ai.sunk[ship]) {
                    continue;
                }
                for (orient = 0; orient < 2 && !possible; orient++) {
                    int offset;

                    for (offset = 0; offset < length && !possible; offset++) {
                        int fits = 1;
                        int n;

                        for (n = 0; n < length; n++) {
                            int rr = row + (orient ? n - offset : 0);
                            int cc = col + (orient ? 0 : n - offset);
                            int idx = fleet_index(rr, cc);

                            if (idx < 0 || (ai.shot[idx] &&
                                            ai.result[idx] == FLEET_SHOT_MISS)) {
                                fits = 0;
                                break;
                            }
                        }
                        possible = fits;
                    }
                }
            }
            if (!possible) {
                printf("     seed %d: no ship afloat can occupy %d,%d\n", seed, row, col);
                always_possible = 0;
                break;
            }
            result = fleet_board_fire(&board, row, col, &sunk);
            fleet_ai_observe(&ai, row, col, result, sunk);
        }
    }
    check("Admiral never fires where no ship could be", always_possible);
}

int main(void)
{
    double mean[FLEET_DIFFICULTY_COUNT];

    test_basics();
    test_no_cheating();
    test_legality_and_termination(mean);
    test_officer_hunts();
    test_commander_parity();
    test_admiral_density();

    check("Officer beats Recruit", mean[FLEET_OFFICER] < mean[FLEET_RECRUIT]);
    check("Commander beats Officer", mean[FLEET_COMMANDER] < mean[FLEET_OFFICER]);
    check("Admiral beats Commander", mean[FLEET_ADMIRAL] < mean[FLEET_COMMANDER]);
    check("Recruit is a blind search (over 85 shots on average)", mean[FLEET_RECRUIT] > 85.0);
    check("Officer averages under 75 shots", mean[FLEET_OFFICER] < 75.0);
    check("Commander averages under 65 shots", mean[FLEET_COMMANDER] < 65.0);
    check("Admiral averages under 55 shots", mean[FLEET_ADMIRAL] < 55.0);

    printf("fleet_ai_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
