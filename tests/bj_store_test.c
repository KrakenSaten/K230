/*
 * PG Blackjack save file: the byte layout; an exact round trip of every kind
 * of position - between rounds, a hand in play, each of the seven outcomes, a
 * double, out of chips, a new bankroll, a shoe run dry mid-round; the shoe's
 * cards conserved; chips neither lost nor made; a resumed session that plays
 * on exactly as the original; refusals of damaged, truncated, padded,
 * other-version and impossible files; and the file itself.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "bj_store.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHOE_AT 38
#define PLAYER_AT 143
#define DEALER_AT 160
#define SUM_AT 177

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

/* The same session: every field, and the live cards of the shoe and both
 * hands in order. Not memcmp: slots past a count do not decide it. */
static int same_game(const struct bj_game *a, const struct bj_game *b)
{
    return a->bankroll == b->bankroll && a->bet == b->bet && a->stake == b->stake && a->last_delta == b->last_delta &&
           a->rounds == b->rounds && a->phase == b->phase && a->outcome == b->outcome && a->doubled == b->doubled &&
           a->reshuffled == b->reshuffled && a->shoe.rng.state == b->shoe.rng.state &&
           a->shoe.shuffles == b->shoe.shuffles && a->shoe.n == b->shoe.n &&
           memcmp(a->shoe.card, b->shoe.card, a->shoe.n) == 0 && a->player.n == b->player.n &&
           memcmp(a->player.card, b->player.card, a->player.n) == 0 && a->dealer.n == b->dealer.n &&
           memcmp(a->dealer.card, b->dealer.card, a->dealer.n) == 0;
}

/* Every card in the shoe and on the table, by id. */
static int count_cards(const struct bj_game *g, int counts[BJ_DECK])
{
    int total = g->shoe.n + g->player.n + g->dealer.n;
    int i;

    memset(counts, 0, sizeof(int) * BJ_DECK);
    for (i = 0; i < g->shoe.n; i++) {
        counts[g->shoe.card[i]]++;
    }
    for (i = 0; i < g->player.n; i++) {
        counts[g->player.card[i]]++;
    }
    for (i = 0; i < g->dealer.n; i++) {
        counts[g->dealer.card[i]]++;
    }
    return total;
}

/* The shoe becomes every card not on the table, each id at most twice, with
 * these ranks on top in the order they are dealt. */
static void stack_fair(struct bj_game *g, ...)
{
    uint8_t left[BJ_DECK];
    bj_card_t picked[40];
    int k = 0;
    int n = 0;
    int c;
    int i;
    va_list ap;

    memset(left, BJ_DECKS, sizeof(left));
    for (i = 0; i < g->player.n; i++) {
        left[g->player.card[i]]--;
    }
    for (i = 0; i < g->dealer.n; i++) {
        left[g->dealer.card[i]]--;
    }
    va_start(ap, g);
    for (;;) {
        int r = va_arg(ap, int);
        int s;

        if (r == 0) {
            break;
        }
        for (s = 0; s < BJ_SUITS && !left[bj_card(r, (enum bj_suit)s)]; s++) {
        }
        picked[k] = bj_card(r, (enum bj_suit)(s % BJ_SUITS));
        left[picked[k++]]--;
    }
    va_end(ap);
    for (c = 0; c < BJ_DECK; c++) {
        for (i = 0; i < left[c]; i++) {
            g->shoe.card[n++] = (bj_card_t)c;
        }
    }
    for (i = k - 1; i >= 0; i--) {
        g->shoe.card[n++] = picked[i];
    }
    g->shoe.n = (uint8_t)n;
}

/* Rounds by a fixed recipe: double on 10 or 11, hit below 15, else stand. */
static void play_rounds(struct bj_game *g, int rounds)
{
    int r;

    for (r = 0; r < rounds; r++) {
        if (bj_deal(g) != BJ_OK) {
            bj_new_bankroll(g);
            continue;
        }
        while (g->phase == BJ_PLAYER) {
            int total = bj_hand_total(&g->player, NULL);

            if ((total == 10 || total == 11) && bj_double(g) == BJ_OK) {
                break;
            }
            if (total < 15) {
                bj_hit(g);
            } else {
                bj_stand(g);
            }
        }
        if (r % 3 == 0) {
            bj_bet_up(g);
        }
    }
}

