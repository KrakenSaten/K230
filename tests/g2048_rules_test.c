/*
 * PG 2048 rules: slide and merge, no double merge, both directions of the
 * same line, new tiles, score, the goal, the end of a game, determinism, and
 * random games that must never break an invariant.
 *
 * Expected boards are written out by hand from the rules in g2048_rules.h,
 * never computed by the code under test.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_rules.h"

#include <stdio.h>
#include <string.h>

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

/* Exponents are unreadable in a failure; boards are written as values. */
static uint8_t e(uint32_t value)
{
    uint8_t x = 0;

    while (value > 1) {
        value >>= 1;
        x++;
    }
    return x;
}

static void set_board(struct g2048_game *g, const uint32_t values[G2048_CELLS])
{
    int i;

    for (i = 0; i < G2048_CELLS; i++) {
        g->cell[i] = e(values[i]);
    }
}

static int board_is(const struct g2048_game *g, const uint32_t values[G2048_CELLS])
{
    int i;

    for (i = 0; i < G2048_CELLS; i++) {
        if (g2048_value(g->cell[i]) != values[i]) {
            return 0;
        }
    }
    return 1;
}

static void print_board(const struct g2048_game *g)
{
    int i;

    for (i = 0; i < G2048_CELLS; i++) {
        printf("%6u%s", (unsigned)g2048_value(g->cell[i]), (i % 4 == 3) ? "\n" : " ");
    }
}

/* A game with a board and nothing else, and a fixed generator. */
static void game_with(struct g2048_game *g, const uint32_t values[G2048_CELLS])
{
    memset(g, 0, sizeof(*g));
    g2048_rng_seed(&g->rng, 12345);
    set_board(g, values);
}

static uint32_t board_sum(const struct g2048_game *g)
{
    uint32_t s = 0;
    int i;

    for (i = 0; i < G2048_CELLS; i++) {
        s += g2048_value(g->cell[i]);
    }
    return s;
}

/* ---- one line ----------------------------------------------------------- */

struct line_case {
    const char *what;
    uint32_t in[4];
    uint32_t out[4];
    uint32_t gained;
};

static void test_lines(void)
{
    static const struct line_case cases[] = {
        { "an empty line stays empty", { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 },
        { "a lone tile slides to the edge", { 0, 0, 0, 2 }, { 2, 0, 0, 0 }, 0 },
        { "a tile already at the edge stays", { 2, 0, 0, 0 }, { 2, 0, 0, 0 }, 0 },
        { "two equal tiles with a gap merge", { 2, 0, 2, 0 }, { 4, 0, 0, 0 }, 4 },
        { "unequal neighbours do not merge", { 2, 4, 2, 4 }, { 2, 4, 2, 4 }, 0 },
        { "2 2 2 2 is 4 4, never 8", { 2, 2, 2, 2 }, { 4, 4, 0, 0 }, 8 },
        { "2 2 4 is 4 4: the new 4 does not merge again", { 2, 2, 4, 0 }, { 4, 4, 0, 0 }, 4 },
        { "4 4 8 is 8 8, never 16", { 4, 4, 8, 0 }, { 8, 8, 0, 0 }, 8 },
        { "2 2 2 keeps the far one: the two nearest the edge merge", { 2, 2, 2, 0 }, { 4, 2, 0, 0 }, 4 },
        { "a gap before a pair", { 0, 2, 2, 0 }, { 4, 0, 0, 0 }, 4 },
        { "a pair then a different tile", { 2, 2, 0, 8 }, { 4, 8, 0, 0 }, 4 },
        { "8 4 4 8: only the middle pair merges", { 8, 4, 4, 8 }, { 8, 8, 8, 0 }, 8 },
        { "two pairs of different values", { 4, 4, 2, 2 }, { 8, 4, 0, 0 }, 12 },
        { "1024 1024 makes 2048", { 1024, 0, 0, 1024 }, { 2048, 0, 0, 0 }, 2048 },
        { "no merge across a different tile", { 2, 4, 0, 2 }, { 2, 4, 2, 0 }, 0 },
    };
    size_t c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t in[4];
        uint8_t out[4];
        int8_t dest[4];
        uint8_t merged[4];
        uint32_t gained;
        int ok = 1;
        int k;

        for (k = 0; k < 4; k++) {
            in[k] = e(cases[c].in[k]);
        }
        gained = g2048_slide_line(in, out, dest, merged);
        for (k = 0; k < 4; k++) {
            ok &= g2048_value(out[k]) == cases[c].out[k];
        }
        check(cases[c].what, ok);
        if (!ok) {
            printf("     got %u %u %u %u\n", (unsigned)g2048_value(out[0]), (unsigned)g2048_value(out[1]),
                   (unsigned)g2048_value(out[2]), (unsigned)g2048_value(out[3]));
        }
        check(cases[c].what, gained == cases[c].gained);
    }

    /* Where each tile went, for the animation. */
    {
        uint8_t in[4] = { 1, 1, 1, 0 }; /* 2 2 2 . */
        uint8_t out[4];
        int8_t dest[4];
        uint8_t merged[4];

        g2048_slide_line(in, out, dest, merged);
        check("2 2 2: the first two land in slot 0 as a merge",
              dest[0] == 0 && dest[1] == 0 && merged[0] && merged[1]);
        check("the third lands in slot 1 unmerged", dest[2] == 1 && !merged[2]);
        check("an empty cell has no destination", dest[3] == -1 && !merged[3]);
    }
    {
        uint8_t in[4] = { 17, 17, 0, 0 };
        uint8_t out[4];
        int8_t dest[4];
        uint8_t merged[4];
        uint32_t gained = g2048_slide_line(in, out, dest, merged);

        check("the largest possible tile never merges past the exponent bound",
              out[0] == 17 && out[1] == 17 && gained == 0);
    }
}

