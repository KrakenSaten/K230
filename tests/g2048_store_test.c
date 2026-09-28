/*
 * PG 2048 save file: the byte layout, refusals, the file itself, and the
 * property that matters most - a resumed game is the same game.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "g2048_store.h"

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

static void played(struct g2048_game *g, uint32_t seed, int moves)
{
    static const enum g2048_dir order[] = { G2048_LEFT, G2048_DOWN, G2048_RIGHT, G2048_DOWN };
    int i;

    g2048_new_game(g, seed, 0);
    for (i = 0; i < moves && !g->over; i++) {
        g2048_move(g, order[i % 4], NULL);
    }
}

/* Recompute the trailing checksum after editing a blob, so a test reaches
 * the check behind it rather than stopping at the checksum. */
static void reseal(uint8_t *b)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < 39; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    b[39] = (uint8_t)h;
    b[40] = (uint8_t)(h >> 8);
    b[41] = (uint8_t)(h >> 16);
    b[42] = (uint8_t)(h >> 24);
}

static void test_codec(void)
{
    struct g2048_game g;
    struct g2048_game back;
    uint8_t blob[64];
    int n;
    int i;

    played(&g, 31, 60);
    g.best = g.score + 1000;
    n = g2048_save_encode(&g, blob, sizeof(blob));
    check("a save is 43 bytes", n == G2048_SAVE_SIZE && n == 43);
    check("it starts with PG48", memcmp(blob, "PG48", 4) == 0);
    check("then version 1, little-endian", blob[4] == 1 && blob[5] == 0);
    check("the cells are the exponents, in board order", memcmp(blob + 6, g.cell, 16) == 0);
    check("the score is little-endian at 22",
          (uint32_t)(blob[22] | blob[23] << 8 | blob[24] << 16 | (uint32_t)blob[25] << 24) == g.score);
    check("the best at 26",
          (uint32_t)(blob[26] | blob[27] << 8 | blob[28] << 16 | (uint32_t)blob[29] << 24) == g.best);
    check("an unfinished, unwon game has no flags", blob[38] == 0);

    memset(&back, 0xAB, sizeof(back));
    check("it decodes", g2048_save_decode(&back, blob, (size_t)n) == 0);
    check("to the same game", memcmp(back.cell, g.cell, 16) == 0 && back.score == g.score &&
                                  back.best == g.best && back.moves == g.moves &&
                                  back.rng.state == g.rng.state && back.won == g.won &&
                                  back.keep_going == g.keep_going && back.over == g.over);

    check("a buffer one byte short is refused", g2048_save_encode(&g, blob, 42) == -1);
    {
        struct g2048_game bad = g;

        bad.cell[0] = 30;
        check("an invalid game is not written", g2048_save_encode(&bad, blob, sizeof(blob)) == -1);
    }

    /* Every single-byte change is refused, and leaves the target alone. */
    n = g2048_save_encode(&g, blob, sizeof(blob));
    {
        int refused = 0;
        int untouched = 1;

        for (i = 0; i < G2048_SAVE_SIZE; i++) {
            uint8_t copy[64];

            memcpy(copy, blob, sizeof(copy));
            copy[i] ^= 0x10;
            memset(&back, 0x5A, sizeof(back));
            if (g2048_save_decode(&back, copy, G2048_SAVE_SIZE) == -1) {
                refused++;
            }
            untouched &= back.score == 0x5A5A5A5Au;
        }
        check("flipping any one byte is refused", refused == G2048_SAVE_SIZE);
        check("and a refused decode writes nothing", untouched);
    }
    check("42 bytes are not a save", g2048_save_decode(&back, blob, 42) == -1);
    check("44 bytes are not a save", g2048_save_decode(&back, blob, 44) == -1);

    /* Past the checksum: what a correctly sealed but wrong file meets. */
    {
        uint8_t copy[64];

        memcpy(copy, blob, sizeof(copy));
        copy[4] = 2;
        reseal(copy);
        check("a sealed save from version 2 is refused", g2048_save_decode(&back, copy, 43) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[38] = 0x08;
        reseal(copy);
        check("a flag this version never writes is refused", g2048_save_decode(&back, copy, 43) == -1);
        memcpy(copy, blob, sizeof(copy));
        memset(copy + 6, 0, 16);
        copy[6] = 1;
        reseal(copy);
        check("a sealed board with one tile is refused", g2048_save_decode(&back, copy, 43) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[38] = 0x04; /* over, while moves remain */
        reseal(copy);
        check("a sealed game marked over with moves left is refused",
              g2048_save_decode(&back, copy, 43) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[38] = 0x02; /* keep going, never won */
        reseal(copy);
        check("keep going without a win is refused", g2048_save_decode(&back, copy, 43) == -1);
        memcpy(copy, blob, sizeof(copy));
        reseal(copy);
        check("and the untouched blob, resealed, still decodes", g2048_save_decode(&back, copy, 43) == 0);
    }

    /* The flags survive. */
    {
        static const uint8_t goal[16] = { 11, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

        memset(&g, 0, sizeof(g));
        memcpy(g.cell, goal, 16);
        g.score = 2048;
        g.best = 4096;
        g.moves = 300;
        g.rng.state = 77;
        g.won = 1;
        g.keep_going = 1;
        n = g2048_save_encode(&g, blob, sizeof(blob));
        check("a won game carries won and keep going", n == 43 && blob[38] == 0x03);
        check("and decodes with both", g2048_save_decode(&back, blob, 43) == 0 && back.won &&
                                           back.keep_going && !back.over);
    }
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static void write_bytes(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(b, 1, n, f);
        fclose(f);
    }
}

static int dir_entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    }
    closedir(d);
    return n;
}

static void test_file(void)
{
    char root[] = "/tmp/g2048_store.XXXXXX";
    char nested[256];
    struct g2048_game g;
    struct g2048_game back;
    uint8_t blob[G2048_SAVE_SIZE];

    if (!mkdtemp(root)) {
        check("temporary directory", 0);
        return;
    }
    snprintf(nested, sizeof(nested), "%s/deep/state", root);
    setenv("POCKETOS_STATE_DIR", nested, 1);
    check("the path is under the state directory",
          strcmp(g2048_store_path(), "/tmp") > 0 && strstr(g2048_store_path(), "/deep/state/2048/game.v1"));

    memset(&back, 0, sizeof(back));
    check("no file is 1, not an error", g2048_store_load(&back) == 1);

    played(&g, 8, 40);
    check("a save creates the missing directories", g2048_store_save(&g) == 0 &&
                                                      exists(g2048_store_path()));
    check("and leaves no temporary file", dir_entries(g2048_store_dir()) == 1);
    {
        struct stat st;

        check("the file is 43 bytes", stat(g2048_store_path(), &st) == 0 && st.st_size == 43);
    }
    check("it loads", g2048_store_load(&back) == 0 && memcmp(back.cell, g.cell, 16) == 0 &&
                          back.score == g.score && back.rng.state == g.rng.state);

    /* A resumed game is the same game, new tiles included. */
    {
        static const enum g2048_dir more[] = { G2048_UP, G2048_LEFT, G2048_DOWN, G2048_RIGHT };
        struct g2048_game straight = g;
        struct g2048_game resumed;
        int i;

        g2048_store_load(&resumed);
        for (i = 0; i < 200; i++) {
            g2048_move(&straight, more[i % 4], NULL);
            g2048_move(&resumed, more[i % 4], NULL);
        }
        check("a saved and loaded game plays on exactly as the original",
              memcmp(&straight, &resumed, sizeof(straight)) == 0);
    }

    /* A second save replaces the first in place. */
    g2048_move(&g, G2048_UP, NULL);
    g2048_move(&g, G2048_RIGHT, NULL);
    check("a later save replaces the file", g2048_store_save(&g) == 0 &&
                                              g2048_store_load(&back) == 0 && back.moves == g.moves);
    check("still one file", dir_entries(g2048_store_dir()) == 1);

    /* Damaged files are refused and left alone. */
    g2048_save_encode(&g, blob, sizeof(blob));
    write_bytes(g2048_store_path(), blob, 20);
    memset(&back, 0x33, sizeof(back));
    check("a truncated file is refused", g2048_store_load(&back) == -1);
    check("without touching the game", back.score == 0x33333333u);
    check("and is left in place", exists(g2048_store_path()));
    {
        uint8_t longer[G2048_SAVE_SIZE + 5];

        memcpy(longer, blob, sizeof(blob));
        memset(longer + G2048_SAVE_SIZE, 0, 5);
        write_bytes(g2048_store_path(), longer, sizeof(longer));
        check("a file with trailing bytes is refused", g2048_store_load(&back) == -1);
    }
    write_bytes(g2048_store_path(), (const uint8_t *)"not a save file at all, 43 bytes long ....", 43);
    check("a foreign file of the right length is refused", g2048_store_load(&back) == -1);
    check("the next save replaces a damaged file", g2048_store_save(&g) == 0 &&
                                                     g2048_store_load(&back) == 0);

    /* Somewhere that cannot be written. */
    {
        char blocker[300];

        snprintf(blocker, sizeof(blocker), "%s/blocked", root);
        write_bytes(blocker, (const uint8_t *)"x", 1);
        setenv("POCKETOS_STATE_DIR", blocker, 1);
        check("a save that cannot create its directory fails", g2048_store_save(&g) == -1);
        check("and loading there is an error, not a missing file", g2048_store_load(&back) != 0);
        unlink(blocker);
    }

    {
        char cmd[400];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", root);
        }
    }
    unsetenv("POCKETOS_STATE_DIR");
    check("without the override the default is /var/lib/pocketos/2048",
          strcmp(g2048_store_path(), "/var/lib/pocketos/2048/game.v1") == 0);
}

int main(void)
{
    test_codec();
    test_file();
    printf("g2048_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
