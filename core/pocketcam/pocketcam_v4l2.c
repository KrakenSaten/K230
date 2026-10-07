/*
 * pocketcam's v4l2 backend: the real camera on the T-Display K230, through
 * the standard V4L2 API. See pocketcam.h for the interface, and
 * docs/hardware/CAMERA_PLATFORM_RESEARCH.md §10 for the unit A measurements
 * every default below comes from.
 *
 * The camera is the vendor's vvcam ISP driver fed by `isp_media_server`
 * (started at boot by S31canaan_isp). Its three capture nodes are the ISP's
 * three outputs of one sensor; they stream at the same time (VERIFIED on unit
 * A, 2026-09-25), so:
 *
 *   preview  /dev/video2 (a self path), 640 x 360 NV16 - the vendor app's
 *            preview size, scaled by the ISP;
 *   still    /dev/video1 (the main path), 1920 x 1080 NV16 - the sensor's
 *            only mode - opened for one still and closed again, while the
 *            preview keeps running, so the still starts with the exposure the
 *            preview has already settled.
 *
 * Either can be changed without rebuilding: the config string (the helper's
 * --config, or $POCKETOS_CAMERA_CONFIG) takes preview=, still=, size=WxH,
 * still_size=WxH, fmt=nv16|nv12|bg3p (the preview's pixel format; bgr is the
 * driver's planar "BG3P", what the Vision helper feeds the AI2D engine) and
 * mount=R[m].
 *
 * What the driver does that this has to live with (all seen on unit A):
 *
 *   - After STREAMON the first ~14 frames are black (luma 0) while the ISP's
 *     auto exposure starts; frames then brighten over ~1 s. Frames whose
 *     sampled luma is still ~0 are dropped here, for at most
 *     V4L2_SETTLE_MAX_MS, so no black picture reaches the screen or a photo.
 *   - A second opener gets EBUSY from VIDIOC_REQBUFS, not from open():
 *     reported as -EBUSY.
 *   - VIDIOC_QUERYCTRL enumeration on the main path never returns
 *     (`v4l2-ctl --list-ctrls` spins). Nothing here enumerates controls.
 *   - VIDIOC_ENUM_FMT on /dev/video1 lists nothing, yet S_FMT works; so the
 *     format is set and the answer checked, never looked up.
 *
 * Every wait is a poll() bounded by the caller's timeout. A frame is only
 * read between DQBUF and its QBUF (release), never after.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketcam.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The driver's planar BGR ("24-bit BGR planer" in vvcam_video_register.c),
 * not in the kernel's videodev2.h. */
#ifndef V4L2_PIX_FMT_BG3P
#define V4L2_PIX_FMT_BG3P v4l2_fourcc('B', 'G', '3', 'P')
#endif
#define V4L2_PREVIEW_NODE "/dev/video2"
#define V4L2_STILL_NODE "/dev/video1"
#define V4L2_PREVIEW_W 640
#define V4L2_PREVIEW_H 360
#define V4L2_STILL_W 1920
#define V4L2_STILL_H 1080
#define V4L2_BUFS 4
/* How the sensor sits on unit A: a frame needs a quarter turn clockwise to be
 * upright on the native (portrait) panel, and is not mirrored. VERIFIED
 * 2026-09-25 with printed text, the owner holding the unit: upright in
 * portrait (display 0, turn 90) and in the shell's landscape (display 270,
 * turn 180). pocketcam_view_rotation() takes the display rotation off. */
#define V4L2_MOUNT_ROTATION 90
#define V4L2_STILL_BUFS 3
/* How long to drop black frames after a stream starts. On unit A light
 * arrives after ~0.7 s; a scene that is really black is shown after this. */
#define V4L2_SETTLE_MAX_MS 3000
/* A frame whose sampled luma averages below this is the ISP not having
 * started yet (measured 0.0-0.2), not a dark room (1.5 and up). */
