/*
 * core/pocketcam's gallery side on a host: the photo metadata (EXIF written
 * and read back, a clock that was not set, damaged and hostile blocks, the
 * PPM comments), the image reader (every refusal, the fit, the orientation,
 * a file cut short) on synthetic pictures whose every quadrant has its own
 * colour, the library's order, and the export to Files.
 *
 * The JPEG half runs when the build has libjpeg (POCKETCAM_JPEG=1); the rest
 * runs in both builds.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam/pocketcam.h"
#include "pocketcam/pocketcam_codec.h"
#include "pocketcam/pocketcam_exif.h"
#include "pocketcam/pocketcam_image.h"
#include "pocketcam/pocketcam_store.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static char root[128];

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static void path_in(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", root, name);
}

static void write_bytes(const char *path, const void *data, size_t n)
{
    FILE *fp = fopen(path, "wb");

    if (fp) {
        if (n) {
            fwrite(data, 1, n, fp);
        }
        fclose(fp);
    }
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int first_byte(const char *path)
{
    FILE *fp = fopen(path, "rb");
    int c = -1;

    if (fp) {
        c = fgetc(fp);
        fclose(fp);
    }
    return c;
}

/* Export temporaries (.IMG_*.export) in dir. */
static int export_temps(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (strncmp(e->d_name, ".IMG_", 5) == 0 && len > 7 &&
            strcmp(e->d_name + len - 7, ".export") == 0) {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* A date the test can name: 2026-09-26 10:15:30 local time. */
static int64_t a_valid_time(void)
{
    struct tm tm = { 0 };

    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 8;
    tm.tm_mday = 26;
    tm.tm_hour = 10;
    tm.tm_min = 15;
    tm.tm_sec = 30;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

/* ---- the synthetic picture ---------------------------------------------------- *
 * w x h RGB565, four quadrants: red top-left, green top-right, blue
 * bottom-left, white bottom-right. */

#define RED 0xF800
#define GREEN 0x07E0
#define BLUE 0x001F
#define WHITE 0xFFFF

static uint16_t quad[1920 * 1080];

static void make_quad(struct pocketcam_frame *f, uint32_t w, uint32_t h)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool right = x >= w / 2;
            bool bottom = y >= h / 2;

            quad[y * w + x] = bottom ? (right ? WHITE : BLUE) : (right ? GREEN : RED);
        }
    }
    memset(f, 0, sizeof(*f));
    f->format = POCKETCAM_FMT_RGB565;
    f->width = w;
    f->height = h;
    f->stride = w * 2;
    f->data = (const uint8_t *)quad;
    f->bytes = (size_t)w * h * 2;
}

#ifdef POCKETCAM_HAVE_JPEG
/* Which quadrant colour a decoded pixel is, allowing for JPEG's loss. */
static uint16_t colour(uint16_t px)
{
    int r = (px >> 11) << 3;
    int g = ((px >> 5) & 0x3F) << 2;
    int b = (px & 0x1F) << 3;

    if (r > 180 && g > 180 && b > 180) {
        return WHITE;
    }
    if (r > 180 && g < 90 && b < 90) {
        return RED;
    }
    if (g > 180 && r < 90 && b < 90) {
        return GREEN;
    }
    if (b > 180 && r < 90 && g < 90) {
        return BLUE;
    }
    return 0;
}

static int encode_to(const char *path, const struct pocketcam_frame *f,
                     const struct pocketcam_photo_meta *meta)
{
    FILE *fp = fopen(path, "wb");
    int r;

    if (!fp) {
        return -errno;
    }
    r = pocketcam_encode_meta(fp, f, 0, false, meta);
    fclose(fp);
    return r;
}
#endif

/* A PPM as pos-camera writes one in a build without libjpeg (the codec in
 * this build may be JPEG): the comment lines, then the pixels. */
