/*
 * PocketFleet colour contract: the pairs the tactical grid actually puts on
 * top of each other must stay legible in every theme and every display mode.
 *
 * PocketFleet invents no colours; it composes Design System tokens. The DS
 * contrast gate (tests/theme_test) checks each token against bg, but not the
 * combinations an app chooses, so this test states them and holds every
 * theme to them. When a theme is added (the sixth, doors, in DS §32), this
 * is what tells us whether the grid still reads in it.
 *
 * Night is deliberately below AA (DS §13), so it has its own lower floors.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_theme.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

struct pair {
    const char *what;
    enum pos_color_token fg;
    enum pos_color_token bg;
    double floor_normal;   /* also used for Outdoor */
    double floor_night;
};

/* Every foreground/background combination the grid draws (fleet_grid.c). */
static const struct pair pairs[] = {
    /* Hit squares, sunk crosses and hull outlines are all text_on_accent on
     * a filled accent, which is the rule DS §4 states for that situation. */
    { "marks on your fire", POS_COLOR_TEXT_ON_ACCENT, POS_COLOR_RADIO_TX, 3.0, 2.0 },
    { "marks on incoming fire", POS_COLOR_TEXT_ON_ACCENT, POS_COLOR_RADIO_RX, 3.0, 2.0 },
    { "miss dot on a spent square", POS_COLOR_TEXT_SECONDARY, POS_COLOR_BG, 3.0, 2.5 },
    { "grid captions", POS_COLOR_TEXT_SECONDARY, POS_COLOR_BG, 3.0, 2.5 },
    { "spent square border", POS_COLOR_LINE, POS_COLOR_BG, 1.1, 1.02 },
    { "own hull border on its fill", POS_COLOR_TEXT_SECONDARY, POS_COLOR_SURFACE_RAISED, 2.5, 1.6 },
    { "crosshair on open water", POS_COLOR_FOCUS, POS_COLOR_SURFACE, 2.5, 1.4 },
    /* Deliberately near 1: a spent square is told from open water by its dot
     * and border, not by its fill (DS feasibility H1, RGB565 quantisation). */
    { "spent square against open water", POS_COLOR_BG, POS_COLOR_SURFACE, 1.02, 1.02 },
    { "sweep over open water", POS_COLOR_ACCENT_PRIMARY, POS_COLOR_SURFACE, 2.5, 1.4 },
};

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    static const enum pos_mode modes[POS_MODE_COUNT] = {
        POS_MODE_NORMAL, POS_MODE_OUTDOOR, POS_MODE_NIGHT
    };
    double worst[sizeof(pairs) / sizeof(pairs[0])];
    size_t p;
    int t;
    int m;

    for (p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        worst[p] = 1e9;
    }
    for (t = 0; t < pos_theme_count(); t++) {
        const struct pos_theme_def *def = pos_theme_at(t);

        for (m = 0; m < POS_MODE_COUNT; m++) {
            struct pos_theme_tokens tokens;
            char what[160];

            pos_theme_resolve(def, modes[m], &tokens);

            /* The two fire colours must stay tellable apart: they are the
             * whole difference between your shots and the enemy's. */
            snprintf(what, sizeof(what), "%s/%s: your fire differs from incoming fire",
                     def->id, pos_mode_name(modes[m]));
            check(what, tokens.color[POS_COLOR_RADIO_TX] != tokens.color[POS_COLOR_RADIO_RX]);

            for (p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
                double ratio = pos_contrast(tokens.color[pairs[p].fg],
                                            tokens.color[pairs[p].bg]);
                double floor = modes[m] == POS_MODE_NIGHT ? pairs[p].floor_night
                                                          : pairs[p].floor_normal;

                if (ratio < worst[p]) {
                    worst[p] = ratio;
                }
                snprintf(what, sizeof(what), "%s/%s: %s is %.2f, needs %.2f", def->id,
                         pos_mode_name(modes[m]), pairs[p].what, ratio, floor);
                check(what, ratio >= floor);
            }
        }
    }
    for (p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        printf("     worst across all themes and modes: %-38s %.2f\n", pairs[p].what,
               worst[p]);
    }
    printf("fleet_theme_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
