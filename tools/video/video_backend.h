/*
 * pos-video's decoding seam: what the player engine (video_player.c) needs
 * from something that reads a file and turns it into pictures and sound.
 *
 *   ffmpeg (video_backend_ffmpeg.c, POCKETVIDEO_FFMPEG=1): libavformat reads
 *          the container, the K230's hardware decoder decodes H.264 through
 *          FFmpeg's V4L2 memory-to-memory wrapper (h264_v4l2m2m on
 *          /dev/video_vpu) and scales it on the way out, libswscale turns it
 *          into RGB565, libavcodec decodes the sound and libswresample makes
 *          it mono 48 kHz. All of it is the FFmpeg already in the image.
 *   fake   (video_backend_fake.c): a "file" that is a line of text naming a
 *          picture size, a frame rate, a length, sound or not, and the
 *          failure to act out. Every pixel is a function of its position and
 *          the frame number. Tests, the simulator.
 *
 * The engine owns time, pacing, slots and the sound card; a backend only
 * decodes, as fast as it is asked. Single-threaded: only the engine's main
 * thread calls a backend.
 *
 * Pictures and sound are asked for separately. A container interleaves the
 * two only roughly (FFmpeg's MP4 reader takes file order within a second of
 * decode time), so a backend that could only hand out "the next thing in the
 * file" would make the sound wait behind pictures the screen does not want
 * yet - an 80 ms device buffer runs dry long before a second of pictures is
 * shown. A backend therefore keeps the other stream's packets aside, within a
 * bound, and says VIDEO_ITEM_AGAIN when that bound is reached.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VIDEO_BACKEND_H
#define POCKETOS_VIDEO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define VIDEO_BACKEND_WORD_MAX 16
#define VIDEO_BACKEND_TEXT_MAX 160
/* The sound comes out as mono signed 16-bit at this rate: pocketaudio's. */
#define VIDEO_AUDIO_RATE 48000
/* The most samples one audio item carries. */
#define VIDEO_AUDIO_CHUNK_MAX 8192

struct video_media_info {
    int64_t duration_ms;       /* 0 when the file does not say */
    uint32_t src_w;            /* the pictures as stored */
    uint32_t src_h;
    uint32_t fps_x100;         /* 0 when unknown */
    int has_audio;             /* the file has a sound track ... */
    int audio_decodable;       /* ... that this build can decode */
    char codec[VIDEO_BACKEND_WORD_MAX];  /* "h264" */
};

enum video_item_kind {
    VIDEO_ITEM_PICTURE, /* a decoded picture is held: picture() converts it */
    VIDEO_ITEM_AUDIO,   /* samples/count at pts_ms */
    VIDEO_ITEM_EOF,     /* nothing more of this stream until a seek */
    VIDEO_ITEM_AGAIN,   /* not now: the other stream's packets set aside are
                           at their bound, ask for the other stream first */
    VIDEO_ITEM_ERROR    /* text says what; the file cannot be played on */
};

struct video_item {
    enum video_item_kind kind;
    int64_t pts_ms;
    const int16_t *samples;    /* VIDEO_ITEM_AUDIO: valid until the next call */
    size_t count;
    char text[VIDEO_BACKEND_TEXT_MAX];
};

struct video_backend {
    const char *name;
    /* Open a file. 0 with *ctx and *info filled, or -1 with reason (one of
     * VIDEO_OPENFAIL_*) and text; nothing is left open on failure. want_audio
     * 0 skips the sound track entirely. */
    int (*open)(void **ctx, const char *path, int want_audio, struct video_media_info *info,
                char *reason, char *text, size_t textlen);
    /* The size pictures come out at from now on (already fitted and aligned
     * by the engine). A backend may restart its decoder for it; the engine
     * seeks afterwards, so no picture of the old size is expected. */
    int (*output)(void *ctx, uint32_t w, uint32_t h, char *text, size_t textlen);
    /* Go to ms: the next items start at the key picture at or before it
     * (the engine discards what comes before ms). 0, or -1 with text. */
    int (*seek)(void *ctx, int64_t ms, char *text, size_t textlen);
    /* The next picture (PICTURE, EOF, AGAIN or ERROR). Never returns without
     * an item; may take as long as one decode step. */
    void (*next_picture)(void *ctx, struct video_item *item);
    /* The next sound (AUDIO, EOF, AGAIN or ERROR), mono at VIDEO_AUDIO_RATE.
     * A file opened without want_audio, or without a decodable sound track,
     * answers EOF. */
    void (*next_audio)(void *ctx, struct video_item *item);
    /* Convert the picture the last next_picture() returned into dst (w x h
     * RGB565 at the output size, stride w * 2). 0, or -1 when there is none.
     * The picture is held until then, or until the next next_picture() or
     * seek drops it. */
    int (*picture)(void *ctx, uint16_t *dst, uint32_t w, uint32_t h);
    void (*close)(void *ctx);
};

const struct video_backend *video_backend_fake(void);
#ifdef POCKETVIDEO_HAVE_FFMPEG
const struct video_backend *video_backend_ffmpeg(void);
#endif

/* A backend by name ("ffmpeg", "fake"), or NULL when this build lacks it. */
const struct video_backend *video_backend_by_name(const char *name);

#endif
