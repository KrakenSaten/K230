/*
 * core/pocketcam on a host: frame validation, the fake backend and every
 * fault its script can raise, the v4l2 backend on a host that has no camera,
 * its refusal of bad settings, pixel conversion in all
 * four turns (checked against the fake's own pattern, not hard-coded bytes),
 * the encoder this build has, and the photo store - names, atomic writes, the
 * limits, a full disk and a disk that fills up mid-photo.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketcam/pocketcam.h"
#include "pocketcam/pocketcam_codec.h"
#include "pocketcam/pocketcam_convert.h"
#include "pocketcam/pocketcam_fake.h"
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

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- frames ------------------------------------------------------------------ */

static void test_frame_check(void)
{
    static uint8_t buf[64 * 36 * 2];
    struct pocketcam_frame f = { POCKETCAM_FMT_NV16, 64, 36, 64, buf, sizeof(buf), 1, 0, 0 };

    check("a whole NV16 frame is valid", pocketcam_frame_check(&f) == 0);
    f.bytes = sizeof(buf) - 1;
    check("one byte short is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.bytes = sizeof(buf);
    f.width = 63;
    check("an odd width is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.width = 64;
    f.stride = 32;
    check("a stride shorter than a line is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.stride = 64;
    f.height = 5000;
    check("a height past the limit is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.height = 36;
    f.format = POCKETCAM_FMT_NV12;
    check("the same buffer holds an NV12 frame", pocketcam_frame_check(&f) == 0);
    check("NV12 needs one and a half planes",
          pocketcam_frame_bytes(POCKETCAM_FMT_NV12, 64, 36, 64) == 64 * 54);
    f.height = 35;
    check("an odd NV12 height is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.height = 36;
    f.data = NULL;
    check("no data is refused", pocketcam_frame_check(&f) == -EPROTO);
    f.data = buf;
    f.format = (enum pocketcam_format)99;
    check("an unknown format is refused", pocketcam_frame_check(&f) == -EPROTO);
}

/* ---- the fake backend ---------------------------------------------------------- */

static void test_fake(void)
{
    struct pocketcam_backend b;
    struct pocketcam_backend b2;
    struct pocketcam_info info;
    struct pocketcam_frame f;
    struct pocketcam_frame f2;
    struct pocketcam_frame f3;
    int64_t t0;
    int r;

    /* The real backend on a host: no camera nodes, so no camera. */
    check("v4l2 on a host without the nodes: no camera",
          access("/dev/video1", F_OK) == 0 || pocketcam_open(&b, "v4l2", NULL, &info) == -ENODEV);
    check("v4l2 refuses unknown settings", pocketcam_open(&b, "v4l2", "zoom=2", &info) == -EINVAL);
    check("v4l2 refuses a size the ISP cannot align",
          pocketcam_open(&b, "v4l2", "size=641x360", &info) == -EINVAL);
    check("v4l2 refuses a node outside /dev",
          pocketcam_open(&b, "v4l2", "preview=/tmp/x", &info) == -EINVAL);
    check("an unknown backend is refused", pocketcam_open(&b, "webcam", NULL, &info) == -ENOTSUP);
    check("a misspelt script key is refused",
          pocketcam_open(&b, "fake", "frmaes=3", &info) == -EINVAL);
    check("an odd size is refused", pocketcam_open(&b, "fake", "size=65x36", &info) == -EINVAL);
    check("open=nodev is no camera", pocketcam_open(&b, "fake", "open=nodev", &info) == -ENODEV);
    check("open=busy is busy", pocketcam_open(&b, "fake", "open=busy", &info) == -EBUSY);
    check("open=fail is an I/O error", pocketcam_open(&b, "fake", "open=fail", &info) == -EIO);

    r = pocketcam_open(&b, "fake", "size=64x36,still=128x72,period=5,mount=270m", &info);
    check("the fake opens", r == 0);
    check("it says it is simulated", info.simulated);
    check("with the scripted sizes", info.preview_w == 64 && info.preview_h == 36 &&
                                         info.still_w == 128 && info.still_h == 72);
    check("and the scripted mount", info.mount_rotation == 270 && info.mount_mirror);
    check("no frame before start", pocketcam_next(&b, 10, &f) == -EINVAL);
    check("start", pocketcam_start(&b) == 0);
    check("a frame", pocketcam_next(&b, 200, &f) == 0);
    check("frame 1, NV16, the preview size",
          f.seq == 1 && f.format == POCKETCAM_FMT_NV16 && f.width == 64 && f.height == 36);
    check("a second frame while the first is held", pocketcam_next(&b, 200, &f2) == 0);
    check("no third while both are held", pocketcam_next(&b, 200, &f3) == -EIO);
    pocketcam_release(&b, &f);
    pocketcam_release(&b, &f2);
    check("after release frames come again", pocketcam_next(&b, 200, &f) == 0 && f.seq == 3);
    {
        /* Deterministic: the same frame number is the same bytes. */
        uint8_t *expect = malloc(f.bytes);

        pocketcam_fake_fill(expect, POCKETCAM_FMT_NV16, 64, 36, 3);
        check("frame 3 is exactly the pattern for 3", memcmp(expect, f.data, f.bytes) == 0);
        pocketcam_fake_fill(expect, POCKETCAM_FMT_NV16, 64, 36, 4);
        check("and differs from frame 4", memcmp(expect, f.data, f.bytes) != 0);
        free(expect);
    }
    pocketcam_release(&b, &f);
    check("a still", pocketcam_still(&b, 1000, &f) == 0);
    check("at the still size", f.width == 128 && f.height == 72);
    check("a still stops the stream", !b.streaming);
    pocketcam_release(&b, &f);
    pocketcam_close(&b);
    check("close forgets the backend", b.ops == NULL && b.priv == NULL);

    /* The faults. */
    pocketcam_open(&b, "fake", "size=64x36,period=1,frames=2", &info);
    pocketcam_start(&b);
    pocketcam_next(&b, 100, &f);
    pocketcam_release(&b, &f);
    pocketcam_next(&b, 100, &f);
    pocketcam_release(&b, &f);
    t0 = now_ms();
    check("frames=2: the third never comes", pocketcam_next(&b, 60, &f) == -ETIMEDOUT);
    check("and the wait is bounded", now_ms() - t0 < 1000);
    pocketcam_close(&b);

    pocketcam_open(&b, "fake", "size=64x36,period=1,lost_after=1", &info);
    pocketcam_start(&b);
    pocketcam_next(&b, 100, &f);
    pocketcam_release(&b, &f);
    check("lost_after=1: then the camera is gone", pocketcam_next(&b, 100, &f) == -ENODEV);
    pocketcam_close(&b);

    pocketcam_open(&b, "fake", "size=64x36,period=1,malformed_at=2", &info);
    pocketcam_start(&b);
    pocketcam_next(&b, 100, &f);
    pocketcam_release(&b, &f);
    check("malformed_at=2: frame 2 is refused, not handed out",
          pocketcam_next(&b, 100, &f) == -EPROTO);
    check("and frame 3 is fine again", pocketcam_next(&b, 100, &f) == 0 && f.seq == 3);
    pocketcam_release(&b, &f);
    pocketcam_close(&b);

    pocketcam_open(&b, "fake", "size=64x36,period=1,delay_at=1:300", &info);
    pocketcam_start(&b);
    check("delay_at=1:300: not within 50 ms", pocketcam_next(&b, 50, &f) == -ETIMEDOUT);
    check("but within the next 500", pocketcam_next(&b, 500, &f) == 0 && f.seq == 1);
    pocketcam_release(&b, &f);
    pocketcam_close(&b);

    pocketcam_open(&b, "fake", "capture=fail", &info);
    check("capture=fail", pocketcam_still(&b, 100, &f) == -EIO);
    pocketcam_close(&b);
    pocketcam_open(&b, "fake", "capture=lost", &info);
    check("capture=lost", pocketcam_still(&b, 100, &f) == -ENODEV);
    pocketcam_close(&b);
    pocketcam_open(&b, "fake", "capture_delay=500", &info);
    check("a still slower than its bound times out", pocketcam_still(&b, 50, &f) == -ETIMEDOUT);
    pocketcam_close(&b);

    /* Two fakes at once do not share buffers. */
    pocketcam_open(&b, "fake", "size=64x36,period=1", &info);
    pocketcam_open(&b2, "fake", "size=32x18,period=1", &info);
    pocketcam_start(&b);
    pocketcam_start(&b2);
    check("two fakes run side by side",
          pocketcam_next(&b, 100, &f) == 0 && pocketcam_next(&b2, 100, &f2) == 0 &&
              f.data != f2.data && f.width == 64 && f2.width == 32);
    pocketcam_release(&b, &f);
    pocketcam_release(&b2, &f2);
    pocketcam_close(&b);
    pocketcam_close(&b2);
}

/* ---- conversion ---------------------------------------------------------------- */

static void rgb_of(uint16_t px, int *r, int *g, int *b)
{
    *r = ((px >> 11) & 0x1f) << 3;
    *g = ((px >> 5) & 0x3f) << 2;
    *b = (px & 0x1f) << 3;
}

static int is_orange(uint16_t px)
{
    int r;
    int g;
    int b;

    rgb_of(px, &r, &g, &b);
    return r > 215 && g > 100 && g < 160 && b < 40;
}

static int near(uint16_t px, const uint8_t rgb[3])
{
    int r;
    int g;
    int b;

    rgb_of(px, &r, &g, &b);
    return abs(r - rgb[0]) < 24 && abs(g - rgb[1]) < 24 && abs(b - rgb[2]) < 24;
}

static void test_convert(void)
{
    enum { W = 64, H = 36 };
    static uint8_t nv16[W * H * 2];
    static uint8_t nv12[W * H * 3 / 2];
    struct pocketcam_frame f = { POCKETCAM_FMT_NV16, W, H, W, nv16, sizeof(nv16), 1, 0, 0 };
    struct pocketcam_frame g = { POCKETCAM_FMT_NV12, W, H, W, nv12, sizeof(nv12), 1, 0, 0 };
    static uint16_t out[W * W];
    static uint8_t row[W * 3];
    uint32_t tw;
    uint32_t th;
    uint8_t rgb[3];
    int ok;
    uint32_t x;

    pocketcam_fake_fill(nv16, POCKETCAM_FMT_NV16, W, H, 1);
    pocketcam_fake_fill(nv12, POCKETCAM_FMT_NV12, W, H, 1);

    check("unturned: the marker is top-left",
          pocketcam_to_rgb565(&f, 0, false, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
              is_orange(out[1 * W + 1]) && !is_orange(out[1 * W + W - 2]));
    ok = 1;
    for (x = 0; x < 8; x++) {
        pocketcam_fake_rgb_at(x * 8 + 4, H - 4, 1, W, H, rgb);
        ok &= near(out[(H - 4) * W + x * 8 + 4], rgb);
    }
    check("every bar comes out its own colour", ok);
    check("mirrored: the marker is top-right",
          pocketcam_to_rgb565(&f, 0, true, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
              is_orange(out[1 * W + W - 2]) && !is_orange(out[1 * W + 1]));
    /* A quarter turn clockwise puts the sensor's top-left at the top-right. */
    check("90: the marker is top-right of a tall picture",
          pocketcam_to_rgb565(&f, 90, false, POCKETCAM_FIT_COVER, out, H, W, H) == 0 &&
              is_orange(out[1 * H + H - 2]) && !is_orange(out[1 * H + 1]));
    check("180: bottom-right",
          pocketcam_to_rgb565(&f, 180, false, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
              is_orange(out[(H - 2) * W + W - 2]) && !is_orange(out[1 * W + 1]));
    check("270: bottom-left",
          pocketcam_to_rgb565(&f, 270, false, POCKETCAM_FIT_COVER, out, H, W, H) == 0 &&
              is_orange(out[(W - 2) * H + 1]) && !is_orange(out[1 * H + H - 2]));
    check("NV12 gives the same picture",
          pocketcam_to_rgb565(&g, 0, false, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
              is_orange(out[1 * W + 1]));
    {
        /* The ISP's BG3P (what the Vision helper reads): three planes, and
         * the picture exactly the pattern's colours. */
        static uint8_t bgr[W * H * 3];
        struct pocketcam_frame p = { POCKETCAM_FMT_BG3P, W, H, W, bgr, sizeof(bgr), 1, 0, 0 };

        /* The plane order on its own, not through the fake (which could be
         * wrong the same way): plane 0 full and the rest empty is RED. On
         * unit B a red blanket came out blue while this was read as B. */
        memset(bgr, 0, sizeof(bgr));
        memset(bgr, 255, (size_t)W * H);
        check("BG3P plane 0 is red, as the K230 ISP delivers it",
              pocketcam_to_rgb565(&p, 0, false, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
                  out[W * H / 2] == 0xf800);
        pocketcam_fake_fill(bgr, POCKETCAM_FMT_BG3P, W, H, 1);
        check("BG3P needs three planes",
              pocketcam_frame_bytes(POCKETCAM_FMT_BG3P, W, H, W) == W * H * 3 &&
                  pocketcam_frame_check(&p) == 0);
        ok = pocketcam_to_rgb565(&p, 0, false, POCKETCAM_FIT_COVER, out, W, H, W) == 0 &&
             is_orange(out[1 * W + 1]);
        for (x = 0; x < 8; x++) {
            pocketcam_fake_rgb_at(x * 8 + 4, H - 4, 1, W, H, rgb);
            ok &= near(out[(H - 4) * W + x * 8 + 4], rgb);
        }
        check("BG3P gives the same picture, every bar its own colour", ok);
        p.bytes = W * H * 3 - 1;
        check("a planar frame one byte short is refused", pocketcam_frame_check(&p) == -EPROTO);
    }
    /* Contain into a square: black above and below, the picture between. */
    check("contain leaves black bars",
          pocketcam_to_rgb565(&f, 0, false, POCKETCAM_FIT_CONTAIN, out, W, W, W) == 0 &&
              out[0] == 0 && out[(W - 1) * W] == 0 && out[W / 2 * W + W / 2] != 0);
    /* Cover into a square: the middle of the picture, scaled up. */
    check("cover fills a different shape",
          pocketcam_to_rgb565(&f, 0, false, POCKETCAM_FIT_COVER, out, W, W, W) == 0 &&
              out[0] != 0 && out[(W - 1) * W] != 0);
    check("downscaling to a few pixels works",
          pocketcam_to_rgb565(&f, 90, true, POCKETCAM_FIT_COVER, out, 3, 5, 3) == 0);
    check("a bad rotation is refused",
          pocketcam_to_rgb565(&f, 45, false, POCKETCAM_FIT_COVER, out, W, H, W) == -EINVAL);
    check("a stride under the width is refused",
          pocketcam_to_rgb565(&f, 0, false, POCKETCAM_FIT_COVER, out, W, H, W - 1) == -EINVAL);
    f.bytes = 100;
    check("a short frame is refused before a pixel is read",
          pocketcam_to_rgb565(&f, 0, false, POCKETCAM_FIT_COVER, out, W, H, W) == -EPROTO);
    check("and by the line converter",
          pocketcam_row_rgb888(&f, 0, false, 0, row) == -EPROTO);
    f.bytes = sizeof(nv16);
    pocketcam_turned_size(W, H, 90, &tw, &th);
    check("a turned picture swaps its sides", tw == H && th == W);
    check("line 0 turned 90 starts... at the sensor's bottom-left",
          pocketcam_row_rgb888(&f, 90, false, 0, row) == 0);
    pocketcam_fake_rgb_at(0, H - 1, 1, W, H, rgb);
    check("which is that pixel's colour",
          abs(row[0] - rgb[0]) < 24 && abs(row[1] - rgb[1]) < 24 && abs(row[2] - rgb[2]) < 24);
    check("a line past the end is refused", pocketcam_row_rgb888(&f, 90, false, W, row) == -EINVAL);
    check("the display's rotation is taken off the mount (unit A: 90 -> 90 and 180)",
          pocketcam_view_rotation(90, 0) == 90 && pocketcam_view_rotation(90, 270) == 180 &&
              pocketcam_view_rotation(90, 90) == 0 && pocketcam_view_rotation(0, 90) == 270);
}

/* ---- the encoder ---------------------------------------------------------------- */

static void test_codec(void)
{
    enum { W = 64, H = 36 };
    static uint8_t nv16[W * H * 2];
    struct pocketcam_frame f = { POCKETCAM_FMT_NV16, W, H, W, nv16, sizeof(nv16), 1, 0, 0 };
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    int r;

    pocketcam_fake_fill(nv16, POCKETCAM_FMT_NV16, W, H, 1);
    r = pocketcam_encode(fp, &f, 90, false);
    fclose(fp);
    check("the still encodes", r == 0);
    if (strcmp(pocketcam_codec_ext(), "ppm") == 0) {
        check("as a PPM of the turned size", len == strlen("P6\n36 64\n255\n") + W * H * 3 &&
                                                 memcmp(buf, "P6\n36 64\n255\n", 13) == 0);
        /* The first pixel is the sensor's bottom-left (a quarter turn). */
        {
            uint8_t rgb[3];
            const uint8_t *p = (const uint8_t *)buf + 13;

            pocketcam_fake_rgb_at(0, H - 1, 1, W, H, rgb);
            check("whose first pixel is right", abs(p[0] - rgb[0]) < 24 &&
                                                    abs(p[1] - rgb[1]) < 24 &&
                                                    abs(p[2] - rgb[2]) < 24);
        }
    } else {
        check("as a JPEG", len > 4 && (uint8_t)buf[0] == 0xff && (uint8_t)buf[1] == 0xd8 &&
                               (uint8_t)buf[len - 2] == 0xff && (uint8_t)buf[len - 1] == 0xd9);
    }
    check("under its own estimate", len <= pocketcam_codec_estimate(W, H));
    free(buf);
    f.bytes = 10;
    fp = open_memstream(&buf, &len);
    check("a damaged still is not encoded", pocketcam_encode(fp, &f, 0, false) == -EPROTO);
    fclose(fp);
    free(buf);
}

/* ---- the store ------------------------------------------------------------------ */

static char tmpdir[256];
static int64_t fake_free = -1;

static int64_t free_hook(const char *dir)
{
    (void)dir;
    return fake_free;
}

static int write_bytes(FILE *fp, void *user)
{
    size_t n = *(size_t *)user;
    size_t i;

    for (i = 0; i < n; i++) {
        if (fputc('x', fp) == EOF) {
            return -errno;
        }
    }
    return 0;
}

static int write_fail(FILE *fp, void *user)
{
    (void)user;
    fputs("half a photo", fp);
    return -EIO;
}

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void test_store(void)
{
    struct pocketcam_store s;
    char dir[512];
    char path[PATH_MAX];
    char name[POCKETCAM_STORE_NAME_MAX];
    size_t n = 1000;
    uint64_t bytes = 0;
    FILE *fp;

    snprintf(dir, sizeof(dir), "%s/state/camera", tmpdir);
    check("open makes the folder", pocketcam_store_open(&s, dir) == 0 && s.files == 0 &&
                                       s.next_seq == 1 && s.last[0] == '\0');

    check("valid: IMG_0001.jpg", pocketcam_store_valid_name("IMG_0001.jpg"));
    check("valid: IMG_20260924_101500_0002.ppm",
          pocketcam_store_valid_name("IMG_20260924_101500_0002.ppm"));
    check("not: three digits", !pocketcam_store_valid_name("IMG_001.jpg"));
    check("not: seven digits", !pocketcam_store_valid_name("IMG_0000001.jpg"));
    check("not: another extension", !pocketcam_store_valid_name("IMG_0001.png"));
    check("not: a path", !pocketcam_store_valid_name("IMG_../../etc/passwd.jpg"));
    check("not: a slash", !pocketcam_store_valid_name("IMG_00/1.jpg"));
    check("not: a hidden temporary", !pocketcam_store_valid_name(".IMG_0001.jpg.tmp"));
    check("not: nothing", !pocketcam_store_valid_name("") && !pocketcam_store_valid_name(NULL));

    pocketcam_store_next_name(&s, 1000, "jpg", name, sizeof(name));
    check("no clock: IMG_0001.jpg", strcmp(name, "IMG_0001.jpg") == 0);
    pocketcam_store_next_name(&s, 1790000000LL, "jpg", name, sizeof(name));
    check("a clock: the date and time, then the number",
          strcmp(name, "IMG_20260921_141320_0001.jpg") == 0);

    fake_free = 1LL << 40;
    pocketcam_store_free_hook = free_hook;
    check("room for a photo", pocketcam_store_room(&s, 1000) == 0);
    check("a photo is written", pocketcam_store_write(&s, "IMG_0001.jpg", write_bytes, &n,
                                                      &bytes) == 0 &&
                                    bytes == 1000);
    snprintf(path, sizeof(path), "%s/IMG_0001.jpg", dir);
    {
        struct stat st;

        check("in place, whole", stat(path, &st) == 0 && st.st_size == 1000);
    }
    check("and nothing else is left", count_entries(dir) == 1);
    check("the store counts it", s.files == 1 && s.bytes == 1000 && s.next_seq == 2 &&
                                     strcmp(s.last, "IMG_0001.jpg") == 0);
    check("a taken name is refused",
          pocketcam_store_write(&s, "IMG_0001.jpg", write_bytes, &n, NULL) == -EEXIST);
    check("a name that is not a photo name is refused",
          pocketcam_store_write(&s, "notes.txt", write_bytes, &n, NULL) == -EINVAL);
    check("a failing writer leaves nothing",
          pocketcam_store_write(&s, "IMG_0002.jpg", write_fail, NULL, NULL) == -EIO &&
              count_entries(dir) == 1);
    pocketcam_store_fail_after = 500;
    check("a disk that fills mid-photo: -ENOSPC",
          pocketcam_store_write(&s, "IMG_0002.jpg", write_bytes, &n, NULL) == -ENOSPC);
    check("and no half photo, no temporary", count_entries(dir) == 1);
    pocketcam_store_fail_after = -1;

    fake_free = (int64_t)(POCKETCAM_STORE_RESERVE_BYTES + 500);
    check("the reserve is kept: -ENOSPC", pocketcam_store_room(&s, 1000) == -ENOSPC);
    fake_free = 1LL << 40;
    s.max_files = 1;
    check("the photo limit: -EDQUOT", pocketcam_store_room(&s, 1000) == -EDQUOT);
    s.max_files = POCKETCAM_STORE_MAX_FILES;
    s.max_bytes = 1500;
    check("the byte limit: -EDQUOT", pocketcam_store_room(&s, 1000) == -EDQUOT);
    s.max_bytes = POCKETCAM_STORE_MAX_BYTES;

    /* What a power cut leaves: a temporary, removed by the next open. */
    snprintf(path, sizeof(path), "%s/.IMG_0002.jpg.tmp", dir);
    fp = fopen(path, "w");
    fputs("partial", fp);
    fclose(fp);
    snprintf(path, sizeof(path), "%s/holiday.txt", dir);
    fp = fopen(path, "w");
    fclose(fp);
    check("reopen removes the leftover", pocketcam_store_open(&s, dir) == 0 &&
                                             count_entries(dir) == 2);
    check("and leaves what is not its own", access(path, F_OK) == 0);
    check("and does not count it", s.files == 1);

    check("delete refuses a path", pocketcam_store_delete(&s, "../camera/IMG_0001.jpg") == -EINVAL);
    check("delete refuses what is not a photo", pocketcam_store_delete(&s, "holiday.txt") == -EINVAL);
    check("delete of a missing photo", pocketcam_store_delete(&s, "IMG_0009.jpg") == -ENOENT);
    check("delete", pocketcam_store_delete(&s, "IMG_0001.jpg") == 0 && s.files == 0);
    check("an empty folder starts from 1 again", s.next_seq == 1);
    pocketcam_store_free_hook = NULL;
    check("the real free space reads", pocketcam_store_room(&s, 1) == 0 ||
                                           pocketcam_store_room(&s, 1) == -ENOSPC);
}

int main(void)
{
    char tmpl[] = "/tmp/pocketcam-test-XXXXXX";

    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("TZ", "UTC", 1);
    tzset();
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s", tmpl);
    setenv("POCKETOS_STATE_DIR", tmpdir, 1);

    test_frame_check();
    test_fake();
    test_convert();
    test_codec();
    test_store();
    {
        char cmd[PATH_MAX + 16];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", tmpdir);
        }
    }
    printf("pocketcam_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