#define V4L2_BLACK_LUMA_X10 8
#define V4L2_HANDLE_STILL 100
/* Opening again after an EIO, while the ISP daemon starts. */
#define V4L2_OPEN_RETRIES 4
#define V4L2_OPEN_RETRY_MS 500

struct node {
    int fd;
    char path[64];
    uint32_t w;
    uint32_t h;
    uint32_t stride;
    uint32_t size;
    enum pocketcam_format fmt;
    unsigned nbuf;
    void *map[V4L2_BUFS];
    size_t len[V4L2_BUFS];
    bool queued[V4L2_BUFS];
    bool streaming;
    int64_t started_ms;
    bool settled;
};

struct cam {
    struct node pv;
    struct node st;
    char pv_path[64];
    uint32_t pv_w;
    uint32_t pv_h;
    enum pocketcam_format pv_fmt; /* what the preview node is asked for */
    char still_path[64];
    uint32_t still_w;
    uint32_t still_h;
    int mount_rotation;
    bool mount_mirror;
};

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* errno from open or an ioctl, as pocketcam's errors. */
static int cam_err(int e)
{
    switch (e) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
    case EPIPE:
        return -ENODEV;
    case EBUSY:
        return -EBUSY;
    case EAGAIN:
    case ETIMEDOUT:
        return -ETIMEDOUT;
    case ENOMEM:
        return -ENOMEM;
    default:
        return -EIO;
    }
}

/* ---- one capture node ------------------------------------------------------- */

static void node_init(struct node *n)
{
    memset(n, 0, sizeof(*n));
    n->fd = -1;
}

static void node_close(struct node *n)
{
    unsigned i;

    if (n->fd >= 0) {
        if (n->streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            xioctl(n->fd, VIDIOC_STREAMOFF, &type);
        }
        for (i = 0; i < n->nbuf; i++) {
            if (n->map[i] && n->map[i] != MAP_FAILED) {
                munmap(n->map[i], n->len[i]);
            }
        }
        if (n->nbuf) {
            struct v4l2_requestbuffers req = { 0 };

            req.count = 0;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            xioctl(n->fd, VIDIOC_REQBUFS, &req);
        }
        close(n->fd);
    }
    node_init(n);
}

/* The V4L2 name of a pocketcam format this backend can ask for. */
static uint32_t fourcc_of(enum pocketcam_format f)
{
    switch (f) {
    case POCKETCAM_FMT_NV12: return V4L2_PIX_FMT_NV12;
    case POCKETCAM_FMT_BG3P: return V4L2_PIX_FMT_BG3P;
    default: return V4L2_PIX_FMT_NV16;
    }
}

static int node_open(struct node *n, const char *path, uint32_t w, uint32_t h, unsigned want,
                     enum pocketcam_format pfmt)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    uint32_t caps;
    uint32_t fourcc = fourcc_of(pfmt);
    unsigned i;
    int r;

    node_init(n);
    n->fmt = pfmt;
    snprintf(n->path, sizeof(n->path), "%s", path);
    n->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (n->fd < 0) {
        return cam_err(errno);
    }
    memset(&cap, 0, sizeof(cap));
    if (xioctl(n->fd, VIDIOC_QUERYCAP, &cap) != 0) {
        r = cam_err(errno);
        goto fail;
    }
    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        r = -ENODEV;
        goto fail;
    }
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(n->fd, VIDIOC_S_FMT, &fmt) != 0) {
        r = cam_err(errno);
        goto fail;
    }
    /* The driver may adjust: take only exactly what was asked for. */
    if (fmt.fmt.pix.pixelformat != fourcc || fmt.fmt.pix.width != w ||
        fmt.fmt.pix.height != h) {
        r = -EIO;
        goto fail;
    }
    n->w = w;
    n->h = h;
    n->stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : w;
    n->size = fmt.fmt.pix.sizeimage;
    if (pocketcam_frame_bytes(pfmt, w, h, n->stride) == 0) {
        r = -EIO;
        goto fail;
    }
    memset(&req, 0, sizeof(req));
    req.count = want;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(n->fd, VIDIOC_REQBUFS, &req) != 0) {
        r = cam_err(errno); /* EBUSY here: another program holds it */
        goto fail;
    }
    if (req.count < 2) {
        r = -ENOMEM;
        goto fail;
    }
    n->nbuf = req.count > V4L2_BUFS ? V4L2_BUFS : req.count;
    for (i = 0; i < n->nbuf; i++) {
        struct v4l2_buffer b;

        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(n->fd, VIDIOC_QUERYBUF, &b) != 0) {
            r = cam_err(errno);
            goto fail;
        }
        n->len[i] = b.length;
        n->map[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, n->fd, b.m.offset);
        if (n->map[i] == MAP_FAILED) {
            n->map[i] = NULL;
            r = -ENOMEM;
            goto fail;
        }
    }
    return 0;
