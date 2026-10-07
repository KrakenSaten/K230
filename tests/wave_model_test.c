/*
 * Wave's presets and history: the table, its bounds and lookups; the ring,
 * its bound, folding of repeats, and the text form the store writes - round
 * trips, damaged lines, a foreign file, and the bound on the way in.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_history.h"
#include "wave_preset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void test_presets(void)
{
    int i;
    int seen_profile[WAVE_PROFILE_COUNT] = { 0 };

    check("there are three built-in presets", wave_preset_count() == 3);
    for (i = 0; i < wave_preset_count(); i++) {
        const struct wave_preset *p = wave_preset_get(i);
        char what[96];

        snprintf(what, sizeof(what), "preset %d (%s) is inside every bound", i, p ? p->id : "?");
        check(what, wave_preset_valid(p));
        snprintf(what, sizeof(what), "preset %s is found by its id", p->id);
        check(what, wave_preset_find(p->id) == i);
        seen_profile[p->profile]++;
    }
    check("together they cover all three audible speeds",
          seen_profile[WAVE_PROFILE_NORMAL] == 1 && seen_profile[WAVE_PROFILE_FAST] == 1 &&
              seen_profile[WAVE_PROFILE_FASTEST] == 1);
    check("the default is STANDARD, on FAST",
          wave_preset_get(WAVE_PRESET_DEFAULT)->profile == WAVE_PROFILE_FAST &&
              strcmp(wave_preset_get(WAVE_PRESET_DEFAULT)->label, "STANDARD") == 0);
    check("ROBUST plays two copies at the slowest speed",
          wave_preset_get(WAVE_PRESET_ROBUST)->copies == 2 &&
              wave_preset_get(WAVE_PRESET_ROBUST)->profile == WAVE_PROFILE_NORMAL);
    check("no preset listens longer than the privacy bound", ({
              int ok = 1;
              for (i = 0; i < wave_preset_count(); i++) {
                  ok &= wave_preset_get(i)->listen_seconds <= WAVE_LISTEN_SECONDS;
              }
              ok;
          }));
    check("no capture is longer than the helper records",
          wave_preset_get(WAVE_PRESET_ROBUST)->capture_seconds <= WAVE_CAPTURE_MAX_SECONDS);
    check("an unknown id is -1", wave_preset_find("loud") == -1 && wave_preset_find(NULL) == -1);
    check("outside the table is NULL", !wave_preset_get(-1) && !wave_preset_get(3));
    check("next cycles through all and wraps",
          wave_preset_next(0) == 1 && wave_preset_next(1) == 2 && wave_preset_next(2) == 0);
    check("next of nonsense is the default", wave_preset_next(99) == WAVE_PRESET_DEFAULT);
    check("the speed words", strcmp(wave_preset_speed_label(wave_preset_get(WAVE_PRESET_QUICK)),
                                    "FASTEST") == 0);
    {
        struct wave_preset p = *wave_preset_get(0);

        p.copies = WAVE_PRESET_MAX_COPIES + 1;
        check("a preset with too many copies is refused", !wave_preset_valid(&p));
        p = *wave_preset_get(0);
        p.listen_seconds = WAVE_LISTEN_SECONDS + 1;
        check("a longer listen than the privacy bound is refused", !wave_preset_valid(&p));
        p = *wave_preset_get(0);
        p.capture_seconds = 0;
        check("a zero capture is refused", !wave_preset_valid(&p));
        p = *wave_preset_get(0);
        p.id = "has space";
        check("an id that is not a stored word is refused", !wave_preset_valid(&p));
        p.id = "a_later_preset_1";
        check("an id as long as the store takes is refused", !wave_preset_valid(&p));
        p.id = "later_1";
        check("a later preset inside the bounds is accepted", wave_preset_valid(&p));
    }
}

static struct wave_history h;
static char text[WAVE_HISTORY_TEXT_MAX];

static void test_ring(void)
{
    const struct wave_history_entry *e;
    int i;

    wave_history_init(&h);
    check("a new history is empty", wave_history_count(&h) == 0 && !wave_history_at(&h, 0));
    wave_history_add_tx(&h, "standard", "HELLO", 5, WAVE_RESULT_OK, 1, 1000);
    e = wave_history_at(&h, 0);
    check("a send is kept with its metadata",
          e && e->dir == WAVE_DIR_TX && e->result == WAVE_RESULT_OK && e->count == 1 &&
              e->when == 1000 && strcmp(e->preset, "standard") == 0 && e->len == 5 &&
              strcmp(e->data, "HELLO") == 0);
    check("a reception is a new entry", wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1001,
                                                            5000, 10000) == 0);
    check("newest first", strcmp(wave_history_at(&h, 0)->data, "DOORS") == 0 &&
                              strcmp(wave_history_at(&h, 1)->data, "HELLO") == 0);
    check("the same bytes again within the window fold into it",
          wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1003, 8000, 10000) == 1 &&
              wave_history_count(&h) == 2 && wave_history_at(&h, 0)->count == 2);
    check("and again, measured from the last hearing",
          wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1010, 17000, 10000) == 1 &&
              wave_history_at(&h, 0)->count == 3);
    check("but not after the window",
          wave_history_add_rx(&h, "standard", "DOORS", 5, 0, 1030, 40000, 10000) == 0 &&
              wave_history_count(&h) == 3);
    check("different bytes never fold",
          wave_history_add_rx(&h, "standard", "DOORS!", 6, 0, 1031, 40100, 10000) == 0);
    check("a clock going backwards does not fold",
          wave_history_add_rx(&h, "standard", "DOORS!", 6, 0, 1031, 100, 10000) == 0);
    wave_history_add_tx(&h, "standard", "DOORS!", 6, WAVE_RESULT_OK, 1, 1032);
    check("a send between two hearings stops the fold",
          wave_history_add_rx(&h, "standard", "DOORS!", 6, 0, 1033, 200, 10000) == 0);
    wave_history_add_undecoded(&h, "robust", 1040);
    e = wave_history_at(&h, 0);
    check("an empty capture is an undecoded RX entry with no bytes",
          e->dir == WAVE_DIR_RX && e->result == WAVE_RESULT_UNDECODED && e->len == 0 && e->captured);
    check("a window of 0 never folds", ({
              wave_history_add_rx(&h, "quick", "X", 1, 0, 0, 500, 0);
              wave_history_add_rx(&h, "quick", "X", 1, 0, 0, 501, 0) == 0;
          }));
    check("an unknown clock is kept as 0", wave_history_at(&h, 0)->when == 0);

    {
        char big[WAVE_HISTORY_DATA_MAX + 20];

        memset(big, 'a', sizeof(big));
        wave_history_add_rx(&h, "quick", big, sizeof(big), 0, 5, 900, 0);
        e = wave_history_at(&h, 0);
        check("more bytes than are kept are cut and marked",
              e->len == WAVE_HISTORY_DATA_MAX && e->truncated);
    }

    wave_history_init(&h);
    for (i = 0; i < WAVE_HISTORY_MAX + 7; i++) {
        char msg[16];

        snprintf(msg, sizeof(msg), "m%d", i);
        wave_history_add_tx(&h, "standard", msg, strlen(msg), WAVE_RESULT_OK, 1, i);
    }
    check("the ring holds at most WAVE_HISTORY_MAX", wave_history_count(&h) == WAVE_HISTORY_MAX);
    check("the newest is kept", strcmp(wave_history_at(&h, 0)->data, "m46") == 0);
    check("the oldest went first",
          strcmp(wave_history_at(&h, WAVE_HISTORY_MAX - 1)->data, "m7") == 0);
    check("sequence numbers keep growing", wave_history_at(&h, 0)->seq == WAVE_HISTORY_MAX + 7);
    {
        uint32_t next = h.next_seq;
        unsigned changes = h.changes;

        wave_history_clear(&h);
        check("clear empties it", wave_history_count(&h) == 0);
        check("clear keeps counting sequence numbers and says it changed",
              h.next_seq == next && h.changes != changes);
    }
}

static void test_text(void)
{
    struct wave_history back;
    long n;
    int skipped = -1;
    int i;
    int same = 1;

    wave_history_init(&h);
    wave_history_add_tx(&h, "standard", "HELLO", 5, WAVE_RESULT_OK, 1, 1758900000);
    wave_history_add_tx(&h, "robust", "two", 3, WAVE_RESULT_OK, 2, 1758900010);
    wave_history_add_tx(&h, "robust", "cut", 3, WAVE_RESULT_STOPPED, 1, 0);
    wave_history_add_tx(&h, "quick", "no", 2, WAVE_RESULT_FAILED, 0, 5);
    wave_history_add_rx(&h, "standard", "\x00\xff\x41", 3, 0, 7, 1, 10000);
    wave_history_add_rx(&h, "standard", "\x00\xff\x41", 3, 0, 7, 2, 10000);
    wave_history_add_rx(&h, "quick", "caf\xc3\xa9 ok", 8, 1, 8, 50000, 10000);
    wave_history_add_undecoded(&h, "robust", 9);

    n = wave_history_format(&h, text, sizeof(text));
    check("the text form is written", n > 0 && strncmp(text, WAVE_HISTORY_MAGIC "\n", 15) == 0);
    check("one line per entry after the magic", ({
              int lines = 0;
              for (i = 0; i < n; i++) {
                  lines += text[i] == '\n';
              }
              lines == 1 + 7;
          }));
    check("it reads back", wave_history_parse(&back, text, (size_t)n, &skipped) == 0 && skipped == 0);
    check("with every entry", wave_history_count(&back) == wave_history_count(&h));
    for (i = 0; i < wave_history_count(&h); i++) {
        const struct wave_history_entry *a = wave_history_at(&h, i);
        const struct wave_history_entry *b = wave_history_at(&back, i);

        same &= a->seq == b->seq && a->when == b->when && a->dir == b->dir &&
                a->result == b->result && a->count == b->count && a->captured == b->captured &&
                a->truncated == b->truncated && strcmp(a->preset, b->preset) == 0 &&
                a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
    }
    check("identical, field by field, binary bytes included", same);
    check("new entries continue the sequence", back.next_seq == h.next_seq);
    check("nothing folds into an entry read from disk",
          wave_history_add_rx(&back, "robust", "x", 1, 0, 0, 0, 60000) == 0);

    {
        char damaged[4096];
        int len = snprintf(damaged, sizeof(damaged),
                           "%s\n"
                           "1 100 T ok 1 - standard 48454c4c4f\n"
                           "2 100 X ok 1 - standard 41\n"          /* bad direction */
                           "3 100 R ok 1 - standard 4\n"           /* odd hex */
                           "4 100 R ok 1 - standard zz\n"          /* not hex */
                           "5 100 R sent 1 - standard 41\n"        /* bad result */
                           "6 100 T undecoded 1 - standard 41\n"   /* RX result on TX */
                           "7 100 R undecoded 1 c standard 41\n"   /* bytes on undecoded */
                           "8 100 R ok 1 q standard 41\n"          /* bad flag */
                           "9 100 R ok 1 - standard 41 extra\n"    /* extra field */
                           "10 -5 R ok 1 - standard 41\n"          /* negative time */
                           "0 100 R ok 1 - standard 41\n"          /* seq 0 */
                           "11 100 R ok 1 - Bad/Name 41\n"         /* preset not a word */
                           "\n"
                           "12 100 R ok 3 c quick 444f4f5253\n"
                           "13 100 R ok 1 - standard", /* no newline at the end */
                           WAVE_HISTORY_MAGIC);

        check("damaged lines are skipped, the rest kept",
              wave_history_parse(&back, damaged, (size_t)len, &skipped) == 0 &&
                  wave_history_count(&back) == 1 + 1 && skipped == 12);
        check("the good entries survive intact",
              strcmp(wave_history_at(&back, 0)->data, "DOORS") == 0 &&
                  wave_history_at(&back, 0)->count == 3 && wave_history_at(&back, 0)->captured &&
                  strcmp(wave_history_at(&back, 1)->data, "HELLO") == 0);
        check("the sequence continues after the highest", back.next_seq == 13);
    }
    check("a file without the magic is refused and leaves an empty history",
          wave_history_parse(&back, "notes 1\n1 1 T ok 1 - s 41\n", 25, NULL) == -1 &&
              wave_history_count(&back) == 0);
    check("a later format is refused", wave_history_parse(&back, "wave-history 2\n", 15, NULL) == -1);
    check("an empty history is just the magic", ({
              struct wave_history e;
              wave_history_init(&e);
              wave_history_format(&e, text, sizeof(text)) == 15;
          }));
    check("a buffer too small is an error, not a cut file",
          wave_history_format(&h, text, 40) == -1);

    {
        /* More entries on disk than the ring holds: the newest are kept. */
        char *many = malloc(64 * 1024);
        size_t off = (size_t)snprintf(many, 64 * 1024, "%s\n", WAVE_HISTORY_MAGIC);

        for (i = 1; i <= WAVE_HISTORY_MAX + 10; i++) {
            off += (size_t)snprintf(many + off, 64 * 1024 - off, "%d 0 T ok 1 - standard 41\n", i);
        }
        check("a file with too many entries keeps the newest WAVE_HISTORY_MAX",
              wave_history_parse(&back, many, off, NULL) == 0 &&
                  wave_history_count(&back) == WAVE_HISTORY_MAX &&
                  wave_history_at(&back, 0)->seq == WAVE_HISTORY_MAX + 10 &&
                  wave_history_at(&back, WAVE_HISTORY_MAX - 1)->seq == 11);
        free(many);
    }

    {
        /* The largest history there can be fits the declared bound. */
        char full[WAVE_HISTORY_DATA_MAX];

        memset(full, 0xff, sizeof(full));
        wave_history_init(&h);
        for (i = 0; i < WAVE_HISTORY_MAX; i++) {
            wave_history_add_tx(&h, "abcdefghijklmno", full, sizeof(full), WAVE_RESULT_FAILED, 999,
                                (int64_t)9e18);
            h.ring[(h.head + h.count - 1) % WAVE_HISTORY_MAX].seq = 0xFFFFFFFFu;
            h.ring[(h.head + h.count - 1) % WAVE_HISTORY_MAX].captured = 1;
            h.ring[(h.head + h.count - 1) % WAVE_HISTORY_MAX].truncated = 1;
        }
        check("the longest possible history fits WAVE_HISTORY_TEXT_MAX",
              wave_history_format(&h, text, sizeof(text)) > 0);
    }
}

int main(void)
{
    test_presets();
    test_ring();
    test_text();
    printf("wave_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
