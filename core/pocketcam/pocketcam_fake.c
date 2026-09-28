/*
 * pocketcam's fake backend: deterministic test frames and scripted faults.
 *
 * It exists so that the helper, the session, the state machine and the
 * screen can be developed and tested on a host. It proves nothing about a
 * real camera: the frame rate is whatever `period` says, the pixels are a test
 * pattern, and a still costs nothing. Every figure a test prints while running
 * on it is a figure about the code above the backend.
 *
 * THE PATTERN, in sensor space (landscape, as the frame comes off the
 * sensor): eight vertical colour bars, white to black; an orange marker in
 * the top-left eighth of the frame, so a test can tell which way a converted
 * picture was turned; and a grey square that moves along the middle with the
 * frame number, so consecutive frames differ. Every pixel is a pure function
 * of (x, y, seq, width, height): tests recompute what they expect.
 *
 * THE SCRIPT, a comma-separated list of key=value (all optional):
 *
 *   open=ok|nodev|busy|fail|hang   how open() ends (hang: it never returns)
 *   open_delay=MS                  open() takes this long first
 *   size=WxH                       the preview frame (default 640x360)
 *   still=WxH                      the still (default 1920x1080)
 *   format=nv16|nv12|bgr           (default nv16, as the vendor app reads;
 *                                  bgr is the planar BGR the Vision helper
 *                                  asks for)
 *   period=MS                      one frame every MS (default 66)
 *   frames=N                       after N frames nothing more arrives
 *   lost_after=N                   after N frames the camera is gone (-ENODEV)
 *   malformed_at=N                 frame N claims fewer bytes than it needs
 *   delay_at=N:MS                  frame N arrives MS late
 *   hang_at=N                      asking for frame N never returns (a driver
 *                                  stuck in the kernel)
 *   crash_at=N                     the process aborts at frame N
 *   capture=ok|fail|lost|hang      how still() ends
 *   capture_delay=MS               still() takes this long first
 *   mount=R or mount=Rm            mount rotation (0/90/180/270, default 90,
 *                                  as unit A), m mirrored
 *
 * Unknown keys are refused (-EINVAL), so a misspelt fault in a test fails
 * the test rather than silently testing the happy path.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketcam.h"
#include "pocketcam_fake.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum fake_end { END_OK = 0, END_NODEV, END_BUSY, END_FAIL, END_HANG, END_LOST };

struct fake {
    enum fake_end open_end;
    int open_delay;
    uint32_t w, h, still_w, still_h;
    enum pocketcam_format format;
    int period;
    uint32_t frames;       /* 0: unlimited */
    uint32_t lost_after;   /* 0: never */
    uint32_t malformed_at; /* 0: never (frames count from 1) */
    uint32_t delay_at;
    int delay_ms;
    uint32_t hang_at;
    uint32_t crash_at;
    enum fake_end capture_end;
    int capture_delay;
    int mount_rotation;
    bool mount_mirror;

    uint8_t *buf[2];
    bool held[2];
    int next_buf;
    uint8_t *still_buf;
    bool still_held;
    uint32_t seq;          /* frames delivered so far */
    int64_t last_ms;
};

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms)
{
    struct timespec d;

    if (ms <= 0) {
        return;
    }
    d.tv_sec = ms / 1000;
    d.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&d, &d) != 0 && errno == EINTR) {
    }
}

static void hang_forever(void)
{
    for (;;) {
        sleep_ms(3600 * 1000);
    }
}

/* ---- the pattern ----------------------------------------------------------- */