/* ---- the board ------------------------------------------------------------ */

static void test_directions(void)
{
    /* One row, both ways: the same line gives different boards. */
    static const uint32_t row[16] = { 2, 2, 2, 0,
                                      0, 0, 0, 0,
                                      0, 0, 0, 0,
                                      0, 0, 0, 0 };
    static const uint32_t row_left[16] = { 4, 2, 0, 0,
                                           0, 0, 0, 0,
                                           0, 0, 0, 0,
                                           0, 0, 0, 0 };
    static const uint32_t row_right[16] = { 0, 0, 2, 4,
                                            0, 0, 0, 0,
                                            0, 0, 0, 0,
                                            0, 0, 0, 0 };
    /* One column, both ways. */
    static const uint32_t col[16] = { 4, 0, 0, 0,
                                      4, 0, 0, 0,
                                      8, 0, 0, 0,
                                      0, 0, 0, 0 };
    static const uint32_t col_up[16] = { 8, 0, 0, 0,
                                         8, 0, 0, 0,
                                         0, 0, 0, 0,
                                         0, 0, 0, 0 };
    /* Down, the tiles meet the bottom edge in the order 8, 4, 4: the 8
     * cannot take a 4, so the two 4s make the second 8. */
    static const uint32_t col_down[16] = { 0, 0, 0, 0,
                                           0, 0, 0, 0,
                                           8, 0, 0, 0,
                                           8, 0, 0, 0 };
    struct g2048_game g;
    struct g2048_turn t;
    uint8_t before;

    game_with(&g, row);
    g2048_move(&g, G2048_LEFT, &t);
    g.cell[t.spawn] = 0; /* ignore the new tile when comparing */
    check("2 2 2 LEFT is 4 2", board_is(&g, row_left));

    game_with(&g, row);
    g2048_move(&g, G2048_RIGHT, &t);
    g.cell[t.spawn] = 0;
    check("2 2 2 RIGHT is 2 4: the pair nearest the right edge merges", board_is(&g, row_right));
    if (!board_is(&g, row_right)) {
        print_board(&g);
    }

    game_with(&g, col);
    g2048_move(&g, G2048_UP, &t);
    g.cell[t.spawn] = 0;
    check("4 4 8 UP is 8 8 at the top, never 16", board_is(&g, col_up));

    game_with(&g, col);
    g2048_move(&g, G2048_DOWN, &t);
    g.cell[t.spawn] = 0;
    check("4 4 8 DOWN is 8 8 at the bottom, never 16 either", board_is(&g, col_down));

    /* A move that changes nothing is not a move. */
    game_with(&g, row_left);
    before = g.cell[0];
    {
        uint32_t rng = g.rng.state;
        int moved = g2048_move(&g, G2048_LEFT, &t);

        check("sliding into a wall that is already full is not a move", moved == 0 && !t.moved);
        check("no tile appears", t.spawn == -1 && g2048_tile_count(&g) == 2);
        check("the move counter does not advance", g.moves == 0);
        check("no random draw is spent", g.rng.state == rng);
        check("and the board is untouched", g.cell[0] == before && board_is(&g, row_left));
        check("can_move_dir agrees", !g2048_can_move_dir(&g, G2048_LEFT) &&
                                         g2048_can_move_dir(&g, G2048_RIGHT) &&
                                         g2048_can_move_dir(&g, G2048_DOWN) &&
                                         !g2048_can_move_dir(&g, G2048_UP));
    }

    /* A real move: one tile appears in a cell that was empty after the slide. */
    game_with(&g, row);
    check("a real move reports itself", g2048_move(&g, G2048_LEFT, &t) == 1 && t.moved);
    check("the new tile is in a cell the slide left empty",
          t.spawn >= 2 && t.spawn < G2048_CELLS && g.cell[t.spawn] == t.spawn_exp);
    check("and is a 2 or a 4", t.spawn_exp == 1 || t.spawn_exp == 2);
    check("the move counter advances", g.moves == 1);
    check("the score gains the merge", g.score == 4 && t.gained == 4);
    check("the best follows the score", g.best == 4);
    check("three tiles plus one new: three on the board", g2048_tile_count(&g) == 3);

    /* An invalid direction is refused. */
    game_with(&g, row);
    check("an out-of-range direction is refused", g2048_move(&g, (enum g2048_dir)7, &t) == 0);
    check("a NULL game is refused", g2048_move(NULL, G2048_UP, &t) == 0);
}

