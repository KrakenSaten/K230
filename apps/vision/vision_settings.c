/*
 * Vision's settings. See vision_settings.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_settings.h"

#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 128

static const uint32_t distances_cm[VISION_DISTANCES] = { 100, 200, 500, 1000, 1500, 2000, 3000, 5000 };
static const uint32_t tols[VISION_TOLS] = { 48, 96, 160 };
static const char *const tol_words[VISION_TOLS] = { "low", "med", "high" };
static const char *const mode_names[VISION_MODES] = {
    "DETECT", "TRACK", "TRAFFIC", "FACE", "RECOGNIZE", "READ", "COLOR", "EDGE", "LINE TRACE",
};
static const char *const mode_words[VISION_MODES] = {
    "detect", "track", "traffic", "face", "recognize", "read", "color", "edge", "trace",
};
static const enum vision_group mode_groups[VISION_MODES] = {
    VISION_GROUP_GENERAL, VISION_GROUP_GENERAL, VISION_GROUP_ROAD,  VISION_GROUP_PEOPLE, VISION_GROUP_PEOPLE,
    VISION_GROUP_TEXT,    VISION_GROUP_TOOLS,   VISION_GROUP_TOOLS, VISION_GROUP_TOOLS,
};
static const char *const group_names[VISION_GROUPS] = { "GENERAL", "ROAD", "PEOPLE", "TEXT", "TOOLS" };
static const char *const line_words[VISION_LINE_MODES] = { "off", "across", "down" };
static const char *const speed_words[VISION_SPEED_MODES] = { "off", "narrow", "wide" };
/* The ranges' words: the helper's own (vision_range.c), kept here so the shell
 * links none of the pipeline. */
static const char *const range_words[VISION_RANGES] = { "near", "normal", "far" };

void vision_settings_defaults(struct vision_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = VISION_MODE_DETECT;
    s->track.line = VISION_LINE_ACROSS;
    s->track.trails = true;
    s->traffic.range = VISION_RANGE_NORMAL;
    s->traffic.labels = true;
    s->traffic.speeds = true;
    s->traffic.trails = true;
    s->traffic.line = VISION_LINE_ACROSS;
    s->traffic.orient = VISION_LINE_ACROSS;
    s->traffic.speed = VISION_SPEED_OFF;
    s->traffic.distance_idx = VISION_DISTANCE_DEFAULT;
    s->color.tol_idx = VISION_TOL_DEFAULT;
    s->edge.hard = false;
    s->trace.dark = true;
}

int vision_settings_sanitize(struct vision_settings *s)
{
    struct vision_settings d;
    int fixed = 0;

    vision_settings_defaults(&d);
    if ((int)s->mode < 0 || s->mode >= VISION_MODES) {
        s->mode = d.mode;
        fixed++;
    }
    if ((int)s->track.line < 0 || s->track.line >= VISION_LINE_MODES) {
        s->track.line = d.track.line;
        fixed++;
    }
    if ((int)s->traffic.range < 0 || s->traffic.range >= VISION_RANGES) {
        s->traffic.range = d.traffic.range;
        fixed++;
    }
    if ((int)s->traffic.line < 0 || s->traffic.line >= VISION_LINE_MODES) {
        s->traffic.line = d.traffic.line;
        fixed++;
    }
    if (s->traffic.orient != VISION_LINE_ACROSS && s->traffic.orient != VISION_LINE_DOWN) {
        s->traffic.orient = s->traffic.line != VISION_LINE_OFF ? s->traffic.line : d.traffic.orient;
        fixed++;
    }
    if ((int)s->traffic.speed < 0 || s->traffic.speed >= VISION_SPEED_MODES) {
        s->traffic.speed = d.traffic.speed;
        fixed++;
    }
    if (s->traffic.distance_idx < 0 || s->traffic.distance_idx >= VISION_DISTANCES) {
        s->traffic.distance_idx = d.traffic.distance_idx;
        fixed++;
    }
    if (s->color.tol_idx < 0 || s->color.tol_idx >= VISION_TOLS) {
        s->color.tol_idx = d.color.tol_idx;
        fixed++;
    }
    return fixed;
}

enum vision_group vision_mode_group(enum vision_mode mode)
{
    return (int)mode >= 0 && mode < VISION_MODES ? mode_groups[mode] : VISION_GROUP_GENERAL;
}

const char *vision_mode_name(enum vision_mode mode)
{
    return (int)mode >= 0 && mode < VISION_MODES ? mode_names[mode] : "?";
}

const char *vision_mode_word(enum vision_mode mode)
{
    return (int)mode >= 0 && mode < VISION_MODES ? mode_words[mode] : "detect";
}

const char *vision_group_name(enum vision_group g)
{
    return (int)g >= 0 && g < VISION_GROUPS ? group_names[g] : "?";
}