fail:
    node_close(n);
    return r;
}

static int node_queue(struct node *n, unsigned index)
{
    struct v4l2_buffer b;

    if (index >= n->nbuf || n->queued[index]) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    if (xioctl(n->fd, VIDIOC_QBUF, &b) != 0) {
        return cam_err(errno);
    }
    n->queued[index] = true;
    return 0;
}

static int node_start(struct node *n)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned i;
    int r;

    if (n->streaming) {
        return 0;
    }
    for (i = 0; i < n->nbuf; i++) {
        r = node_queue(n, i);
        if (r != 0) {
            return r;
        }
    }
    if (xioctl(n->fd, VIDIOC_STREAMON, &type) != 0) {
        return cam_err(errno);
    }
    n->streaming = true;
    n->started_ms = mono_ms();
    n->settled = false;
    return 0;
}

/* Sampled luma average, times ten. */
static unsigned luma_x10(const uint8_t *y, uint32_t w, uint32_t h, uint32_t stride)
{
    uint64_t sum = 0;
    unsigned n = 0;
    uint32_t row;
    uint32_t x;

    for (row = 0; row < h; row += 16) {
        for (x = 0; x < w; x += 16) {
            sum += y[(size_t)row * stride + x];
            n++;
        }
    }
    return n ? (unsigned)(sum * 10 / n) : 0;
}

/* The next frame of n, black start-up frames dropped. */
static int node_next(struct node *n, int timeout_ms, struct pocketcam_frame *f, int handle_base)
{
    int64_t end = mono_ms() + (timeout_ms < 0 ? 0 : timeout_ms);

    for (;;) {
        struct pollfd p = { .fd = n->fd, .events = POLLIN };
        struct v4l2_buffer b;
        int64_t left = end - mono_ms();
        int pr;

        if (left < 0) {
            left = 0;
        }
        pr = poll(&p, 1, (int)left);
        if (pr < 0 && errno != EINTR) {
            return cam_err(errno);
        }
        if (pr <= 0) {
            return -ETIMEDOUT;
        }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!(p.revents & POLLIN)) {
                return -ENODEV;
            }
        }
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(n->fd, VIDIOC_DQBUF, &b) != 0) {
            if (errno == EAGAIN) {
                continue;
            }
            return cam_err(errno);
        }
        if (b.index >= n->nbuf) {
            return -EIO;
        }
        n->queued[b.index] = false;
        if (!n->settled) {
            bool black = b.bytesused >= n->stride * n->h &&
                         luma_x10(n->map[b.index], n->w, n->h, n->stride) < V4L2_BLACK_LUMA_X10;

            if (black && mono_ms() - n->started_ms < V4L2_SETTLE_MAX_MS) {
                int r = node_queue(n, b.index);

                if (r != 0) {
                    return r;
                }
                continue;
            }
            n->settled = true;
        }
        memset(f, 0, sizeof(*f));
        f->format = n->fmt;
        f->width = n->w;
        f->height = n->h;
        f->stride = n->stride;
        f->data = n->map[b.index];
        /* bytesused as the driver says; pocketcam_frame_check() refuses a
         * frame it does not cover. */
        f->bytes = b.bytesused ? b.bytesused : n->size;
        if (f->bytes > n->len[b.index]) {
            f->bytes = n->len[b.index];
        }
        f->seq = b.sequence;
        f->mono_ms = mono_ms();
        f->handle = handle_base + (int)b.index;
        return 0;
    }
}

