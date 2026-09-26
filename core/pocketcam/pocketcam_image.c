/*
 * pocketcam's image reader. See pocketcam_image.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef POCKETCAM_HAVE_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#endif

/* A JPEG's markers before its image data are read up to this many. */
#define JPEG_MARKERS_MAX 256
/* The EXIF block is read up to this size (a marker segment is at most 64 KB). */
#define EXIF_READ_MAX 65535
/* A PPM header, comments included, is at most this long. */
#define PPM_HEADER_MAX 4096
/* Decoding a progressive JPEG holds all its coefficients: about three bytes a
 * pixel. This many pixels is the most that is decoded at all. */
#define IMAGE_MAX_PIXELS (16u * 1024u * 1024u)

const char *pocketcam_image_error_word(int err)
{
    switch (err) {
    case -ENOENT: return "missing";
    case -EBADMSG: return "corrupt";
    case -ENOTSUP: return "unsupported";
    case -EFBIG: return "toolarge";
    default: return "io";
    }
}

static int errno_neg(void)
{
    return errno ? -errno : -EIO;
}

static void shown_size(struct pocketcam_image_info *info)
{
    if (info->exif.orientation >= 5) {
        info->shown_w = info->height;
        info->shown_h = info->width;
    } else {
        info->shown_w = info->width;
        info->shown_h = info->height;
    }
}

static int check_size(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) {
        return -EBADMSG;
    }
    if (w > POCKETCAM_IMAGE_MAX_DIM || h > POCKETCAM_IMAGE_MAX_DIM ||
        (uint64_t)w * h > IMAGE_MAX_PIXELS) {
        return -EFBIG;
    }
    return 0;
}

/* ---- JPEG markers ------------------------------------------------------------- */

static bool is_sof(int m)
{
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

static int probe_jpeg(FILE *fp, struct pocketcam_image_info *info)
{
    bool have_sof = false;
    bool have_exif = false;
    int markers;

    for (markers = 0; markers < JPEG_MARKERS_MAX; markers++) {
        int c = getc(fp);
        int m;
        int hi;
        int lo;
        long len;

        if (c == EOF) {
            break;
        }
        if (c != 0xFF) {
            return -EBADMSG;
        }
        do {
            m = getc(fp);
        } while (m == 0xFF);
        if (m == EOF) {
            break;
        }
        if (m == 0xD9 || m == 0xDA) {
            break; /* the end, or the image data: no more markers to read */
        }
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            continue; /* no length */
        }
        hi = getc(fp);
        lo = getc(fp);
        if (hi == EOF || lo == EOF) {
            return -EBADMSG;
        }
        len = (long)(hi << 8 | lo) - 2;
        if (len < 0) {
            return -EBADMSG;
        }
        if (is_sof(m) && !have_sof) {
            uint8_t s[5];

            if (len < 5 || fread(s, 1, 5, fp) != 5) {
                return -EBADMSG;
            }
            info->height = (uint32_t)(s[1] << 8 | s[2]);
            info->width = (uint32_t)(s[3] << 8 | s[4]);
            have_sof = true;
            len -= 5;
        } else if (m == 0xE1 && !have_exif && len >= 14) {
            uint8_t *b = malloc((size_t)len);

            if (!b) {
                return -ENOMEM;
            }
            if (fread(b, 1, (size_t)len, fp) != (size_t)len) {
                free(b);
                return -EBADMSG;
            }
            have_exif = pocketcam_exif_parse(b, (size_t)len, &info->exif) == 0;
            if (!have_exif) {
                pocketcam_exif_clear(&info->exif);
            }
            free(b);
            len = 0;
        }
        if (len > 0 && fseek(fp, len, SEEK_CUR) != 0) {
            return -EBADMSG;
        }
    }
    return have_sof ? 0 : -EBADMSG;
}

/* ---- PPM header --------------------------------------------------------------- */

/* The next number of a PPM header, reading comments (ours are metadata) on
 * the way. -1 when there is none. */
