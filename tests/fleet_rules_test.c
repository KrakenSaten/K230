/*
 * PocketFleet rules test: the vocabulary helpers, fleet deployment
 * (bounds, overlap, transactional moves, auto-deploy), firing
 * (miss/hit/sunk/invalid), turn order, statistics, the win condition and
 * whole-match determinism.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_rules.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* FNV-1a over every defined field, so two matches can be compared without
 * relying on struct padding. */
static uint32_t digest_bytes(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t digest_board(uint32_t h, const struct fleet_board *b)
{
    int i;

    h = digest_bytes(h, b->ship_at, sizeof(b->ship_at));
    h = digest_bytes(h, b->shot, sizeof(b->shot));
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        h = digest_bytes(h, &b->ships[i].row, 1);
        h = digest_bytes(h, &b->ships[i].col, 1);
        h = digest_bytes(h, &b->ships[i].orient, 1);
        h = digest_bytes(h, &b->ships[i].hits, 1);
        h = digest_bytes(h, &b->ships[i].placed, 1);
    }
    h = digest_bytes(h, &b->ships_placed, 1);
    h = digest_bytes(h, &b->ships_afloat, 1);
    return h;
}

static uint32_t digest_game(const struct fleet_game *g)
{
    uint32_t h = 2166136261u;
    int side;

    h = digest_bytes(h, &g->seed, sizeof(g->seed));
    h = digest_bytes(h, &g->rng_setup.state, sizeof(g->rng_setup.state));
    h = digest_bytes(h, &g->rng_ai.state, sizeof(g->rng_ai.state));
    h = digest_bytes(h, &g->difficulty, 1);
    h = digest_bytes(h, &g->phase, 1);
    h = digest_bytes(h, &g->winner, 1);
    h = digest_bytes(h, &g->turn, sizeof(g->turn));
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        h = digest_board(h, &g->board[side]);
        h = digest_bytes(h, &g->stats[side].shots, sizeof(g->stats[side].shots));
        h = digest_bytes(h, &g->stats[side].hits, sizeof(g->stats[side].hits));
    }
    return h;
}

static int occupied_cells(const struct fleet_board *b)
{
    int i;
    int n = 0;

    for (i = 0; i < FLEET_CELLS; i++) {
        if (b->ship_at[i] != FLEET_NO_SHIP) {
            n++;
        }
    }
    return n;
}

/* Every placed ship covers exactly its own cells in ship_at[]. */
static int layout_consistent(const struct fleet_board *b)
{
    int ship;

    for (ship = 0; ship < FLEET_SHIP_COUNT; ship++) {
        int n;

        if (!b->ships[ship].placed) {
            continue;
        }
        for (n = 0; n < fleet_ship_length((enum fleet_ship)ship); n++) {
            int row = -1;
            int col = -1;
            int idx;

            if (fleet_board_ship_cell(b, (enum fleet_ship)ship, n, &row, &col) != 0) {
                return 0;
            }
            idx = fleet_index(row, col);
            if (idx < 0 || b->ship_at[idx] != (uint8_t)ship) {
                return 0;
            }
        }
    }
    return 1;
}

static void test_vocabulary(void)
{
    enum fleet_difficulty d;
    char name[FLEET_CELL_NAME_MAX];
    int total = 0;
    int i;

    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        total += fleet_ship_length((enum fleet_ship)i);
    }
    check("roster totals 17 cells", total == FLEET_HULL_CELLS);
    check("carrier is 5", fleet_ship_length(FLEET_SHIP_CARRIER) == 5);
    check("destroyer is 2", fleet_ship_length(FLEET_SHIP_DESTROYER) == 2);
    check("out-of-range ship length is 0", fleet_ship_length((enum fleet_ship)9) == 0);
    check("ship name", strcmp(fleet_ship_name(FLEET_SHIP_CRUISER), "Cruiser") == 0);
    check("out-of-range ship name", strcmp(fleet_ship_name((enum fleet_ship)9), "?") == 0);
    check("result name", strcmp(fleet_shot_result_name(FLEET_SHOT_SUNK), "SUNK") == 0);

    for (i = 0; i < FLEET_DIFFICULTY_COUNT; i++) {
        if (fleet_difficulty_parse(fleet_difficulty_name((enum fleet_difficulty)i), &d) != 0 ||
            (int)d != i) {
            check("difficulty round trip", 0);
            return;
        }
    }
    check("difficulty round trip", 1);
    check("unknown difficulty rejected", fleet_difficulty_parse("Cadet", &d) == -1);

    check("in bounds", fleet_in_bounds(0, 0) && fleet_in_bounds(9, 9));
    check("out of bounds", !fleet_in_bounds(-1, 0) && !fleet_in_bounds(0, 10));
    check("index", fleet_index(3, 4) == 34 && fleet_index(10, 0) == -1);
    check("cell name A1", fleet_cell_name(0, 0, name, sizeof(name)) == 0 &&
                          strcmp(name, "A1") == 0);
    check("cell name J10", fleet_cell_name(9, 9, name, sizeof(name)) == 0 &&
                           strcmp(name, "J10") == 0);
    check("cell name D7", fleet_cell_name(6, 3, name, sizeof(name)) == 0 &&
                          strcmp(name, "D7") == 0);
    check("cell name out of bounds", fleet_cell_name(10, 0, name, sizeof(name)) == -1 &&
                                     strcmp(name, "?") == 0);
}

