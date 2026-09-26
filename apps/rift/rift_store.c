/*
 * RIFT's preferences file. See rift_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_store.h"

#include "pocketpaths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char dir_buf[POCKETOS_PATH_MAX];
static char path_buf[POCKETOS_PATH_MAX + 32];

void rift_prefs_defaults(struct rift_prefs *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
        p->dm_sound = RIFT_PREF_DM_SOUND_DEFAULT;
    }
}

/* One "key=value" line, trimmed of the spaces settings.conf also allows. */
static int parse_line(struct rift_prefs *p, char *line)
{
    char *eq = strchr(line, '=');
    char *key = line;
    char *value;
    char *end;

    while (*key == ' ' || *key == '\t') {
        key++;
    }
    if (!eq || *key == '#' || *key == '\0') {
        return 0;
    }
    *eq = '\0';
    value = eq + 1;
    for (end = eq; end > key && (end[-1] == ' ' || end[-1] == '\t'); end--) {
        end[-1] = '\0';
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    for (end = value + strlen(value); end > value && (end[-1] == ' ' || end[-1] == '\t' ||
                                                     end[-1] == '\r');
         end--) {
        end[-1] = '\0';
    }
    if (strcmp(key, RIFT_PREF_DM_SOUND) == 0) {
        /* Exactly what this build writes. Anything else is not a choice the
         * reader made here, and the default stands rather than a guess. */
        if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) {
            p->dm_sound = value[0] == '1';
            return 0;
        }
        return 1;
    }
    return 0;
}

int rift_prefs_parse(struct rift_prefs *p, const char *text)
{
    char line[RIFT_STORE_TEXT_MAX];
    int bad = 0;

    if (!p || !text) {
        return 0;
    }
    while (*text) {
        size_t n = strcspn(text, "\n");
        size_t keep = n < sizeof(line) - 1 ? n : sizeof(line) - 1;

        memcpy(line, text, keep);
        line[keep] = '\0';
        /* A line longer than any this build writes is not one of its lines. */
        if (n == keep) {
            bad |= parse_line(p, line);
        }
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
    return bad;
}

int rift_prefs_format(const struct rift_prefs *p, char *out, size_t out_len)
{
    int n;

    if (!p || !out) {
        return -1;
    }
    n = snprintf(out, out_len,
                 "# RIFT preferences. Written by RIFT; see docs/apps/RIFT.md.\n"
                 "%s=%d\n",
                 RIFT_PREF_DM_SOUND, p->dm_sound ? 1 : 0);
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}

const char *rift_store_dir(void)
{
    snprintf(dir_buf, sizeof(dir_buf), "%s/%s", pocketos_state_dir(), RIFT_STORE_SUBDIR);
    return dir_buf;
}

const char *rift_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", rift_store_dir(), RIFT_STORE_FILE);
    return path_buf;
}

int rift_store_load(struct rift_prefs *p)
{
    char text[RIFT_STORE_TEXT_MAX * 4];
    struct rift_prefs read;
    size_t got;
    FILE *f;

    if (!p) {
        return -1;
    }
    rift_prefs_defaults(p);
    f = fopen(rift_store_path(), "r");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(text, 1, sizeof(text) - 1, f);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    text[got] = '\0';
    read = *p;
    rift_prefs_parse(&read, text);
    *p = read;
    return 0;
}

int rift_store_save(const struct rift_prefs *p)
{
    char text[RIFT_STORE_TEXT_MAX];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;
    int n;

    n = rift_prefs_format(p, text, sizeof(text));
    if (n < 0) {
        return -1;
    }
    if (pocketos_mkdir_p(rift_store_dir(), 0755) != 0) {
        return -1;
    }
    path = rift_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        return -1;
    }
    if (fwrite(text, 1, (size_t)n, f) != (size_t)n || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
