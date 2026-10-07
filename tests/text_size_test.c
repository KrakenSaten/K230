/*
 * Text size (DS §46), the pure half: the type scale every semantic role
 * takes at Small, Medium and Large, the names a stored value may have, and
 * the selection the shared styles listen to. No LVGL: what size a role is
 * drawn at is a table in pos_theme.c, and this proves the table's promises.
 *
 *   - Small is exactly the Design System's §3 scale, role by role, in every
 *     display mode it was ever drawn in (Outdoor's 20 px body included);
 *   - every role grows or stays as the size goes up, never shrinks, and the
 *     reading roles grow by the fifth and the two fifths §46 promises;
 *   - display roles (heroes, the environment's 40 px titles) never move;
 *   - Outdoor's body floor holds at every size;
 *   - a name parses to its size and back; anything else is refused;
 *   - selecting a size notifies the listeners once, a repeat is silent, and
 *     a value out of range is refused and leaves Small.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pos_theme.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* DS §3, as it stood before text sizes existed: role, face, px. */
static const struct {
    enum pos_type_role role;
    enum pos_type_face face;
    int px;
} ds3[] = {
    { POS_TYPE_BODY, POS_FACE_SANS, 16 },
    { POS_TYPE_LABEL, POS_FACE_SANS, 20 },
    { POS_TYPE_TITLE, POS_FACE_SANS_SEMIBOLD, 24 },
    { POS_TYPE_META, POS_FACE_MONO, 14 },
    { POS_TYPE_BUTTON, POS_FACE_MONO_MEDIUM, 16 },
    { POS_TYPE_VALUE, POS_FACE_MONO, 20 },
    { POS_TYPE_DISPLAY_40, POS_FACE_SANS_SEMIBOLD, 40 },
    { POS_TYPE_DISPLAY_48, POS_FACE_SANS_SEMIBOLD, 48 },
    { POS_TYPE_ENV_LABEL, POS_FACE_SANS, 20 },
    { POS_TYPE_ENV_SMALL, POS_FACE_SANS, 16 },
    { POS_TYPE_ENV_CAPTION, POS_FACE_SANS, 16 },
    { POS_TYPE_ENV_TITLE, POS_FACE_SANS_SEMIBOLD, 40 },
};

#define NDS3 ((int)(sizeof(ds3) / sizeof(ds3[0])))

static int notified;

static void listener(void *user)
{
    (void)user;
    notified++;
}

static int is_display(enum pos_type_role r)
{
    return r == POS_TYPE_DISPLAY_40 || r == POS_TYPE_DISPLAY_48 || r == POS_TYPE_ENV_TITLE;
}

