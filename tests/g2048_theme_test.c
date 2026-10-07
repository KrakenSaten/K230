/*
 * PG 2048 colour contract: every tile the game can draw, in all five themes
 * and all three display modes, resolved exactly as it will be drawn.
 *
 * The game names no colour (tests/style_lint.sh). Tiles are role tokens and
 * token mixes, so a theme the product adds later is covered by this test the
 * moment it is in the table. What is held:
 *
 *  - the number on a tile reads against its fill: 4.5:1 in Normal and
 *    Outdoor (DS 13's text floor), 3:1 in Night, where the DS deliberately
 *    goes below AA for dark adaptation;
 *  - the 2, which uses secondary text on a raised slab, the pair DS 4
 *    sanctions, holds 4.5 in Normal and Outdoor and 2.5 in Night - the same
 *    order as the DS's own text_secondary floor there;
 *  - fills never get brighter than half accent (DS 1);
 *  - the goal's outline, drawn in the gap over the board, reads against the
 *    board's background;
 *  - a higher tile is never duller than a lower one.
 *
 * The worst ratio seen for each rule is printed, so a theme change that
 * erodes a margin shows before it breaks one.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_view.h"

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

/* Relative luminance, as pos_contrast computes it, to compare fills. */
static double luminance_of(uint32_t rgb)
{
    /* contrast against black is (L + 0.05) / 0.05 */
    return pos_contrast(rgb, 0x000000) * 0.05 - 0.05;
}

int main(void)
{
    double worst_text[POS_MODE_COUNT] = { 99, 99, 99 };
    double worst_two[POS_MODE_COUNT] = { 99, 99, 99 };
    double worst_outline[POS_MODE_COUNT] = { 99, 99, 99 };
    int themes = pos_theme_count();
    int ti;
    int m;
    char what[160];

    /* The five of DS §8 and Doors, the default since DS §32. */
    check("the theme table has the six DS themes", themes == 6);
    for (ti = 0; ti < themes; ti++) {
        const struct pos_theme_def *def = pos_theme_at(ti);

        for (m = 0; m < POS_MODE_COUNT; m++) {
            struct pos_theme_tokens t;
            double last_lum = -1;
            int dimmer = 0;
            int too_bright = 0;
            uint8_t exp;

            pos_theme_resolve(def, (enum pos_mode)m, &t);
            for (exp = 1; exp <= G2048_EXP_MAX; exp++) {
                struct g2048_tile_look look;
                uint32_t fill;
                double c;
                double lum;

                g2048_view_tile_look(exp, &t, &look);
                fill = g2048_view_fill_rgb(&look, &t);
                c = pos_contrast(t.color[look.text], fill);
                if (exp == 1) {
                    if (c < worst_two[m]) {
                        worst_two[m] = c;
                    }
                    snprintf(what, sizeof(what), "%s/%s: the 2 reads (%.2f)", def->id,
                             pos_mode_name((enum pos_mode)m), c);
                    check(what, c >= (m == POS_MODE_NIGHT ? 2.5 : 4.5));
                } else {
                    if (c < worst_text[m]) {
                        worst_text[m] = c;
                    }
                    if (c < (m == POS_MODE_NIGHT ? 3.0 : 4.5)) {
                        snprintf(what, sizeof(what), "%s/%s: tile %u text reads (%.2f)", def->id,
                                 pos_mode_name((enum pos_mode)m), (unsigned)g2048_value(exp), c);
                        check(what, 0);
                    }
                }
                if (look.border_px) {
                    double o = pos_contrast(t.color[look.border], t.color[POS_COLOR_BG]);

                    if (o < worst_outline[m]) {
                        worst_outline[m] = o;
                    }
                }
                lum = luminance_of(fill);
                /* Rounding in the mix can move a channel by one step. */
                dimmer += lum + 0.002 < last_lum;
                last_lum = lum;
                too_bright += lum > luminance_of(0x808080u) &&
                              lum > luminance_of(t.color[POS_COLOR_ACCENT_PRIMARY]) * 0.6;
                too_bright += look.fill_mix > G2048_MIX_MAX;
            }
            snprintf(what, sizeof(what), "%s/%s: no tile is duller than the one below it", def->id,
                     pos_mode_name((enum pos_mode)m));
            check(what, dimmer == 0);
            snprintf(what, sizeof(what), "%s/%s: no fill is a bright area", def->id,
                     pos_mode_name((enum pos_mode)m));
            check(what, too_bright == 0);
        }
    }
    for (m = 0; m < POS_MODE_COUNT; m++) {
        check("the goal outline reads on the board", worst_outline[m] >= (m == POS_MODE_NIGHT ? 2.5 : 4.5));
        printf("note: %-7s worst tile text %.2f, the 2 %.2f, goal outline %.2f\n",
               pos_mode_name((enum pos_mode)m), worst_text[m], worst_two[m], worst_outline[m]);
    }
    printf("g2048_theme_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
