/*
 * Vision's settings: the owner's choices, per mode, kept between opens.
 *
 * One struct with a section per mode that has choices, so a new mode adds
 * its own fields and nothing else moves. The Traffic section is Traffic's
 * alone: its line, its speed lines and its distance are not TRACK's line,
 * and changing one never changes the other.
 *
 * Kept as DeskBuddy and RIFT keep theirs: an app-owned file in
 * settings.conf's key=value format, $POCKETOS_STATE_DIR/vision/settings.v1
 * (vision_store.c does the file):
 *
 *   mode=detect|track|traffic|face|recognize|read|color|edge|trace
 *   track.line=off|across|down
 *   track.trails=0|1
 *   traffic.range=near|normal|far      the detection range (vision_range.h)
 *   traffic.labels=0|1                 id and class on the boxes
 *   traffic.speeds=0|1                 a measured speed on its box
 *   traffic.trails=0|1                 where each vehicle has been
 *   traffic.line=off|across|down
 *   traffic.orient=across|down         the way the speed lines lie (the last
 *                                      line that was not off)
 *   traffic.speed=off|narrow|wide
 *   traffic.distance_cm=<one of the distances offered>
 *   color.tol=low|med|high
 *   edge.hard=0|1
 *   trace.dark=0|1
 *
 * A value this build could not have written leaves the default standing and
 * is counted; an unknown key is ignored and not carried over.
 *
 * Pure C, no I/O (tests/vision_settings_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_SETTINGS_H
#define VISION_SETTINGS_H

#include "pocketvision/vision_range.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The modes, in the order the picker lists them within their groups. */
enum vision_mode {
    VISION_MODE_DETECT = 0,  /* GENERAL: every object the detector knows, boxed */
    VISION_MODE_TRACK,       /* GENERAL: the same with ids, trails and a counting line */
    VISION_MODE_TRAFFIC,     /* ROAD */
    VISION_MODE_FACE,        /* PEOPLE: faces found */
    VISION_MODE_RECOGNIZE,   /* PEOPLE: the owner's face told from others */
    VISION_MODE_READ,        /* TEXT: text found and read */
    VISION_MODE_COLOR,       /* TOOLS */
    VISION_MODE_EDGE,        /* TOOLS */
    VISION_MODE_TRACE,       /* TOOLS: LINE TRACE */
    VISION_MODES
};

enum vision_group {
    VISION_GROUP_GENERAL = 0,
    VISION_GROUP_ROAD,
    VISION_GROUP_PEOPLE,
    VISION_GROUP_TEXT,
    VISION_GROUP_TOOLS,
    VISION_GROUPS
};

/* The counting line's place. */
enum vision_line_mode {
    VISION_LINE_OFF = 0,
    VISION_LINE_ACROSS,   /* horizontal, mid-height: counts DOWN and UP */
    VISION_LINE_DOWN,     /* vertical, mid-width: counts LEFT and RIGHT */
    VISION_LINE_MODES
};

/* The speed lines: none, or a pair either side of the middle, parallel to
 * the counting line, closer or further apart. */
enum vision_speed_mode {
    VISION_SPEED_OFF = 0,
    VISION_SPEED_NARROW,  /* at 40 % and 60 % */
    VISION_SPEED_WIDE,    /* at 25 % and 75 % */
    VISION_SPEED_MODES
};

/* The ground distances offered, in centimetres. */
/* The detector files Vision knows by name (docs/apps/VISION.md, "The
 * model"): DOORS' own R0 training, which the image carries from 0.3.6 and a
 * fresh installation selects, and the upstream YOLOX-Tiny it was trained to
 * replace, which a bench unit may be given by hand for an A/B comparison.
 * Both are YOLOX-Tiny 416 with the same input and output. */
enum vision_detector {
    VISION_DET_R0 = 0,
    VISION_DET_UPSTREAM,
    VISION_DETECTORS
};

#define VISION_DISTANCES 8
#define VISION_DISTANCE_DEFAULT 3  /* 10 m */
/* The colour tolerances: LOW, MED, HIGH. */
#define VISION_TOLS 3
#define VISION_TOL_DEFAULT 1

struct vision_settings {
    enum vision_mode mode;           /* the mode last shown */
    enum vision_detector detector;   /* the A/B detector last confirmed loaded */
    struct {
        enum vision_line_mode line;
        bool trails;
    } track;
    struct {
        enum vision_range range;
        enum vision_line_mode line;
        enum vision_line_mode orient; /* the last line that was not OFF: the speed lines' way */
        enum vision_speed_mode speed;
        int distance_idx;
        bool labels;                  /* id and class on the boxes */
        bool speeds;                  /* a measured speed on its box */
        bool trails;
    } traffic;
    struct {
        int tol_idx;
    } color;
    struct {
        bool hard;
    } edge;
    struct {
        bool dark;
    } trace;
};

#define VISION_SETTINGS_TEXT_MAX 512

void vision_settings_defaults(struct vision_settings *s);
/* Every field in range; out of range ones back to their defaults. Returns
 * how many were put back. */
int vision_settings_sanitize(struct vision_settings *s);

/* The mode's group, its name on the screen and its word (the protocol's
 * and the file's). */
enum vision_group vision_mode_group(enum vision_mode mode);
const char *vision_mode_name(enum vision_mode mode);
const char *vision_mode_word(enum vision_mode mode);
/* The detector's name on the screen (R0 · Beta, UPSTREAM) and its word in
 * the file. */
const char *vision_detector_name(enum vision_detector d);
const char *vision_detector_word(enum vision_detector d);
/* A group's caption. */
const char *vision_group_name(enum vision_group g);
/* The mode for a word, or -1. */
int vision_mode_parse(const char *word);

uint32_t vision_distance_cm(int idx);
uint32_t vision_tol_value(int idx);

/* Read text over s. Returns the number of known keys whose value was
 * refused (they keep what s had). */
int vision_settings_parse(struct vision_settings *s, const char *text);
/* Returns the length, or -1 when out is too small. */
int vision_settings_format(const struct vision_settings *s, char *out, size_t out_len);

#endif