/* ---- the config string ---------------------------------------------------------- */

static int parse_size(const char *s, uint32_t *w, uint32_t *h)
{
    unsigned a;
    unsigned b;
    char tail;

    if (sscanf(s, "%ux%u%c", &a, &b, &tail) != 2 || a < 32 || b < 16 || a > POCKETCAM_MAX_DIM ||
        b > POCKETCAM_MAX_DIM || a % 16 || b % 8) {
        return -EINVAL;
    }
    *w = a;
    *h = b;
    return 0;
}

static int parse_config(const char *config, char *pv_path, uint32_t *pw, uint32_t *ph,
                        struct cam *c)
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
        if (strcmp(item, "preview") == 0 && strncmp(val, "/dev/", 5) == 0 && strlen(val) < 64) {
            snprintf(pv_path, 64, "%s", val);
        } else if (strcmp(item, "still") == 0 && strncmp(val, "/dev/", 5) == 0 &&
                   strlen(val) < 64) {
            snprintf(c->still_path, sizeof(c->still_path), "%s", val);
        } else if (strcmp(item, "size") == 0) {
            r = parse_size(val, pw, ph);
        } else if (strcmp(item, "still_size") == 0) {
            r = parse_size(val, &c->still_w, &c->still_h);
        } else if (strcmp(item, "fmt") == 0) {
            /* What the preview node is asked for. NV16 is what the vendor
             * app reads and Camera shows; the Vision helper asks for the
             * planar BGR the KPU demos read. Stills stay NV16. */
            if (strcmp(val, "nv16") == 0) {
                c->pv_fmt = POCKETCAM_FMT_NV16;
            } else if (strcmp(val, "nv12") == 0) {
                c->pv_fmt = POCKETCAM_FMT_NV12;
            } else if (strcmp(val, "bg3p") == 0) {
                c->pv_fmt = POCKETCAM_FMT_BG3P;
            } else {
                r = -EINVAL;
            }
        } else if (strcmp(item, "mount") == 0) {
            size_t n = strlen(val);
            char *end;
            long rot;

            c->mount_mirror = n > 0 && val[n - 1] == 'm';
            if (c->mount_mirror) {
                val[n - 1] = '\0';
            }
            rot = strtol(val, &end, 10);
            if (end == val || *end || rot < 0 || rot >= 360 || rot % 90) {
                r = -EINVAL;
            } else {
                c->mount_rotation = (int)rot;
            }
        } else {
            r = -EINVAL;
        }
    }
    free(copy);
    return r;
}

/* ---- the backend ------------------------------------------------------------------ */

static int v4l2_open(struct pocketcam_backend *b, const char *config, struct pocketcam_info *info)
{
    struct cam *c = calloc(1, sizeof(*c));
    char pv_path[64] = V4L2_PREVIEW_NODE;
    uint32_t pw = V4L2_PREVIEW_W;
    uint32_t ph = V4L2_PREVIEW_H;
    int attempt;
    int r;

    if (!c) {
        return -ENOMEM;
    }
    b->priv = c;
    node_init(&c->pv);
    node_init(&c->st);
    snprintf(c->still_path, sizeof(c->still_path), "%s", V4L2_STILL_NODE);
    c->still_w = V4L2_STILL_W;
    c->still_h = V4L2_STILL_H;
    c->mount_rotation = V4L2_MOUNT_ROTATION;
    c->pv_fmt = POCKETCAM_FMT_NV16;
    r = parse_config(config, pv_path, &pw, &ph, c);
    if (r != 0) {
        return r;
    }
    if (access(c->still_path, F_OK) != 0) {
        return -ENODEV;
    }
    snprintf(c->pv_path, sizeof(c->pv_path), "%s", pv_path);
    c->pv_w = pw;
    c->pv_h = ph;
    /* Right after boot the ISP daemon may not have the sensor up yet: on unit
     * A an open about two minutes after power-on failed with EIO once and
     * worked a minute later. A few bounded retries, well inside the session's
     * CAMERA_OPEN_MS. */
    for (attempt = 0;; attempt++) {
        r = node_open(&c->pv, pv_path, pw, ph, V4L2_BUFS, c->pv_fmt);
        if (r != -EIO || attempt >= V4L2_OPEN_RETRIES) {
            break;
        }
        {
            struct timespec d = { 0, V4L2_OPEN_RETRY_MS * 1000000L };

            while (nanosleep(&d, &d) != 0 && errno == EINTR) {
            }
        }
    }
    if (r != 0) {
        return r;
    }
    snprintf(info->name, sizeof(info->name), "v4l2");
    info->preview_w = pw;
    info->preview_h = ph;
    info->still_w = c->still_w;
    info->still_h = c->still_h;
    info->mount_rotation = c->mount_rotation;
    info->mount_mirror = c->mount_mirror;
    info->simulated = false;
    return 0;
}