static const uint8_t bars[8][3] = {
    { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
    { 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 0, 0, 0 },
};
static const uint8_t marker[3] = { 255, 128, 0 };
static const uint8_t square[3] = { 128, 128, 128 };

void pocketcam_fake_rgb_at(uint32_t x, uint32_t y, uint32_t seq, uint32_t w, uint32_t h,
                           uint8_t rgb[3])
{
    uint32_t side = h / 6 ? h / 6 : 1;
    uint32_t span = w > side ? w - side : 1;
    uint32_t sx = (seq * 8u) % span;
    uint32_t sy = h / 2 - side / 2;
    const uint8_t *c;

    if (x < w / 8 && y < h / 8) {
        c = marker;
    } else if (x >= sx && x < sx + side && y >= sy && y < sy + side) {
        c = square;
    } else {
        c = bars[(x * 8u) / w];
    }
    memcpy(rgb, c, 3);
}

/* BT.601, limited range: what the K230 ISP is expected to produce, and what
 * pocketcam_convert.c undoes. */
void pocketcam_fake_rgb_to_yuv(const uint8_t rgb[3], uint8_t *y, uint8_t *u, uint8_t *v)
{
    int r = rgb[0];
    int g = rgb[1];
    int b = rgb[2];

    *y = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    *u = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    *v = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void pocketcam_fake_fill(uint8_t *dst, enum pocketcam_format fmt, uint32_t w, uint32_t h,
                         uint32_t seq)
{
    uint8_t *yp = dst;
    uint8_t *uvp = dst + (size_t)w * h;
    uint32_t chroma_lines = fmt == POCKETCAM_FMT_NV12 ? h / 2 : h;
    uint32_t x;
    uint32_t y;
    uint8_t rgb[3];
    uint8_t yy;
    uint8_t uu;
    uint8_t vv;

    if (fmt == POCKETCAM_FMT_BGR888P) {
        size_t plane = (size_t)w * h;

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t at = (size_t)y * w + x;

                pocketcam_fake_rgb_at(x, y, seq, w, h, rgb);
                dst[at] = rgb[2];
                dst[plane + at] = rgb[1];
                dst[2 * plane + at] = rgb[0];
            }
        }
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            pocketcam_fake_rgb_at(x, y, seq, w, h, rgb);
            pocketcam_fake_rgb_to_yuv(rgb, &yy, &uu, &vv);
            yp[(size_t)y * w + x] = yy;
        }
    }
    for (y = 0; y < chroma_lines; y++) {
        uint32_t src_y = fmt == POCKETCAM_FMT_NV12 ? y * 2 : y;

        for (x = 0; x < w; x += 2) {
            pocketcam_fake_rgb_at(x, src_y, seq, w, h, rgb);
            pocketcam_fake_rgb_to_yuv(rgb, &yy, &uu, &vv);
            uvp[(size_t)y * w + x] = uu;
            uvp[(size_t)y * w + x + 1] = vv;
        }
    }
}

/* ---- the script ------------------------------------------------------------ */