static void test_placement(void)
{
    struct fleet_board b;
    int row = -1;
    int col = -1;

    fleet_board_clear(&b);
    check("cleared board is empty", occupied_cells(&b) == 0 && b.ships_placed == 0);
    check("cleared board has a full fleet afloat", b.ships_afloat == FLEET_SHIP_COUNT);
    check("cleared board is incomplete", !fleet_board_complete(&b));

    check("carrier fits at A1 horizontal",
          fleet_board_can_place(&b, FLEET_SHIP_CARRIER, 0, 0, FLEET_HORIZONTAL));
    check("carrier fits at F1 horizontal",
          fleet_board_can_place(&b, FLEET_SHIP_CARRIER, 0, 5, FLEET_HORIZONTAL));
    check("carrier overruns at G1 horizontal",
          !fleet_board_can_place(&b, FLEET_SHIP_CARRIER, 0, 6, FLEET_HORIZONTAL));
    check("carrier overruns at A7 vertical",
          !fleet_board_can_place(&b, FLEET_SHIP_CARRIER, 6, 0, FLEET_VERTICAL));
    check("negative coordinates rejected",
          !fleet_board_can_place(&b, FLEET_SHIP_CARRIER, -1, 0, FLEET_HORIZONTAL));
    check("bad orientation rejected",
          !fleet_board_can_place(&b, FLEET_SHIP_CARRIER, 0, 0, (enum fleet_orient)7));

    check("place carrier", fleet_board_place(&b, FLEET_SHIP_CARRIER, 0, 0,
                                             FLEET_HORIZONTAL) == 0);
    check("carrier occupies 5 cells", occupied_cells(&b) == 5);
    check("carrier recorded", b.ships_placed == 1 && b.ships[FLEET_SHIP_CARRIER].placed);
    check("carrier layout consistent", layout_consistent(&b));
    check("carrier cell 4 is E1",
          fleet_board_ship_cell(&b, FLEET_SHIP_CARRIER, 4, &row, &col) == 0 &&
          row == 0 && col == 4);
    check("carrier cell 5 out of range",
          fleet_board_ship_cell(&b, FLEET_SHIP_CARRIER, 5, &row, &col) == -1);

    check("overlapping battleship rejected",
          fleet_board_place(&b, FLEET_SHIP_BATTLESHIP, 0, 3, FLEET_HORIZONTAL) == -1);
    check("rejected placement changed nothing",
          occupied_cells(&b) == 5 && b.ships_placed == 1);
    check("touching battleship accepted",
          fleet_board_place(&b, FLEET_SHIP_BATTLESHIP, 0, 5, FLEET_HORIZONTAL) == 0);
    check("two ships occupy 9 cells", occupied_cells(&b) == 9);

    /* Moving a ship onto itself is legal; the old cells are freed first. */
    check("carrier moves onto itself",
          fleet_board_place(&b, FLEET_SHIP_CARRIER, 0, 0, FLEET_VERTICAL) == 0);
    check("moved carrier still occupies 5 cells", occupied_cells(&b) == 9);
    check("moved carrier layout consistent", layout_consistent(&b));
    check("ship count unchanged by a move", b.ships_placed == 2);

    /* An illegal move leaves the ship where it was. */
    check("illegal move rejected",
          fleet_board_place(&b, FLEET_SHIP_CARRIER, 0, 5, FLEET_HORIZONTAL) == -1);
    check("carrier restored after illegal move",
          b.ships[FLEET_SHIP_CARRIER].placed &&
          b.ships[FLEET_SHIP_CARRIER].row == 0 &&
          b.ships[FLEET_SHIP_CARRIER].col == 0 &&
          b.ships[FLEET_SHIP_CARRIER].orient == FLEET_VERTICAL);
    check("board intact after illegal move",
          occupied_cells(&b) == 9 && b.ships_placed == 2 && layout_consistent(&b));

    check("unplace carrier", fleet_board_unplace(&b, FLEET_SHIP_CARRIER) == 0);
    check("unplace frees the cells", occupied_cells(&b) == 4 && b.ships_placed == 1);
    check("unplace twice rejected", fleet_board_unplace(&b, FLEET_SHIP_CARRIER) == -1);
}

