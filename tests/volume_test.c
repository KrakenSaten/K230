/*
 * System volume test (ui/shell/volume.c): the stored values and their rules,
 * the slider rounding, mute, whether there is a sound card to play to, and
 * the round trip through the real settings store the shell persists them in.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings.h"
#include "volume.h"

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
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    struct volume_state v;
    char dir[] = "/tmp/pos_volume.XXXXXX";
    char path[256];
    int x = -7;
    int bad;

    /* ---- stored values ---- */
    check("50 parses", volume_parse_percent("50", &x) == 0 && x == 50);
    check("10 and 100 are the ends",
          volume_parse_percent("10", &x) == 0 && x == 10 && volume_parse_percent("100", &x) == 0);
    check("0 is not a volume (mute is its own key)", volume_parse_percent("0", &x) < 0);
    check("110 is refused", volume_parse_percent("110", &x) < 0);
    check("a value off the step is refused", volume_parse_percent("55", &x) < 0);
    check("a sign is refused", volume_parse_percent("+50", &x) < 0 && volume_parse_percent("-50", &x) < 0);
    check("trailing text is refused", volume_parse_percent("50%", &x) < 0);
    check("empty and NULL are refused", volume_parse_percent("", &x) < 0 && volume_parse_percent(NULL, &x) < 0);
    check("muted 1 and 0", volume_parse_muted("1", &x) == 0 && x == 1 &&
                               volume_parse_muted("0", &x) == 0 && x == 0);
    check("muted yes is refused", volume_parse_muted("yes", &x) < 0);

    /* ---- defaults and bad stores ---- */
    bad = volume_load(&v, NULL, NULL);
    check("nothing stored: 100 %, not muted", bad == 0 && v.percent == 100 && !v.muted);
    check("nothing stored plays at the validated level", volume_effective(&v) == 100);
    bad = volume_load(&v, "40", "1");
    check("stored 40, muted", bad == 0 && v.percent == 40 && v.muted);
    check("muted plays nothing", volume_effective(&v) == 0);
    bad = volume_load(&v, "loud", "maybe");
    check("unusable values are reported, both", bad == 3);
    check("and fall back to the defaults", v.percent == 100 && !v.muted);
    bad = volume_load(&v, "70", "2");
    check("one bad key does not spoil the other", bad == 2 && v.percent == 70 && !v.muted);

    /* ---- the slider ---- */
    check("a slider position is rounded to the step", volume_round(44) == 40 && volume_round(45) == 50);
    check("below the floor is the floor", volume_round(3) == 10 && volume_round(-20) == 10);
    check("above the top is the top", volume_round(140) == 100);

    /* ---- is there anything to play to ---- */
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    check("no /proc/asound: unavailable", volume_output_present(dir) == 0);
    snprintf(path, sizeof(path), "%s/asound", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/asound/cards", dir);
    put(path, "--- no soundcards ---\n");
    check("no soundcards: unavailable", volume_output_present(dir) == 0);
    put(path, " 0 [K230I2SINNO    ]: K230_I2S_INNO - K230_I2S_INNO\n"
              "                      K230_I2S_INNO\n");
    check("the K230 card: available", volume_output_present(dir) == 1);

    /* ---- persistence: the round trip the shell makes ---- */
    setenv("POCKETOS_CONFIG_DIR", dir, 1);
    settings_init();
    check("stored through the settings store",
          settings_set(VOLUME_SETTING, "30") == 0 && settings_set(VOLUME_MUTED_SETTING, "1") == 0);
    settings_init(); /* a restart reads the file again */
    bad = volume_load(&v, settings_get(VOLUME_SETTING, NULL), settings_get(VOLUME_MUTED_SETTING, NULL));
    check("and read back after a restart", bad == 0 && v.percent == 30 && v.muted);
    settings_set(VOLUME_MUTED_SETTING, "0");
    settings_init();
    bad = volume_load(&v, settings_get(VOLUME_SETTING, NULL), settings_get(VOLUME_MUTED_SETTING, NULL));
    check("unmuting keeps the level", bad == 0 && v.percent == 30 && !v.muted && volume_effective(&v) == 30);

    {
        char cmd[300];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "could not remove %s\n", dir);
        }
    }
    printf("volume_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
