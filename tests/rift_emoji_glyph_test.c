/*
 * RIFT colour emoji, the proof: one RGB565A8 image glyph drawn inline in a
 * plain lv_label between Plex letters, with the LVGL configuration the device
 * has - no imgfont module, no image cache, no FreeType in use, nothing
 * decoded at run time.
 *
 *   - "Hello 😀 world" in a copy of Plex Sans 20 falling back to the colour
 *     font: the emoji is found as an image glyph, measured at its width, and
 *     drawn in colour (yellow face) where the measurement puts it, while the
 *     Plex letters stay plain grey-on-black antialiasing - no colour.
 *   - The Plex font itself is untouched: still no fallback, still no emoji.
 *   - A label too narrow for the line wraps it, and the emoji is drawn on a
 *     line below the first.
 *   - Every image of the set is a well-formed RGB565A8 descriptor inside the
 *     embedded pixels.
 *
 * EMOJI_SHOTS=<dir> keeps the frame as rift-emoji-glyph.png. Built by
 * ui/shell/CMakeLists.txt (host builds), and once against the device's own
 * lv_conf.h for the proof (docs/apps/RIFT.md, "Colour emoji").
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "rift_emoji_font.h"
#include "rift_emoji_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LV_CACHE_DEF_SIZE != 0
#error "the device's LVGL has no image cache (LV_CACHE_DEF_SIZE 0); test with the same"
#endif
#if LV_USE_IMGFONT
#error "the device's LVGL is built without imgfont; test without it"
#endif
#if LV_COLOR_DEPTH != 16
#error "the device's panel is 16-bit"
#endif

#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif

LV_FONT_DECLARE(pos_font_sans_20)

#define W 480
#define H 160

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

static uint16_t fb[W * H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void rgb(int x, int y, int *r, int *g, int *b)
{
    uint16_t v = fb[y * W + x];

    *r = ((v >> 11) & 31) * 255 / 31;
    *g = ((v >> 5) & 63) * 255 / 63;
    *b = (v & 31) * 255 / 31;
}

/* In the box: pixels with real colour (channels far apart), and yellow ones. */
static void colour_in(int x0, int y0, int x1, int y1, int *coloured, int *yellow, int *lit)
{
    int x;
    int y;

    *coloured = *yellow = *lit = 0;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            int r, g, b, hi, lo;

            if (x < 0 || y < 0 || x >= W || y >= H) {
                continue;
            }
            rgb(x, y, &r, &g, &b);
            hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
            lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
            *lit += hi > 40;
            *coloured += hi - lo >= 64;
            *yellow += r > 180 && g > 130 && b < 110;
        }
    }
}