static int parse_u32(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v > 100000000UL) {
        return -EINVAL;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_int(const char *s, int *out)
{
    uint32_t v;

    if (parse_u32(s, &v) != 0 || v > 3600000u) {
        return -EINVAL;
    }
    *out = (int)v;
    return 0;
}

static int parse_size(const char *s, uint32_t *w, uint32_t *h)
{
    unsigned a;
    unsigned b;
    char tail;

    if (sscanf(s, "%ux%u%c", &a, &b, &tail) != 2 || a < 2 || b < 2 || a > POCKETCAM_MAX_DIM ||
        b > POCKETCAM_MAX_DIM || a % 2 || b % 2) {
        return -EINVAL;
    }
    *w = a;
    *h = b;
    return 0;
}

static int parse_end(const char *s, enum fake_end *out, bool for_capture)
{
    if (strcmp(s, "ok") == 0) {
        *out = END_OK;
    } else if (strcmp(s, "fail") == 0) {
        *out = END_FAIL;
    } else if (strcmp(s, "hang") == 0) {
        *out = END_HANG;
    } else if (!for_capture && strcmp(s, "nodev") == 0) {
        *out = END_NODEV;
    } else if (!for_capture && strcmp(s, "busy") == 0) {
        *out = END_BUSY;
    } else if (for_capture && strcmp(s, "lost") == 0) {
        *out = END_LOST;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int parse_script(struct fake *f, const char *config)
{
    char *copy;
    char *save = NULL;
    char *item;
    int r = 0;

    if (!config || !*config) {
        return 0;
    }
    copy = strdup(config);
    if (!copy) {
        return -ENOMEM;
    }
    for (item = strtok_r(copy, ",", &save); item && r == 0; item = strtok_r(NULL, ",", &save)) {
        char *val = strchr(item, '=');

        if (!val) {
            r = -EINVAL;
            break;
        }
        *val++ = '\0';
        if (strcmp(item, "open") == 0) {
            r = parse_end(val, &f->open_end, false);
        } else if (strcmp(item, "open_delay") == 0) {
            r = parse_int(val, &f->open_delay);
        } else if (strcmp(item, "size") == 0) {
            r = parse_size(val, &f->w, &f->h);
        } else if (strcmp(item, "still") == 0) {
            r = parse_size(val, &f->still_w, &f->still_h);
        } else if (strcmp(item, "format") == 0) {
            if (strcmp(val, "nv16") == 0) {
                f->format = POCKETCAM_FMT_NV16;
            } else if (strcmp(val, "nv12") == 0) {
                f->format = POCKETCAM_FMT_NV12;
            } else if (strcmp(val, "bgr") == 0) {
                f->format = POCKETCAM_FMT_BGR888P;
            } else {
                r = -EINVAL;
            }
        } else if (strcmp(item, "period") == 0) {
            r = parse_int(val, &f->period);
        } else if (strcmp(item, "frames") == 0) {
            r = parse_u32(val, &f->frames);
        } else if (strcmp(item, "lost_after") == 0) {
            r = parse_u32(val, &f->lost_after);
        } else if (strcmp(item, "malformed_at") == 0) {
            r = parse_u32(val, &f->malformed_at);
        } else if (strcmp(item, "delay_at") == 0) {
            char *colon = strchr(val, ':');

            if (!colon) {
                r = -EINVAL;
            } else {
                *colon++ = '\0';
                r = parse_u32(val, &f->delay_at);
                if (r == 0) {
                    r = parse_int(colon, &f->delay_ms);
                }
            }
        } else if (strcmp(item, "hang_at") == 0) {
            r = parse_u32(val, &f->hang_at);
        } else if (strcmp(item, "crash_at") == 0) {
            r = parse_u32(val, &f->crash_at);
        } else if (strcmp(item, "capture") == 0) {
            r = parse_end(val, &f->capture_end, true);
        } else if (strcmp(item, "capture_delay") == 0) {
            r = parse_int(val, &f->capture_delay);
        } else if (strcmp(item, "mount") == 0) {
            size_t n = strlen(val);
            uint32_t rot;

            f->mount_mirror = n > 0 && val[n - 1] == 'm';
            if (f->mount_mirror) {
                val[n - 1] = '\0';
            }
            r = parse_u32(val, &rot);
            if (r == 0 && (rot % 90 || rot >= 360)) {
                r = -EINVAL;
            }
            f->mount_rotation = (int)rot;
        } else {
            r = -EINVAL;
        }
    }
    free(copy);
    return r;
}

/* ---- the backend ------------------------------------------------------------ */

static int fake_open(struct pocketcam_backend *b, const char *config, struct pocketcam_info *info)
{
    struct fake *f = calloc(1, sizeof(*f));
    int r;

    if (!f) {
        return -ENOMEM;
    }
    b->priv = f;
    f->w = 640;
    f->h = 360;
    f->still_w = 1920;
    f->still_h = 1080;
    f->format = POCKETCAM_FMT_NV16;
    f->period = 66;
    /* Mounted like unit A's sensor, so the fake turns the way the real one
     * does: upright in portrait. */
    f->mount_rotation = 90;
    r = parse_script(f, config);
    if (r != 0) {
        return r;
    }
    sleep_ms(f->open_delay);
    switch (f->open_end) {
    case END_NODEV: return -ENODEV;
    case END_BUSY: return -EBUSY;
    case END_FAIL: return -EIO;
    case END_HANG: hang_forever(); break;
    default: break;
    }
    f->buf[0] = malloc(pocketcam_frame_bytes(f->format, f->w, f->h, f->w));
    f->buf[1] = malloc(pocketcam_frame_bytes(f->format, f->w, f->h, f->w));
    if (!f->buf[0] || !f->buf[1]) {
        return -ENOMEM;
    }
    snprintf(info->name, sizeof(info->name), "fake");
    info->preview_w = f->w;
    info->preview_h = f->h;
    info->still_w = f->still_w;
    info->still_h = f->still_h;
    info->mount_rotation = f->mount_rotation;
    info->mount_mirror = f->mount_mirror;
    info->simulated = true;
    return 0;
}

static int fake_start(struct pocketcam_backend *b)
{
    struct fake *f = b->priv;

    f->last_ms = mono_ms();
    return 0;
}

static int fake_next(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *out)
{
    struct fake *f = b->priv;
    uint32_t n = f->seq + 1; /* the frame this call would deliver */
    int64_t due;
    int64_t now = mono_ms();
    int slot;

    if (f->lost_after && f->seq >= f->lost_after) {
        return -ENODEV;
    }
    if (f->hang_at && n == f->hang_at) {
        hang_forever();
    }
    if (f->frames && f->seq >= f->frames) {
        sleep_ms(timeout_ms);
        return -ETIMEDOUT;
    }
    due = f->last_ms + f->period + (f->delay_at == n ? f->delay_ms : 0);
    if (due > now) {
        if (due - now > timeout_ms) {
            sleep_ms(timeout_ms);
            return -ETIMEDOUT;
        }
        sleep_ms(due - now);
    }
    if (f->crash_at && n == f->crash_at) {
        abort();
    }
    slot = f->next_buf;
    if (f->held[slot]) {
        slot ^= 1;
        if (f->held[slot]) {
            return -EIO; /* every buffer is out: the caller never released */
        }
    }
    f->next_buf = slot ^ 1;
    pocketcam_fake_fill(f->buf[slot], f->format, f->w, f->h, n);
    f->held[slot] = true;
    f->seq = n;
    f->last_ms = mono_ms();
    memset(out, 0, sizeof(*out));
    out->format = f->format;
    out->width = f->w;
    out->height = f->h;
    out->stride = f->w;
    out->data = f->buf[slot];
    out->bytes = pocketcam_frame_bytes(f->format, f->w, f->h, f->w);
    if (f->malformed_at == n) {
        out->bytes /= 2;
    }
    out->seq = n;
    out->mono_ms = f->last_ms;
    out->handle = slot;
    return 0;
}

static int fake_still(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *out)
{
    struct fake *f = b->priv;
    size_t bytes = pocketcam_frame_bytes(f->format, f->still_w, f->still_h, f->still_w);

    if (f->capture_delay > timeout_ms) {
        sleep_ms(timeout_ms);
        return -ETIMEDOUT;
    }
    sleep_ms(f->capture_delay);
    switch (f->capture_end) {
    case END_FAIL: return -EIO;
    case END_LOST: return -ENODEV;
    case END_HANG: hang_forever(); break;
    default: break;
    }
    if (f->still_held) {
        return -EIO;
    }
    if (!f->still_buf) {
        f->still_buf = malloc(bytes);
        if (!f->still_buf) {
            return -ENOMEM;
        }
    }
    pocketcam_fake_fill(f->still_buf, f->format, f->still_w, f->still_h, f->seq);
    f->still_held = true;
    memset(out, 0, sizeof(*out));
    out->format = f->format;
    out->width = f->still_w;
    out->height = f->still_h;
    out->stride = f->still_w;
    out->data = f->still_buf;
    out->bytes = bytes;
    out->seq = f->seq;
    out->mono_ms = mono_ms();
    out->handle = 2;
    return 0;
}

static void fake_release(struct pocketcam_backend *b, struct pocketcam_frame *fr)
{
    struct fake *f = b->priv;

    if (fr->handle == 2) {
        f->still_held = false;
    } else if (fr->handle == 0 || fr->handle == 1) {
        f->held[fr->handle] = false;
    }
}

static void fake_stop(struct pocketcam_backend *b)
{
    (void)b;
}

static void fake_close(struct pocketcam_backend *b)
{
    struct fake *f = b->priv;

    if (f) {
        free(f->buf[0]);
        free(f->buf[1]);
        free(f->still_buf);
        free(f);
    }
    b->priv = NULL;
}

const struct pocketcam_backend_ops pocketcam_fake_ops = {
    .name = "fake",
    .open = fake_open,
    .start = fake_start,
    .next = fake_next,
    .still = fake_still,
    .release = fake_release,
    .stop = fake_stop,
    .close = fake_close,
};
