/*
 * pocketcam: the camera layer below the Camera app (docs/apps/CAMERA.md,
 * docs/decisions/ADR-006-camera-ownership.md).
 *
 * A camera backend delivers frames: a preview stream, and a single still on
 * request. Everything that is particular to a board or a driver - the device
 * node, the pixel format it can be asked for, how the sensor is mounted
 * relative to the panel - stays behind struct pocketcam_backend_ops, so no
 * caller ever names a video node or an ioctl.
 *
 * Who uses it: only pos-camera (tools/camera/pos_camera.c), the short-lived
 * helper process that owns the camera for as long as the Camera screen is
 * open. The shell and the app never link a backend.
 *
 * Backends:
 *
 *   fake   deterministic test frames with scripted faults (pocketcam_fake.c).
 *          It exists to develop and test everything above the driver on a
 *          host. It measures nothing about a real camera: its timing is
 *          whatever the script says, and its pixels are a test pattern.
 *   v4l2   the real camera (pocketcam_v4l2.c): standard V4L2 on the capture
 *          nodes of the K230's vvcam ISP driver, preview and still on two of
 *          the ISP's outputs. Measured on unit A in
 *          docs/hardware/CAMERA_PLATFORM_RESEARCH.md §10.
 *
 * Frames are borrowed: a frame returned by next() or still() stays valid
 * until release(), and a backend may hand out a bounded number at once
 * (POCKETCAM_MAX_HELD). Nothing here allocates per frame.
 *
 * Errors are negative errno values:
 *
 *   -ENODEV     no camera, or the camera went away (a lost stream)
 *   -EBUSY      the camera is there but someone else holds it
 *   -ENOTSUP    this backend is not built in
 *   -ETIMEDOUT  no frame within the wait (the stream is still up)
 *   -EPROTO     the driver delivered something that is not a valid frame
 *   -EIO        anything else the driver refused
 *
 * No threads, no LVGL. Every call is bounded by the timeout it is given.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_H
#define POCKETOS_POCKETCAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest frame any backend may deliver, either side. GC2093, the sensor
 * on the T-Display K230, has 1920 x 1080 modes only (DOCUMENTED, vendor
 * driver); this leaves room for a 4K sensor without letting a corrupt size
 * turn into a multi-gigabyte conversion. */
#define POCKETCAM_MAX_DIM 4096
/* Frames a backend may have out at once (a preview frame and a still). */
#define POCKETCAM_MAX_HELD 2
#define POCKETCAM_NAME_MAX 32

enum pocketcam_format {
    /* Y plane, then interleaved CbCr at half width and half height. */
    POCKETCAM_FMT_NV12 = 0,
    /* Y plane, then interleaved CbCr at half width and full height (what the
     * vendor Camera app reads from the K230 ISP). */
    POCKETCAM_FMT_NV16,
    /* Little-endian RGB565, one plane. */
    POCKETCAM_FMT_RGB565,
    /* Three 8-bit planes, B then G then R, each `height` lines of `stride`
     * bytes: the K230 ISP's "BG3P" output, which the vendor's KPU demos read
     * and the AI2D engine takes as it is. Used by the Vision app's helper
     * (docs/apps/VISION.md). */
    POCKETCAM_FMT_BGR888P,
    POCKETCAM_FMT_COUNT
};

struct pocketcam_frame {
    enum pocketcam_format format;
    uint32_t width;
    uint32_t height;
    /* Bytes from one line to the next: the Y plane's for NV12 and NV16 (the
     * chroma plane has the same stride and starts right after the last Y
     * line), the only plane's for RGB565, each plane's for BGR888P (the
     * three planes follow one another, `height` lines each). */
    uint32_t stride;
    const uint8_t *data;
    /* How many bytes of data the backend says are valid. A frame is only
     * used when this covers every line (pocketcam_frame_check). */
    size_t bytes;
    uint32_t seq;
    int64_t mono_ms;
    int handle;        /* the backend's, for release() */
};

/* What an opened camera is. Rotation and mirroring are how the sensor is
 * mounted: the clockwise turn, then mirror, that makes "up" in the picture
 * "up" on the panel in its native orientation (display rotation 0, portrait
 * on the K230). Unit A: 90, not mirrored (VERIFIED 2026-09-25). */
struct pocketcam_info {
    char name[POCKETCAM_NAME_MAX];
    uint32_t preview_w;
    uint32_t preview_h;
    uint32_t still_w;
    uint32_t still_h;
    int mount_rotation;  /* 0, 90, 180 or 270, clockwise */
    bool mount_mirror;   /* mirrored left to right after the rotation */
    bool simulated;      /* frames are made up (the fake backend) */
};

struct pocketcam_backend;

struct pocketcam_backend_ops {
    const char *name;
    /* Open and configure, without streaming. config is backend-specific and
     * may be NULL. */
    int (*open)(struct pocketcam_backend *b, const char *config, struct pocketcam_info *info);
    int (*start)(struct pocketcam_backend *b);
    /* The next preview frame, waiting at most timeout_ms. */
    int (*next)(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f);
    /* One still at the still size. The preview stream is stopped by this
     * call if it was running, and not restarted: the caller starts it again
     * when it wants it. */
    int (*still)(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f);
    void (*release)(struct pocketcam_backend *b, struct pocketcam_frame *f);
    void (*stop)(struct pocketcam_backend *b);
    /* Stop if needed, release everything, free priv. Safe after a failed
     * open. */
    void (*close)(struct pocketcam_backend *b);
};

struct pocketcam_backend {
    const struct pocketcam_backend_ops *ops;
    void *priv;
    bool streaming;
};

/* Pick a backend by name ("fake", "v4l2") and open it. 0, or a negative
 * errno with *b left closed. */
int pocketcam_open(struct pocketcam_backend *b, const char *name, const char *config,
                   struct pocketcam_info *info);
int pocketcam_start(struct pocketcam_backend *b);
int pocketcam_next(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f);
int pocketcam_still(struct pocketcam_backend *b, int timeout_ms, struct pocketcam_frame *f);
void pocketcam_release(struct pocketcam_backend *b, struct pocketcam_frame *f);
void pocketcam_stop(struct pocketcam_backend *b);
void pocketcam_close(struct pocketcam_backend *b);

/* Whether a frame is usable as it claims to be: a known format, a size
 * inside POCKETCAM_MAX_DIM, an even width (and an even height for NV12), a
 * stride that holds a line, and bytes that cover every line of every plane.
 * 0, or -EPROTO. Everything that reads pixels calls this first, so a short
 * or lying buffer from a driver is refused rather than read past. */
int pocketcam_frame_check(const struct pocketcam_frame *f);

/* The bytes a frame of this shape needs, or 0 when the shape is invalid. */
size_t pocketcam_frame_bytes(enum pocketcam_format fmt, uint32_t width, uint32_t height,
                             uint32_t stride);

const char *pocketcam_strerror(int err);

/* The backends. Exposed for the helper's `probe` and for the tests. */
extern const struct pocketcam_backend_ops pocketcam_fake_ops;
extern const struct pocketcam_backend_ops pocketcam_v4l2_ops;

#endif