static int write_ppm(const char *path, const struct pocketcam_frame *f,
                     const struct pocketcam_photo_meta *meta)
{
    char comments[256];
    const uint16_t *px = (const uint16_t *)(const void *)f->data;
    FILE *fp = fopen(path, "wb");
    uint32_t i;

    if (!fp) {
        return -errno;
    }
    pocketcam_exif_ppm_comments(meta, comments, sizeof(comments));
    fprintf(fp, "P6\n%s%u %u\n255\n", comments, f->width, f->height);
    for (i = 0; i < f->width * f->height; i++) {
        uint8_t rgb[3] = { (uint8_t)((px[i] >> 11) << 3), (uint8_t)(((px[i] >> 5) & 0x3F) << 2),
                           (uint8_t)((px[i] & 0x1F) << 3) };

        fwrite(rgb, 1, 3, fp);
    }
    return fclose(fp) == 0 ? 0 : -errno;
}

/* ---- metadata ------------------------------------------------------------------ */

static void test_exif(void)
{
    uint8_t buf[POCKETCAM_EXIF_MAX];
    struct pocketcam_photo_meta meta = { a_valid_time(), false };
    struct pocketcam_exif e;
    size_t n;
    size_t cut;
    bool never_past = true;
    char date[POCKETCAM_EXIF_DATE_LEN];

    check("a valid clock gives an EXIF date",
          pocketcam_exif_date(meta.taken, date, sizeof(date)) &&
              strcmp(date, "2026:09:26 10:15:30") == 0);
    check("an unset clock gives none", !pocketcam_exif_date(0, date, sizeof(date)) && !date[0]);
    check("nor does the day before the floor",
          !pocketcam_exif_date(POCKETCAM_WALL_VALID_FROM - 1, date, sizeof(date)));

    n = pocketcam_exif_build(&meta, buf, sizeof(buf));
    check("an EXIF block is built", n > 20 && n <= sizeof(buf));
    check("and parses back", pocketcam_exif_parse(buf, n, &e) == 0);
    check("upright", e.orientation == 1);
    check("with the date it was taken", strcmp(e.taken, "2026:09:26 10:15:30") == 0);
    check("and Doors as the software", strncmp(e.software, "Doors ", 6) == 0);
    check("and no description for a real photo", e.description[0] == '\0');

    meta.taken = 1000; /* 1970: the clock was not set */
    meta.simulated = true;
    n = pocketcam_exif_build(&meta, buf, sizeof(buf));
    check("an unset clock writes no date at all",
          pocketcam_exif_parse(buf, n, &e) == 0 && e.taken[0] == '\0' && e.orientation == 1);
    check("and a simulated picture says so", strcmp(e.description, "Simulated picture") == 0);
    check("a block that does not fit is not built", pocketcam_exif_build(&meta, buf, 20) == 0);

    check("not EXIF: refused", pocketcam_exif_parse((const uint8_t *)"JFIF\0\0IIxxxxxx", 14, &e) == -1);
    meta.taken = a_valid_time();
    n = pocketcam_exif_build(&meta, buf, sizeof(buf));
    /* Every truncation of a good block: fewer facts, never a read past it
     * (ASan in camera-san-test would say). */
    for (cut = 0; cut < n; cut++) {
        uint8_t *copy = malloc(cut ? cut : 1);

        memcpy(copy, buf, cut);
        if (pocketcam_exif_parse(copy, cut, &e) == 0 && e.orientation != 1) {
            never_past = false;
        }
        free(copy);
    }
    check("every truncated block parses safely", never_past);
    {
        /* Big-endian, orientation 6, a date in the Exif IFD, and an Exif IFD
         * pointer that loops back to IFD0. */
        static const uint8_t be[] = {
            'E', 'x', 'i', 'f', 0, 0,
            'M', 'M', 0, 42, 0, 0, 0, 8,
            0, 2,                                   /* IFD0: 2 entries */
            0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, /* Orientation 6 */
            0x87, 0x69, 0, 4, 0, 0, 0, 1, 0, 0, 0, 38, /* Exif IFD at 38 */
            0, 0, 0, 0,
            0, 2,                                   /* Exif IFD: 2 entries */
            0x90, 0x03, 0, 2, 0, 0, 0, 20, 0, 0, 0, 68, /* DateTimeOriginal */
            0x87, 0x69, 0, 4, 0, 0, 0, 1, 0, 0, 0, 8,  /* nonsense: back to IFD0 */
            0, 0, 0, 0,
            '2', '0', '2', '5', ':', '0', '1', ':', '0', '2', ' ',
            '0', '3', ':', '0', '4', ':', '0', '5', 0,
        };

        check("a big-endian block parses", pocketcam_exif_parse(be, sizeof(be), &e) == 0);
        check("its orientation", e.orientation == 6);
        check("and its original date", strcmp(e.taken, "2025:01:02 03:04:05") == 0);
    }
    check("a zero date is no date", !pocketcam_exif_date_valid("0000:00:00 00:00:00"));
    check("nor a blank one", !pocketcam_exif_date_valid("    :  :     :  :  "));
    check("nor the 13th month", !pocketcam_exif_date_valid("2026:13:01 00:00:00"));
    check("a real one is", pocketcam_exif_date_valid("2026:09:26 23:59:59"));
    check("an unset camera clock's 1970 is no date",
          !pocketcam_exif_date_valid("1970:01:01 00:00:05"));
    check("nor its 1980", !pocketcam_exif_date_valid("1980:01:01 00:00:00"));
    check("dates are believed from the year EXIF began",
          pocketcam_exif_date_valid("1995:01:01 00:00:00"));
}