static long ppm_number(FILE *fp, struct pocketcam_image_info *info, long *used)
{
    long v = -1;
    int c;

    for (;;) {
        c = getc(fp);
        if (++*used > PPM_HEADER_MAX || c == EOF) {
            return -1;
        }
        if (c == '#') {
            char line[128];
            size_t n = 0;

            while ((c = getc(fp)) != EOF && c != '\n') {
                if (++*used > PPM_HEADER_MAX) {
                    return -1;
                }
                if (n + 1 < sizeof(line)) {
                    line[n++] = (char)c;
                }
            }
            line[n] = '\0';
            pocketcam_exif_ppm_parse_comment(line, &info->exif);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        break;
    }
    while (c >= '0' && c <= '9') {
        v = (v < 0 ? 0 : v) * 10 + (c - '0');
        if (v > 1000000 || ++*used > PPM_HEADER_MAX) {
            return -1;
        }
        c = getc(fp);
    }
    /* Exactly one whitespace byte ends a number (the last one before the
     * pixels included); anything else is not a PPM we read. */
    if (v < 0 || !(c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
        return -1;
    }
    return v;
}

static int probe_ppm(FILE *fp, struct pocketcam_image_info *info)
{
    long used = 2;
    long w;
    long h;
    long max;

    w = ppm_number(fp, info, &used);
    h = w >= 0 ? ppm_number(fp, info, &used) : -1;
    max = h >= 0 ? ppm_number(fp, info, &used) : -1;
    if (w <= 0 || h <= 0 || max <= 0) {
        return -EBADMSG;
    }
    if (max > 255) {
        return -ENOTSUP; /* 16-bit PPM: never written here */
    }
    info->width = (uint32_t)w;
    info->height = (uint32_t)h;
    return 0;
}

/* Open and probe; on success fp is positioned after the header (PPM) or at
 * the start (JPEG). */
static int open_probe(const char *path, FILE **out, struct pocketcam_image_info *info)
{
    struct stat st;
    uint8_t magic[2];
    FILE *fp;
    int r;

    memset(info, 0, sizeof(*info));
    pocketcam_exif_clear(&info->exif);
    *out = NULL;
    errno = 0;
    fp = fopen(path, "rbe");
    if (!fp) {
        return errno == ENOENT ? -ENOENT : errno_neg();
    }
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        return -ENOTSUP;
    }
    info->bytes = (uint64_t)st.st_size;
    info->mtime = (int64_t)st.st_mtime;
    if (fread(magic, 1, 2, fp) != 2) {
        fclose(fp);
        return -EBADMSG;
    }
    if (magic[0] == 0xFF && magic[1] == 0xD8) {
        info->kind = POCKETCAM_IMAGE_JPEG;
        r = probe_jpeg(fp, info);
    } else if (magic[0] == 'P' && magic[1] == '6') {
        info->kind = POCKETCAM_IMAGE_PPM;
        r = probe_ppm(fp, info);
    } else {
        r = -ENOTSUP;
    }
    if (r == 0) {
        r = check_size(info->width, info->height);
    }
    if (r != 0) {
        fclose(fp);
        return r;
    }
    shown_size(info);
    *out = fp;
    return 0;
}

int pocketcam_image_probe(const char *path, struct pocketcam_image_info *info)
{
    struct pocketcam_image_info local;
    FILE *fp;
    int r = open_probe(path, &fp, info ? info : &local);

    if (fp) {
        fclose(fp);
    }
    return r;
}

void pocketcam_image_fit_size(uint32_t shown_w, uint32_t shown_h, enum pocketcam_fit fit,
                              uint32_t box_w, uint32_t box_h, uint32_t *out_w, uint32_t *out_h)
{
    uint64_t w = box_w;
    uint64_t h = box_h;

    if (fit == POCKETCAM_FIT_CONTAIN && shown_w && shown_h) {
        if ((uint64_t)shown_w * box_h > (uint64_t)shown_h * box_w) {
            h = (uint64_t)shown_h * box_w / shown_w;
        } else {
            w = (uint64_t)shown_w * box_h / shown_h;
        }
    }
    *out_w = w ? (uint32_t)w : 1;
    *out_h = h ? (uint32_t)h : 1;
}

/* ---- streaming into the destination -------------------------------------------
 *
 * Every output pixel comes from one source pixel. For an orientation that
 * does not swap the axes (1..4) the source column depends only on the output
 * column and the source row only on the output row; for one that does (5..8)
 * the source row depends only on the output column and the source column
 * only on the output row. So two small tables say everything, and each
 * source line, as it is decoded, is written to the output lines (or columns)
 * that sample it. */

struct plan {
    uint32_t src_w;      /* the decoded picture, as stored */
    uint32_t src_h;
    uint32_t out_w;
    uint32_t out_h;
    bool swap;           /* orientation 5..8 */
    uint32_t *by_col;    /* out_w entries: source column (no swap) or row (swap) */
    uint32_t *by_row;    /* out_h entries: source row (no swap) or column (swap) */
    uint16_t *dst;
};

static void plan_free(struct plan *p)
{
    free(p->by_col);
    free(p->by_row);
    p->by_col = p->by_row = NULL;
}

/* The source coordinate for shown coordinate v along an axis of n pixels,
 * reversed when asked. */
static uint32_t along(uint32_t v, uint32_t n, bool reverse)
{
    return reverse ? n - 1 - v : v;
}

static int plan_make(struct plan *p, uint32_t src_w, uint32_t src_h, int orientation,
                     enum pocketcam_fit fit, uint32_t out_w, uint32_t out_h, uint16_t *dst)
{
    bool swap = orientation >= 5;
    uint32_t sw = swap ? src_h : src_w; /* shown */
    uint32_t sh = swap ? src_w : src_h;
    uint64_t cw = sw;                   /* the part of the shown picture used */
    uint64_t ch = sh;
    uint64_t ox = 0;
    uint64_t oy = 0;
    /* Which shown axis runs backwards through the stored one. */
    bool rev_x;
    bool rev_y;
    uint32_t i;

    memset(p, 0, sizeof(*p));
    p->src_w = src_w;
    p->src_h = src_h;
    p->out_w = out_w;
    p->out_h = out_h;
    p->swap = swap;
    p->dst = dst;
    if (fit == POCKETCAM_FIT_COVER) {
        if ((uint64_t)sw * out_h > (uint64_t)sh * out_w) {
            cw = (uint64_t)sh * out_w / out_h;
            ox = (sw - cw) / 2;
        } else {
            ch = (uint64_t)sw * out_h / out_w;
            oy = (sh - ch) / 2;
        }
        cw = cw ? cw : 1;
        ch = ch ? ch : 1;
    }
    p->by_col = malloc(sizeof(uint32_t) * out_w);
    p->by_row = malloc(sizeof(uint32_t) * out_h);
    if (!p->by_col || !p->by_row) {
        plan_free(p);
        return -ENOMEM;
    }
    /* EXIF orientation: 1 as is, 2 mirrored, 3 half-turned, 4 flipped,
     * 5 transposed, 6 turned right, 7 transversed, 8 turned left. */
    switch (orientation) {
    case 2: rev_x = true; rev_y = false; break;
    case 3: rev_x = true; rev_y = true; break;
    case 4: rev_x = false; rev_y = true; break;
    case 5: rev_x = false; rev_y = false; break;
    case 6: rev_x = true; rev_y = false; break;
    case 7: rev_x = true; rev_y = true; break;
    case 8: rev_x = false; rev_y = true; break;
    default: rev_x = false; rev_y = false; break;
    }
    for (i = 0; i < out_w; i++) {
        uint32_t x = (uint32_t)(ox + ((uint64_t)2 * i + 1) * cw / (2 * (uint64_t)out_w));

        /* Shown x is the stored x (no swap), or the stored y (swap). */
        p->by_col[i] = along(x < sw ? x : sw - 1, sw, rev_x);
    }
    for (i = 0; i < out_h; i++) {
        uint32_t y = (uint32_t)(oy + ((uint64_t)2 * i + 1) * ch / (2 * (uint64_t)out_h));

        p->by_row[i] = along(y < sh ? y : sh - 1, sh, rev_y);
    }
    return 0;
}

static uint16_t rgb565(const uint8_t *px)
{
    return (uint16_t)((px[0] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[2] >> 3);
}

/* Source line y (RGB888, src_w pixels) into every output pixel that samples it. */
static void plan_line(const struct plan *p, uint32_t y, const uint8_t *line)
{
    uint32_t i;
    uint32_t j;

    if (!p->swap) {
        for (j = 0; j < p->out_h; j++) {
            if (p->by_row[j] != y) {
                continue;
            }
            for (i = 0; i < p->out_w; i++) {
                p->dst[(size_t)j * p->out_w + i] = rgb565(line + (size_t)p->by_col[i] * 3);
            }
        }
        return;
    }
    for (i = 0; i < p->out_w; i++) {
        if (p->by_col[i] != y) {
            continue;
        }
        for (j = 0; j < p->out_h; j++) {
            p->dst[(size_t)j * p->out_w + i] = rgb565(line + (size_t)p->by_row[j] * 3);
        }
    }
}

/* ---- PPM ---------------------------------------------------------------------- */

static int decode_ppm(FILE *fp, struct pocketcam_image_info *info, const struct plan *p)
{
    uint8_t *line = malloc((size_t)info->width * 3);
    uint32_t y;

    if (!line) {
        return -ENOMEM;
    }
    for (y = 0; y < info->height; y++) {
        if (fread(line, 3, info->width, fp) != info->width) {
            info->damaged = true; /* cut short: what is missing stays black */
            break;
        }
        plan_line(p, y, line);
    }
    free(line);
    return 0;
}

/* ---- JPEG --------------------------------------------------------------------- */

#ifdef POCKETCAM_HAVE_JPEG

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    bool warned;
};

static void on_jpeg_error(j_common_ptr cinfo)
{
    longjmp(((struct jerr *)cinfo->err)->jump, 1);
}

static void on_jpeg_message(j_common_ptr cinfo, int level)
{
    /* level -1 is a warning: damaged data libjpeg recovered from. */
    if (level < 0) {
        ((struct jerr *)cinfo->err)->warned = true;
    }
}

/* The smallest DCT scale n/8 whose picture still covers what is needed, so
 * nothing is magnified that a larger scale would have given. */
static unsigned pick_scale(const struct pocketcam_image_info *info, enum pocketcam_fit fit,
                           uint32_t out_w, uint32_t out_h)
{
    unsigned n;

    for (n = 1; n < 8; n++) {
        uint64_t w = ((uint64_t)info->width * n + 7) / 8;
        uint64_t h = ((uint64_t)info->height * n + 7) / 8;
        uint64_t sw = info->exif.orientation >= 5 ? h : w;
        uint64_t sh = info->exif.orientation >= 5 ? w : h;
        uint64_t cw = sw;
        uint64_t ch = sh;

        if (fit == POCKETCAM_FIT_COVER) {
            if (sw * out_h > sh * out_w) {
                cw = sh * out_w / out_h;
            } else {
                ch = sw * out_h / out_w;
            }
        }
        if (cw >= out_w && ch >= out_h) {
            break;
        }
    }
    return n;
}

static int decode_jpeg(FILE *fp, struct pocketcam_image_info *info, enum pocketcam_fit fit,
                       uint16_t *dst, uint32_t out_w, uint32_t out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    struct plan plan;
    uint8_t *volatile line = NULL;
    volatile bool planned = false;
    int r;

    memset(&plan, 0, sizeof(plan));
    rewind(fp);
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_jpeg_error;
    err.pub.emit_message = on_jpeg_message;
    err.warned = false;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        free(line);
        if (planned) {
            plan_free(&plan);
        }
        return -EBADMSG;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    cinfo.scale_num = pick_scale(info, fit, out_w, out_h);
    cinfo.scale_denom = 8;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_calc_output_dimensions(&cinfo);
    if (cinfo.output_components != 3 || cinfo.output_width == 0 || cinfo.output_height == 0) {
        jpeg_destroy_decompress(&cinfo);
        return -ENOTSUP;
    }
    r = plan_make(&plan, cinfo.output_width, cinfo.output_height, info->exif.orientation, fit,
                  out_w, out_h, dst);
    if (r != 0) {
        jpeg_destroy_decompress(&cinfo);
        return r;
    }
    planned = true;
    line = malloc((size_t)cinfo.output_width * 3);
    if (!line) {
        jpeg_destroy_decompress(&cinfo);
        plan_free(&plan);
        return -ENOMEM;
    }
    jpeg_start_decompress(&cinfo);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rows[1] = { line };
        JDIMENSION y = cinfo.output_scanline;

        if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) {
            break;
        }
        plan_line(&plan, y, line);
    }
    jpeg_finish_decompress(&cinfo);
    info->damaged = err.warned;
    jpeg_destroy_decompress(&cinfo);
    free(line);
    plan_free(&plan);
    return 0;
}