static void test_autoplace(void)
{
    struct fleet_board a;
    struct fleet_board b;
    struct fleet_board c;
    struct fleet_rng rng;
    int i;
    int all_complete = 1;
    int all_consistent = 1;

    for (i = 0; i < 200; i++) {
        fleet_board_clear(&a);
        fleet_rng_seed(&rng, (uint32_t)(i + 1));
        if (fleet_board_autoplace(&a, &rng) != 0) {
            all_complete = 0;
            break;
        }
        all_complete &= fleet_board_complete(&a);
        all_complete &= occupied_cells(&a) == FLEET_HULL_CELLS;
        all_consistent &= layout_consistent(&a);
    }
    check("auto-deploy completes the fleet for 200 seeds", all_complete);
    check("auto-deploy never overlaps", all_consistent);

    fleet_board_clear(&a);
    fleet_rng_seed(&rng, 4242u);
    fleet_board_autoplace(&a, &rng);
    fleet_board_clear(&b);
    fleet_rng_seed(&rng, 4242u);
    fleet_board_autoplace(&b, &rng);
    check("auto-deploy is deterministic",
          memcmp(a.ship_at, b.ship_at, sizeof(a.ship_at)) == 0);
    fleet_board_clear(&c);
    fleet_rng_seed(&rng, 4243u);
    fleet_board_autoplace(&c, &rng);
    check("a different seed deploys differently",
          memcmp(a.ship_at, c.ship_at, sizeof(a.ship_at)) != 0);

    /* Ships already placed by hand are kept. */
    fleet_board_clear(&a);
    fleet_board_place(&a, FLEET_SHIP_CARRIER, 0, 0, FLEET_HORIZONTAL);
    fleet_rng_seed(&rng, 7u);
    check("auto-deploy fills the rest", fleet_board_autoplace(&a, &rng) == 0);
    check("hand placement kept",
          a.ships[FLEET_SHIP_CARRIER].row == 0 && a.ships[FLEET_SHIP_CARRIER].col == 0 &&
          a.ships[FLEET_SHIP_CARRIER].orient == FLEET_HORIZONTAL);
    check("auto-deploy completed the fleet",
          fleet_board_complete(&a) && occupied_cells(&a) == FLEET_HULL_CELLS);
}

static void test_firing(void)
{
    struct fleet_board b;
    int sunk = 0;

    fleet_board_clear(&b);
    fleet_board_place(&b, FLEET_SHIP_DESTROYER, 4, 4, FLEET_HORIZONTAL);

    check("shot off grid is invalid",
          fleet_board_fire(&b, -1, 0, &sunk) == FLEET_SHOT_INVALID);
    check("shot past the grid is invalid",
          fleet_board_fire(&b, 0, FLEET_GRID, &sunk) == FLEET_SHOT_INVALID);
    check("empty water misses", fleet_board_fire(&b, 0, 0, &sunk) == FLEET_SHOT_MISS);
    check("miss reports no sunk ship", sunk == -1);
    check("repeat shot is invalid", fleet_board_fire(&b, 0, 0, &sunk) == FLEET_SHOT_INVALID);
    check("first destroyer cell hits", fleet_board_fire(&b, 4, 4, &sunk) == FLEET_SHOT_HIT);
    check("hit reports no sunk ship", sunk == -1);
    check("destroyer still afloat",
          b.ships_afloat == FLEET_SHIP_COUNT && !fleet_board_ship_sunk(&b, FLEET_SHIP_DESTROYER));
    check("second destroyer cell sinks it",
          fleet_board_fire(&b, 4, 5, &sunk) == FLEET_SHOT_SUNK);
    check("sunk reports the ship", sunk == FLEET_SHIP_DESTROYER);
    check("afloat count drops", b.ships_afloat == FLEET_SHIP_COUNT - 1);
    check("ship reports sunk", fleet_board_ship_sunk(&b, FLEET_SHIP_DESTROYER));
    check("sunk cells cannot be fired at again",
          fleet_board_fire(&b, 4, 4, &sunk) == FLEET_SHOT_INVALID);
    check("NULL sunk pointer tolerated",
          fleet_board_fire(&b, 9, 9, NULL) == FLEET_SHOT_MISS);
}