int main(void)
{
    char what[160];
    int i;
    int m;

    check("every role is in the Small table", NDS3 == POS_TYPE_COUNT);

    /* Small is §3, in every mode; Outdoor's body is §6's 20 px. */
    for (i = 0; i < NDS3; i++) {
        for (m = 0; m < POS_MODE_COUNT; m++) {
            struct pos_type_spec s = pos_type_resolve(ds3[i].role, POS_TEXT_SIZE_SMALL, (enum pos_mode)m);
            int want = ds3[i].px;

            if (ds3[i].role == POS_TYPE_BODY && m == POS_MODE_OUTDOOR) {
                want = 20;
            }
            snprintf(what, sizeof(what), "small %s in %s is %s %d px", pos_type_role_name(ds3[i].role),
                     pos_mode_name((enum pos_mode)m), ds3[i].face == POS_FACE_SANS ? "sans" : "its face", want);
            check(what, s.face == ds3[i].face && s.px == want);
        }
    }

    /* Up the steps: never smaller, never another face. */
    for (i = 0; i < POS_TYPE_COUNT; i++) {
        enum pos_type_role r = (enum pos_type_role)i;
        struct pos_type_spec sm = pos_type_resolve(r, POS_TEXT_SIZE_SMALL, POS_MODE_NORMAL);
        struct pos_type_spec md = pos_type_resolve(r, POS_TEXT_SIZE_MEDIUM, POS_MODE_NORMAL);
        struct pos_type_spec lg = pos_type_resolve(r, POS_TEXT_SIZE_LARGE, POS_MODE_NORMAL);

        snprintf(what, sizeof(what), "%s keeps its face at every size", pos_type_role_name(r));
        check(what, sm.face == md.face && md.face == lg.face);
        snprintf(what, sizeof(what), "%s never shrinks: %d <= %d <= %d", pos_type_role_name(r), sm.px, md.px,
                 lg.px);
        check(what, sm.px <= md.px && md.px <= lg.px);
        if (is_display(r)) {
            snprintf(what, sizeof(what), "%s is display type, the same at every size", pos_type_role_name(r));
            check(what, sm.px == md.px && md.px == lg.px);
            continue;
        }
        snprintf(what, sizeof(what), "%s grows at Medium and again at Large", pos_type_role_name(r));
        check(what, sm.px < md.px && md.px < lg.px);
        /* Reading text: Medium about a fifth up, Large about two fifths. The
         * launcher's cell names are held to less (their cells are fixed,
         * DS §46.4) and are checked separately below. */
        if (r != POS_TYPE_ENV_LABEL) {
            double gm = (double)md.px / sm.px;
            double gl = (double)lg.px / sm.px;

            snprintf(what, sizeof(what), "%s at Medium is 1.15-1.25 x Small (%.2f)", pos_type_role_name(r), gm);
            check(what, gm >= 1.15 && gm <= 1.25);
            snprintf(what, sizeof(what), "%s at Large is 1.30-1.40 x Small (%.2f)", pos_type_role_name(r), gl);
            check(what, gl >= 1.30 && gl <= 1.40);
        }
    }
    {
        struct pos_type_spec md = pos_type_resolve(POS_TYPE_ENV_LABEL, POS_TEXT_SIZE_MEDIUM, POS_MODE_NORMAL);
        struct pos_type_spec lg = pos_type_resolve(POS_TYPE_ENV_LABEL, POS_TEXT_SIZE_LARGE, POS_MODE_NORMAL);

        check("launcher names: Medium 22 px, Large 24 px (the 124 px cell)", md.px == 22 && lg.px == 24);
    }

    /* The hierarchy survives every step: a title over a label over body
     * text, a value over a caption. */
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        enum pos_text_size z = (enum pos_text_size)i;

        snprintf(what, sizeof(what), "%s: title > label > body, value > meta", pos_text_size_name(z));
        check(what, pos_type_resolve(POS_TYPE_TITLE, z, POS_MODE_NORMAL).px >
                        pos_type_resolve(POS_TYPE_LABEL, z, POS_MODE_NORMAL).px &&
                    pos_type_resolve(POS_TYPE_LABEL, z, POS_MODE_NORMAL).px >
                        pos_type_resolve(POS_TYPE_BODY, z, POS_MODE_NORMAL).px &&
                    pos_type_resolve(POS_TYPE_VALUE, z, POS_MODE_NORMAL).px >
                        pos_type_resolve(POS_TYPE_META, z, POS_MODE_NORMAL).px);
    }

    /* Outdoor's floor is a floor, not a size: at Large the body is Large's. */
    check("outdoor body at Medium is 20 (the floor over 19)",
          pos_type_resolve(POS_TYPE_BODY, POS_TEXT_SIZE_MEDIUM, POS_MODE_OUTDOOR).px == 20);
    check("outdoor body at Large is 22 (above the floor)",
          pos_type_resolve(POS_TYPE_BODY, POS_TEXT_SIZE_LARGE, POS_MODE_OUTDOOR).px == 22);
    check("night body is the size's own", pos_type_resolve(POS_TYPE_BODY, POS_TEXT_SIZE_MEDIUM, POS_MODE_NIGHT).px ==
                                              pos_type_resolve(POS_TYPE_BODY, POS_TEXT_SIZE_MEDIUM, POS_MODE_NORMAL).px);

    /* Out of range comes back as Small in Normal, never as garbage. */
    check("a size out of range resolves as Small",
          pos_type_resolve(POS_TYPE_LABEL, (enum pos_text_size)7, POS_MODE_NORMAL).px == 20 &&
              pos_type_resolve(POS_TYPE_LABEL, (enum pos_text_size)-1, POS_MODE_OUTDOOR).px == 20);
    check("a role out of range resolves as body",
          pos_type_resolve((enum pos_type_role)99, POS_TEXT_SIZE_SMALL, POS_MODE_NORMAL).px == 16);

    /* Names. */
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        enum pos_text_size out = POS_TEXT_SIZE_COUNT;

        snprintf(what, sizeof(what), "\"%s\" parses back to itself", pos_text_size_name((enum pos_text_size)i));
        check(what, pos_text_size_parse(pos_text_size_name((enum pos_text_size)i), &out) == 0 && (int)out == i);
    }
    {
        static const char *const bad[] = { "", "Small", "LARGE", "huge", "medium ", " large", "1", "smal" };
        enum pos_text_size out = POS_TEXT_SIZE_MEDIUM;
        size_t k;

        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            snprintf(what, sizeof(what), "\"%s\" is refused and the output left alone", bad[k]);
            check(what, pos_text_size_parse(bad[k], &out) < 0 && out == POS_TEXT_SIZE_MEDIUM);
        }
        check("NULL is refused", pos_text_size_parse(NULL, &out) < 0);
        check("an unknown size has no name", strcmp(pos_text_size_name((enum pos_text_size)5), "?") == 0);
    }

    /* Selection. */
    check("the default is Small", pos_theme_current_text_size() == POS_TEXT_SIZE_SMALL &&
                                      POS_TEXT_SIZE_DEFAULT == POS_TEXT_SIZE_SMALL);
    check("a listener can be added", pos_theme_add_listener(listener, NULL) == 0);
    check("selecting Small at Small is silent",
          pos_theme_select_text_size(POS_TEXT_SIZE_SMALL) == 0 && notified == 0);
    check("selecting Medium notifies once", pos_theme_select_text_size(POS_TEXT_SIZE_MEDIUM) == 0 && notified == 1 &&
                                                pos_theme_current_text_size() == POS_TEXT_SIZE_MEDIUM);
    check("and the roles follow", pos_type_current(POS_TYPE_BODY).px == 19);
    check("selecting it again is silent", pos_theme_select_text_size(POS_TEXT_SIZE_MEDIUM) == 0 && notified == 1);
    check("Large notifies once more", pos_theme_select_text_size(POS_TEXT_SIZE_LARGE) == 0 && notified == 2 &&
                                          pos_type_current(POS_TYPE_TITLE).px == 32);
    {
        char why[64];

        /* The mode is the theme engine's; a mode change reaches the type. */
        pos_theme_select(NULL, "outdoor", why, sizeof(why));
        check("a mode change notifies and outdoor's floor holds at Large",
              notified == 3 && pos_type_current(POS_TYPE_BODY).px == 22);
        pos_theme_select(NULL, "normal", why, sizeof(why));
    }
    notified = 0;
    check("a value out of range is refused", pos_theme_select_text_size((enum pos_text_size)9) < 0);
    check("and Small is applied, with a notification", pos_theme_current_text_size() == POS_TEXT_SIZE_SMALL &&
                                                          notified == 1);
    check("a negative value likewise", pos_theme_select_text_size((enum pos_text_size)-3) < 0 &&
                                           pos_theme_current_text_size() == POS_TEXT_SIZE_SMALL && notified == 1);
    pos_theme_remove_listener(listener, NULL);
    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    check("a removed listener hears nothing", notified == 1);

    printf("text_size_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