static void test_steps(void)
{
    static const uint32_t b[16] = { 2, 2, 4, 0,
                                    0, 8, 0, 8,
                                    2, 0, 0, 0,
                                    4, 4, 4, 4 };
    struct g2048_game g;
    struct g2048_turn t;
    uint8_t after[G2048_CELLS];
    int tiles_before;
    int landed[G2048_CELLS];
    int ok_consistent = 1;
    int i;

    game_with(&g, b);
    tiles_before = g2048_tile_count(&g);
    g2048_move(&g, G2048_LEFT, &t);
    memcpy(after, g.cell, sizeof(after));
    after[t.spawn] = 0;
    check("one step per tile on the board before the move", t.steps == tiles_before);
    memset(landed, 0, sizeof(landed));
    for (i = 0; i < t.steps; i++) {
        const struct g2048_step *s = &t.step[i];

        landed[s->to]++;
        if (s->merged) {
            ok_consistent &= after[s->to] == s->exp + 1;
        } else {
            ok_consistent &= after[s->to] == s->exp;
        }
        /* Along a row, left: same row, never further right. */
        ok_consistent &= s->to / 4 == s->from / 4 && s->to % 4 <= s->from % 4;
    }
    check("every step lands on its tile's value after the move, or one more if it merged",
          ok_consistent);
    {
        int ok_counts = 1;

        for (i = 0; i < G2048_CELLS; i++) {
            if (after[i] == 0) {
                ok_counts &= landed[i] == 0;
            }
        }
        check("nothing lands in a cell left empty", ok_counts);
    }
    check("the merges of 2 2, 8 . 8 and 4 4 4 4 score 4 + 16 + 16", t.gained == 36);
}

/* ---- new tiles ---------------------------------------------------------- */

