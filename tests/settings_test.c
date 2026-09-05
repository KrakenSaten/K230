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

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("settings_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
