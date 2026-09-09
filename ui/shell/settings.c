/*
 * Settings store implementation. See settings.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct entry {
    char key[SETTINGS_KEY_MAX];
    char value[SETTINGS_VALUE_MAX];
};

static struct entry entries[SETTINGS_MAX_KEYS];
static int count;
static char path[512];

static int key_valid(const char *key)
{
    size_t i;
    size_t n = key ? strlen(key) : 0;

    if (n == 0 || n >= SETTINGS_KEY_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!isalnum((unsigned char)key[i]) && key[i] != '_' && key[i] != '.') {
            return 0;
        }
    }
    return 1;
}

static struct entry *find(const char *key)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].key, key) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static void trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static void store(const char *key, const char *value)
{
    struct entry *e = find(key);

    if (!e) {
        if (count >= SETTINGS_MAX_KEYS) {
            return;
        }
        e = &entries[count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    snprintf(e->value, sizeof(e->value), "%s", value);
}

int settings_init(void)
{
    const char *dir = pocketos_config_dir();
    FILE *f;
    char line[SETTINGS_KEY_MAX + SETTINGS_VALUE_MAX + 16];

    count = 0;
    snprintf(path, sizeof(path), "%s/%s", dir, SETTINGS_FILE);
    f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        char *value;
        char *key = line;

        while (isspace((unsigned char)*key)) {
            key++;
        }
        if (*key == '#' || *key == '\0') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim(key);
        value = eq + 1;
        while (isspace((unsigned char)*value)) {
            value++;
        }
        trim(value);
        if (!key_valid(key)) {
            continue;
        }
        store(key, value);
    }
    fclose(f);
    return 0;
}

const char *settings_path(void)
{
    return path;
}

const char *settings_get(const char *key, const char *fallback)
{
    struct entry *e = key ? find(key) : NULL;

    return e ? e->value : fallback;
}

int settings_count(void)
{
    return count;
}

static int write_file(void)
{
    char tmp[560];
    FILE *f;
    int i;
    char *slash;

    /* make sure the directory exists (best effort, parents included) */
    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        pocketos_mkdir_p(tmp, 0755);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "# PocketOS settings (key=value). Written by the shell; edit while it is stopped.\n");
    for (i = 0; i < count; i++) {
        fprintf(f, "%s=%s\n", entries[i].key, entries[i].value);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int settings_set(const char *key, const char *value)
{
    /* Transactional: the new table is written to disk first; the in-memory
     * table keeps the new state only when that succeeded, otherwise it is
     * rolled back to what it was before the call. */
    struct entry saved[SETTINGS_MAX_KEYS];
    int saved_count = count;
    int rc;

    if (!key_valid(key)) {
        return -1;
    }
    if (value != NULL && (strlen(value) >= SETTINGS_VALUE_MAX || strchr(value, '\n'))) {
        return -1;
    }
    memcpy(saved, entries, sizeof(saved));
    if (value == NULL) {
        struct entry *e = find(key);

        if (e) {
            *e = entries[--count];
            memset(&entries[count], 0, sizeof(entries[count]));
        }
    } else {
        store(key, value);
        if (!find(key)) {
            rc = -1; /* table full */
            goto rollback;
        }
    }
    rc = write_file();
    if (rc == 0) {
        return 0;
    }
rollback:
    memcpy(entries, saved, sizeof(saved));
    count = saved_count;
    return rc;
}