static void test_spawn(void)
{
    static const uint32_t full[16] = { 2, 4, 2, 4,
                                       4, 2, 4, 2,
                                       2, 4, 2, 4,
                                       4, 2, 4, 2 };
    static const uint32_t one_hole[16] = { 2, 4, 2, 4,
                                           4, 2, 4, 2,
                                           2, 4, 0, 4,
                                           4, 2, 4, 2 };
    struct g2048_game g;
    int count[G2048_CELLS];
    int fours = 0;
    int total = 0;
    uint32_t rng;
    int i;
    int s;

    game_with(&g, full);
    rng = g.rng.state;
    check("a full board has nowhere to put a tile", g2048_spawn(&g, NULL) == -1);
    check("and spends no draw finding that out", g.rng.state == rng);

    game_with(&g, one_hole);
    check("a board with one hole fills that hole", g2048_spawn(&g, NULL) == 10);

    /* Placement is uniform over the empty cells and the value is a 4 about
     * one time in ten. 20000 spawns on an empty board: each cell expects
     * 1250 and a 4 expects 2000; the bounds are a little over five
     * standard deviations, so a correct rule does not fail by chance. */
    memset(count, 0, sizeof(count));
    memset(&g, 0, sizeof(g));
    g2048_rng_seed(&g.rng, 99);
    for (s = 0; s < 20000; s++) {
        uint8_t exp = 0;
        int cell;

        memset(g.cell, 0, sizeof(g.cell));
        cell = g2048_spawn(&g, &exp);
        if (cell < 0 || cell >= G2048_CELLS || exp < 1 || exp > 2 || g.cell[cell] != exp ||
            g2048_tile_count(&g) != 1) {
            total = -1;
            break;
        }
        count[cell]++;
        fours += exp == 2;
        total++;
    }
    check("every spawn puts exactly one 2 or 4 in an empty cell", total == 20000);
    {
        int ok = 1;

        for (i = 0; i < G2048_CELLS; i++) {
            ok &= count[i] > 1070 && count[i] < 1430;
        }
        check("placement is uniform over the cells", ok);
    }
    check("about one new tile in ten is a 4", fours > 1760 && fours < 2240);

    /* On a partly filled board the tile only ever goes where there is room. */
    {
        static const uint32_t half[16] = { 2, 2, 2, 2,
                                           0, 0, 0, 0,
                                           4, 4, 4, 4,
                                           0, 0, 0, 0 };
        int wrong = 0;

        for (s = 0; s < 2000; s++) {
            int cell;

            game_with(&g, half);
            g2048_rng_seed(&g.rng, (uint32_t)s + 1u);
            cell = g2048_spawn(&g, NULL);
            wrong += !((cell >= 4 && cell < 8) || cell >= 12);
        }
        check("a tile never lands on an occupied cell", wrong == 0);
    }
}

/* ---- new game, score, determinism ----------------------------------------- */

static void test_new_game(void)
{
    struct g2048_game a;
    struct g2048_game b;
    int differ = 0;
    uint32_t s;

    g2048_new_game(&a, 2026, 5000);
    check("a new game has exactly two tiles", g2048_tile_count(&a) == 2);
    {
        int ok = 1;
        int i;

        for (i = 0; i < G2048_CELLS; i++) {
            ok &= a.cell[i] <= 2;
        }
        check("both are 2s or 4s", ok);
    }
    check("the score starts at 0", a.score == 0 && a.moves == 0);
    check("the best is carried in", a.best == 5000);
    check("nothing is won or over", !a.won && !a.keep_going && !a.over);
    check("it is a valid game", g2048_game_valid(&a));
    check("and playing", g2048_state_of(&a) == G2048_PLAYING);

    g2048_new_game(&b, 2026, 5000);
    check("the same seed gives the same game", memcmp(&a, &b, sizeof(a)) == 0);
    for (s = 1; s <= 50; s++) {
        g2048_new_game(&b, s, 0);
        differ += memcmp(a.cell, b.cell, sizeof(a.cell)) != 0;
    }
    check("different seeds give different openings", differ > 40);

    /* The same seed and the same moves are the same game, all the way. */
    {
        static const enum g2048_dir moves[] = { G2048_LEFT, G2048_UP, G2048_RIGHT, G2048_DOWN };
        int i;

        g2048_new_game(&a, 777, 0);
        g2048_new_game(&b, 777, 0);
        for (i = 0; i < 400; i++) {
            g2048_move(&a, moves[i % 4], NULL);
            g2048_move(&b, moves[i % 4], NULL);
        }
        check("a seed and a list of moves replay exactly", memcmp(&a, &b, sizeof(a)) == 0);
    }
}

