/*
 * Settings store test: absent file, round trip, malformed lines, removal,
 * atomic write, limits.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static int file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int found = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            found = 1;
        }
    }
    fclose(f);
    return found;
}

int main(void)
{
    char dir[] = "/tmp/pos_settings.XXXXXX";
    char file[600];
    char tmp[600];
    char cmd[700];
    FILE *f;
    struct stat st;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETOS_CONFIG_DIR", dir, 1);
    snprintf(file, sizeof(file), "%s/settings.conf", dir);
    snprintf(tmp, sizeof(tmp), "%s/settings.conf.tmp", dir);

    check("absent file reports 1", settings_init() == 1);
    check("path under config dir", strcmp(settings_path(), file) == 0);
    check("get returns fallback", strcmp(settings_get("theme", "ice"), "ice") == 0);
    check("get NULL fallback", settings_get("theme", NULL) == NULL);

    check("set writes", settings_set("theme", "brass") == 0);
    check("set second key", settings_set("display_mode", "outdoor") == 0);
    check("file has theme", file_has(file, "theme=brass"));
    check("file has mode", file_has(file, "display_mode=outdoor"));
    check("no temp file left", stat(tmp, &st) != 0);

    check("reload reads file", settings_init() == 0);
    check("value round trip", strcmp(settings_get("theme", "?"), "brass") == 0);
    check("count after reload", settings_count() == 2);

    check("overwrite", settings_set("theme", "olive") == 0 && strcmp(settings_get("theme", "?"), "olive") == 0);
    check("remove key", settings_set("display_mode", NULL) == 0 && settings_get("display_mode", NULL) == NULL);
    check("removed key not in file", !file_has(file, "display_mode="));

    /* malformed content is ignored, valid lines survive */
    f = fopen(file, "w");
    fprintf(f, "# comment\n\n  theme = slate  \nno_equals_here\n=novalue\nbad key=1\ndisplay_mode=night\r\n");
    fclose(f);
    check("reload malformed file", settings_init() == 0);
    check("trimmed key/value", strcmp(settings_get("theme", "?"), "slate") == 0);
    check("CRLF value trimmed", strcmp(settings_get("display_mode", "?"), "night") == 0);
    check("malformed lines dropped", settings_count() == 2);

    check("invalid key rejected", settings_set("bad key", "x") < 0);
    check("newline in value rejected", settings_set("theme", "a\nb") < 0);
    {
        char big[SETTINGS_VALUE_MAX + 8];

        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        check("oversized value rejected", settings_set("theme", big) < 0);
    }

    /* persistence failure must not change the in-memory table (Finding 4) */
    if (geteuid() != 0) {
        settings_init();
        settings_set("theme", "brass");
        chmod(dir, 0555);
        check("write failure reported", settings_set("theme", "carbon") < 0);
        check("value unchanged after failed write", strcmp(settings_get("theme", "?"), "brass") == 0);
        check("remove rolled back after failed write", settings_set("theme", NULL) < 0 &&
                                                        strcmp(settings_get("theme", "?"), "brass") == 0);
        check("count unchanged after failed write", settings_count() == 2);
        chmod(dir, 0755);
        check("file still has old value", file_has(file, "theme=brass"));
        check("write works again", settings_set("theme", "carbon") == 0 &&
                                   strcmp(settings_get("theme", "?"), "carbon") == 0);
    } else {
        printf("skip write-failure checks (running as root)\n");
    }

    /* unreadable file */
    chmod(file, 0);
    if (geteuid() != 0) {
        check("unreadable file reports -1", settings_init() == -1);
    } else {
        printf("skip unreadable-file check (running as root)\n");
    }
    chmod(file, 0644);

    /* A file that is there but not read whole is never written over: every
     * setting in it would be lost to a table holding none or some of them. */
    settings_init();
    check("seed: two settings on disk", settings_set("theme", "brass") == 0 &&
                                          settings_set("display_mode", "night") == 0);
    if (geteuid() != 0) {
        chmod(file, 0);
        check("unreadable: init reports -1", settings_init() == -1);
        check("unreadable: set refused", settings_set("theme", "carbon") < 0);
        check("unreadable: remove refused", settings_set("display_mode", NULL) < 0);
        check("unreadable: nothing in memory changed", settings_get("theme", NULL) == NULL);
        chmod(file, 0644);
        check("unreadable: file content survived", file_has(file, "theme=brass") &&
                                                     file_has(file, "display_mode=night"));
        check("unreadable: no temp file", stat(tmp, &st) != 0);
    } else {
        printf("skip unreadable-file set checks (running as root)\n");
    }
    /* A read error part-way (EISDIR: opening a directory for reading works,
     * reading it fails) - ferror(), not fopen(), is what sees this one, and
     * it does so as root too. */
    {
        char moved[640];

        snprintf(moved, sizeof(moved), "%s.real", file);
        check("read error: file moved aside", rename(file, moved) == 0 && mkdir(file, 0755) == 0);
        check("read error: init reports -1", settings_init() == -1);
        check("read error: set refused", settings_set("theme", "carbon") < 0);
        check("read error: nothing written", stat(tmp, &st) != 0);
        check("read error: file restored", rmdir(file) == 0 && rename(moved, file) == 0);
        check("read error: content survived", file_has(file, "theme=brass") && file_has(file, "display_mode=night"));
    }
    check("readable again: init 0 and set works", settings_init() == 0 && settings_set("theme", "olive") == 0 &&
                                                   file_has(file, "theme=olive") &&
                                                   file_has(file, "display_mode=night"));
    /* More keys than the table holds: the ones past the limit were not read,
     * so writing would drop them. */
    f = fopen(file, "w");
    {
        int i;

        for (i = 0; i < SETTINGS_MAX_KEYS + 3; i++) {
            fprintf(f, "k%d=%d\n", i, i);
        }
    }
    fclose(f);
    check("too many keys: init reports -1", settings_init() == -1);
    check("too many keys: first keys readable", strcmp(settings_get("k0", "?"), "0") == 0);
    check("too many keys: set refused", settings_set("k0", "x") < 0);
    {
        char last[32];

        snprintf(last, sizeof(last), "k%d=", SETTINGS_MAX_KEYS + 2);
        check("too many keys: the last key survived", file_has(file, last));
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("settings_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
