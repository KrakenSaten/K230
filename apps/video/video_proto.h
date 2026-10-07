/*
 * The link between the Video app's session (apps/video/video_session.c) and
 * the pos-video helper (tools/video/pos_video.c): the shared memory the
 * pictures travel in, and the lines the two exchange. Shared, so the two
 * cannot drift. The helper is the only side that opens a video file, the
 * hardware decoder or the sound card (docs/decisions/ADR-012-video-playback.md).
 *
 * TRANSPORT. The session starts `pos-video session ...` with a socketpair on
 * stdin and stdout and a sealed memfd on descriptor 3 - the same shape as
 * Camera's (core/pocketcam/pocketcam_proto.h). Commands go in one per line,
 * events come out one per line; pixels never go through the socket. One
 * helper plays one file: it is started when a file is chosen and ended when
 * the player is left, so nothing a damaged file did outlives its playback.
 *
 * SHARED MEMORY. A header page, then VIDEO_SLOTS slots of VIDEO_SLOT_BYTES,
 * each able to hold one RGB565 picture of up to VIDEO_VIEW_MAX_PIXELS pixels
 * (and at most VIDEO_VIEW_MAX_W x VIDEO_VIEW_MAX_H). The session creates it,
 * writes the header, seals its size (so the helper cannot shrink it under the
 * shell's mapping) and maps it read-only.
 *
 * OWNERSHIP OF A SLOT is passed by messages, never shared: the helper writes a
 * slot only while it owns it, announces it with `frame` when the picture is
 * due on screen, and does not touch it again until the session answers
 * `release`. The session copies the newest frame out in the same timer tick it
 * reads the event and releases it at once; an older frame not taken yet is
 * released unseen. The app never holds a pointer into this memory.
 *
 * COMMANDS (session to helper):
 *   view <w> <h>          the box pictures are fitted into (letterboxed,
 *                         never enlarged); allowed before and after open
 *   open <path>           the rest of the line is the path; answered by
 *                         `opened` or `openfail`. Playback starts paused on
 *                         the first picture.
 *   play                  answered by `state playing <ms>`
 *   pause                 answered by `state paused <ms>`
 *   seek <ms>             answered by `seeked <ms>` (then a frame)
 *   stop                  pause and go back to the start: `state stopped 0`
 *   release <slot>        the session is done with a slot
 *   quit                  close everything and leave; `bye`
 *
 * EVENTS (helper to session):
 *   hello <version> <backend>
 *   opened <duration_ms> <src_w> <src_h> <fps_x100> <audio> <codec>
 *                         audio is one of VIDEO_AUDIO_*: none (the file has
 *                         no sound), on, muted (volume 0), busy (another
 *                         program owns the sound card), unsupported (a sound
 *                         codec this build cannot decode), error
 *   openfail <reason> <text>
 *                         reason is one of VIDEO_OPENFAIL_*; the helper
 *                         stays and can be told to open another file
 *   frame <slot> <seq> <w> <h> <pts_ms>
 *   state <playing|paused|stopped|ended> <ms>
 *   pos <ms>              while playing, every VIDEO_POS_EVERY_MS
 *   seeked <ms>
 *   audio <word>          the sound changed while playing (it failed:
 *                         `audio error`); the picture goes on
 *   stats <fps_x10> <shown> <dropped> <late> <cpu_pct> <rss_kb> <xruns>
 *                         once a second while playing: pictures per second
 *                         put on screen, and since open the pictures shown,
 *                         dropped (decoded too late to be worth showing) and
 *                         shown late; the helper's CPU share and resident set
 *   error <decode|device|io> <text>
 *                         playback cannot go on; the file is closed and the
 *                         helper waits for another open or quit
 *   bye
 *
 * Unknown lines are ignored by both sides, so either can learn a word the
 * other does not know yet. A line longer than VIDEO_LINE_MAX is a protocol
 * error, and so is a picture event that does not add up.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VIDEO_PROTO_H
#define POCKETOS_VIDEO_PROTO_H

#include <stdint.h>

#define VIDEO_PROTO_VERSION 1
/* A path of up to VIDEO_PATH_MAX - 1 bytes fits in an open line. */
#define VIDEO_PATH_MAX 256
#define VIDEO_LINE_MAX (VIDEO_PATH_MAX + 16)

#define VIDEO_SHM_MAGIC 0x44495650u /* "PVID" */
#define VIDEO_SHM_FD 3
/* One on screen (the session's), one due next, one being converted, one
 * spare so the decoder never waits on the screen. */
#define VIDEO_SLOTS 4
/* The largest picture: the whole reference panel in either orientation fits
 * (1232 x 568 landscape, 568 x 1232 portrait) and so does 1280 x 720. */
#define VIDEO_VIEW_MAX_W 1280
#define VIDEO_VIEW_MAX_H 1280
#define VIDEO_VIEW_MAX_PIXELS (1280u * 720u)
#define VIDEO_SLOT_BYTES (VIDEO_VIEW_MAX_PIXELS * 2u)
#define VIDEO_SHM_HEADER 4096u
#define VIDEO_SHM_BYTES (VIDEO_SHM_HEADER + VIDEO_SLOTS * VIDEO_SLOT_BYTES)

/* The helper sizes pictures to these multiples: the K230 decoder's scaler
 * leaves a stripe of unset chroma at the right of an odd width (unit B,
 * 2026-09-29: 1009 x 567 has a green right column, 1008 x 568 does not). */
#define VIDEO_ALIGN_W 8
#define VIDEO_ALIGN_H 2

/* How often `pos` is sent while playing. */
#define VIDEO_POS_EVERY_MS 250
#define VIDEO_STATS_EVERY_MS 1000
/* A decoded picture this late against the clock is dropped, not shown. */
#define VIDEO_LATE_DROP_MS 100
/* The longest file the progress arithmetic accepts: 24 hours. */
#define VIDEO_DURATION_MAX_MS (24ll * 3600 * 1000)

/* Audio words on `opened` and `audio`. */
#define VIDEO_AUDIO_NONE "none"
#define VIDEO_AUDIO_ON "on"
#define VIDEO_AUDIO_MUTED "muted"
#define VIDEO_AUDIO_BUSY "busy"
#define VIDEO_AUDIO_UNSUPPORTED "unsupported"
#define VIDEO_AUDIO_ERROR "error"

/* Reasons on `openfail`. */
#define VIDEO_OPENFAIL_MISSING "missing"         /* no such file, or not a file */
#define VIDEO_OPENFAIL_UNSUPPORTED "unsupported" /* a container or codec v0.1 does not play */
#define VIDEO_OPENFAIL_CORRUPT "corrupt"         /* it claims to be a video and is not one */
#define VIDEO_OPENFAIL_DEVICE "device"           /* the decoder could not be opened */
#define VIDEO_OPENFAIL_IO "io"                   /* reading failed */

struct video_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t slots;
    uint32_t slot_bytes;
    uint32_t max_w;
    uint32_t max_h;
    uint32_t max_pixels;
};

static inline uint32_t video_slot_offset(uint32_t slot)
{
    return VIDEO_SHM_HEADER + slot * (uint32_t)VIDEO_SLOT_BYTES;
}

#endif
