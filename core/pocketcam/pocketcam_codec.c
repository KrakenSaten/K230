/*
 * pocketcam's still encoder. See pocketcam_codec.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam_codec.h"
#include "pocketcam_convert.h"

#include <errno.h>
#include <stdlib.h>

#ifdef POCKETCAM_HAVE_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#endif

/* The error a failed stream write left, as a negative errno. */
static int stream_error(FILE *fp)
{
    int e = errno;

    (void)fp;
    return -(e == ENOSPC || e == EDQUOT ? ENOSPC : e ? e : EIO);
}

#ifdef POCKETCAM_HAVE_JPEG

const char *pocketcam_codec_ext(void)
{
    return "jpg";
}

uint64_t pocketcam_codec_estimate(uint32_t w, uint32_t h)
{
    /* Generous: a baseline JPEG at quality 88 of a camera picture is far
     * below one byte a pixel; a checkerboard can reach it. */
    return (uint64_t)w * h + 65536;
}

/* libjpeg's default error handler calls exit(). A full card must end the
 * photo, not the helper. */
struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void on_jpeg_error(j_common_ptr cinfo)
{
    struct jerr *e = (struct jerr *)cinfo->err;

    longjmp(e->jump, 1);
}

static void on_jpeg_message(j_common_ptr cinfo)
{
    (void)cinfo; /* warnings are not for the helper's event stream */
}

int pocketcam_encode_meta(FILE *fp, const struct pocketcam_frame *f, int rotation, bool mirror,
                          const struct pocketcam_photo_meta *meta)
{
    struct jpeg_compress_struct cinfo;
    struct jerr err;
    uint32_t tw;
    uint32_t th;
    uint8_t *volatile row = NULL;

    if (pocketcam_frame_check(f) != 0) {
        return -EPROTO;
    }
    pocketcam_turned_size(f->width, f->height, rotation, &tw, &th);
    row = malloc((size_t)tw * 3);
    if (!row) {
        return -ENOMEM;
    }
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_jpeg_error;
    err.pub.output_message = on_jpeg_message;
    if (setjmp(err.jump)) {
        int r = stream_error(fp);

        jpeg_destroy_compress(&cinfo);
        free(row);
        return r;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = tw;
    cinfo.image_height = th;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, POCKETCAM_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    if (meta) {
        uint8_t exif[POCKETCAM_EXIF_MAX];
        size_t n = pocketcam_exif_build(meta, exif, sizeof(exif));

        if (n > 0) {
            jpeg_write_marker(&cinfo, JPEG_APP0 + 1, exif, (unsigned int)n);
        }
    }
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rows[1];
        int e = pocketcam_row_rgb888(f, rotation, mirror, cinfo.next_scanline, row);

        if (e != 0) {
            jpeg_abort_compress(&cinfo);
            jpeg_destroy_compress(&cinfo);
            free(row);
            return e;
        }
        rows[0] = row;
        jpeg_write_scanlines(&cinfo, rows, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    if (ferror(fp)) {
        return stream_error(fp);
    }
    return 0;
}

#else /* PPM */

const char *pocketcam_codec_ext(void)
{
    return "ppm";
}

uint64_t pocketcam_codec_estimate(uint32_t w, uint32_t h)
{
    return (uint64_t)w * h * 3 + 32;
}

int pocketcam_encode_meta(FILE *fp, const struct pocketcam_frame *f, int rotation, bool mirror,
                          const struct pocketcam_photo_meta *meta)
{
    char comments[256];
    uint32_t tw;
    uint32_t th;
    uint32_t y;
    uint8_t *row;
    int r = pocketcam_frame_check(f);

    if (r != 0) {
        return r;
    }
    pocketcam_turned_size(f->width, f->height, rotation, &tw, &th);
    row = malloc((size_t)tw * 3);
    if (!row) {
        return -ENOMEM;
    }
    errno = 0;
    if (meta) {
        pocketcam_exif_ppm_comments(meta, comments, sizeof(comments));
    } else {
        comments[0] = '\0';
    }
    if (fprintf(fp, "P6\n%s%u %u\n255\n", comments, tw, th) < 0) {
        free(row);
        return stream_error(fp);
    }
    for (y = 0; y < th; y++) {
        r = pocketcam_row_rgb888(f, rotation, mirror, y, row);
        if (r != 0) {
            break;
        }
        if (fwrite(row, 3, tw, fp) != tw) {
            r = stream_error(fp);
            break;
        }
    }
    free(row);
    return r;
}

#endif

int pocketcam_encode(FILE *fp, const struct pocketcam_frame *f, int rotation, bool mirror)
{
    return pocketcam_encode_meta(fp, f, rotation, mirror, NULL);
}
