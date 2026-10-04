/*
 * RIFT colour emoji in a message body and a conversation preview, through
 * the styles RIFT gives those labels (ui/rift_emoji_style.h) and the folding
 * it puts their text through (rift_emoji.h).
 *
 *   - The owner's sample - faces, a thumb, a heart with and without its
 *     selector, the heart on fire, a family, two flags, a keycap, fire,
 *     rocket, party, antenna, camera, notes - folds to one code point per
 *     emoji, and each is an image glyph, never a placeholder, in the body
 *     and the caption face at every text size.
 *   - Skin tones are stripped: a toned thumb, a toned wave show their base.
 *   - The styles follow the text size: after the theme-changed event each
 *     label draws in a RIFT copy of exactly the font its role style now has.
 *   - Drawn, every emoji row has colour in it and the text rows' letters are
 *     the theme's text colour.
 *   - Cost: a frame of emoji text against the same frame in plain text, and
 *     the lookups (notes; host timings, not the board's).
 *
 * EMOJI_SHOTS=<dir> keeps rift-emoji-small.png and rift-emoji-large.png.
 * Built by ui/shell/CMakeLists.txt (host builds), run by
 * tests/rift_emoji_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_styles.h"
#include "pos_theme.h"
#include "rift_emoji.h"
#include "rift_emoji_font.h"
#include "rift_emoji_pick.h"
#include "rift_emoji_style.h"
#include "rift_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif

#define W 760
#define H 760

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

static const struct {
    const char *text;
    const char *say;
    int emoji;  /* emoji the folded text must hold, one code point each */
} samples[] = {
    { "Hei \xF0\x9F\x98\x80 \xF0\x9F\x98\x82 \xF0\x9F\x98\x8E \xF0\x9F\xA4\x94", "faces", 4 },
    { "Bra jobba \xF0\x9F\x91\x8D", "a thumb", 1 },
    { "\xE2\x9D\xA4\xEF\xB8\x8F \xE2\x9D\xA4 \xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5",
      "a heart with and without its selector, the heart on fire", 3 },
    { "Familie \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", "a family", 1 },
    { "Norge \xF0\x9F\x87\xB3\xF0\x9F\x87\xB4 Sverige \xF0\x9F\x87\xB8\xF0\x9F\x87\xAA", "two flags", 2 },
    { "Keycap 1\xEF\xB8\x8F\xE2\x83\xA3", "a keycap", 1 },
    { "\xF0\x9F\x94\xA5 \xF0\x9F\x9A\x80 \xF0\x9F\x8E\x89 \xF0\x9F\x93\xA1 \xF0\x9F\x93\xB7 \xF0\x9F\x8E\xB5",
      "fire, rocket, party, antenna, camera, notes", 6 },
    { "Tone \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF \xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD",
      "toned thumbs and wave, shown as their base", 3 },
    { "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xB3\xF3\xA0\x81\xA3\xF3\xA0\x81\xB4\xF3\xA0\x81\xBF"
      " \xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88",
      "Scotland's tag flag, the rainbow flag", 2 },
};
#define SAMPLE_COUNT ((int)(sizeof(samples) / sizeof(samples[0])))

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    int i;

    if (len > 1) {
        c &= 0x3F >> (len - 1);
    }
    for (i = 1; i < len; i++) {
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += len;
    return c;
}

/* Image glyphs and placeholders font draws text with. */
static void count_glyphs(const lv_font_t *font, const char *s, int *images, int *boxes)
{
    *images = *boxes = 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        lv_font_glyph_dsc_t g;

        if (cp < 0x20) {
            continue;
        }
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0) || g.is_placeholder) {
            (*boxes)++;
        } else if (g.format == LV_FONT_GLYPH_FORMAT_IMAGE) {
            (*images)++;
        }
    }
}

/* Pixels of the images text folds to that have real colour in the artwork
 * itself (opaque, channels far apart): a grey keycap or family has few. */
