/*
 * PG Solitaire save file: the byte layout, an exact round trip of positions
 * from every stage of a game, refusals of damaged, truncated, foreign,
 * other-version and impossible files, the file itself, and a resumed game
 * that plays on exactly as the original.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "sol_store.h"

#include <dirent.h>
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

/* Two games are the same game: every pile's cards, in order, and face-down
 * counts, and every counter. Compared field by field, not by memcmp, so
 * padding and bytes past a pile's top do not decide it. */
static int same_game(const struct sol_game *a, const struct sol_game *b)
{
    int p;

    if (a->seed != b->seed || a->moves != b->moves || a->passes != b->passes || a->won != b->won) {
        return 0;
    }
    for (p = 0; p < SOL_PILES; p++) {
        if (a->pile[p].n != b->pile[p].n || a->pile[p].down != b->pile[p].down ||
            memcmp(a->pile[p].card, b->pile[p].card, a->pile[p].n) != 0) {
            return 0;
        }
    }
    return 1;
}

/* Play a deal forward by a fixed, legal recipe: every foundation move there
 * is, else a tableau move, else a draw. */
static void play(struct sol_game *g, int steps)
{
    int s;

    for (s = 0; s < steps && !g->won; s++) {
        int from;
        int to;
        int moved = 0;

        for (from = SOL_WASTE; from < SOL_PILES && !moved; from++) {
            int top = g->pile[from].n - 1;
            int home = sol_foundation_target(g, from, top);

            if (top >= 0 && !sol_is_foundation(from) && home >= 0) {
                moved = sol_move(g, from, top, home, NULL) == SOL_OK;
            }
        }
        for (from = SOL_WASTE; from < SOL_PILES && !moved; from++) {
            int index = sol_is_column(from) ? g->pile[from].down : g->pile[from].n - 1;

            if (sol_is_foundation(from) || g->pile[from].n == 0) {
                continue;
            }
            for (to = SOL_T0; to < SOL_PILES && !moved; to++) {
                /* Kings shuttling between empty columns would loop forever. */
                if (to != from && !(g->pile[to].n == 0 && index == 0 && sol_is_column(from))) {
                    moved = sol_move(g, from, index, to, NULL) == SOL_OK;
                }
            }
        }
        if (!moved) {
            sol_draw(g);
        }
    }
}

static void reseal(uint8_t *b)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < 97; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    b[97] = (uint8_t)h;
    b[98] = (uint8_t)(h >> 8);
    b[99] = (uint8_t)(h >> 16);
    b[100] = (uint8_t)(h >> 24);
}

