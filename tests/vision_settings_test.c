/*
 * Vision's settings: the defaults, every key read back as written, values
 * this build could not have written refused one by one with the rest
 * standing, the modes' words and groups, and the store on a scratch state
 * directory (atomic write, private files, a missing and an unreadable file).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "vision_settings.h"
#include "vision_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static void test_defaults(void)
{
    struct vision_settings s;

    vision_settings_defaults(&s);
    check("the default mode is DETECT", s.mode == VISION_MODE_DETECT);
    check("TRACK and TRAFFIC each start with a line ACROSS", s.track.line == VISION_LINE_ACROSS &&
                                                              s.traffic.line == VISION_LINE_ACROSS);
    check("TRAFFIC starts with no speed lines and 10 m", s.traffic.speed == VISION_SPEED_OFF &&
                                                            vision_distance_cm(s.traffic.distance_idx) == 1000);
    check("the tools start at MED, soft, dark", vision_tol_value(s.color.tol_idx) == 96 && !s.edge.hard && s.trace.dark);
    check("TRAFFIC starts at NORMAL range with labels, speeds and trails shown; TRACK with trails",
          s.traffic.range == VISION_RANGE_NORMAL && s.traffic.labels && s.traffic.speeds && s.traffic.trails &&
              s.track.trails);
    check("defaults need no sanitising", vision_settings_sanitize(&s) == 0);
}

static void test_modes(void)
{
    int i;
    int ok = 1;

    for (i = 0; i < VISION_MODES; i++) {
        ok &= vision_mode_parse(vision_mode_word((enum vision_mode)i)) == i;
    }
    check("every mode's word reads back as the mode", ok);
    check("an unknown word is no mode", vision_mode_parse("look") == -1 && vision_mode_parse("") == -1 &&
                                            vision_mode_parse(NULL) == -1);
    check("DETECT and TRACK are GENERAL", vision_mode_group(VISION_MODE_DETECT) == VISION_GROUP_GENERAL &&
                                              vision_mode_group(VISION_MODE_TRACK) == VISION_GROUP_GENERAL);
    check("TRAFFIC is ROAD", vision_mode_group(VISION_MODE_TRAFFIC) == VISION_GROUP_ROAD);
    check("FACE and RECOGNIZE are PEOPLE", vision_mode_group(VISION_MODE_FACE) == VISION_GROUP_PEOPLE &&
                                               vision_mode_group(VISION_MODE_RECOGNIZE) == VISION_GROUP_PEOPLE);
    check("READ is TEXT", vision_mode_group(VISION_MODE_READ) == VISION_GROUP_TEXT);
    check("COLOR, EDGE and LINE TRACE are TOOLS",
          vision_mode_group(VISION_MODE_COLOR) == VISION_GROUP_TOOLS && vision_mode_group(VISION_MODE_EDGE) == VISION_GROUP_TOOLS &&
              vision_mode_group(VISION_MODE_TRACE) == VISION_GROUP_TOOLS && strcmp(vision_mode_name(VISION_MODE_TRACE), "LINE TRACE") == 0);
    check("the group captions", strcmp(vision_group_name(VISION_GROUP_GENERAL), "GENERAL") == 0 &&
                                    strcmp(vision_group_name(VISION_GROUP_TOOLS), "TOOLS") == 0 &&
                                    strcmp(vision_group_name(VISION_GROUPS), "?") == 0);
    check("out-of-range lookups are safe", strcmp(vision_mode_name(VISION_MODES), "?") == 0 &&
                                               strcmp(vision_mode_word((enum vision_mode)-1), "detect") == 0 &&
                                               vision_distance_cm(99) == 1000 && vision_tol_value(-3) == 96);
}

static void test_round_trip(void)
{
    struct vision_settings a;
    struct vision_settings b;
    char text[VISION_SETTINGS_TEXT_MAX];
    int n;

    vision_settings_defaults(&a);
    a.mode = VISION_MODE_TRAFFIC;
    a.track.line = VISION_LINE_OFF;
    a.traffic.line = VISION_LINE_DOWN;
    a.traffic.orient = VISION_LINE_DOWN;
    a.traffic.speed = VISION_SPEED_WIDE;
    a.traffic.distance_idx = 6;
    a.color.tol_idx = 2;
    a.edge.hard = true;
    a.trace.dark = false;
    a.traffic.range = VISION_RANGE_FAR;
    a.traffic.labels = false;
    a.traffic.speeds = false;
    a.traffic.trails = false;
    a.track.trails = false;
    n = vision_settings_format(&a, text, sizeof(text));
    check("the settings format", n > 0 && (size_t)n < sizeof(text) && strstr(text, "mode=traffic\n") &&
                                     strstr(text, "traffic.distance_cm=3000\n") && strstr(text, "track.line=off\n"));
    vision_settings_defaults(&b);
    check("and read back with nothing refused", vision_settings_parse(&b, text) == 0);
    check("every field as it was", memcmp(&a, &b, sizeof(a)) == 0);
    check("too small a buffer is refused", vision_settings_format(&a, text, 20) == -1);

    /* TRACK's line and TRAFFIC's line are two settings. */
    vision_settings_defaults(&b);
    vision_settings_parse(&b, "track.line=down\n");
    check("TRACK's line never moves TRAFFIC's", b.track.line == VISION_LINE_DOWN && b.traffic.line == VISION_LINE_ACROSS);
    vision_settings_parse(&b, "traffic.line=off\n");
    check("TRAFFIC's line off keeps the way the speed lines lie", b.traffic.line == VISION_LINE_OFF &&
                                                                  b.traffic.orient == VISION_LINE_ACROSS);
}