static void test_ppm_comments(void)
{
    struct pocketcam_photo_meta meta = { a_valid_time(), true };
    struct pocketcam_exif e;
    char buf[256];
    char *line;
    char *save = NULL;

    pocketcam_exif_ppm_comments(&meta, buf, sizeof(buf));
    pocketcam_exif_clear(&e);
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        pocketcam_exif_ppm_parse_comment(line + 1, &e);
    }
    check("PPM comments carry the date", strcmp(e.taken, "2026:09:26 10:15:30") == 0);
    check("the software and the simulation",
          strncmp(e.software, "Doors ", 6) == 0 && strcmp(e.description, "Simulated picture") == 0);
    pocketcam_exif_clear(&e);
    pocketcam_exif_ppm_parse_comment(" doors-taken 1970:01:01 00:00:00 extra", &e);
    pocketcam_exif_ppm_parse_comment(" someone-else's comment", &e);
    check("a malformed date and foreign comments are ignored", e.taken[0] == '\0');
}

/* ---- the reader ------------------------------------------------------------------ */

static void test_refusals(void)
{
    struct pocketcam_image_info info;
    char p[PATH_MAX];
    uint16_t px[16];
    static const uint8_t jpeg_head[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0, 17, 8, 0x01, 0x40, 0x00, 0xF0, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1,
        0xFF, 0xDA, 0, 2,
    };

    path_in(p, sizeof(p), "nothing-here.jpg");
    check("a missing file is -ENOENT", pocketcam_image_probe(p, &info) == -ENOENT);
    check("and says missing", strcmp(pocketcam_image_error_word(-ENOENT), "missing") == 0);
    path_in(p, sizeof(p), "empty.jpg");
    write_bytes(p, "", 0);
    check("an empty file is corrupt", pocketcam_image_probe(p, &info) == -EBADMSG);
    path_in(p, sizeof(p), "text.jpg");
    write_bytes(p, "hello world", 11);
    check("a text file is not a picture we read", pocketcam_image_probe(p, &info) == -ENOTSUP);
    path_in(p, sizeof(p), "zero.ppm");
    write_bytes(p, "P6\n0 0\n255\n", 11);
    check("a 0 x 0 PPM is corrupt", pocketcam_image_probe(p, &info) == -EBADMSG);
    path_in(p, sizeof(p), "huge.ppm");
    write_bytes(p, "P6\n9000 10\n255\n", 15);
    check("a 9000-pixel-wide PPM is too large", pocketcam_image_probe(p, &info) == -EFBIG);
    check("and says toolarge", strcmp(pocketcam_image_error_word(-EFBIG), "toolarge") == 0);
    path_in(p, sizeof(p), "deep.ppm");
    write_bytes(p, "P6\n4 4\n65535\n", 13);
    check("a 16-bit PPM is not read", pocketcam_image_probe(p, &info) == -ENOTSUP);
    path_in(p, sizeof(p), "cut.ppm");
    write_bytes(p, "P6\n4 ", 5);
    check("a header cut short is corrupt", pocketcam_image_probe(p, &info) == -EBADMSG);
    check("a folder is not a picture", pocketcam_image_probe(root, &info) == -ENOTSUP);
    path_in(p, sizeof(p), "garbage.jpg");
    write_bytes(p, "\xFF\xD8garbage", 9);
    check("a JPEG whose markers are garbage is corrupt", pocketcam_image_probe(p, &info) == -EBADMSG);

    path_in(p, sizeof(p), "head.jpg");
    write_bytes(p, jpeg_head, sizeof(jpeg_head));
    check("a JPEG's size is read from its header alone",
          pocketcam_image_probe(p, &info) == 0 && info.kind == POCKETCAM_IMAGE_JPEG &&
              info.width == 240 && info.height == 320 && info.exif.orientation == 1);
#ifdef POCKETCAM_HAVE_JPEG
    {
        uint16_t *big = malloc(64 * 64 * 2);
        int r = pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, big, 64, 64, NULL, NULL, &info);

        check("a JPEG with no image data does not decode as a clean picture",
              r == -EBADMSG || (r == 0 && info.damaged));
        free(big);
    }