static int artwork_colour(const char *s)
{
    int n = 0;

    while (*s) {
        const lv_image_dsc_t *d = rift_emoji_image(next_cp(&s));
        uint32_t i;
        uint32_t px;

        if (!d) {
            continue;
        }
        px = d->header.w * d->header.h;
        for (i = 0; i < px; i++) {
            uint16_t v = (uint16_t)(d->data[i * 2] | (d->data[i * 2 + 1] << 8));
            int r = ((v >> 11) & 31) * 255 / 31;
            int g = ((v >> 5) & 63) * 255 / 63;
            int b = (v & 31) * 255 / 31;
            int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);

            n += d->data[px * 2 + i] > 200 && hi - lo >= 80;
        }
    }
    return n;
}

static const lv_font_t *role_font(enum pos_style_role role)
{
    lv_style_value_t v;

    return lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND ? v.ptr : NULL;
}

/* ---- the frame --------------------------------------------------------- */

static uint16_t fb[W * H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static int coloured_in(const lv_area_t *a)
{
    int n = 0;
    int x;
    int y;

    for (y = a->y1; y <= a->y2 && y < H; y++) {
        for (x = a->x1; x <= a->x2 && x < W; x++) {
            uint16_t v;
            int r, g, b, hi, lo;

            if (x < 0 || y < 0) {
                continue;
            }
            v = fb[y * W + x];
            r = ((v >> 11) & 31) * 255 / 31;
            g = ((v >> 5) & 63) * 255 / 63;
            b = (v & 31) * 255 / 31;
            hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
            lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
            n += hi - lo >= 80;
        }
    }
    return n;
}

static void save_png(const char *dir, const char *name)
{
#if LV_USE_LODEPNG
    char path[512];
    unsigned char *out = malloc((size_t)W * H * 3);
    int i;

    if (!out) {
        return;
    }
    for (i = 0; i < W * H; i++) {
        uint16_t v = fb[i];

        out[i * 3] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
        out[i * 3 + 1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
        out[i * 3 + 2] = (unsigned char)((v & 31) * 255 / 31);
    }
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    check("a PNG is written", lodepng_encode24_file(path, out, W, H) == 0);
    free(out);
#else
    (void)dir;
    (void)name;
#endif
}

static void on_theme(lv_event_t *e)
{
    (void)e;
    rift_emoji_style_refresh(); /* what rift_app.c's listener does */
}

static lv_obj_t *labels_body[SAMPLE_COUNT];
static lv_obj_t *labels_preview[SAMPLE_COUNT];

static lv_obj_t *make_label(lv_obj_t *parent, enum pos_style_role role, const char *text, int32_t w)
{
    lv_obj_t *l = lv_label_create(parent);
    char shown[256];

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    rift_emoji_style_add(l, role);
    rift_emoji_fold(text, shown, sizeof(shown));
    lv_label_set_text(l, shown);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static double ms_for_frames(lv_display_t *disp, lv_obj_t *screen, int frames)
{
    struct timespec a;
    struct timespec b;
    int i;

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < frames; i++) {
        lv_obj_invalidate(screen);
        lv_refr_now(disp);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6) / frames;
}

/* ---- the composer's picker (rift_emoji_pick.h) ---------------------------- */

/* Every emoji the picker offers is one image in the colour font, untoned,
 * offered once; the composer's field draws each as that one image and its
 * variation selector as nothing; the recent list goes first and stays
 * bounded. */
static void picker_table(void)
{
    const lv_font_t *field = rift_emoji_font(role_font(POS_STYLE_FIELD));
    const char *seen[RIFT_EMOJI_GROUPS * RIFT_EMOJI_CELLS + RIFT_EMOJI_GROUPS];
    unsigned n_seen = 0;
    unsigned g;
    unsigned i;
    int ok_one = 1;
    int ok_tone = 1;
    int ok_once = 1;
    int ok_field = 1;
    char what[200];

    for (g = 0; g < RIFT_EMOJI_GROUPS; g++) {
        const struct rift_emoji_group *grp = &rift_emoji_groups[g];

        snprintf(what, sizeof(what), "picker: %s fills the grid", grp->title);
        check(what, grp->count == RIFT_EMOJI_CELLS);
        for (i = 0; i <= grp->count; i++) {
            const char *e = i < grp->count ? grp->items[i] : grp->icon;
            char shown[32];
            const char *s;
            unsigned k;
            int cps = 0;
            int images;
            int boxes;

            for (s = e; *s;) {
                uint32_t cp = next_cp(&s);

                ok_tone = ok_tone && !(cp >= 0x1F3FBu && cp <= 0x1F3FFu);
            }
            rift_emoji_fold(e, shown, sizeof(shown));
            for (s = shown; *s; cps++) {
                ok_one = ok_one && rift_emoji_image(next_cp(&s)) != NULL;
            }
            if (cps != 1 || strlen(e) >= RIFT_EMOJI_PICK_LEN) {
                ok_one = 0;
                printf("  picker: %s %u is not one emoji with an image\n", grp->title, i);
            }
            count_glyphs(field, e, &images, &boxes);
            ok_field = ok_field && images == 1 && boxes == 0;
            if (i == grp->count) {
                continue; /* the tab's icon may be an item too */
            }
            for (k = 0; k < n_seen; k++) {
                ok_once = ok_once && strcmp(seen[k], e) != 0;
            }
            seen[n_seen++] = e;
            ok_once = ok_once && rift_emoji_pick_find(e) == e;
        }
    }
    check("picker: every emoji and tab folds to one image of the colour font", ok_one);
    check("picker: no skin-tone variant is offered", ok_tone);
    check("picker: each emoji is offered once", ok_once);
    check("picker: in the composer's field each is one image and its selector draws nothing",
          ok_field);
    check("picker: the owner's common row comes first, in the owner's order",
          strcmp(rift_emoji_groups[0].items[0], "\xF0\x9F\x99\x82") == 0 &&
              strcmp(rift_emoji_groups[0].items[3], "\xE2\x9D\xA4\xEF\xB8\x8F") == 0 &&
              strcmp(rift_emoji_groups[0].items[14], "\xF0\x9F\x8C\xB2") == 0);
    check("picker: what is not in the table is not found",
          rift_emoji_pick_find("\xE2\x9D\xA4") == NULL &&
              rift_emoji_pick_find("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB") == NULL &&
              rift_emoji_pick_find("") == NULL);

    {
        const char *recent[RIFT_EMOJI_RECENT_MAX];
        const char *items[RIFT_EMOJI_CELLS];
        const char *people0 = rift_emoji_groups[1].items[0];
        const char *fire = rift_emoji_groups[0].items[5];
        char text[RIFT_EMOJI_RECENT_MAX * RIFT_EMOJI_PICK_LEN];
        unsigned n = 0;
        unsigned m;
        int dup = 0;

        n = rift_emoji_recent_push(recent, n, fire);
        n = rift_emoji_recent_push(recent, n, people0);
        n = rift_emoji_recent_push(recent, n, fire);
        check("picker: a recent emoji picked again moves first and is not repeated",
              n == 2 && recent[0] == fire && recent[1] == people0);
        m = rift_emoji_group_items(0, recent, n, items);
        for (i = 1; i < m; i++) {
            dup += items[i] == fire;
        }
        check("picker: the first group shows the recent ones first, then the common ones, once",
              m == RIFT_EMOJI_CELLS && items[0] == fire && items[1] == people0 &&
                  items[2] == rift_emoji_groups[0].items[0] && dup == 0);
        m = rift_emoji_group_items(1, recent, n, items);
        check("picker: the other groups are as the table has them",
              m == RIFT_EMOJI_CELLS && items[0] == people0);
        for (i = 0; i < RIFT_EMOJI_CELLS; i++) {
            n = rift_emoji_recent_push(recent, n, rift_emoji_groups[2].items[i]);
        }
        check("picker: the recent list keeps one row, the newest first",
              n == RIFT_EMOJI_RECENT_MAX && recent[0] == rift_emoji_groups[2].items[14] &&
                  recent[4] == rift_emoji_groups[2].items[10]);
        check("picker: the recent list is written as the emoji, space-separated, and fits the file",
              rift_emoji_recent_format(recent, n, text, sizeof(text)) > 0 &&
                  strlen(text) < RIFT_PREF_EMOJI_RECENT_LEN);
        {
            const char *back[RIFT_EMOJI_RECENT_MAX];

            check("picker: and read back the same",
                  rift_emoji_recent_parse(text, back) == n && back[0] == recent[0] &&
                      back[4] == recent[4]);
            check("picker: unknown, toned and repeated entries are dropped when read",
                  rift_emoji_recent_parse("x \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB \xF0\x9F\x99\x82 "
                                          "\xF0\x9F\x99\x82  \xF0\x9F\x94\xA5",
                                          back) == 2 &&
                      back[0] == rift_emoji_groups[0].items[0] && back[1] == fire);
        }
    }
}

int main(void)
{
    static const enum pos_text_size sizes[] = { POS_TEXT_SIZE_SMALL, POS_TEXT_SIZE_MEDIUM, POS_TEXT_SIZE_LARGE };
    static const char *const size_names[] = { "Small", "Medium", "Large" };
    const char *shots = getenv("EMOJI_SHOTS");
    lv_display_t *disp;
    lv_obj_t *screen;
    char what[200];
    char shown[256];
    size_t z;
    int i;

    lv_init();
    disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    pos_styles_init();

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    pos_style_add(screen, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(screen, W, H);
    lv_obj_set_style_pad_all(screen, 10, 0);
    lv_obj_set_style_pad_row(screen, 3, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_screen_load(screen);
    lv_obj_add_event_cb(screen, on_theme, (lv_event_code_t)pos_event_theme_changed(), NULL);

    /* ---- folding ------------------------------------------------------- */
    for (i = 0; i < SAMPLE_COUNT; i++) {
        const char *s;
        int pua = 0;
        int emoji = 0;

        rift_emoji_fold(samples[i].text, shown, sizeof(shown));
        for (s = shown; *s;) {
            uint32_t cp = next_cp(&s);

            pua += cp >= RIFT_EMOJI_PUA;
            emoji += cp >= 0x2000 && rift_emoji_image(cp) != NULL;
        }
        snprintf(what, sizeof(what), "%s: %d emoji, each one image (%d found)", samples[i].say, samples[i].emoji, emoji);
        check(what, emoji == samples[i].emoji);
        (void)pua;
    }
    rift_emoji_fold(samples[7].text, shown, sizeof(shown));
    check("skin tones are stripped: the thumbs and the wave are their base",
          strcmp(shown, "Tone \xF0\x9F\x91\x8D \xF0\x9F\x91\x8D \xF0\x9F\x91\x8B") == 0);

    picker_table();

    /* ---- the labels ---------------------------------------------------- */
    for (i = 0; i < SAMPLE_COUNT; i++) {
        labels_body[i] = make_label(screen, POS_STYLE_TEXT_PRIMARY, samples[i].text, W - 20);
    }
    for (i = 0; i < 4; i++) {
        labels_preview[i] = make_label(screen, POS_STYLE_CAPTION, samples[i * 2].text, W - 20);
    }

    for (z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
        const lv_font_t *body;
        const lv_font_t *cap;
        int ok = 1;
        int boxes_all = 0;

        pos_theme_select_text_size(sizes[z]);
        lv_refr_now(disp);
        body = lv_obj_get_style_text_font(labels_body[0], 0);
        cap = lv_obj_get_style_text_font(labels_preview[0], 0);
        snprintf(what, sizeof(what), "%s: the body and preview draw in RIFT copies of their roles' fonts", size_names[z]);
        check(what, rift_emoji_font_is(body) && rift_emoji_font_is(cap) && body == rift_emoji_font(role_font(POS_STYLE_TEXT_PRIMARY)) &&
                        cap == rift_emoji_font(role_font(POS_STYLE_CAPTION)) && body->line_height == role_font(POS_STYLE_TEXT_PRIMARY)->line_height);
        for (i = 0; i < SAMPLE_COUNT; i++) {
            int images, boxes;

            count_glyphs(body, lv_label_get_text(labels_body[i]), &images, &boxes);
            boxes_all += boxes;
            if (images != samples[i].emoji) {
                printf("note %s %s: %d images\n", size_names[z], samples[i].say, images);
                ok = 0;
            }
            count_glyphs(cap, lv_label_get_text(labels_body[i]), &images, &boxes);
            boxes_all += boxes;
            ok = ok && images == samples[i].emoji;
        }
        snprintf(what, sizeof(what), "%s: every emoji is an image glyph in the body and caption faces, no box (%d boxes)",
                 size_names[z], boxes_all);
        check(what, ok && boxes_all == 0);
        {
            /* Each row shows at least half the colour its artwork has (the
             * theme's text is not coloured; a grey keycap barely is). */
            int coloured_rows = 0;
            int rows = 0;

            for (i = 0; i < SAMPLE_COUNT + 4; i++) {
                lv_obj_t *l = i < SAMPLE_COUNT ? labels_body[i] : labels_preview[i - SAMPLE_COUNT];
                int want = artwork_colour(lv_label_get_text(l)) / 2;
                lv_area_t a;
                int got;

                lv_obj_get_coords(l, &a);
                got = coloured_in(&a);
                rows++;
                if (got >= want) {
                    coloured_rows++;
                } else {
                    printf("note %s row %d: %d coloured pixels drawn, %d wanted\n", size_names[z], i, got, want);
                }
            }
            snprintf(what, sizeof(what), "%s: every row is drawn with its artwork's colour (%d of %d)", size_names[z],
                     coloured_rows, rows);
            check(what, coloured_rows == rows);
        }
        {
            lv_area_t a;

            lv_obj_get_coords(labels_preview[3], &a);
            snprintf(what, sizeof(what), "%s: the sample fits the frame", size_names[z]);
            check(what, a.y2 < H);
        }
        if (shots && *shots && (z == 0 || z == 2)) {
            save_png(shots, z == 0 ? "rift-emoji-small.png" : "rift-emoji-large.png");
        }
    }
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    lv_refr_now(disp);

    /* ---- cost ---------------------------------------------------------- */
    {
        double with = ms_for_frames(disp, screen, 20);
        double plain;

        for (i = 0; i < SAMPLE_COUNT; i++) {
            char ascii[256];
            size_t n = strlen(lv_label_get_text(labels_body[i]));

            memset(ascii, 'm', n < sizeof(ascii) - 1 ? n / 3 : 80);
            ascii[n < sizeof(ascii) - 1 ? n / 3 : 80] = '\0';
            lv_label_set_text(labels_body[i], ascii);
        }
        plain = ms_for_frames(disp, screen, 20);
        printf("note frame %dx%d, host: %.2f ms with the emoji sample, %.2f ms with plain text in its place\n", W, H, with,
               plain);
    }
    {
        enum { N = 200000 };
        const lv_font_t *body = lv_obj_get_style_text_font(labels_body[0], 0);
        lv_font_glyph_dsc_t g;
        struct timespec a;
        struct timespec b;
        volatile int sink = 0;
        int k;

        clock_gettime(CLOCK_MONOTONIC, &a);
        for (k = 0; k < N; k++) {
            sink += lv_font_get_glyph_dsc(body, &g, 0x1F600, 0);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("note lookup, host: an emoji through the body face %.0f ns\n",
               ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / N);
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (k = 0; k < N / 10; k++) {
            sink += (int)rift_emoji_fold(samples[3].text, shown, sizeof(shown));
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("note fold, host: the family line %.0f ns\n",
               ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / (N / 10));
        (void)sink;
    }

    printf("rift_emoji_ui_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