static void test_score(void)
{
    static const uint32_t b[16] = { 2, 2, 0, 0,
                                    4, 4, 0, 0,
                                    8, 8, 0, 0,
                                    16, 16, 0, 0 };
    struct g2048_game g;
    struct g2048_turn t;

    game_with(&g, b);
    g.best = 100;
    g2048_move(&g, G2048_LEFT, &t);
    check("a move scores the sum of the tiles it made: 4 + 8 + 16 + 32", t.gained == 60 && g.score == 60);
    check("a best above the score is kept", g.best == 100);
    game_with(&g, b);
    g.best = 10;
    g2048_move(&g, G2048_LEFT, &t);
    check("a score above the best raises it", g.best == 60);
    {
        uint32_t sum_before;
        struct g2048_turn t2;

        game_with(&g, b);
        sum_before = board_sum(&g);
        g2048_move(&g, G2048_LEFT, &t2);
        check("merging keeps the board's total; only the new tile adds to it",
              board_sum(&g) == sum_before + g2048_value(t2.spawn_exp));
    }
}

/* ---- the goal and the end ------------------------------------------------ */

static void test_goal_and_end(void)
{
    static const uint32_t near_goal[16] = { 1024, 1024, 0, 0,
                                            0, 0, 0, 0,
                                            0, 0, 0, 0,
                                            0, 0, 0, 2 };
    /* Full, and no two neighbours equal in either direction. */
    static const uint32_t stuck[16] = { 2, 4, 2, 4,
                                        4, 2, 4, 2,
                                        2, 4, 2, 4,
                                        4, 2, 4, 2 };
    /* Full, with one horizontal pair. */
    static const uint32_t pair_row[16] = { 2, 4, 2, 4,
                                           4, 2, 4, 2,
                                           2, 8, 8, 4,
                                           4, 2, 4, 2 };
    /* Full, with one vertical pair. */
    static const uint32_t pair_col[16] = { 2, 4, 2, 4,
                                           4, 2, 4, 2,
                                           2, 8, 2, 4,
                                           4, 8, 4, 2 };
    struct g2048_game g;
    struct g2048_turn t;

    game_with(&g, near_goal);
    g2048_move(&g, G2048_LEFT, &t);
    check("merging two 1024s reaches the goal", t.reached_goal == 1 && g.won == 1);
    check("the game waits for the player", g2048_state_of(&g) == G2048_WON);
    check("and refuses moves until then", g2048_move(&g, G2048_DOWN, &t) == 0 && !t.moved);
    g2048_keep_going(&g);
    check("keep going resumes play", g2048_state_of(&g) == G2048_PLAYING && g.keep_going);
    check("and moves are accepted again", g2048_move(&g, G2048_DOWN, &t) == 1);
    check("the goal is reported once per game, not again", t.reached_goal == 0);
    check("a won game that continues is still valid", g2048_game_valid(&g));

    game_with(&g, stuck);
    check("a full board with no equal neighbours has no move", !g2048_can_move(&g));
    game_with(&g, pair_row);
    check("one equal pair in a row is a move", g2048_can_move(&g) &&
                                               g2048_can_move_dir(&g, G2048_LEFT) &&
                                               g2048_can_move_dir(&g, G2048_RIGHT) &&
                                               !g2048_can_move_dir(&g, G2048_UP) &&
                                               !g2048_can_move_dir(&g, G2048_DOWN));
    game_with(&g, pair_col);
    check("one equal pair in a column is a move", g2048_can_move(&g) &&
                                                  g2048_can_move_dir(&g, G2048_UP) &&
                                                  g2048_can_move_dir(&g, G2048_DOWN) &&
                                                  !g2048_can_move_dir(&g, G2048_LEFT) &&
                                                  !g2048_can_move_dir(&g, G2048_RIGHT));

    /* One move from the end. Sliding the top row left merges 2 2 into 4 and
     * leaves one hole, in the top right corner, above a 4. A new 2 there
     * matches nothing and the game is over; a new 4 matches the 4 below it
     * and the game goes on. Seeds are tried until both have happened. */
    {
        static const uint32_t almost[16] = { 2, 2, 64, 256,
                                             8, 16, 32, 4,
                                             16, 32, 128, 512,
                                             32, 128, 512, 1024 };
        uint32_t seed;
        int seen_end = 0;
        int seen_continue = 0;

        for (seed = 1; seed < 200 && !(seen_end && seen_continue); seed++) {
            game_with(&g, almost);
            g2048_rng_seed(&g.rng, seed);
            if (!g2048_move(&g, G2048_LEFT, &t)) {
                continue;
            }
            if (t.ended) {
                if (!seen_end) {
                    check("the move that fills the board with no move left ends the game",
                          g.over == 1 && g2048_state_of(&g) == G2048_OVER && !g2048_can_move(&g));
                    check("a finished game refuses every direction",
                          !g2048_move(&g, G2048_UP, NULL) && !g2048_move(&g, G2048_DOWN, NULL) &&
                          !g2048_move(&g, G2048_LEFT, NULL) && !g2048_move(&g, G2048_RIGHT, NULL));
                    check("and is a valid finished game", g2048_game_valid(&g));
                    g2048_keep_going(&g);
                    check("keep going does not revive it", g2048_state_of(&g) == G2048_OVER);
                }
                seen_end = 1;
            } else {
                seen_continue |= !g.over && g2048_can_move(&g);
            }
        }
        check("some new tile ends that game", seen_end);
        check("and some other new tile does not", seen_continue);
    }
}