#else
    check("without libjpeg a JPEG is probed but not decoded",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, px, 4, 4, NULL, NULL, &info) ==
              -ENOTSUP);
#endif
    check("a box of nothing is refused",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, px, 0, 4, NULL, NULL, NULL) == -EINVAL);
}

static void test_fit_size(void)
{
    uint32_t w;
    uint32_t h;

    pocketcam_image_fit_size(1080, 1920, POCKETCAM_FIT_CONTAIN, 528, 850, &w, &h);
    check("a portrait photo fits a portrait box by its height", w == 478 && h == 850);
    pocketcam_image_fit_size(1920, 1080, POCKETCAM_FIT_CONTAIN, 528, 850, &w, &h);
    check("a landscape photo fits it by its width", w == 528 && h == 297);
    pocketcam_image_fit_size(1920, 1080, POCKETCAM_FIT_COVER, 170, 170, &w, &h);
    check("cover is the box", w == 170 && h == 170);
    pocketcam_image_fit_size(8000, 1, POCKETCAM_FIT_CONTAIN, 100, 100, &w, &h);
    check("a sliver is at least one pixel", w == 100 && h == 1);
}

static void test_ppm_decode(void)
{
    struct pocketcam_frame f;
    struct pocketcam_image_info info;
    struct pocketcam_photo_meta meta = { a_valid_time(), false };
    static uint16_t out[64 * 64];
    char p[PATH_MAX];
    uint32_t w = 0;
    uint32_t h = 0;
    long size;
    FILE *fp;
    char *all;

    make_quad(&f, 40, 20);
    path_in(p, sizeof(p), "quad.ppm");
    check("a PPM photo is written with its metadata", write_ppm(p, &f, &meta) == 0);
    check("probed: PPM, 40 x 20, upright", pocketcam_image_probe(p, &info) == 0 &&
                                                info.kind == POCKETCAM_IMAGE_PPM &&
                                                info.width == 40 && info.height == 20 &&
                                                info.shown_w == 40 && info.shown_h == 20);
    check("its date and size", strcmp(info.exif.taken, "2026:09:26 10:15:30") == 0 &&
                                   (long)info.bytes == file_size(p) && info.mtime > 0);

    check("decoded to fit a 20 x 20 box",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, out, 20, 20, &w, &h, &info) == 0 &&
              w == 20 && h == 10 && !info.damaged);
    check("with every quadrant where it belongs",
          out[0] == RED && out[19] == GREEN && out[9 * 20] == BLUE && out[9 * 20 + 19] == WHITE);
    check("cover: 10 x 10 from the middle",
          pocketcam_image_decode(p, POCKETCAM_FIT_COVER, out, 10, 10, &w, &h, NULL) == 0 &&
              w == 10 && h == 10 && out[0] == RED && out[9] == GREEN && out[90] == BLUE &&
              out[99] == WHITE);
    check("a picture larger than itself is drawn larger (the box decides)",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, out, 64, 64, &w, &h, NULL) == 0 &&
              w == 64 && h == 32 && out[0] == RED && out[31 * 64 + 63] == WHITE);

    /* Cut the file in the middle of its pixels. */
    size = file_size(p);
    all = malloc((size_t)size);
    fp = fopen(p, "rb");
    if (all && fp && fread(all, 1, (size_t)size, fp) == (size_t)size) {
        fclose(fp);
        fp = NULL;
        path_in(p, sizeof(p), "quad-cut.ppm");
        write_bytes(p, all, (size_t)(size - 40 * 10 * 3 + 5));
    }
    if (fp) {
        fclose(fp);
    }
    free(all);
    check("a PPM cut short still decodes, marked damaged",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, out, 40, 20, &w, &h, &info) == 0 &&
              info.damaged);
    check("with what is there, and black where nothing is",
          out[0] == RED && out[19 * 40 + 39] == 0);
}