static int32_t width_of(const char *t, const lv_font_t *f)
{
    lv_point_t p;

    lv_text_get_size(&p, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

static lv_obj_t *plain_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    return l;
}

static void save_png(const char *dir)
{
#if LV_USE_LODEPNG
    char path[512];
    unsigned char *out = malloc((size_t)W * H * 3);
    int i;

    if (!out) {
        return;
    }
    for (i = 0; i < W * H; i++) {
        int r, g, b;

        rgb(i % W, i / W, &r, &g, &b);
        out[i * 3] = (unsigned char)r;
        out[i * 3 + 1] = (unsigned char)g;
        out[i * 3 + 2] = (unsigned char)b;
    }
    snprintf(path, sizeof(path), "%s/rift-emoji-glyph.png", dir);
    check("rift-emoji-glyph.png is written", lodepng_encode24_file(path, out, W, H) == 0);
    free(out);
#else
    (void)dir;
    printf("note no LodePNG: no PNG\n");
#endif
}

int main(void)
{
    static const char hello[] = "Hello \xF0\x9F\x98\x80 world";
    const lv_font_t *face;
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *one;
    lv_obj_t *wrapped;
    lv_font_glyph_dsc_t g;
    char what[160];
    unsigned i;
    int bad = 0;

    lv_init();
    disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    /* ---- the images ---------------------------------------------------- */
    for (i = 0; i < rift_emoji_img_count; i++) {
        const struct rift_emoji_img *e = &rift_emoji_imgs[i];
        const lv_image_dsc_t *d = rift_emoji_image(e->key);

        if (!d || d->header.cf != LV_COLOR_FORMAT_RGB565A8 || d->header.w != e->w || d->header.h != e->h ||
            d->header.stride != (uint32_t)e->w * 2 || e->off % 4 != 0 ||
            e->off + d->data_size > rift_emoji_px_size || (i && rift_emoji_imgs[i - 1].key >= e->key)) {
            bad++;
        }
    }
    snprintf(what, sizeof(what), "all %u images are RGB565A8 inside the %u embedded bytes, sorted", rift_emoji_img_count,
             rift_emoji_px_size);
    check(what, bad == 0 && rift_emoji_img_count > 0);

    /* ---- the font ------------------------------------------------------ */
    face = rift_emoji_font(&pos_font_sans_20);
    check("a copy is made, and asking again gives the same copy",
          face != &pos_font_sans_20 && rift_emoji_font(&pos_font_sans_20) == face && rift_emoji_font(face) == face);
    check("Plex Sans 20 itself is untouched: no fallback, no emoji",
          pos_font_sans_20.fallback == NULL && !lv_font_get_glyph_dsc(&pos_font_sans_20, &g, 0x1F600, 0));
    check("the copy draws letters from Plex",
          lv_font_get_glyph_dsc(face, &g, 'H', 'e') && g.format == LV_FONT_GLYPH_FORMAT_A4 && g.resolved_font == face);
    check("and U+1F600 as an 18 px image glyph from the colour font",
          lv_font_get_glyph_dsc(face, &g, 0x1F600, ' ') && g.format == LV_FONT_GLYPH_FORMAT_IMAGE &&
              g.resolved_font == face->fallback && g.box_w == 18 && g.box_h == 18 && g.adv_w == 20 &&
              lv_image_src_get_type(g.gid.src) == LV_IMAGE_SRC_VARIABLE);
    check("its measured width is Plex's letters plus the image's advance",
          width_of(hello, face) == width_of("Hello ", face) + 20 + width_of(" world", face));

    /* ---- drawn --------------------------------------------------------- */
    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);
    one = plain_label(screen, face, hello);
    lv_obj_set_pos(one, 10, 10);
    wrapped = plain_label(screen, face, hello);
    lv_obj_set_width(wrapped, width_of("Hello", face) + 6);
    lv_label_set_long_mode(wrapped, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(wrapped, 10, 50);
    lv_refr_now(disp);

    {
        int32_t line = face->line_height;
        int32_t ex = 10 + width_of("Hello ", face) + 1;
        int32_t ey = 10 + (line - 18) / 2;
        int coloured, yellow, lit;
        int tc, ty, tl;

        colour_in(ex, ey, ex + 17, ey + 17, &coloured, &yellow, &lit);
        snprintf(what, sizeof(what), "the emoji is drawn in colour where it was measured (%d coloured, %d yellow of 324)",
                 coloured, yellow);
        check(what, coloured >= 120 && yellow >= 60);
        colour_in(10, 10, 10 + width_of("Hello", face) - 1, 10 + line - 1, &tc, &ty, &tl);
        snprintf(what, sizeof(what), "\"Hello\" is plain Plex: lit, and no colour (%d lit, %d coloured)", tl, tc);
        check(what, tl >= 40 && tc == 0);
        colour_in(ex + 19, 10, 10 + width_of(hello, face) - 1, 10 + line - 1, &tc, &ty, &tl);
        snprintf(what, sizeof(what), "\" world\" is plain Plex too (%d lit, %d coloured)", tl, tc);
        check(what, tl >= 40 && tc == 0);

        snprintf(what, sizeof(what), "a narrow label wraps (%d px high, %d a line)", (int)lv_obj_get_height(wrapped),
                 (int)line);
        check(what, lv_obj_get_height(wrapped) >= 2 * line);
        colour_in(10, 50, 10 + lv_obj_get_width(wrapped), 50 + line - 1, &coloured, &yellow, &lit);
        tc = coloured;
        colour_in(10, 50 + line, 10 + lv_obj_get_width(wrapped), 50 + lv_obj_get_height(wrapped) - 1, &coloured, &yellow,
                  &lit);
        snprintf(what, sizeof(what), "and the emoji is drawn below the first line (%d coloured there, %d on line 1)",
                 coloured, tc);
        check(what, coloured >= 120 && tc == 0);
    }

    if (getenv("EMOJI_SHOTS") && *getenv("EMOJI_SHOTS")) {
        save_png(getenv("EMOJI_SHOTS"));
    }
    printf("rift_emoji_glyph_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