/* Fire at every opponent hull cell in index order; the opponent answers with
 * a fixed sweep so the turn machine is exercised from both sides. */
static uint32_t play_scripted_match(uint32_t seed, enum fleet_difficulty difficulty,
                                    struct fleet_game *out, int *player_shots,
                                    int *opponent_shots)
{
    struct fleet_game g;
    struct fleet_rng deploy;
    int opponent_cell = 0;
    int i;

    fleet_game_new(&g, seed, difficulty);
    /* The player deploys from a stream of their own, mimicking auto-deploy
     * in the UI without disturbing the match streams. */
    fleet_rng_seed(&deploy, seed ^ 0x5A5A5A5Au);
    fleet_board_autoplace(&g.board[FLEET_SIDE_PLAYER], &deploy);
    fleet_game_start(&g);

    if (player_shots) {
        *player_shots = 0;
    }
    if (opponent_shots) {
        *opponent_shots = 0;
    }
    for (i = 0; i < FLEET_CELLS && !fleet_game_is_over(&g); i++) {
        if (g.board[FLEET_SIDE_OPPONENT].ship_at[i] == FLEET_NO_SHIP) {
            continue;
        }
        if (fleet_game_fire(&g, FLEET_SIDE_PLAYER, i / FLEET_GRID, i % FLEET_GRID,
                            NULL) != FLEET_SHOT_INVALID && player_shots) {
            (*player_shots)++;
        }
        if (fleet_game_is_over(&g)) {
            break;
        }
        while (opponent_cell < FLEET_CELLS &&
               fleet_game_fire(&g, FLEET_SIDE_OPPONENT, opponent_cell / FLEET_GRID,
                               opponent_cell % FLEET_GRID, NULL) == FLEET_SHOT_INVALID) {
            opponent_cell++;
        }
        opponent_cell++;
        if (opponent_shots) {
            (*opponent_shots)++;
        }
    }
    if (out) {
        *out = g;
    }
    return digest_game(&g);
}