/* ---- invariants over random games --------------------------------------- */

static void test_random_games(void)
{
    struct g2048_rng pick;
    uint32_t seed;
    int broken = 0;
    int finished = 0;
    int max_seen = 0;

    g2048_rng_seed(&pick, 4242);
    for (seed = 1; seed <= 300; seed++) {
        struct g2048_game g;
        uint32_t scored = 0;
        int turns;

        g2048_new_game(&g, seed, 0);
        for (turns = 0; turns < 5000 && !g.over; turns++) {
            struct g2048_turn t;
            enum g2048_dir d = (enum g2048_dir)g2048_rng_below(&pick, 4);
            uint32_t sum = board_sum(&g);
            int could = g2048_can_move_dir(&g, d);
            int moved;

            if (g2048_state_of(&g) == G2048_WON) {
                g2048_keep_going(&g);
            }
            moved = g2048_move(&g, d, &t);
            if (moved != could) {
                broken++;
                printf("FAIL seed %u: can_move_dir said %d, move said %d\n", (unsigned)seed, could, moved);
            }
            if (moved) {
                scored += t.gained;
                if (board_sum(&g) != sum + g2048_value(t.spawn_exp)) {
                    broken++;
                    printf("FAIL seed %u: a move lost or made tiles\n", (unsigned)seed);
                }
            } else if (board_sum(&g) != sum) {
                broken++;
            }
            if (!g2048_game_valid(&g) || g.score != scored || g.best < g.score) {
                broken++;
                printf("FAIL seed %u turn %d: invariant broken\n", (unsigned)seed, turns);
                print_board(&g);
                break;
            }
        }
        finished += g.over;
        if (g2048_max_exp(&g) > max_seen) {
            max_seen = g2048_max_exp(&g);
        }
    }
    check("300 random games keep every invariant on every turn", broken == 0);
    check("random play always ends the game", finished == 300);
    check("and gets somewhere doing it", max_seen >= 7);
}

static void test_validity(void)
{
    struct g2048_game g;
    struct g2048_game bad;

    g2048_new_game(&g, 5, 0);
    g2048_move(&g, G2048_LEFT, NULL);
    g2048_move(&g, G2048_UP, NULL);
    check("a played game is valid", g2048_game_valid(&g));

    bad = g;
    bad.cell[0] = G2048_EXP_MAX + 1;
    check("a tile past 131072 is refused", !g2048_game_valid(&bad));
    bad = g;
    memset(bad.cell, 0, sizeof(bad.cell));
    bad.cell[3] = 1;
    check("a board with one tile is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.score |= 1;
    bad.best = bad.score;
    check("an odd score is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.best = 0;
    bad.score = 8;
    check("a best below the score is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.keep_going = 1;
    check("keep going without a win is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.won = 1;
    check("a win without the goal tile is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.over = 1;
    check("over while moves remain is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.rng.state = 0;
    check("a spent generator state is refused", !g2048_game_valid(&bad));
    bad = g;
    bad.moves = 0;
    bad.score = 4;
    bad.best = 4;
    check("points without a move are refused", !g2048_game_valid(&bad));
    check("NULL is not a game", !g2048_game_valid(NULL));
}

int main(void)
{
    test_lines();
    test_directions();
    test_steps();
    test_spawn();
    test_new_game();
    test_score();
    test_goal_and_end();
    test_random_games();
    test_validity();
    printf("g2048_rules_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
