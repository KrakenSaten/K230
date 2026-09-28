/*
 * pocketcam: backend dispatch and frame validation (the backends are
 * pocketcam_fake.c and pocketcam_v4l2.c).
 * See pocketcam.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam.h"

#include <errno.h>
#include <string.h>

size_t pocketcam_frame_bytes(enum pocketcam_format fmt, uint32_t width, uint32_t height,
                             uint32_t stride)
{
    uint64_t lines;

    if (width == 0 || height == 0 || width > POCKETCAM_MAX_DIM || height > POCKETCAM_MAX_DIM) {
        return 0;
    }
    switch (fmt) {
    case POCKETCAM_FMT_NV12:
        if (width % 2 || height % 2 || stride < width) {
            return 0;
        }
        lines = (uint64_t)height + height / 2;
        break;
    case POCKETCAM_FMT_NV16:
        if (width % 2 || stride < width) {
            return 0;
        }
        lines = (uint64_t)height * 2;
        break;
    case POCKETCAM_FMT_RGB565:
        if (stride < width * 2u) {
            return 0;
        }
        lines = height;
        break;
    case POCKETCAM_FMT_BGR888P:
        if (stride < width) {
            return 0;
        }
        lines = (uint64_t)height * 3;
        break;
    default:
        return 0;
    }
    /* stride is at most a few times POCKETCAM_MAX_DIM in anything real; a
     * stride past 4 bytes a pixel is refused rather than trusted. */
    if (stride > POCKETCAM_MAX_DIM * 4u) {
        return 0;
    }
    return (size_t)(lines * stride);
}

int pocketcam_frame_check(const struct pocketcam_frame *f)
{
    size_t need;

    if (!f || !f->data) {
        return -EPROTO;
    }
    need = pocketcam_frame_bytes(f->format, f->width, f->height, f->stride);
    if (need == 0 || f->bytes < need) {
        return -EPROTO;
    }
    return 0;
}

const char *pocketcam_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case -ENODEV: return "no camera";
    case -EBUSY: return "the camera is in use";
    case -ENOTSUP: return "camera support is not built in";
    case -ETIMEDOUT: return "the camera did not answer in time";
    case -EPROTO: return "the camera delivered a damaged frame";
    case -ENOMEM: return "out of memory";
    case -EINVAL: return "invalid request";
    case -ENOSPC: return "storage is full";
    case -EDQUOT: return "the photo limit is reached";
    default: return "camera error";
    }
}

/* ---- dispatch ------------------------------------------------------------ */

static const struct pocketcam_backend_ops *const backends[] = {
    &pocketcam_fake_ops,
    &pocketcam_v4l2_ops,
};

int pocketcam_open(struct pocketcam_backend *b, const char *name, const char *config,
                   struct pocketcam_info *info)
{
    size_t i;
    int r;

    memset(b, 0, sizeof(*b));
    memset(info, 0, sizeof(*info));
    for (i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (name && strcmp(name, backends[i]->name) == 0) {
            b->ops = backends[i];
            break;
        }
    }
    if (!b->ops) {
        return -ENOTSUP;
    }
    r = b->ops->open(b, config, info);
    if (r != 0) {
        b->ops->close(b);
        memset(b, 0, sizeof(*b));
        return r;
    }
    if (info->mount_rotation % 90 != 0 || info->mount_rotation < 0 || info->mount_rotation >= 360) {
        info->mount_rotation = 0;
    }
    return 0;
}

int pocketcam_start(struct pocketcam_backend *b)
{
    int r;

    if (!b->ops) {
        return -ENODEV;
    }
    if (b->streaming) {
        return 0;
    }
    r = b->ops->start(b);
    if (r == 0) {
        b->streaming = true;
    }
    return r;
}

int pocketcam_next(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f)
{
    int r;

    if (!b->ops || !b->streaming) {
        return -EINVAL;
    }
    r = b->ops->next(b, timeout_ms, f);
    if (r == 0 && pocketcam_frame_check(f) != 0) {
        b->ops->release(b, f);
        return -EPROTO;
    }
    return r;
}

int pocketcam_still(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f)
{
    int r;

    if (!b->ops) {
        return -ENODEV;
    }
    r = b->ops->still(b, timeout_ms, f);
    b->streaming = false;
    if (r == 0 && pocketcam_frame_check(f) != 0) {
        b->ops->release(b, f);
        return -EPROTO;
    }
    return r;
}

void pocketcam_release(struct pocketcam_backend *b, struct pocketcam_frame *f)
{
    if (b->ops && f && f->data) {
        b->ops->release(b, f);
        f->data = NULL;
    }
}

void pocketcam_stop(struct pocketcam_backend *b)
{
    if (b->ops && b->streaming) {
        b->ops->stop(b);
        b->streaming = false;
    }
}

void pocketcam_close(struct pocketcam_backend *b)
{
    if (b->ops) {
        pocketcam_stop(b);
        b->ops->close(b);
    }
    memset(b, 0, sizeof(*b));
}