/* The detector choice: kept by its word. A fresh installation, and a file
 * from before the choice existed, has none, which is R0 (0.3.6 on). */
static void test_detector(void)
{
    struct vision_settings s;
    char text[VISION_SETTINGS_TEXT_MAX];

    vision_settings_defaults(&s);
    check("the detector defaults to R0", s.detector == VISION_DET_R0);
    check("R0 is written as its word",
          vision_settings_format(&s, text, sizeof(text)) > 0 && strstr(text, "\ndetector=r0\n") != NULL);
    s.detector = VISION_DET_UPSTREAM;
    check("UPSTREAM is written as its word",
          vision_settings_format(&s, text, sizeof(text)) > 0 && strstr(text, "\ndetector=upstream\n") != NULL);
    vision_settings_defaults(&s);
    check("and read back", vision_settings_parse(&s, text) == 0 && s.detector == VISION_DET_UPSTREAM);
    vision_settings_defaults(&s);
    check("a file without it keeps R0",
          vision_settings_parse(&s, "mode=track\n") == 0 && s.detector == VISION_DET_R0);
    s.detector = VISION_DET_UPSTREAM;
    check("an unknown detector is refused and changes nothing",
          vision_settings_parse(&s, "detector=yolov8n\n") == 1 && s.detector == VISION_DET_UPSTREAM);
    s.detector = (enum vision_detector)7;
    check("an out-of-range one is put back to R0", vision_settings_sanitize(&s) == 1 && s.detector == VISION_DET_R0);
    check("the names and words", strcmp(vision_detector_name(VISION_DET_R0), "R0 · Beta") == 0 &&
                                     strcmp(vision_detector_name(VISION_DET_UPSTREAM), "UPSTREAM") == 0 &&
                                     strcmp(vision_detector_word(VISION_DET_R0), "r0") == 0 &&
                                     strcmp(vision_detector_word(VISION_DET_UPSTREAM), "upstream") == 0 &&
                                     strcmp(vision_detector_word((enum vision_detector)-1), "r0") == 0 &&
                                     strcmp(vision_detector_name((enum vision_detector)-1), "?") == 0);
}

