/*
 * Display brightness HAL test (ui/shell/brightness.c) against a fake sysfs
 * tree: absence, discovery, the percent/raw mapping, the floor, a failing
 * write, and the rules for a stored value.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "brightness.h"

#include <errno.h>
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

static int read_file_int(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;

    if (f) {
        if (fscanf(f, "%d", &v) != 1) {
            v = -1;
        }
        fclose(f);
    }
    return v;
}

static void make_device(const char *root, const char *name, const char *max, const char *cur)
{
    char path[600];

    snprintf(path, sizeof(path), "%s/class", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/class/backlight", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/class/backlight/%s", root, name);
    mkdir(path, 0755);
    if (max) {
        snprintf(path, sizeof(path), "%s/class/backlight/%s/max_brightness", root, name);
        put(path, max);
    }
    if (cur) {
        snprintf(path, sizeof(path), "%s/class/backlight/%s/brightness", root, name);
        put(path, cur);
    }
}

static char roots[8][64];
static int root_count;

static void new_root(char *dir)
{
    strcpy(dir, "/tmp/pos_brightness.XXXXXX");
    if (!mkdtemp(dir) || root_count >= 8) {
        perror("mkdtemp");
        exit(1);
    }
    snprintf(roots[root_count++], sizeof(roots[0]), "%s", dir);
}

static void remove_roots(void)
{
    char cmd[128];
    int i;

    for (i = 0; i < root_count; i++) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%.63s'", roots[i]);
        if (system(cmd) != 0) {
            fprintf(stderr, "could not remove %s\n", roots[i]);
        }
    }
}

int main(void)
{
    struct brightness b;
    char root[64];
    char path[600];
    int pct;
    int ok;
    int raw;
    int max;

    /* ---- absence ---------------------------------------------------------- */
    new_root(root);
    check("no class directory: probe reports none", brightness_probe(&b, root) == 1);
    check("no class directory: unsupported", b.supported == 0);
    check("unsupported: get is -1", brightness_get_percent(&b) == -1);
    errno = 0;
    check("unsupported: set is -1", brightness_set_percent(&b, 50) == -1);
    check("unsupported: set says ENODEV", errno == ENODEV);
    check("NULL root is none", brightness_probe(&b, NULL) == 1 && !b.supported);

    snprintf(path, sizeof(path), "%s/class", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/class/backlight", root);
    mkdir(path, 0755);
    check("empty backlight class: unsupported", brightness_probe(&b, root) == 1 && !b.supported);

    /* ---- the K230 shape: rm69a10, 255 levels, 254 at boot ------------------ */
    make_device(root, "rm69a10", "255\n", "254\n");
    check("rm69a10 found", brightness_probe(&b, root) == 0 && b.supported);
    check("device name recorded", strcmp(b.name, "rm69a10") == 0);
    check("max_brightness 255", b.max_raw == 255);
    check("boot level 254 reads as 100 %", brightness_get_percent(&b) == 100);
    snprintf(path, sizeof(path), "%s/class/backlight/rm69a10/brightness", root);
    check("set 50 applies 50", brightness_set_percent(&b, 50) == 50);
    check("50 % is raw 128", read_file_int(path) == 128);
    check("and reads back as 50", brightness_get_percent(&b) == 50);
    check("set 100 applies 100", brightness_set_percent(&b, 100) == 100);
    check("100 % is raw 255", read_file_int(path) == 255);
    check("set 10 applies 10", brightness_set_percent(&b, 10) == 10);
    check("10 % is raw 26, above the vendor floor of 20", read_file_int(path) == 26);

    /* ---- the floor and the ceiling ----------------------------------------- */
    check("set 0 is clamped to the floor", brightness_set_percent(&b, 0) == BRIGHTNESS_MIN_PCT);
    check("so raw 0 is never written", read_file_int(path) == 26);
    check("set -40 is clamped to the floor", brightness_set_percent(&b, -40) == BRIGHTNESS_MIN_PCT);
    check("set 9 is clamped to the floor", brightness_set_percent(&b, 9) == BRIGHTNESS_MIN_PCT);
    check("set 101 is clamped to 100", brightness_set_percent(&b, 101) == 100);
    check("set 5000 is clamped to 100", brightness_set_percent(&b, 5000) == 100);
    check("clamped ceiling is raw 255", read_file_int(path) == 255);
    check("clamp below", brightness_clamp_percent(3) == 10);
    check("clamp above", brightness_clamp_percent(130) == 100);
    check("clamp inside", brightness_clamp_percent(42) == 42);

    /* A level something else wrote below the floor reads as itself. */
    put(path, "0\n");
    check("a raw 0 written elsewhere reads as 0 %", brightness_get_percent(&b) == 0);
    put(path, "20\n");
    check("the vendor floor 20 reads as 8 %", brightness_get_percent(&b) == 8);
    put(path, "garbage\n");
    check("an unreadable level is -1", brightness_get_percent(&b) == -1);
    put(path, "254\n");

    /* ---- the mapping over every step and many panels ----------------------- */
    ok = 1;
    for (max = BRIGHTNESS_MIN_LEVELS; max <= 4095; max++) {
        for (pct = BRIGHTNESS_MIN_PCT; pct <= BRIGHTNESS_MAX_PCT; pct += BRIGHTNESS_STEP_PCT) {
            raw = brightness_percent_to_raw(pct, max);
            if (raw < 1 || raw > max) {
                ok = 0;
            }
            /* On a panel with 100 levels or more every step survives the
             * round trip; below that, steps may merge but never go dark. */
            if (max >= 100 && brightness_raw_to_percent(raw, max) != pct) {
                ok = 0;
            }
        }
        if (brightness_percent_to_raw(100, max) != max) {
            ok = 0;
        }
    }
    check("every step maps inside [1, max] and round-trips on >= 100 levels", ok);
    ok = 1;
    for (pct = BRIGHTNESS_MIN_PCT; pct < BRIGHTNESS_MAX_PCT; pct++) {
        if (brightness_percent_to_raw(pct + 1, 255) < brightness_percent_to_raw(pct, 255)) {
            ok = 0;
        }
    }
    check("the mapping is monotonic", ok);
    check("raw above max reads as 100", brightness_raw_to_percent(300, 255) == 100);
    check("negative raw is -1", brightness_raw_to_percent(-1, 255) == -1);
    check("max 0 is -1", brightness_raw_to_percent(10, 0) == -1);

    /* ---- the HAL failing ---------------------------------------------------- */
    /* A directory where the attribute should be: open for writing fails with
     * EISDIR even for root, so this does not depend on who runs the test. */
    new_root(root);
    make_device(root, "broken", "255\n", NULL);
    snprintf(path, sizeof(path), "%s/class/backlight/broken/brightness", root);
    mkdir(path, 0755);
    check("a device whose attribute cannot be written is still found",
          brightness_probe(&b, root) == 0 && b.supported);
    errno = 0;
    check("the failing write reports -1", brightness_set_percent(&b, 60) == -1);
    check("with the kernel's errno", errno == EISDIR);
    check("and the level is unreadable", brightness_get_percent(&b) == -1);

    /* ---- what does not count as a brightness control ----------------------- */
    new_root(root);
    make_device(root, "onoff", "1\n", "1\n");
    check("a two-level device is not a brightness control", brightness_probe(&b, root) == 1);
    make_device(root, "nomax", NULL, "5\n");
    check("a device without max_brightness is skipped", brightness_probe(&b, root) == 1);
    make_device(root, "junkmax", "25five\n", "5\n");
    check("a device with a malformed max is skipped", brightness_probe(&b, root) == 1);
    make_device(root, "nolevel", "255\n", NULL);
    check("a device without brightness is skipped", brightness_probe(&b, root) == 1);
    make_device(root, "zpanel", "100\n", "40\n");
    make_device(root, "apanel", "255\n", "128\n");
    check("with several, the first usable one in name order wins",
          brightness_probe(&b, root) == 0 && strcmp(b.name, "apanel") == 0);
    check("and reads its own level", brightness_get_percent(&b) == 50);

    /* ---- stored values --------------------------------------------------------*/
    pct = -7;
    check("setting 60 is valid", brightness_parse_setting("60", &pct) == 0 && pct == 60);
    check("setting 10 is valid", brightness_parse_setting("10", &pct) == 0 && pct == 10);
    check("setting 100 is valid", brightness_parse_setting("100", &pct) == 0 && pct == 100);
    check("setting 55 is valid (not only steps)", brightness_parse_setting("55", &pct) == 0 && pct == 55);
    pct = -7;
    check("setting 9 is invalid, not clamped", brightness_parse_setting("9", &pct) == -1 && pct == -7);
    check("setting 0 is invalid", brightness_parse_setting("0", &pct) == -1);
    check("setting 101 is invalid", brightness_parse_setting("101", &pct) == -1);
    check("setting -50 is invalid", brightness_parse_setting("-50", &pct) == -1);
    check("setting +50 is invalid", brightness_parse_setting("+50", &pct) == -1);
    check("setting empty is invalid", brightness_parse_setting("", &pct) == -1);
    check("setting NULL is invalid", brightness_parse_setting(NULL, &pct) == -1);
    check("setting with a space is invalid", brightness_parse_setting(" 50", &pct) == -1);
    check("setting with a unit is invalid", brightness_parse_setting("50%", &pct) == -1);
    check("setting with a fraction is invalid", brightness_parse_setting("50.5", &pct) == -1);
    check("setting with a leading zero is invalid", brightness_parse_setting("050", &pct) == -1);
    check("setting too long is invalid", brightness_parse_setting("1000", &pct) == -1);
    check("setting as a word is invalid", brightness_parse_setting("high", &pct) == -1);

    remove_roots();
    printf("brightness_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