/* A stream is never started twice on one open node: after a stop the node was
 * closed, and it is opened afresh here. Restarting a stopped stream on the
 * same descriptor (STREAMOFF, then STREAMON) is the step after which unit A
 * locked up entirely on 2026-09-25 (CAMERA_GATE.md, H1); the vendor app never
 * does it either - it closes and reopens the node for every preview. */
static int v4l2_start(struct pocketcam_backend *b)
{
    struct cam *c = b->priv;
    int r;

    if (c->pv.fd < 0) {
        r = node_open(&c->pv, c->pv_path, c->pv_w, c->pv_h, V4L2_BUFS, c->pv_fmt);
        if (r != 0) {
            return r;
        }
    }
    return node_start(&c->pv);
}

static int v4l2_next(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f)
{
    struct cam *c = b->priv;

    return node_next(&c->pv, timeout_ms, f, 0);
}

static int v4l2_still(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f)
{
    struct cam *c = b->priv;
    int64_t end = mono_ms() + timeout_ms;
    int r;

    if (c->st.fd >= 0) {
        return -EIO; /* the last still was never released */
    }
    /* The preview keeps running while the still is taken: the main path then
     * starts from the exposure the preview has already found. */
    r = node_open(&c->st, c->still_path, c->still_w, c->still_h, V4L2_STILL_BUFS,
                  POCKETCAM_FMT_NV16);
    if (r == 0) {
        r = node_start(&c->st);
    }
    if (r == 0) {
        int64_t left = end - mono_ms();

        r = node_next(&c->st, left > 0 ? (int)left : 0, f, V4L2_HANDLE_STILL);
    }
    /* The interface's promise: after a still the preview is stopped, and the
     * caller starts it again when it wants it - on a freshly opened node. */
    node_close(&c->pv);
    if (r != 0) {
        node_close(&c->st);
    }
    return r;
}

static void v4l2_release(struct pocketcam_backend *b, struct pocketcam_frame *f)
{
    struct cam *c = b->priv;

    if (f->handle >= V4L2_HANDLE_STILL) {
        node_close(&c->st);
    } else if (f->handle >= 0 && c->pv.fd >= 0) {
        node_queue(&c->pv, (unsigned)f->handle);
    }
}

static void v4l2_stop(struct pocketcam_backend *b)
{
    struct cam *c = b->priv;

    node_close(&c->pv);
}

static void v4l2_close(struct pocketcam_backend *b)
{
    struct cam *c = b->priv;

    if (c) {
        node_close(&c->st);
        node_close(&c->pv);
        free(c);
    }
    b->priv = NULL;
}

const struct pocketcam_backend_ops pocketcam_v4l2_ops = {
    .name = "v4l2",
    .open = v4l2_open,
    .start = v4l2_start,
    .next = v4l2_next,
    .still = v4l2_still,
    .release = v4l2_release,
    .stop = v4l2_stop,
    .close = v4l2_close,
};