static void test_codec(void)
{
    struct sol_game g;
    struct sol_game back;
    uint8_t blob[128];
    int stage;
    int n;

    sol_deal(&g, 424242);
    n = sol_save_encode(&g, blob, sizeof(blob));
    check("a save is 101 bytes", n == SOL_SAVE_SIZE && n == 101);
    check("it starts with PGSL and version 1", memcmp(blob, "PGSL", 4) == 0 && blob[4] == 1 && blob[5] == 0);
    check("the seed is little-endian at 6", (uint32_t)(blob[6] | blob[7] << 8 | blob[8] << 16 | (uint32_t)blob[9] << 24) == 424242u);
    check("the stock's count and face-down count come first among the piles",
          blob[19] == 24 && blob[20] == 24);
    check("the seventh column holds 7 with 6 face down", blob[19 + 2 * SOL_T0 + 12] == 7 && blob[19 + 2 * SOL_T0 + 13] == 6);
    check("the stock's cards are the first of the 52, bottom first", memcmp(blob + 45, g.pile[SOL_STOCK].card, 24) == 0);

    /* Positions from the whole life of a game, including passes and a win. */
    for (stage = 0; stage <= 12; stage++) {
        char what[96];

        sol_deal(&g, 7000u + (uint32_t)stage);
        play(&g, stage * 25);
        memset(&back, 0xEE, sizeof(back));
        n = sol_save_encode(&g, blob, sizeof(blob));
        snprintf(what, sizeof(what), "stage %d (%u moves, %u passes): encodes", stage, (unsigned)g.moves,
                 (unsigned)g.passes);
        check(what, n == SOL_SAVE_SIZE);
        snprintf(what, sizeof(what), "stage %d: decodes to the same game, card for card", stage);
        check(what, sol_save_decode(&back, blob, (size_t)n) == 0 && same_game(&g, &back));
    }
    {
        int s;
        int r;

        memset(&g, 0, sizeof(g));
        for (s = 0; s < SOL_SUITS; s++) {
            for (r = 1; r <= SOL_KING; r++) {
                g.pile[SOL_F0 + s].card[g.pile[SOL_F0 + s].n++] = sol_card(r, (enum sol_suit)s);
            }
        }
        g.won = 1;
        g.moves = 300;
        g.seed = 9;
        n = sol_save_encode(&g, blob, sizeof(blob));
        check("a won game round-trips", n == SOL_SAVE_SIZE && sol_save_decode(&back, blob, 101) == 0 &&
                                            same_game(&g, &back) && back.won);
    }

    /* Face down, stock and waste, specifically. */
    sol_deal(&g, 55);
    sol_draw(&g);
    sol_draw(&g);
    sol_draw(&g);
    n = sol_save_encode(&g, blob, sizeof(blob));
    sol_save_decode(&back, blob, 101);
    check("the waste comes back in order, face up", back.pile[SOL_WASTE].n == 3 && back.pile[SOL_WASTE].down == 0 &&
                                                        memcmp(back.pile[SOL_WASTE].card, g.pile[SOL_WASTE].card, 3) == 0);
    check("the stock comes back in order, face down", back.pile[SOL_STOCK].n == 21 &&
                                                          back.pile[SOL_STOCK].down == 21 &&
                                                          memcmp(back.pile[SOL_STOCK].card, g.pile[SOL_STOCK].card, 21) == 0);
    {
        int ok = 1;
        int c;

        for (c = 0; c < SOL_COLUMNS; c++) {
            ok &= back.pile[SOL_T0 + c].down == c;
        }
        check("every column's face-down count comes back", ok);
    }
    check("the next draw is the same card", sol_top(&back, SOL_STOCK) == sol_top(&g, SOL_STOCK));
    {
        uint8_t seen[SOL_DECK];
        int p;
        int i;
        int once = 1;

        memset(seen, 0, sizeof(seen));
        for (p = 0; p < SOL_PILES; p++) {
            for (i = 0; i < back.pile[p].n; i++) {
                seen[back.pile[p].card[i]]++;
            }
        }
        for (i = 0; i < SOL_DECK; i++) {
            once &= seen[i] == 1;
        }
        check("all 52 cards come back exactly once", once);
    }

    /* Refusals. */
    n = sol_save_encode(&g, blob, sizeof(blob));
    {
        int refused = 0;
        int untouched = 1;
        int i;

        for (i = 0; i < SOL_SAVE_SIZE; i++) {
            uint8_t copy[128];

            memcpy(copy, blob, sizeof(copy));
            copy[i] ^= 0x01;
            memset(&back, 0x5A, sizeof(back));
            refused += sol_save_decode(&back, copy, SOL_SAVE_SIZE) == -1;
            untouched &= back.moves == 0x5A5A5A5Au;
        }
        check("flipping any bit of any byte is refused", refused == SOL_SAVE_SIZE);
        check("and a refused decode restores nothing", untouched);
    }
    check("100 bytes are refused (truncated)", sol_save_decode(&back, blob, 100) == -1);
    check("102 bytes are refused (padded)", sol_save_decode(&back, blob, 102) == -1);
    check("a missing encode buffer is refused", sol_save_encode(&g, blob, 100) == -1);
    {
        uint8_t copy[128];

        memcpy(copy, blob, sizeof(copy));
        copy[4] = 2;
        reseal(copy);
        check("a sealed file from version 2 is refused", sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[45] = copy[46];
        reseal(copy);
        check("a sealed file with one card twice and one missing is refused", sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[19 + 2 * SOL_T0 + 1] = 1; /* the first column's only card face down */
        reseal(copy);
        check("a sealed file with a face-down card on top of a column is refused",
              sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[19] = 20;
        copy[21] = 7;
        reseal(copy);
        check("a sealed file whose counts do not add to 52 is refused", sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[19] = 200;
        reseal(copy);
        check("a sealed file with an impossible count is refused", sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[18] = 1;
        reseal(copy);
        check("a sealed file marked won with cards still out is refused", sol_save_decode(&back, copy, 101) == -1);
        memcpy(copy, blob, sizeof(copy));
        reseal(copy);
        check("the untouched blob, resealed, still decodes", sol_save_decode(&back, copy, 101) == 0);
    }
    {
        struct sol_game bad = g;

        bad.pile[SOL_T0].card[0] = bad.pile[SOL_T0 + 1].card[0];
        check("an impossible position is never written", sol_save_encode(&bad, blob, sizeof(blob)) == -1);
    }
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    }
    closedir(d);
    return n;
}

static void write_bytes(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(b, 1, n, f);
        fclose(f);
    }
}

static void test_file(void)
{
    char root[] = "/tmp/sol_store.XXXXXX";
    char nested[256];
    struct sol_game g;
    struct sol_game back;
    uint8_t blob[SOL_SAVE_SIZE];

    if (!mkdtemp(root)) {
        check("temporary directory", 0);
        return;
    }
    snprintf(nested, sizeof(nested), "%s/a/b", root);
    setenv("POCKETOS_STATE_DIR", nested, 1);
    check("the path is solitaire/game.v1 under the state directory",
          strstr(sol_store_path(), "/a/b/solitaire/game.v1") != NULL);
    check("no file is 1", sol_store_load(&back) == 1);
    sol_deal(&g, 88);
    play(&g, 40);
    check("a save creates the directories", sol_store_save(&g) == 0 && exists(sol_store_path()));
    check("and leaves no temporary file", entries(sol_store_dir()) == 1);
    check("it loads as the same game", sol_store_load(&back) == 0 && same_game(&g, &back));

    /* Resumed, it plays on as the original would. */
    {
        struct sol_game straight = g;
        struct sol_game resumed;

        sol_store_load(&resumed);
        play(&straight, 120);
        play(&resumed, 120);
        check("a saved and loaded game plays on exactly as the original", same_game(&straight, &resumed));
    }

    /* A new game replaces the old one. */
    {
        struct sol_game fresh;

        sol_deal(&fresh, 99);
        check("saving a new deal replaces the file", sol_store_save(&fresh) == 0 && sol_store_load(&back) == 0 &&
                                                         same_game(&fresh, &back) && !same_game(&g, &back));
        check("still one file", entries(sol_store_dir()) == 1);
    }

    sol_save_encode(&g, blob, sizeof(blob));
    write_bytes(sol_store_path(), blob, 60);
    memset(&back, 0x33, sizeof(back));
    check("a truncated file is refused", sol_store_load(&back) == -1 && back.moves == 0x33333333u);
    check("and left in place", exists(sol_store_path()));
    {
        uint8_t longer[SOL_SAVE_SIZE + 3];

        memcpy(longer, blob, sizeof(blob));
        memset(longer + SOL_SAVE_SIZE, 0, 3);
        write_bytes(sol_store_path(), longer, sizeof(longer));
        check("a file with trailing bytes is refused", sol_store_load(&back) == -1);
    }
    check("the next save replaces a damaged file", sol_store_save(&g) == 0 && sol_store_load(&back) == 0);
    {
        char blocker[300];

        snprintf(blocker, sizeof(blocker), "%s/blocked", root);
        write_bytes(blocker, (const uint8_t *)"x", 1);
        setenv("POCKETOS_STATE_DIR", blocker, 1);
        check("a save that cannot make its directory fails", sol_store_save(&g) == -1);
        check("and loading there is an error", sol_store_load(&back) != 0);
    }
    {
        char cmd[320];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", root);
        }
    }
    unsetenv("POCKETOS_STATE_DIR");
    check("the default is /var/lib/pocketos/solitaire/game.v1",
          strcmp(sol_store_path(), "/var/lib/pocketos/solitaire/game.v1") == 0);
}

int main(void)
{
    test_codec();
    test_file();
    printf("sol_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