#else

static int decode_jpeg(FILE *fp, struct pocketcam_image_info *info, enum pocketcam_fit fit,
                       uint16_t *dst, uint32_t out_w, uint32_t out_h)
{
    (void)fp;
    (void)info;
    (void)fit;
    (void)dst;
    (void)out_w;
    (void)out_h;
    return -ENOTSUP; /* no decoder in this build; the header was still read */
}

#endif

int pocketcam_image_decode(const char *path, enum pocketcam_fit fit, uint16_t *dst, uint32_t box_w,
                           uint32_t box_h, uint32_t *out_w, uint32_t *out_h,
                           struct pocketcam_image_info *info)
{
    struct pocketcam_image_info local;
    struct pocketcam_image_info *in = info ? info : &local;
    uint32_t w;
    uint32_t h;
    FILE *fp;
    int r;

    if (!dst || box_w == 0 || box_h == 0 || box_w > POCKETCAM_IMAGE_MAX_DIM ||
        box_h > POCKETCAM_IMAGE_MAX_DIM) {
        return -EINVAL;
    }
    r = open_probe(path, &fp, in);
    if (r != 0) {
        return r;
    }
    pocketcam_image_fit_size(in->shown_w, in->shown_h, fit, box_w, box_h, &w, &h);
    memset(dst, 0, (size_t)w * h * sizeof(uint16_t));
    if (in->kind == POCKETCAM_IMAGE_JPEG) {
        r = decode_jpeg(fp, in, fit, dst, w, h);
    } else {
        struct plan plan;

        r = plan_make(&plan, in->width, in->height, in->exif.orientation, fit, w, h, dst);
        if (r == 0) {
            r = decode_ppm(fp, in, &plan);
            plan_free(&plan);
        }
    }
    fclose(fp);
    if (r == 0) {
        if (out_w) {
            *out_w = w;
        }
        if (out_h) {
            *out_h = h;
        }
    }
    return r;
}
