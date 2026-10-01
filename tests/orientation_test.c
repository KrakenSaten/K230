/*
 * Rotation policy, its persistence and the keyboard-presence state
 * (ui/shell/orientation.c, ui/shell/kbd_presence.c, the settings store).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "kbd_presence.h"
#include "orientation.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", name);
    }
}

static enum kbd_presence heard;
static int calls;

static void on_presence(enum kbd_presence now, void *user)
{
    (void)user;
    heard = now;
    calls++;
}

int main(void)
{
    static const enum kbd_presence all[] = { KBD_PRESENCE_UNKNOWN, KBD_PRESENCE_ABSENT, KBD_PRESENCE_PRESENT };
    char dir[] = "/tmp/pos_orientation.XXXXXX";
    char path[600];
    char name[160];
    size_t i;

    /* ---- the policy ---------------------------------------------------- */
    for (i = 0; i < 3; i++) {
        snprintf(name, sizeof(name), "forced Portrait is rotation 0 with the keyboard %s",
                 kbd_presence_name(all[i]));
        check(name, orientation_resolve(ORIENTATION_PORTRAIT, all[i]) == POS_ROTATION_0);
        snprintf(name, sizeof(name), "forced Landscape is rotation 270 with the keyboard %s",
                 kbd_presence_name(all[i]));
        check(name, orientation_resolve(ORIENTATION_LANDSCAPE, all[i]) == POS_ROTATION_270);
    }
    check("Automatic with the keyboard present is Landscape (270)",
          orientation_resolve(ORIENTATION_AUTOMATIC, KBD_PRESENCE_PRESENT) == POS_ROTATION_270);
    check("Automatic with the keyboard absent is Portrait",
          orientation_resolve(ORIENTATION_AUTOMATIC, KBD_PRESENCE_ABSENT) == POS_ROTATION_0);
    check("Automatic with the keyboard unknown is Portrait",
          orientation_resolve(ORIENTATION_AUTOMATIC, KBD_PRESENCE_UNKNOWN) == POS_ROTATION_0);
    check("Landscape is a landscape of the 568x1232 panel",
          pos_rotation_is_landscape_of(ORIENTATION_LANDSCAPE_ROTATION, 568, 1232));
    check("a manual Portrait overrides a present keyboard",
          orientation_resolve(ORIENTATION_PORTRAIT, KBD_PRESENCE_PRESENT) !=
              orientation_resolve(ORIENTATION_AUTOMATIC, KBD_PRESENCE_PRESENT));
    check("a manual Landscape overrides an absent keyboard",
          orientation_resolve(ORIENTATION_LANDSCAPE, KBD_PRESENCE_ABSENT) !=
              orientation_resolve(ORIENTATION_AUTOMATIC, KBD_PRESENCE_ABSENT));

    /* ---- names --------------------------------------------------------- */
    {
        enum orientation_mode m = ORIENTATION_PORTRAIT;
        bool valid = false;

        check("the three modes parse and print back",
              orientation_mode_parse("automatic", &m) == 0 && m == ORIENTATION_AUTOMATIC &&
                  strcmp(orientation_mode_name(m), "automatic") == 0 &&
                  orientation_mode_parse("portrait", &m) == 0 && m == ORIENTATION_PORTRAIT &&
                  strcmp(orientation_mode_name(m), "portrait") == 0 &&
                  orientation_mode_parse("landscape", &m) == 0 && m == ORIENTATION_LANDSCAPE &&
                  strcmp(orientation_mode_name(m), "landscape") == 0);
        m = ORIENTATION_LANDSCAPE;
        check("case, spaces, degrees and empty are not modes",
              orientation_mode_parse("Landscape", &m) < 0 && orientation_mode_parse(" portrait", &m) < 0 &&
                  orientation_mode_parse("270", &m) < 0 && orientation_mode_parse("", &m) < 0 &&
                  orientation_mode_parse(NULL, &m) < 0 && m == ORIENTATION_LANDSCAPE);
        check("nothing stored is Automatic, and valid",
              orientation_mode_from_setting(NULL, &valid) == ORIENTATION_AUTOMATIC && valid);
        check("an invalid stored value falls back to Automatic and says it was invalid",
              orientation_mode_from_setting("sideways", &valid) == ORIENTATION_AUTOMATIC && !valid);
        check("which resolves to Portrait while the keyboard is unknown",
              orientation_resolve(orientation_mode_from_setting("sideways", &valid), KBD_PRESENCE_UNKNOWN) ==
                  POS_ROTATION_0);
    }

    /* ---- persistence ----------------------------------------------------- */
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETOS_CONFIG_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/settings.conf", dir);
    check("a fresh store has no rotation mode", settings_init() == 1 && settings_get(ORIENTATION_SETTING, NULL) == NULL);
    check("storing Landscape succeeds",
          settings_set(ORIENTATION_SETTING, orientation_mode_name(ORIENTATION_LANDSCAPE)) == 0);
    {
        bool valid = false;

        check("a reload reads Landscape back",
              settings_init() == 0 &&
                  orientation_mode_from_setting(settings_get(ORIENTATION_SETTING, NULL), &valid) ==
                      ORIENTATION_LANDSCAPE &&
                  valid);
    }
    {
        FILE *f = fopen(path, "w");
        bool valid = true;

        if (f) {
            fputs("theme=ice\ndisplay_rotation=upside-down\n", f);
            fclose(f);
        }
        check("a hand-edited invalid value reloads as Automatic, marked invalid",
              settings_init() == 0 &&
                  orientation_mode_from_setting(settings_get(ORIENTATION_SETTING, NULL), &valid) ==
                      ORIENTATION_AUTOMATIC &&
                  !valid);
        check("and the stored value is left for the user to change",
              strcmp(settings_get(ORIENTATION_SETTING, ""), "upside-down") == 0);
    }
    unlink(path);
    rmdir(dir);

    /* ---- keyboard presence ------------------------------------------------ */
    check("presence starts unknown", kbd_presence_get() == KBD_PRESENCE_UNKNOWN);
    kbd_presence_set_listener(on_presence, NULL);
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    check("publishing present is heard once", kbd_presence_get() == KBD_PRESENCE_PRESENT && calls == 1 &&
                                                  heard == KBD_PRESENCE_PRESENT);
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    check("publishing the same state again is not a change", calls == 1);
    kbd_presence_publish(KBD_PRESENCE_ABSENT);
    check("absent is heard", calls == 2 && heard == KBD_PRESENCE_ABSENT);
    kbd_presence_publish((enum kbd_presence)42);
    check("an out-of-range state is treated as unknown", kbd_presence_get() == KBD_PRESENCE_UNKNOWN && calls == 3);
    kbd_presence_set_listener(NULL, NULL);
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    check("with no listener the state still changes", calls == 3 && kbd_presence_get() == KBD_PRESENCE_PRESENT);
    {
        enum kbd_presence p = KBD_PRESENCE_ABSENT;

        check("presence names parse", kbd_presence_parse("present", &p) == 0 && p == KBD_PRESENCE_PRESENT &&
                                          kbd_presence_parse("absent", &p) == 0 && p == KBD_PRESENCE_ABSENT &&
                                          kbd_presence_parse("unknown", &p) == 0 && p == KBD_PRESENCE_UNKNOWN &&
                                          kbd_presence_parse("maybe", &p) < 0);
    }

    printf("orientation_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