static void test_match(void)
{
    struct fleet_game g;
    struct fleet_rng deploy;
    int sunk = 0;
    int player_shots = 0;
    int opponent_shots = 0;
    uint32_t first;
    uint32_t second;
    uint32_t other;

    fleet_game_new(&g, 12345u, FLEET_COMMANDER);
    check("new match is in deployment", g.phase == FLEET_PHASE_DEPLOY);
    check("new match has no winner", g.winner == FLEET_SIDE_NONE && g.turn == 0);
    check("new match keeps the seed and difficulty",
          g.seed == 12345u && g.difficulty == FLEET_COMMANDER);
    check("opponent fleet is deployed",
          fleet_board_complete(&g.board[FLEET_SIDE_OPPONENT]) &&
          occupied_cells(&g.board[FLEET_SIDE_OPPONENT]) == FLEET_HULL_CELLS);
    check("player waters start empty",
          occupied_cells(&g.board[FLEET_SIDE_PLAYER]) == 0);
    check("unknown difficulty falls back", (fleet_game_new(&g, 1u,
          (enum fleet_difficulty)99), g.difficulty == FLEET_OFFICER));

    fleet_game_new(&g, 12345u, FLEET_COMMANDER);
    check("cannot start with an incomplete fleet", fleet_game_start(&g) == -1);
    check("cannot fire during deployment",
          fleet_game_fire(&g, FLEET_SIDE_PLAYER, 0, 0, &sunk) == FLEET_SHOT_INVALID);

    fleet_rng_seed(&deploy, 999u);
    fleet_board_autoplace(&g.board[FLEET_SIDE_PLAYER], &deploy);
    check("start accepted once deployed", fleet_game_start(&g) == 0);
    check("player moves first", g.phase == FLEET_PHASE_PLAYER && g.turn == 1);
    check("starting twice rejected", fleet_game_start(&g) == -1);

    check("target board is the opponent",
          fleet_game_target(&g, FLEET_SIDE_PLAYER) == &g.board[FLEET_SIDE_OPPONENT]);
    check("opponent targets the player",
          fleet_game_target(&g, FLEET_SIDE_OPPONENT) == &g.board[FLEET_SIDE_PLAYER]);

    check("opponent cannot fire out of turn",
          fleet_game_fire(&g, FLEET_SIDE_OPPONENT, 0, 0, &sunk) == FLEET_SHOT_INVALID);
    check("out-of-turn shot changed nothing",
          g.phase == FLEET_PHASE_PLAYER && g.stats[FLEET_SIDE_OPPONENT].shots == 0);

    check("off-grid shot is invalid",
          fleet_game_fire(&g, FLEET_SIDE_PLAYER, 0, -3, &sunk) == FLEET_SHOT_INVALID);
    check("invalid shot does not pass the turn",
          g.phase == FLEET_PHASE_PLAYER && g.stats[FLEET_SIDE_PLAYER].shots == 0);

    check("player fires",
          fleet_game_fire(&g, FLEET_SIDE_PLAYER, 0, 0, &sunk) != FLEET_SHOT_INVALID);
    check("turn passes to the opponent", g.phase == FLEET_PHASE_OPPONENT);
    check("turn counter waits for the round", g.turn == 1);
    check("player shot counted", g.stats[FLEET_SIDE_PLAYER].shots == 1);
    check("opponent fires",
          fleet_game_fire(&g, FLEET_SIDE_OPPONENT, 0, 0, &sunk) != FLEET_SHOT_INVALID);
    check("turn returns to the player", g.phase == FLEET_PHASE_PLAYER);
    check("round advances", g.turn == 2);
    check("repeat shot is invalid and keeps the turn",
          fleet_game_fire(&g, FLEET_SIDE_PLAYER, 0, 0, &sunk) == FLEET_SHOT_INVALID &&
          g.phase == FLEET_PHASE_PLAYER);

    /* Hits are counted, misses are not. */
    {
        struct fleet_game h;
        int i;
        int hits = 0;

        fleet_game_new(&h, 77u, FLEET_RECRUIT);
        fleet_rng_seed(&deploy, 77u);
        fleet_board_autoplace(&h.board[FLEET_SIDE_PLAYER], &deploy);
        fleet_game_start(&h);
        for (i = 0; i < 10 && !fleet_game_is_over(&h); i++) {
            enum fleet_shot_result r = fleet_game_fire(&h, FLEET_SIDE_PLAYER, i, 0, NULL);

            if (r == FLEET_SHOT_HIT || r == FLEET_SHOT_SUNK) {
                hits++;
            }
            fleet_game_fire(&h, FLEET_SIDE_OPPONENT, i, 9, NULL);
        }
        check("hit statistics match the results", h.stats[FLEET_SIDE_PLAYER].hits == hits);
        check("shot statistics match the turns", h.stats[FLEET_SIDE_PLAYER].shots == (uint16_t)i);
    }

    /* Sinking every opponent ship ends the match. */
    {
        struct fleet_game done;

        first = play_scripted_match(2026u, FLEET_ADMIRAL, &done, &player_shots,
                                    &opponent_shots);
        check("match is over", fleet_game_is_over(&done));
        check("player won", done.winner == FLEET_SIDE_PLAYER);
        check("opponent fleet is sunk", done.board[FLEET_SIDE_OPPONENT].ships_afloat == 0);
        check("player fleet survived", done.board[FLEET_SIDE_PLAYER].ships_afloat > 0);
        check("17 shots sank the fleet", done.stats[FLEET_SIDE_PLAYER].shots == FLEET_HULL_CELLS);
        check("every shot hit", done.stats[FLEET_SIDE_PLAYER].hits == FLEET_HULL_CELLS);
        check("the match ended on the last round", done.turn == FLEET_HULL_CELLS);
        check("no firing after the match",
              fleet_game_fire(&done, FLEET_SIDE_PLAYER, 9, 9, NULL) == FLEET_SHOT_INVALID);
        check("the loser cannot fire after the match",
              fleet_game_fire(&done, FLEET_SIDE_OPPONENT, 9, 9, NULL) == FLEET_SHOT_INVALID);
    }
    check("player used 17 shots", player_shots == FLEET_HULL_CELLS);
    check("opponent replied every round", opponent_shots == FLEET_HULL_CELLS - 1);

    /* Determinism: same seed and same actions, same match. */
    second = play_scripted_match(2026u, FLEET_ADMIRAL, NULL, NULL, NULL);
    other = play_scripted_match(2027u, FLEET_ADMIRAL, NULL, NULL, NULL);
    check("identical seed and actions reproduce the match", first == second);
    check("a different seed produces a different match", first != other);
}

int main(void)
{
    test_vocabulary();
    test_placement();
    test_autoplace();
    test_firing();
    test_match();
    printf("fleet_rules_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