#ifdef POCKETCAM_HAVE_JPEG
/* The JPEG at src with its first marker after SOI preceded by a hand-made
 * EXIF block saying orientation o. */
static void with_orientation(const char *src, const char *dst, int o)
{
    static const uint8_t head[] = { 0xFF, 0xD8, 0xFF, 0xE1, 0, 36 };
    uint8_t exif[34] = {
        'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0,
        1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    long size = file_size(src);
    uint8_t *all = malloc((size_t)size);
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");

    exif[24] = (uint8_t)o;
    if (all && in && out && fread(all, 1, (size_t)size, in) == (size_t)size) {
        fwrite(head, 1, sizeof(head), out);
        fwrite(exif, 1, sizeof(exif), out);
        fwrite(all + 2, 1, (size_t)size - 2, out);
    }
    if (in) {
        fclose(in);
    }
    if (out) {
        fclose(out);
    }
    free(all);
}

static void test_jpeg(void)
{
    struct pocketcam_frame f;
    struct pocketcam_image_info info;
    struct pocketcam_photo_meta meta = { a_valid_time(), false };
    static uint16_t out[480 * 480];
    char p[PATH_MAX];
    char q[PATH_MAX];
    uint32_t w = 0;
    uint32_t h = 0;
    long size;
    clock_t t0;

    make_quad(&f, 80, 40);
    path_in(p, sizeof(p), "quad.jpg");
    check("a JPEG photo is written with EXIF", encode_to(p, &f, &meta) == 0);
    check("probed: JPEG, 80 x 40, upright, dated",
          pocketcam_image_probe(p, &info) == 0 && info.kind == POCKETCAM_IMAGE_JPEG &&
              info.width == 80 && info.height == 40 && info.exif.orientation == 1 &&
              strcmp(info.exif.taken, "2026:09:26 10:15:30") == 0 &&
              strncmp(info.exif.software, "Doors ", 6) == 0);
    check("decoded to fit 40 x 40",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, out, 40, 40, &w, &h, &info) == 0 &&
              w == 40 && h == 20 && !info.damaged);
    check("quadrants in place", colour(out[1 * 40 + 1]) == RED &&
                                    colour(out[1 * 40 + 38]) == GREEN &&
                                    colour(out[18 * 40 + 1]) == BLUE &&
                                    colour(out[18 * 40 + 38]) == WHITE);

    /* EXIF orientations: 6 turns right, 3 half-turns, 8 turns left. */
    path_in(q, sizeof(q), "quad-o6.jpg");
    with_orientation(p, q, 6);
    check("orientation 6: shown 40 x 80",
          pocketcam_image_probe(q, &info) == 0 && info.exif.orientation == 6 &&
              info.shown_w == 40 && info.shown_h == 80);
    check("decoded upright into 20 x 40",
          pocketcam_image_decode(q, POCKETCAM_FIT_CONTAIN, out, 40, 40, &w, &h, NULL) == 0 &&
              w == 20 && h == 40);
    check("stored top-left is shown top-right, bottom-left top-left",
          colour(out[1 * 20 + 18]) == RED && colour(out[1 * 20 + 1]) == BLUE &&
              colour(out[38 * 20 + 18]) == GREEN && colour(out[38 * 20 + 1]) == WHITE);
    path_in(q, sizeof(q), "quad-o3.jpg");
    with_orientation(p, q, 3);
    check("orientation 3: half-turned",
          pocketcam_image_decode(q, POCKETCAM_FIT_CONTAIN, out, 40, 40, &w, &h, NULL) == 0 &&
              w == 40 && h == 20 && colour(out[1 * 40 + 1]) == WHITE &&
              colour(out[18 * 40 + 38]) == RED);
    path_in(q, sizeof(q), "quad-o8.jpg");
    with_orientation(p, q, 8);
    check("orientation 8: turned left",
          pocketcam_image_decode(q, POCKETCAM_FIT_CONTAIN, out, 40, 40, &w, &h, NULL) == 0 &&
              w == 20 && h == 40 && colour(out[1 * 20 + 1]) == GREEN &&
              colour(out[38 * 20 + 1]) == RED);

    /* A camera-sized still, as a thumbnail and a screen picture. */
    make_quad(&f, 1920, 1080);
    path_in(p, sizeof(p), "big.jpg");
    encode_to(p, &f, &meta);
    t0 = clock();
    check("a 1920 x 1080 photo as a 170 x 170 thumbnail",
          pocketcam_image_decode(p, POCKETCAM_FIT_COVER, out, 170, 170, &w, &h, NULL) == 0 &&
              w == 170 && h == 170 && colour(out[0]) == RED && colour(out[169]) == GREEN &&
              colour(out[169 * 170]) == BLUE);
    printf("     thumbnail decode: %.1f ms on this host\n",
           (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    check("and as a 480 x 480 screen picture",
          pocketcam_image_decode(p, POCKETCAM_FIT_CONTAIN, out, 480, 480, &w, &h, NULL) == 0 &&
              w == 480 && h == 270 && colour(out[479]) == GREEN);

    /* Damage: cut in half, and garbage in the image data. */
    size = file_size(p);
    {
        uint8_t *all = malloc((size_t)size);
        FILE *fp = fopen(p, "rb");

        if (all && fp && fread(all, 1, (size_t)size, fp) == (size_t)size) {
            int r;

            path_in(q, sizeof(q), "big-cut.jpg");
            write_bytes(q, all, (size_t)size / 2);
            r = pocketcam_image_decode(q, POCKETCAM_FIT_CONTAIN, out, 160, 160, &w, &h, &info);
            check("a JPEG cut in half decodes what is there, marked damaged",
                  (r == 0 && info.damaged) || r == -EBADMSG);
            memset(all + size / 3, 0x5A, (size_t)size / 3);
            path_in(q, sizeof(q), "big-garbled.jpg");
            write_bytes(q, all, (size_t)size);
            r = pocketcam_image_decode(q, POCKETCAM_FIT_CONTAIN, out, 160, 160, &w, &h, &info);
            check("a garbled JPEG does not crash and is not clean",
                  (r == 0 && info.damaged) || r == -EBADMSG);
        }
        if (fp) {
            fclose(fp);
        }
        free(all);
    }
}
#endif

/* ---- the library ------------------------------------------------------------------ */

static int64_t tiny_disk(const char *dir)
{
    (void)dir;
    return 1000;
}

static void test_library(void)
{
    struct pocketcam_store s;
    char lib[256];
    char p[PATH_MAX];
    char dest[256];
    char out[PATH_MAX];
    char names[8][POCKETCAM_STORE_NAME_MAX];
    struct stat a;
    struct stat b;
    uint32_t total = 0;
    bool already = true;
    int n;
    char *home;

    path_in(lib, sizeof(lib), "library");
    check("the library opens", pocketcam_store_open(&s, lib) == 0);
    n = pocketcam_store_list(&s, names, 8, &total);
    check("an empty library lists nothing", n == 0 && total == 0);

    snprintf(p, sizeof(p), "%s/IMG_0001.ppm", lib);
    write_bytes(p, "P6\n1 1\n255\nabc", 14);
    snprintf(p, sizeof(p), "%s/IMG_20260101_101010_0002.ppm", lib);
    write_bytes(p, "P6\n1 1\n255\nabc", 14);
    snprintf(p, sizeof(p), "%s/IMG_0010.jpg", lib);
    write_bytes(p, "x", 1);
    snprintf(p, sizeof(p), "%s/IMG_0009.jpg", lib);
    write_bytes(p, "x", 1);
    snprintf(p, sizeof(p), "%s/notes.txt", lib);
    write_bytes(p, "x", 1);
    snprintf(p, sizeof(p), "%s/IMG_0003.ppm", lib);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/IMG_0004.jpg", lib);
    if (symlink("/etc/passwd", p) != 0) {
        printf("note: no symlink\n");
    }
    n = pocketcam_store_list(&s, names, 8, &total);
    check("only photos are listed: no other names, folders or links", n == 4 && total == 4);
    check("newest first, by the number the store gave, dated or not",
          n == 4 && strcmp(names[0], "IMG_0010.jpg") == 0 &&
              strcmp(names[1], "IMG_0009.jpg") == 0 &&
              strcmp(names[2], "IMG_20260101_101010_0002.ppm") == 0 &&
              strcmp(names[3], "IMG_0001.ppm") == 0);
    n = pocketcam_store_list(&s, names, 2, &total);
    check("a short list keeps the newest and counts the rest",
          n == 2 && total == 4 && strcmp(names[1], "IMG_0009.jpg") == 0);

    /* Export. */
    snprintf(dest, sizeof(dest), "%s/home/Pictures", root);
    check("a photo is exported", pocketcam_store_export(&s, "IMG_0001.ppm", dest, out,
                                                        sizeof(out), &already) == 0 &&
                                     !already);
    snprintf(p, sizeof(p), "%s/IMG_0001.ppm", lib);
    check("to the folder, whole, with the photo's own time",
          stat(out, &b) == 0 && stat(p, &a) == 0 && a.st_size == b.st_size &&
              a.st_mtime == b.st_mtime && strstr(out, "/home/Pictures/IMG_0001.ppm") != NULL);
    check("again: already there, nothing replaced",
          pocketcam_store_export(&s, "IMG_0001.ppm", dest, out, sizeof(out), &already) == 0 &&
              already);
    snprintf(p, sizeof(p), "%s/IMG_0010.jpg", dest);
    write_bytes(p, "someone else's", 14);
    check("a different file of that name is never replaced: the copy takes the next free name",
          pocketcam_store_export(&s, "IMG_0010.jpg", dest, out, sizeof(out), &already) == 0 &&
              !already && file_size(p) == 14 && strstr(out, "/IMG_0010-2.jpg") != NULL &&
              file_size(out) == 1);
    /* A photo number is used again once the newest photo is deleted, so the
     * same name and the same size can be another photo: only the bytes tell. */
    snprintf(p, sizeof(p), "%s/IMG_0009.jpg", dest);
    write_bytes(p, "y", 1);
    check("the same name and size with other bytes is another photo, and is kept",
          pocketcam_store_export(&s, "IMG_0009.jpg", dest, out, sizeof(out), &already) == 0 &&
              !already && strstr(out, "/IMG_0009-2.jpg") != NULL && file_size(p) == 1 &&
              first_byte(p) == 'y' && first_byte(out) == 'x');
    check("exported again, it is found under the name it got",
          pocketcam_store_export(&s, "IMG_0009.jpg", dest, out, sizeof(out), &already) == 0 &&
              already && strstr(out, "/IMG_0009-2.jpg") != NULL);
    snprintf(p, sizeof(p), "%s/IMG_0009-3.jpg", dest);
    check("and no third copy was made", access(p, F_OK) != 0);
    check("no temporary is left by any of it", export_temps(dest) == 0);
    check("a name that is not a photo is refused",
          pocketcam_store_export(&s, "../../etc/passwd", dest, out, sizeof(out), NULL) == -EINVAL);
    check("a photo that is not there is -ENOENT",
          pocketcam_store_export(&s, "IMG_0077.jpg", dest, out, sizeof(out), NULL) == -ENOENT);
    check("a folder named like a photo is not exported",
          pocketcam_store_export(&s, "IMG_0003.ppm", dest, out, sizeof(out), NULL) == -EINVAL);
    pocketcam_store_free_hook = tiny_disk;
    check("a full disk is -ENOSPC",
          pocketcam_store_export(&s, "IMG_20260101_101010_0002.ppm", dest, out, sizeof(out),
                                 NULL) == -ENOSPC);
    pocketcam_store_free_hook = NULL;
    snprintf(p, sizeof(p), "%s/IMG_20260101_101010_0002.ppm", dest);
    check("and leaves nothing behind", access(p, F_OK) != 0 && export_temps(dest) == 0);

    /* What an export killed part way leaves, and what the sweep leaves alone. */
    snprintf(p, sizeof(p), "%s/.IMG_0009.jpg.4242.export", dest);
    write_bytes(p, "half", 4);
    snprintf(p, sizeof(p), "%s/.IMG_0001.ppm.export", dest);
    write_bytes(p, "half", 4);
    snprintf(p, sizeof(p), "%s/.IMG_notes", dest);
    write_bytes(p, "mine", 4);
    pocketcam_export_sweep(dest);
    check("a killed export's temporaries are swept", export_temps(dest) == 0);
    check("and nothing else is", access(p, F_OK) == 0 && file_size(p) == 4);

    home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/someone", 1);
    pocketcam_export_default_dir(out, sizeof(out));
    check("the export folder is Pictures in the owner's home",
          strcmp(out, "/tmp/someone/Pictures") == 0);
    setenv("HOME", "relative", 1);
    pocketcam_export_default_dir(out, sizeof(out));
    check("a HOME that is not absolute falls back to /root", strcmp(out, "/root/Pictures") == 0);
    unsetenv("HOME");
    pocketcam_export_default_dir(out, sizeof(out));
    check("so does no HOME", strcmp(out, "/root/Pictures") == 0);
    if (home) {
        setenv("HOME", home, 1);
        free(home);
    }
}

int main(void)
{
    char cmd[200];

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(root, sizeof(root), "/tmp/pocketcam-gallery-%ld", (long)getpid());
    mkdir(root, 0755);
    test_exif();
    test_ppm_comments();
    test_refusals();
    test_fit_size();
    test_ppm_decode();
#ifdef POCKETCAM_HAVE_JPEG
    test_jpeg();
#else
    printf("     (no libjpeg in this build: the JPEG decode checks are skipped)\n");
#endif
    test_library();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("pocketcam_gallery_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
