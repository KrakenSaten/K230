/*
 * DeskBuddy's preferences. See db_prefs.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_prefs.h"

#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 128

static const char *const keys[DB_PREF_TOGGLE_COUNT] = { "companion", "guard", "night", "owner_recognition",
                                                        "greeting", "idle_animation" };
static const char *const labels[DB_PREF_TOGGLE_COUNT] = { "Companion mode", "Desk guard", "Night mode",
                                                          "Owner recognition", "Greeting", "Idle animation" };
static const char *const modes[DB_MODE_COUNT] = { "companion", "guard", "night" };

#define KEY_ARMED "guard_armed"
#define KEY_MODE "mode"

void db_prefs_defaults(struct db_prefs *p)
{
    int k;

    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        p->on[k] = true;
    }
    p->guard_armed = false;
    p->mode = DB_MODE_COMPANION;
}

const char *db_pref_key(enum db_pref pref)
{
    return ((int)pref >= 0 && pref < DB_PREF_TOGGLE_COUNT) ? keys[pref] : "?";
}

const char *db_pref_label(enum db_pref pref)
{
    return ((int)pref >= 0 && pref < DB_PREF_TOGGLE_COUNT) ? labels[pref] : "?";
}

const char *db_mode_name(enum db_mode mode)
{
    return ((int)mode >= 0 && mode < DB_MODE_COUNT) ? modes[mode] : "?";
}

bool db_prefs_mode_allowed(const struct db_prefs *p, enum db_mode mode)
{
    if (!p) {
        return false;
    }
    switch (mode) {
    case DB_MODE_COMPANION:
        return p->on[DB_PREF_COMPANION];
    case DB_MODE_GUARD:
        return p->on[DB_PREF_GUARD];
    case DB_MODE_NIGHT:
        return p->on[DB_PREF_NIGHT];
    default:
        return false;
    }
}

enum db_mode db_prefs_resolve_mode(const struct db_prefs *p, enum db_mode wish)
{
    int m;

    if (db_prefs_mode_allowed(p, wish)) {
        return wish;
    }
    for (m = 0; m < DB_MODE_COUNT; m++) {
        if (db_prefs_mode_allowed(p, (enum db_mode)m)) {
            return (enum db_mode)m;
        }
    }
    return DB_MODE_COMPANION;
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

static int parse_bool(const char *value, bool *out)
{
    if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) {
        *out = value[0] == '1';
        return 0;
    }
    return 1;
}

static int parse_line(struct db_prefs *p, char *line)
{
    char *eq = strchr(line, '=');
    char *key = line;
    char *value;
    int k;

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
    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        if (strcmp(key, keys[k]) == 0) {
            return parse_bool(value, &p->on[k]);
        }
    }
    if (strcmp(key, KEY_ARMED) == 0) {
        return parse_bool(value, &p->guard_armed);
    }
    if (strcmp(key, KEY_MODE) == 0) {
        for (k = 0; k < DB_MODE_COUNT; k++) {
            if (strcmp(value, modes[k]) == 0) {
                p->mode = (enum db_mode)k;
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

int db_prefs_parse(struct db_prefs *p, const char *text)
{
    int bad = 0;

    while (p && text && *text) {
        char line[LINE_MAX_LEN];
        size_t n = strcspn(text, "\n");

        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = '\0';
            bad += parse_line(p, line);
        }
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
    return bad;
}

int db_prefs_format(const struct db_prefs *p, char *out, size_t out_len)
{
    size_t used;
    int n;
    int k;

    if (!p || !out) {
        return -1;
    }
    n = snprintf(out, out_len, "# DeskBuddy preferences. Written by DeskBuddy; see docs/apps/DESKBUDDY.md.\n");
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    used = (size_t)n;
    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        n = snprintf(out + used, out_len - used, "%s=%d\n", keys[k], p->on[k] ? 1 : 0);
        if (n < 0 || (size_t)n >= out_len - used) {
            return -1;
        }
        used += (size_t)n;
    }
    n = snprintf(out + used, out_len - used, "%s=%d\n%s=%s\n", KEY_ARMED, p->guard_armed ? 1 : 0, KEY_MODE,
                 db_mode_name(p->mode));
    if (n < 0 || (size_t)n >= out_len - used) {
        return -1;
    }
    return (int)(used + (size_t)n);
}