static void reseal(uint8_t *b)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < SUM_AT; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    b[SUM_AT] = (uint8_t)h;
    b[SUM_AT + 1] = (uint8_t)(h >> 8);
    b[SUM_AT + 2] = (uint8_t)(h >> 16);
    b[SUM_AT + 3] = (uint8_t)(h >> 24);
}

/* Encode, decode into a scribbled game, and compare: the same session, the
 * same chips, the same cards, and the same bytes again. */
static void round_trip(const char *name, const struct bj_game *g)
{
    struct bj_game back;
    uint8_t blob[BJ_SAVE_SIZE];
    uint8_t again[BJ_SAVE_SIZE];
    int before[BJ_DECK];
    int after[BJ_DECK];
    char what[128];
    int n;

    memset(&back, 0xEE, sizeof(back));
    n = bj_save_encode(g, blob, sizeof(blob));
    snprintf(what, sizeof(what), "%s: encodes and decodes to the same session", name);
    check(what, n == BJ_SAVE_SIZE && bj_save_decode(&back, blob, (size_t)n) == 0 && same_game(g, &back));
    snprintf(what, sizeof(what), "%s: the same chips, on the table and off it", name);
    check(what, bj_chips(&back) == bj_chips(g) && back.bankroll == g->bankroll && back.stake == g->stake);
    snprintf(what, sizeof(what), "%s: every card where it was, none gained or lost", name);
    check(what, count_cards(g, before) == count_cards(&back, after) && memcmp(before, after, sizeof(before)) == 0);
    snprintf(what, sizeof(what), "%s: and encodes to the same bytes again", name);
    check(what, bj_save_encode(&back, again, sizeof(again)) == BJ_SAVE_SIZE && memcmp(blob, again, sizeof(blob)) == 0);
}

