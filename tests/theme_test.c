/*
 * Theme engine test: every token of every theme and mode in themes.json must
 * be reproduced by pos_theme_resolve(); the JSON contrast audit must match
 * pos_contrast(); the §4 invariants must hold; selection and fallback must
 * behave as §8 says.
 *
 * Usage: theme_test [path/to/themes.json]
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pos_theme.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static void listener(void *user)
{
    (*(int *)user)++;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "docs/design/themes.json";
    char *text = read_file(path);
    cJSON *root;
    cJSON *order;
    cJSON *themes;
    const cJSON *tid;
    int idx = 0;
    char why[128];
    int notified = 0;

    if (!text) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        fprintf(stderr, "invalid JSON in %s\n", path);
        return 1;
    }
    order = cJSON_GetObjectItemCaseSensitive(root, "theme_order");
    themes = cJSON_GetObjectItemCaseSensitive(root, "themes");

    check("theme count matches theme_order", cJSON_GetArraySize(order) == pos_theme_count());
    check("fallback id from JSON", strcmp(cJSON_GetObjectItemCaseSensitive(root, "fallback_theme")->valuestring,
                                          pos_theme_fallback_id()) == 0);

    cJSON_ArrayForEach(tid, order) {
        const struct pos_theme_def *def = pos_theme_find(tid->valuestring);
        const cJSON *theme = cJSON_GetObjectItemCaseSensitive(themes, tid->valuestring);
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(theme, "modes");
        const cJSON *contrast = cJSON_GetObjectItemCaseSensitive(theme, "contrast");
        int m;
        char label[160];

        snprintf(label, sizeof(label), "theme %s exists in table", tid->valuestring);
        check(label, def != NULL);
        if (!def) {
            continue;
        }
        snprintf(label, sizeof(label), "theme %s order", tid->valuestring);
        check(label, pos_theme_at(idx) == def);
        idx++;
        snprintf(label, sizeof(label), "theme %s name", tid->valuestring);
        check(label, strcmp(def->name, cJSON_GetObjectItemCaseSensitive(theme, "name")->valuestring) == 0);
        snprintf(label, sizeof(label), "theme %s passes invariants", tid->valuestring);
        check(label, pos_theme_check(def, why, sizeof(why)) == 0);

        for (m = 0; m < POS_MODE_COUNT; m++) {
            const cJSON *table = cJSON_GetObjectItemCaseSensitive(modes, pos_mode_name((enum pos_mode)m));
            const cJSON *ctable = cJSON_GetObjectItemCaseSensitive(contrast, pos_mode_name((enum pos_mode)m));
            const cJSON *entry;
            struct pos_theme_tokens t;
            int k;

            pos_theme_resolve(def, (enum pos_mode)m, &t);
            for (k = 0; k < POS_COLOR_COUNT; k++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(table, pos_color_token_name((enum pos_color_token)k));
                uint32_t expect = 0;

                snprintf(label, sizeof(label), "%s/%s/%s value", tid->valuestring,
                         pos_mode_name((enum pos_mode)m), pos_color_token_name((enum pos_color_token)k));
                if (!cJSON_IsString(v) || pos_color_parse(v->valuestring, &expect) < 0) {
                    check(label, 0);
                    continue;
                }
                if (t.color[k] != expect) {
                    printf("     got #%06x expected #%06x\n", t.color[k], expect);
                }
                check(label, t.color[k] == expect);
            }
            snprintf(label, sizeof(label), "%s/%s hairline_px", tid->valuestring, pos_mode_name((enum pos_mode)m));
            check(label, (int)cJSON_GetObjectItemCaseSensitive(table, "hairline_px")->valuedouble == t.hairline_px);
            snprintf(label, sizeof(label), "%s/%s type_default_px", tid->valuestring, pos_mode_name((enum pos_mode)m));
            check(label, (int)cJSON_GetObjectItemCaseSensitive(table, "type_default_px")->valuedouble == t.type_default_px);

            /* contrast audit: "a/b": ratio, rounded to one decimal in the JSON */
            cJSON_ArrayForEach(entry, ctable) {
                char a[32];
                char b[32];
                const char *slash = strchr(entry->string, '/');
                int ka = -1;
                int kb = -1;
                double got;

                if (!slash) {
                    continue;
                }
                snprintf(a, sizeof(a), "%.*s", (int)(slash - entry->string), entry->string);
                snprintf(b, sizeof(b), "%s", slash + 1);
                for (k = 0; k < POS_COLOR_COUNT; k++) {
                    if (strcmp(pos_color_token_name((enum pos_color_token)k), a) == 0) ka = k;
                    if (strcmp(pos_color_token_name((enum pos_color_token)k), b) == 0) kb = k;
                }
                snprintf(label, sizeof(label), "%s/%s contrast %s", tid->valuestring,
                         pos_mode_name((enum pos_mode)m), entry->string);
                if (ka < 0 || kb < 0) {
                    check(label, 0);
                    continue;
                }
                got = pos_contrast(t.color[ka], t.color[kb]);
                if (fabs(got - entry->valuedouble) > 0.11) {
                    printf("     got %.2f expected %.1f\n", got, entry->valuedouble);
                }
                check(label, fabs(got - entry->valuedouble) <= 0.11);
            }
        }
    }

    /* §13 thresholds, Normal and Outdoor, over all themes */
    for (idx = 0; idx < pos_theme_count(); idx++) {
        const struct pos_theme_def *def = pos_theme_at(idx);
        int m;

        for (m = POS_MODE_NORMAL; m <= POS_MODE_OUTDOOR; m++) {
            struct pos_theme_tokens t;
            char label[160];
            int k;

            pos_theme_resolve(def, (enum pos_mode)m, &t);
            snprintf(label, sizeof(label), "%s/%s text_primary >= 7", def->id, pos_mode_name((enum pos_mode)m));
            check(label, pos_contrast(t.color[POS_COLOR_TEXT_PRIMARY], t.color[POS_COLOR_BG]) >= 7.0);
            snprintf(label, sizeof(label), "%s/%s text_secondary >= 4.5", def->id, pos_mode_name((enum pos_mode)m));
            check(label, pos_contrast(t.color[POS_COLOR_TEXT_SECONDARY], t.color[POS_COLOR_BG]) >= 4.5);
            /* §46.7: muted text is read too, so it clears the body-text
             * floor - and stays quieter than secondary text, or it would not
             * be muted. */
            snprintf(label, sizeof(label), "%s/%s text_muted >= 4.5 and below text_secondary", def->id,
                     pos_mode_name((enum pos_mode)m));
            check(label, pos_contrast(t.color[POS_COLOR_TEXT_MUTED], t.color[POS_COLOR_BG]) >= 4.5 &&
                             pos_contrast(t.color[POS_COLOR_TEXT_MUTED], t.color[POS_COLOR_BG]) <
                                 pos_contrast(t.color[POS_COLOR_TEXT_SECONDARY], t.color[POS_COLOR_BG]));
            for (k = POS_COLOR_ACCENT_PRIMARY; k <= POS_COLOR_RADIO_TX; k++) {
                snprintf(label, sizeof(label), "%s/%s %s >= 4.5", def->id, pos_mode_name((enum pos_mode)m),
                         pos_color_token_name((enum pos_color_token)k));
                check(label, pos_contrast(t.color[k], t.color[POS_COLOR_BG]) >= 4.5);
            }
            /* The identity accents (DS §37) are drawn beside names on bg, on
             * surface (a panel) and on surface_raised (a selected row), in
             * every theme; Night is the exception the theme's own text also
             * makes, and there they only have to stay apart from bg as far
             * as text_secondary does. */
            for (k = 0; k < POS_IDENTITY_COUNT; k++) {
                uint32_t hue = pos_identity_rgb_mode((unsigned)k, (enum pos_mode)m);
                double floor = m == POS_MODE_NIGHT
                                   ? pos_contrast(t.color[POS_COLOR_TEXT_SECONDARY], t.color[POS_COLOR_BG])
                                   : 4.5;

                snprintf(label, sizeof(label), "%s/%s identity %d on bg", def->id,
                         pos_mode_name((enum pos_mode)m), k);
                check(label, pos_contrast(hue, t.color[POS_COLOR_BG]) >= floor);
                if (m != POS_MODE_NIGHT) {
                    snprintf(label, sizeof(label), "%s/%s identity %d on surface_raised", def->id,
                             pos_mode_name((enum pos_mode)m), k);
                    check(label, pos_contrast(hue, t.color[POS_COLOR_SURFACE_RAISED]) >= 4.5);
                }
            }
            check("every identity hue is its own",
                  pos_identity_rgb(0) != pos_identity_rgb(1) &&
                      pos_identity_rgb(POS_IDENTITY_COUNT) == pos_identity_rgb(0));
        }
    }

    /* §13 plus the component-sanctioned foreground/background pairs
     * (PocketFleet finding 10): surfaces as well as bg. */
    for (idx = 0; idx < pos_theme_count(); idx++) {
        static const int pairs[][2] = {
            { POS_COLOR_TEXT_PRIMARY, POS_COLOR_SURFACE },
            { POS_COLOR_TEXT_SECONDARY, POS_COLOR_SURFACE },
            { POS_COLOR_TEXT_PRIMARY, POS_COLOR_SURFACE_RAISED },
            { POS_COLOR_TEXT_SECONDARY, POS_COLOR_SURFACE_RAISED },
            { POS_COLOR_STATUS_OK, POS_COLOR_SURFACE },
            { POS_COLOR_STATUS_WARN, POS_COLOR_SURFACE },
            { POS_COLOR_STATUS_ERROR, POS_COLOR_SURFACE },
            { POS_COLOR_ACCENT_PRIMARY, POS_COLOR_SURFACE },
            { POS_COLOR_TEXT_ON_ACCENT, POS_COLOR_ACCENT_PRIMARY },
            { POS_COLOR_TEXT_ON_ACCENT, POS_COLOR_RADIO_RX },
            { POS_COLOR_TEXT_ON_ACCENT, POS_COLOR_RADIO_TX },
        };
        const struct pos_theme_def *def = pos_theme_at(idx);
        int m;

        for (m = POS_MODE_NORMAL; m <= POS_MODE_OUTDOOR; m++) {
            struct pos_theme_tokens t;
            size_t k;
            char label[160];

            pos_theme_resolve(def, (enum pos_mode)m, &t);
            for (k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
                snprintf(label, sizeof(label), "%s/%s sanctioned pair %s on %s >= 4.5", def->id,
                         pos_mode_name((enum pos_mode)m),
                         pos_color_token_name((enum pos_color_token)pairs[k][0]),
                         pos_color_token_name((enum pos_color_token)pairs[k][1]));
                check(label, pos_contrast(t.color[pairs[k][0]], t.color[pairs[k][1]]) >= 4.5);
            }
        }
    }
    /* text_primary on a chip fill is not a sanctioned pair; the test documents
     * why: it fails on at least one theme (Slate radio_rx, about 1.3). */
    {
        struct pos_theme_tokens t;

        pos_theme_resolve(pos_theme_find("slate"), POS_MODE_NORMAL, &t);
        check("unsanctioned pair text_primary on radio_rx is below 4.5 (use text_on_accent)",
              pos_contrast(t.color[POS_COLOR_TEXT_PRIMARY], t.color[POS_COLOR_RADIO_RX]) < 4.5);
    }

    /* mix() rounding and helpers */
    check("mix half-up rounding", pos_mix(0x8d99a6, 0x06080b, 0.35) == 0x5e6670);
    check("mix t=0", pos_mix(0x123456, 0xffffff, 0.0) == 0x123456);
    check("mix t=1", pos_mix(0x123456, 0xffffff, 1.0) == 0xffffff);
    check("contrast white/black", fabs(pos_contrast(0xffffff, 0x000000) - 21.0) < 0.01);
    check("parse #rrggbb", pos_color_parse("#8CcFfF", &(uint32_t){0}) == 0);
    check("parse rejects short", pos_color_parse("#fff", &(uint32_t){0}) < 0);
    check("parse rejects junk", pos_color_parse("#zzzzzz", &(uint32_t){0}) < 0);

    /* selection and fallback (§8) */
    pos_theme_add_listener(listener, &notified);
    check("default selection is fallback theme", strcmp(pos_theme_current_def()->id, pos_theme_fallback_id()) == 0);
    check("default mode normal", pos_theme_current_mode() == POS_MODE_NORMAL);
    check("select brass/outdoor ok", pos_theme_select("brass", "outdoor", why, sizeof(why)) == 0);
    check("current is brass", strcmp(pos_theme_current_def()->id, "brass") == 0);
    check("current mode outdoor", pos_theme_current_mode() == POS_MODE_OUTDOOR);
    check("listener notified once", notified == 1);
    check("rgb lookup uses current tokens", pos_theme_rgb(POS_COLOR_BG) == 0x000000 &&
                                            pos_theme_rgb(POS_COLOR_TEXT_PRIMARY) == 0xffffff);
    check("reselect same is silent", pos_theme_select("brass", "outdoor", why, sizeof(why)) == 0 && notified == 1);
    check("unknown theme falls back", pos_theme_select("neon", NULL, why, sizeof(why)) < 0);
    check("fallback theme selected", strcmp(pos_theme_current_def()->id, pos_theme_fallback_id()) == 0);
    check("fallback mode normal", pos_theme_current_mode() == POS_MODE_NORMAL);
    check("fallback gives a reason", strstr(why, "unknown theme") != NULL);
    check("unknown mode falls back", pos_theme_select("slate", "dusk", why, sizeof(why)) < 0 &&
                                     pos_theme_current_mode() == POS_MODE_NORMAL);
    check("mode only change keeps theme", pos_theme_select("olive", NULL, why, sizeof(why)) == 0 &&
                                          pos_theme_select(NULL, "night", why, sizeof(why)) == 0 &&
                                          strcmp(pos_theme_current_def()->id, "olive") == 0 &&
                                          pos_theme_current_mode() == POS_MODE_NIGHT);
    pos_theme_remove_listener(listener, &notified);
    {
        int before = notified;

        pos_theme_select("carbon", "normal", why, sizeof(why));
        check("removed listener not notified", notified == before);
    }

    cJSON_Delete(root);
    printf("theme_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
