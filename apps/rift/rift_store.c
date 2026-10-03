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
        p->ch_sound = RIFT_PREF_CH_SOUND_DEFAULT;
    }
}

/* A conversation key that names a channel: '#', then printable ASCII with
 * no spaces, as rift_channel_key() writes it. */
static int channel_key_ok(const char *k)
{
    size_t i;

    if (!k || k[0] != '#' || strlen(k) >= RIFT_PREF_MUTE_KEY_MAX || !k[1]) {
        return 0;
    }
    for (i = 1; k[i]; i++) {
        if ((unsigned char)k[i] <= 0x20 || (unsigned char)k[i] >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

int rift_prefs_channel_muted(const struct rift_prefs *p, const char *conv_key)
{
    int i;

    for (i = 0; p && conv_key && i < p->mute_count; i++) {
        if (strcmp(p->mute[i], conv_key) == 0) {
            return 1;
        }
    }
    return 0;
}

int rift_prefs_set_channel_muted(struct rift_prefs *p, const char *conv_key, int muted)
{
    int i;

    if (!p || !channel_key_ok(conv_key)) {
        return -1;
    }
    for (i = 0; i < p->mute_count; i++) {
        if (strcmp(p->mute[i], conv_key) == 0) {
            if (!muted) {
                memmove(&p->mute[i], &p->mute[i + 1],
                        sizeof(p->mute[0]) * (size_t)(p->mute_count - i - 1));
                p->mute_count--;
            }
            return 0;
        }
    }
    if (!muted) {
        return 0;
    }
    if (p->mute_count >= RIFT_PREF_MUTE_MAX) {
        return -1;
    }
    snprintf(p->mute[p->mute_count++], RIFT_PREF_MUTE_KEY_MAX, "%s", conv_key);
    return 0;
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
    if (strcmp(key, RIFT_PREF_CH_SOUND) == 0) {
        if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0) {
            p->ch_sound = value[0] == '1';
            return 0;
        }
        return 2;
    }
    if (strcmp(key, RIFT_PREF_CH_MUTE) == 0) {
        /* A channel's key, or not kept; a list already full keeps what it
         * has, and a repeat is one entry. */
        return rift_prefs_set_channel_muted(p, value, 1) == 0 ? 0 : 4;
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
    size_t at;
    int n;
    int i;

    if (!p || !out) {
        return -1;
    }
    n = snprintf(out, out_len,
                 "# RIFT preferences. Written by RIFT; see docs/apps/RIFT.md.\n"
                 "%s=%d\n%s=%d\n",
                 RIFT_PREF_DM_SOUND, p->dm_sound ? 1 : 0, RIFT_PREF_CH_SOUND,
                 p->ch_sound ? 1 : 0);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    at = (size_t)n;
    for (i = 0; i < p->mute_count; i++) {
        n = snprintf(out + at, out_len - at, "%s=%s\n", RIFT_PREF_CH_MUTE, p->mute[i]);
        if (n < 0 || (size_t)n >= out_len - at) {
            return -1;
        }
        at += (size_t)n;
    }
    return (int)at;
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
    char text[RIFT_STORE_FILE_MAX];
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
    char text[RIFT_STORE_FILE_MAX];
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