static void test_codec(void)
{
    struct bj_game g;
    struct bj_game back;
    uint8_t blob[BJ_SAVE_SIZE + 8];
    int counts[BJ_DECK];
    int n;
    int i;
    int ok;

    bj_new_session(&g, 424242);
    n = bj_save_encode(&g, blob, sizeof(blob));
    check("a save is 181 bytes", n == BJ_SAVE_SIZE && n == 181);
    check("it starts with PGBJ and version 1", memcmp(blob, "PGBJ", 4) == 0 && blob[4] == 1 && blob[5] == 0);
    check("the bankroll is little-endian at 6", blob[6] == (1000 & 0xFF) && blob[7] == 1000 >> 8 && blob[8] == 0);
    check("the bet at 10, the phase at 26", blob[10] == 10 && blob[26] == BJ_BETTING);
    check("the full shoe's count at 38, then its cards, bottom first",
          blob[SHOE_AT] == BJ_SHOE && memcmp(blob + SHOE_AT + 1, g.shoe.card, BJ_SHOE) == 0);
    ok = blob[PLAYER_AT] == 0 && blob[DEALER_AT] == 0;
    for (i = 0; i < BJ_HAND_MAX; i++) {
        ok &= blob[PLAYER_AT + 1 + i] == 0xFF && blob[DEALER_AT + 1 + i] == 0xFF;
    }
    check("empty hands write empty slots as FF", ok);
    bj_save_decode(&back, blob, BJ_SAVE_SIZE);
    count_cards(&back, counts);
    ok = 1;
    for (i = 0; i < BJ_DECK; i++) {
        ok &= counts[i] == BJ_DECKS;
    }
    check("a new session's shoe comes back holding every card exactly twice", ok && back.shoe.n == BJ_SHOE);

    /* Between rounds. */
    round_trip("a new session", &g);
    for (i = 0; i < 4; i++) {
        bj_bet_up(&g);
    }
    round_trip("a raised bet", &g);

    /* A hand in play. */
    stack_fair(&g, 1, 10, 2, 7, 1, 2, 3, 0); /* player A 2, dealer 10 7; hits A 2 3 */
    bj_deal(&g);
    check("(a hand in play)", g.phase == BJ_PLAYER && g.stake == 50 && g.bankroll == 950);
    round_trip("a hand just dealt", &g);
    bj_hit(&g);
    bj_hit(&g);
    bj_hit(&g);
    check("(a soft 19 in five cards, still live)", g.phase == BJ_PLAYER && g.player.n == 5 &&
                                                      bj_hand_total(&g.player, NULL) == 19);
    round_trip("a long hand in play", &g);
    {
        uint8_t b[BJ_SAVE_SIZE];

        bj_save_encode(&g, b, sizeof(b));
        check("the hole card is saved as a card like any other", b[DEALER_AT] == 2 &&
                                                                     b[DEALER_AT + 2] == g.dealer.card[1]);
        check("the stake in play is at 14", b[14] == 50 && b[15] == 0);
    }

    /* A resumed hand plays on exactly as the original: the same cards come. */
    {
        struct bj_game resumed;
        uint8_t b[BJ_SAVE_SIZE];

        bj_save_encode(&g, b, sizeof(b));
        bj_save_decode(&resumed, b, sizeof(b));
        bj_stand(&g);
        bj_stand(&resumed);
        check("a resumed hand settles exactly as the original", same_game(&g, &resumed) && g.phase == BJ_SETTLED);
        check("with the same payout", resumed.last_delta == g.last_delta && resumed.bankroll == g.bankroll);
        play_rounds(&g, 60);
        play_rounds(&resumed, 60);
        check("and sixty rounds later they are still the same session", same_game(&g, &resumed));
    }

    /* The seven outcomes, a double each way, out of chips, a new bankroll. */
    bj_new_session(&g, 5);
    stack_fair(&g, 1, 9, 13, 7, 0);
    bj_deal(&g);
    check("(blackjack)", g.outcome == BJ_PLAYER_BLACKJACK);
    round_trip("a player blackjack, paid 3:2", &g);
    stack_fair(&g, 10, 10, 9, 7, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("(win)", g.outcome == BJ_PLAYER_WINS);
    round_trip("a win", &g);
    stack_fair(&g, 10, 10, 7, 6, 6, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("(dealer bust)", g.outcome == BJ_DEALER_BUSTS);
    round_trip("a dealer bust", &g);
    stack_fair(&g, 10, 10, 7, 7, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("(push)", g.outcome == BJ_PUSH);
    round_trip("a push", &g);
    stack_fair(&g, 10, 10, 6, 9, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("(dealer win)", g.outcome == BJ_DEALER_WINS);
    round_trip("a loss", &g);
    stack_fair(&g, 10, 10, 6, 9, 9, 0);
    bj_deal(&g);
    bj_hit(&g);
    check("(bust)", g.outcome == BJ_PLAYER_BUSTS);
    round_trip("a bust", &g);
    stack_fair(&g, 10, 1, 9, 12, 0);
    bj_deal(&g);
    check("(dealer blackjack)", g.outcome == BJ_DEALER_BLACKJACK);
    round_trip("a dealer blackjack", &g);
    stack_fair(&g, 6, 10, 5, 7, 10, 0);
    bj_deal(&g);
    bj_double(&g);
    check("(doubled win)", g.doubled && g.outcome == BJ_PLAYER_WINS && g.stake == 20 && g.last_delta == 20);
    round_trip("a doubled win", &g);
    {
        uint8_t b[BJ_SAVE_SIZE];

        bj_save_encode(&g, b, sizeof(b));
        check("its payout state is in the file: stake 20, result +20, doubled", b[14] == 20 && b[18] == 20 &&
                                                                                   b[19] == 0 && b[28] == 1);
    }
    g.bankroll = 40;
    g.bet = 20;
    stack_fair(&g, 6, 10, 5, 7, 2, 0);
    bj_deal(&g);
    bj_double(&g);
    check("(doubled loss to nothing)", g.doubled && g.outcome == BJ_DEALER_WINS && g.bankroll == 0);
    round_trip("a doubled loss, out of chips", &g);
    {
        uint8_t b[BJ_SAVE_SIZE];

        bj_save_encode(&g, b, sizeof(b));
        check("a negative result is stored as its two's complement", b[18] == 0xD8 && b[19] == 0xFF && b[21] == 0xFF);
    }
    bj_new_bankroll(&g);
    round_trip("a new bankroll", &g);

    /* Far into a session, through shoes, and a shoe run dry mid-round. */
    bj_new_session(&g, 99);
    play_rounds(&g, 333);
    check("(far into a session)", g.rounds > 250 && g.shoe.shuffles > 5);
    round_trip("three hundred rounds in", &g);
    if (!bj_can_deal(&g)) {
        bj_new_bankroll(&g);
    }
    stack_fair(&g, 2, 9, 3, 7, 0); /* 5 against 16 */
    bj_deal(&g);
    g.shoe.n = 0;
    bj_hit(&g);
    check("(a shoe run dry mid-round, refilled)", g.phase == BJ_PLAYER && g.player.n == 3 && g.shoe.n == BJ_SHOE - 5);
    round_trip("a shoe refilled mid-round", &g);

    /* Refusals. */
    bj_new_session(&g, 31);
    stack_fair(&g, 10, 9, 5, 7, 0);
    bj_deal(&g);
    n = bj_save_encode(&g, blob, sizeof(blob));
    {
        int refused = 0;
        int untouched = 1;
        int bit;

        for (i = 0; i < BJ_SAVE_SIZE; i++) {
            for (bit = 0; bit < 8; bit++) {
                uint8_t copy[BJ_SAVE_SIZE];

                memcpy(copy, blob, sizeof(copy));
                copy[i] ^= (uint8_t)(1u << bit);
                memset(&back, 0x5A, sizeof(back));
                refused += bj_save_decode(&back, copy, BJ_SAVE_SIZE) == -1;
                untouched &= back.bankroll == 0x5A5A5A5Au;
            }
        }
        check("flipping any one of the 1448 bits is refused", refused == BJ_SAVE_SIZE * 8);
        check("and a refused decode restores nothing", untouched);
    }
    check("180 bytes are refused (truncated)", bj_save_decode(&back, blob, 180) == -1);
    check("182 bytes are refused (padded)", bj_save_decode(&back, blob, 182) == -1);
    check("a short encode buffer is refused", bj_save_encode(&g, blob, 180) == -1);
    {
        static const struct {
            const char *what;
            int at;
            uint8_t value;
        } sealed[] = {
            { "from version 2", 4, 2 },
            { "with a version high byte", 5, 1 },
            { "with a phase that does not exist", 26, 3 },
            { "with an outcome that does not exist", 27, 8 },
            { "with a double flag of 2", 28, 2 },
            { "with a stuck generator", 30, 0 },
            { "with a bet off the step", 10, 15 },
            { "with a stake that is not the bet in play", 14, 20 },
            { "with a result mid-hand", 18, 10 },
            { "settled with no outcome shown", 26, BJ_SETTLED },
            { "with a shoe count past its slots", SHOE_AT, BJ_SHOE + 1 },
            { "with a hand count past its slots", PLAYER_AT, BJ_HAND_MAX + 1 },
            { "with a card past the shoe's count", SHOE_AT + 1 + 100, 3 },
            { "with a card that does not exist", SHOE_AT + 1, 52 },
        };
        size_t k;
        uint8_t copy[BJ_SAVE_SIZE];

        for (k = 0; k < sizeof(sealed) / sizeof(sealed[0]); k++) {
            char what[128];

            memcpy(copy, blob, sizeof(copy));
            if (sealed[k].at == 30) {
                memset(copy + 30, 0, 4);
            } else {
                copy[sealed[k].at] = sealed[k].value;
            }
            reseal(copy);
            snprintf(what, sizeof(what), "a sealed file %s is refused", sealed[k].what);
            check(what, bj_save_decode(&back, copy, BJ_SAVE_SIZE) == -1);
        }
        memcpy(copy, blob, sizeof(copy));
        copy[SHOE_AT + copy[SHOE_AT]] = copy[PLAYER_AT + 1];
        copy[SHOE_AT + copy[SHOE_AT] - 1] = copy[PLAYER_AT + 1];
        reseal(copy);
        check("a sealed file with a card three times in a two-deck shoe is refused",
              bj_save_decode(&back, copy, BJ_SAVE_SIZE) == -1);
        /* The shoe's top card, a king, moved onto the player's 15 with no
         * card copied: 25, and still in play. */
        memcpy(copy, blob, sizeof(copy));
        copy[PLAYER_AT] = 3;
        copy[PLAYER_AT + 3] = copy[SHOE_AT + copy[SHOE_AT]];
        copy[SHOE_AT]--;
        copy[SHOE_AT + 1 + copy[SHOE_AT]] = 0xFF;
        reseal(copy);
        check("a sealed file with a busted hand still in play is refused",
              bj_hand_total(&g.player, NULL) == 15 && bj_card_rank(copy[PLAYER_AT + 3]) == BJ_KING &&
                  bj_save_decode(&back, copy, BJ_SAVE_SIZE) == -1);
        memcpy(copy, blob, sizeof(copy));
        copy[6] = 0x77;
        reseal(copy);
        check("while a sealed file with any other bankroll is a valid session", bj_save_decode(&back, copy, 181) == 0);
        memcpy(copy, blob, sizeof(copy));
        reseal(copy);
        check("and the untouched blob, resealed, decodes", bj_save_decode(&back, copy, BJ_SAVE_SIZE) == 0);
    }
    {
        struct bj_game bad = g;

        bad.stake = 30;
        check("an impossible position is never written", bj_save_encode(&bad, blob, sizeof(blob)) == -1);
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
    char root[] = "/tmp/bj_store.XXXXXX";
    char nested[256];
    struct bj_game g;
    struct bj_game back;
    uint8_t blob[BJ_SAVE_SIZE];

    if (!mkdtemp(root)) {
        check("temporary directory", 0);
        return;
    }
    snprintf(nested, sizeof(nested), "%s/a/b", root);
    setenv("POCKETOS_STATE_DIR", nested, 1);
    check("the path is blackjack/game.v1 under the state directory",
          strstr(bj_store_path(), "/a/b/blackjack/game.v1") != NULL);
    check("no file is 1", bj_store_load(&back) == 1);
    bj_new_session(&g, 88);
    play_rounds(&g, 25);
    stack_fair(&g, 10, 9, 5, 7, 0);
    bj_deal(&g);
    check("a save creates the directories", bj_store_save(&g) == 0 && exists(bj_store_path()));
    check("and leaves no temporary file", entries(bj_store_dir()) == 1);
    check("it loads as the same session, hand in play", bj_store_load(&back) == 0 && same_game(&g, &back) &&
                                                            back.phase == BJ_PLAYER);

    /* Reopened, it plays on as the original would, chip for chip. */
    {
        struct bj_game straight = g;
        struct bj_game resumed;

        bj_store_load(&resumed);
        bj_hit(&straight);
        bj_hit(&resumed);
        if (straight.phase == BJ_PLAYER) {
            bj_stand(&straight);
            bj_stand(&resumed);
        }
        play_rounds(&straight, 40);
        play_rounds(&resumed, 40);
        check("a saved and loaded session plays on exactly as the original", same_game(&straight, &resumed));
    }

    /* A new session, or a new bankroll, replaces the old one. */
    {
        struct bj_game fresh;

        bj_new_session(&fresh, 99);
        check("saving a new session replaces the file", bj_store_save(&fresh) == 0 && bj_store_load(&back) == 0 &&
                                                            same_game(&fresh, &back) && !same_game(&g, &back));
        check("still one file", entries(bj_store_dir()) == 1);
        fresh.bankroll = 0;
        fresh.phase = BJ_BETTING;
        bj_new_bankroll(&fresh);
        check("and so does a new bankroll", bj_store_save(&fresh) == 0 && bj_store_load(&back) == 0 &&
                                               back.bankroll == BJ_BANKROLL_START && back.phase == BJ_BETTING);
    }

    bj_save_encode(&g, blob, sizeof(blob));
    write_bytes(bj_store_path(), blob, 90);
    memset(&back, 0x33, sizeof(back));
    check("a truncated file is refused", bj_store_load(&back) == -1 && back.bankroll == 0x33333333u);
    check("and left in place", exists(bj_store_path()));
    {
        uint8_t longer[BJ_SAVE_SIZE + 3];

        memcpy(longer, blob, sizeof(blob));
        memset(longer + BJ_SAVE_SIZE, 0, 3);
        write_bytes(bj_store_path(), longer, sizeof(longer));
        check("a file with trailing bytes is refused", bj_store_load(&back) == -1);
    }
    write_bytes(bj_store_path(), (const uint8_t *)"", 0);
    check("an empty file is refused", bj_store_load(&back) == -1);
    check("the next save replaces a damaged file", bj_store_save(&g) == 0 && bj_store_load(&back) == 0 &&
                                                       same_game(&g, &back));
    {
        char blocker[300];

        snprintf(blocker, sizeof(blocker), "%s/blocked", root);
        write_bytes(blocker, (const uint8_t *)"x", 1);
        setenv("POCKETOS_STATE_DIR", blocker, 1);
        check("a save that cannot make its directory fails", bj_store_save(&g) == -1);
        check("and loading there is an error", bj_store_load(&back) != 0);
    }
    {
        char cmd[320];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", root);
        }
    }
    unsetenv("POCKETOS_STATE_DIR");
    check("the default is /var/lib/pocketos/blackjack/game.v1",
          strcmp(bj_store_path(), "/var/lib/pocketos/blackjack/game.v1") == 0);
}

int main(void)
{
    test_codec();
    test_file();
    printf("bj_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