int vision_mode_parse(const char *word)
{
    int i;

    for (i = 0; word && i < VISION_MODES; i++) {
        if (strcmp(word, mode_words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t vision_distance_cm(int idx)
{
    return distances_cm[idx >= 0 && idx < VISION_DISTANCES ? idx : VISION_DISTANCE_DEFAULT];
}

uint32_t vision_tol_value(int idx)
{
    return tols[idx >= 0 && idx < VISION_TOLS ? idx : VISION_TOL_DEFAULT];
}

static void trim(char **s)
{
    char *end;

    while (**s == ' ' || **s == '\t') {
        (*s)++;
    }
    for (end = *s + strlen(*s); end > *s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'); end--) {
        end[-1] = '\0';
    }
}

/* The index of value in words[0..n), or -1. */
static int word_of(const char *value, const char *const *words, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(value, words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int parse_bool(const char *value, bool *out)
{
    if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) {
        *out = value[0] == '1';
        return 0;
    }
    return 1;
}

/* An enum from its word: 0, or 1 leaving *out alone. */
static int parse_word(const char *value, const char *const *words, int n, int *out)
{
    int i = word_of(value, words, n);

    if (i < 0) {
        return 1;
    }
    *out = i;
    return 0;
}

static int parse_line(struct vision_settings *s, char *line)
{
    char *eq = strchr(line, '=');
    char *key = line;
    char *value;
    int v;
    int bad;

    while (*key == ' ' || *key == '\t') {
        key++;
    }
    if (!eq || *key == '#' || *key == '\0') {
        return 0;
    }
    *eq = '\0';
    value = eq + 1;
    trim(&key);
    trim(&value);
    if (strcmp(key, "mode") == 0) {
        v = vision_mode_parse(value);
        if (v < 0) {
            return 1;
        }
        s->mode = (enum vision_mode)v;
        return 0;
    }
    if (strcmp(key, "track.line") == 0) {
        bad = parse_word(value, line_words, VISION_LINE_MODES, &v);
        if (!bad) {
            s->track.line = (enum vision_line_mode)v;
        }
        return bad;
    }
    if (strcmp(key, "track.trails") == 0) {
        return parse_bool(value, &s->track.trails);
    }
    if (strcmp(key, "traffic.range") == 0) {
        v = word_of(value, range_words, VISION_RANGES);
        if (v < 0) {
            return 1;
        }
        s->traffic.range = (enum vision_range)v;
        return 0;
    }
    if (strcmp(key, "traffic.labels") == 0) {
        return parse_bool(value, &s->traffic.labels);
    }
    if (strcmp(key, "traffic.speeds") == 0) {
        return parse_bool(value, &s->traffic.speeds);
    }
    if (strcmp(key, "traffic.trails") == 0) {
        return parse_bool(value, &s->traffic.trails);
    }
    if (strcmp(key, "traffic.line") == 0) {
        bad = parse_word(value, line_words, VISION_LINE_MODES, &v);
        if (!bad) {
            s->traffic.line = (enum vision_line_mode)v;
            if (v != VISION_LINE_OFF) {
                s->traffic.orient = (enum vision_line_mode)v;
            }
        }
        return bad;
    }
    if (strcmp(key, "traffic.orient") == 0) {
        bad = parse_word(value, line_words, VISION_LINE_MODES, &v);
        if (bad || v == VISION_LINE_OFF) {
            return 1;
        }
        s->traffic.orient = (enum vision_line_mode)v;
        return 0;
    }
    if (strcmp(key, "traffic.speed") == 0) {
        bad = parse_word(value, speed_words, VISION_SPEED_MODES, &v);
        if (!bad) {
            s->traffic.speed = (enum vision_speed_mode)v;
        }
        return bad;
    }
    if (strcmp(key, "traffic.distance_cm") == 0) {
        unsigned cm;
        char tail;
        int i;

        if (sscanf(value, "%u%c", &cm, &tail) != 1) {
            return 1;
        }
        for (i = 0; i < VISION_DISTANCES; i++) {
            if (distances_cm[i] == cm) {
                s->traffic.distance_idx = i;
                return 0;
            }
        }
        return 1;
    }
    if (strcmp(key, "color.tol") == 0) {
        bad = parse_word(value, tol_words, VISION_TOLS, &v);
        if (!bad) {
            s->color.tol_idx = v;
        }
        return bad;
    }
    if (strcmp(key, "edge.hard") == 0) {
        return parse_bool(value, &s->edge.hard);
    }
    if (strcmp(key, "trace.dark") == 0) {
        return parse_bool(value, &s->trace.dark);
    }
    return 0;
}

int vision_settings_parse(struct vision_settings *s, const char *text)
{
    int bad = 0;

    while (s && text && *text) {
        char line[LINE_MAX_LEN];
        size_t n = strcspn(text, "\n");

        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = '\0';
            bad += parse_line(s, line);
        } else {
            /* Longer than any line this build writes: not ours. */
            bad++;
        }
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
    if (s) {
        bad += vision_settings_sanitize(s);
    }
    return bad;
}

int vision_settings_format(const struct vision_settings *s, char *out, size_t out_len)
{
    struct vision_settings c = *s;
    int n;

    if (!out) {
        return -1;
    }
    vision_settings_sanitize(&c);
    n = snprintf(out, out_len,
                 "# Vision settings. Written by Vision; see docs/apps/VISION.md.\n"
                 "mode=%s\n"
                 "track.line=%s\n"
                 "track.trails=%d\n"
                 "traffic.range=%s\n"
                 "traffic.labels=%d\n"
                 "traffic.speeds=%d\n"
                 "traffic.trails=%d\n"
                 "traffic.line=%s\n"
                 "traffic.orient=%s\n"
                 "traffic.speed=%s\n"
                 "traffic.distance_cm=%u\n"
                 "color.tol=%s\n"
                 "edge.hard=%d\n"
                 "trace.dark=%d\n",
                 vision_mode_word(c.mode), line_words[c.track.line], c.track.trails ? 1 : 0,
                 range_words[c.traffic.range], c.traffic.labels ? 1 : 0, c.traffic.speeds ? 1 : 0,
                 c.traffic.trails ? 1 : 0, line_words[c.traffic.line], line_words[c.traffic.orient],
                 speed_words[c.traffic.speed], vision_distance_cm(c.traffic.distance_idx), tol_words[c.color.tol_idx],
                 c.edge.hard ? 1 : 0, c.trace.dark ? 1 : 0);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return n;
}