static void test_refusals(void)
{
    struct vision_settings s;
    char longline[300];

    vision_settings_defaults(&s);
    check("an unknown mode is refused, the rest read",
          vision_settings_parse(&s, "mode=look\ntrack.line=down\n") == 1 && s.mode == VISION_MODE_DETECT &&
              s.track.line == VISION_LINE_DOWN);
    vision_settings_defaults(&s);
    check("a distance not on the list is refused", vision_settings_parse(&s, "traffic.distance_cm=1234\n") == 1 &&
                                                       s.traffic.distance_idx == VISION_DISTANCE_DEFAULT);
    check("a distance with junk after it too", vision_settings_parse(&s, "traffic.distance_cm=500m\n") == 1 &&
                                                   s.traffic.distance_idx == VISION_DISTANCE_DEFAULT);
    check("a negative one too", vision_settings_parse(&s, "traffic.distance_cm=-500\n") == 1);
    check("a bool that is not 0 or 1", vision_settings_parse(&s, "edge.hard=yes\ntrace.dark=2\n") == 2 &&
                                           !s.edge.hard && s.trace.dark);
    check("an orientation of off is no orientation", vision_settings_parse(&s, "traffic.orient=off\n") == 1 &&
                                                         s.traffic.orient == VISION_LINE_ACROSS);
    check("a tolerance word it does not know", vision_settings_parse(&s, "color.tol=max\n") == 1 &&
                                                   s.color.tol_idx == VISION_TOL_DEFAULT);
    check("a range it does not know, and one in capitals", vision_settings_parse(&s, "traffic.range=medium\ntraffic.range=FAR\n") == 2 &&
                                                               s.traffic.range == VISION_RANGE_NORMAL);
    check("the range words read", vision_settings_parse(&s, "traffic.range=near\n") == 0 && s.traffic.range == VISION_RANGE_NEAR &&
                                      vision_settings_parse(&s, "traffic.range=normal\n") == 0);
    check("a display toggle that is not 0 or 1", vision_settings_parse(&s, "traffic.trails=on\ntrack.trails=2\n") == 2 &&
                                                     s.traffic.trails && s.track.trails);
    check("unknown keys, comments and blank lines are not errors",
          vision_settings_parse(&s, "# hello\n\nfuture.key=1\n  mode = track  \r\n") == 0 && s.mode == VISION_MODE_TRACK);
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    check("a line longer than any this build writes is refused", vision_settings_parse(&s, longline) == 1 &&
                                                                     s.mode == VISION_MODE_TRACK);
    s.mode = (enum vision_mode)42;
    s.traffic.speed = (enum vision_speed_mode)7;
    s.traffic.distance_idx = -1;
    s.color.tol_idx = 9;
    s.track.line = (enum vision_line_mode)-2;
    s.traffic.range = (enum vision_range)3;
    check("fields out of range are put back", vision_settings_sanitize(&s) == 6 && s.mode == VISION_MODE_DETECT &&
                                                  s.traffic.range == VISION_RANGE_NORMAL &&
                                                  s.traffic.speed == VISION_SPEED_OFF &&
                                                  s.traffic.distance_idx == VISION_DISTANCE_DEFAULT &&
                                                  s.color.tol_idx == VISION_TOL_DEFAULT && s.track.line == VISION_LINE_ACROSS);
    s.mode = (enum vision_mode)42;
    {
        char text[VISION_SETTINGS_TEXT_MAX];

        check("and a struct out of range still formats as a valid file",
              vision_settings_format(&s, text, sizeof(text)) > 0 && strstr(text, "mode=detect\n") != NULL);
    }
    check("parsing nothing is harmless", vision_settings_parse(NULL, "mode=track") == 0 &&
                                             vision_settings_parse(&s, NULL) >= 0);
}

static void test_store(void)
{
    char dir[] = "/tmp/vision-settings-XXXXXX";
    char path[256];
    struct vision_settings s;
    struct vision_settings back;
    struct stat st;
    FILE *f;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    check("no file: the defaults, said as absent", vision_store_load(&s) == 1 && s.mode == VISION_MODE_DETECT);
    s.mode = VISION_MODE_EDGE;
    s.edge.hard = true;
    check("saved", vision_store_save(&s) == 0);
    snprintf(path, sizeof(path), "%s/vision/settings.v1", dir);
    check("the file is private", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    snprintf(path, sizeof(path), "%s/vision", dir);
    check("in a private directory", stat(path, &st) == 0 && (st.st_mode & 0777) == 0700);
    snprintf(path, sizeof(path), "%s/vision/settings.v1.tmp", dir);
    check("with no temporary file left behind", stat(path, &st) != 0);
    check("and read back", vision_store_load(&back) == 0 && back.mode == VISION_MODE_EDGE && back.edge.hard);
    snprintf(path, sizeof(path), "%s/vision/settings.v1", dir);
    f = fopen(path, "w");
    if (f) {
        fputs("mode=nonsense\nedge.hard=1\n", f);
        fclose(f);
    }
    check("a damaged value leaves its default and the rest reads",
          vision_store_load(&back) == 0 && back.mode == VISION_MODE_DETECT && back.edge.hard);
    f = fopen(path, "w");
    if (f) {
        int i;

        for (i = 0; i < 200; i++) {
            fputs("# padding padding padding\n", f);
        }
        fclose(f);
    }
    check("a file larger than any this build writes is unreadable, defaults stand",
          vision_store_load(&back) == -1 && back.mode == VISION_MODE_DETECT && !back.edge.hard);
    unlink(path);
    snprintf(path, sizeof(path), "%s/vision", dir);
    rmdir(path);
    rmdir(dir);
}

int main(void)
{
    test_defaults();
    test_modes();
    test_round_trip();
    test_refusals();
    test_detector();
    test_store();
    printf("vision_settings_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
