/*
 * Recording names (apps/recorder/rec_names.h): what is made with and without
 * a clock, that every name made parses back to the same parts, and that
 * nothing that could leave the folder or pose as a recording is accepted.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rec_names.h"

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
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    struct rec_name p;
    struct rec_name q;
    struct tm tm;
    char name[REC_NAME_MAX];
    static const char *const bad[] = {
        "REC-.wav", "REC-123.wav", "REC-1234567.wav", "REC-20260926-10150.wav",
        "REC-20260926_101500.wav", "rec-0001.wav", "REC-0001.WAV", "REC-0001-1.wav",
        "REC-0001-1000.wav", "REC-0001-02.wav", "REC-0001-recovered-2.wav", "REC-0001.wav.part",
        "../REC-0001.wav", "REC-0001/x.wav", "REC-0001 .wav", "REC-0001-x.wav", "",
    };
    size_t i;
    int all_bad = 1;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 8;
    tm.tm_mday = 26;
    tm.tm_hour = 9;
    tm.tm_min = 5;
    tm.tm_sec = 7;
    rec_name_stamp(&p, &tm, 0);
    check("with a clock: REC-YYYYMMDD-HHMMSS.wav",
          rec_name_build(name, sizeof(name), &p) == 0 && strcmp(name, "REC-20260926-090507.wav") == 0);
    check("which parses back to the same parts",
          rec_name_parse(name, &q) && strcmp(q.stamp, p.stamp) == 0 && q.dup == 0 && !q.recovered);
    check("and has no sequence number", rec_name_seq(name) == 0);

    rec_name_stamp(&p, NULL, 7);
    check("without a clock: REC-0007.wav",
          rec_name_build(name, sizeof(name), &p) == 0 && strcmp(name, "REC-0007.wav") == 0 &&
              rec_name_seq(name) == 7);
    rec_name_stamp(&p, NULL, 123456);
    check("sequence numbers grow past four digits",
          rec_name_build(name, sizeof(name), &p) == 0 && strcmp(name, "REC-123456.wav") == 0 &&
              rec_name_seq(name) == 123456);
    rec_name_stamp(&p, NULL, 9999999);
    check("and stop at REC_SEQ_MAX", strcmp(p.stamp, "999999") == 0);
    rec_name_stamp(&p, NULL, 0);
    check("sequence 0 becomes 1", strcmp(p.stamp, "0001") == 0);

    rec_name_stamp(&p, &tm, 0);
    p.dup = 2;
    p.recovered = true;
    check("a second one in the same second, repaired: -2-recovered",
          rec_name_build(name, sizeof(name), &p) == 0 &&
              strcmp(name, "REC-20260926-090507-2-recovered.wav") == 0 && rec_name_parse(name, &q) &&
              q.dup == 2 && q.recovered);
    p.dup = 1;
    check("dup 1 is refused (the first one has none)", rec_name_build(name, sizeof(name), &p) != 0);
    p.dup = 1000;
    check("dup past 999 is refused", rec_name_build(name, sizeof(name), &p) != 0);
    p.dup = 0;
    check("a buffer that is too small is refused", rec_name_build(name, 10, &p) != 0);

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (rec_name_parse(bad[i], NULL)) {
            printf("     accepted: '%s'\n", bad[i]);
            all_bad = 0;
        }
    }
    check("nothing that is not a recording name parses as one", all_bad);

    check("a recording's .part is a part", rec_name_is_part("REC-0001.wav.part") &&
                                               rec_name_is_part("REC-20260926-090507-3.wav.part"));
    check("anything else ending in .part is not", !rec_name_is_part("song.wav.part") &&
                                                   !rec_name_is_part(".part") && !rec_name_is_part("REC-0001.part"));
    check("the list shows any visible .wav, whatever its case",
          rec_name_listable("REC-0001.wav") && rec_name_listable("Interview.WAV") &&
              rec_name_listable("a b.wav") && rec_name_listable("REC-0001.wav.part"));
    check("and not hidden files, other types, control characters or slashes",
          !rec_name_listable(".REC-0001.wav") && !rec_name_listable("notes.txt") &&
              !rec_name_listable("a\nb.wav") && !rec_name_listable("a/b.wav") &&
              !rec_name_listable(".new-REC-0001.wav.part") && !rec_name_listable(".wav") &&
              !rec_name_listable(""));

    printf("rec_names_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
