/*
 * pos-video's player engine: one open file, the pictures' way into the
 * shared-memory slots at the right moment, and the sound's way to the
 * speaker. The protocol it speaks is apps/video/video_proto.h; pos_video.c
 * only moves lines between it and the socket.
 *
 * TIME. The picture follows a clock. With sound playing, the clock is the
 * sound: the position of the last sample handed to pocketaudio, less what
 * is still queued in the device, extrapolated by at most one period between
 * writes. Without sound (none in the file, muted, the card busy, or the sound
 * track has ended) it is the monotonic clock, re-anchored wherever the sound
 * left off, so a clip whose sound stops early keeps its pictures moving.
 * Starting to play waits up to VIDEO_AUDIO_START_MS for the first sound
 * before the clock runs.
 *
 * PICTURES. A picture is decoded ahead only into a free slot, converted there
 * while the helper owns it, and announced (`frame`) when its time comes. When
 * two are due at once the older is dropped unseen; a picture decoded more
 * than VIDEO_LATE_DROP_MS behind the clock is dropped before it is converted,
 * unless nothing has been shown for VIDEO_SHOW_ANYWAY_MS (a decoder too slow
 * for the file still shows something). A slot announced to the session is
 * untouched until `release`.
 *
 * SOUND. A thread of its own, so a picture being decoded never starves the
 * device (the K230's playback buffer is 80 ms): it owns the pocketaudio stream
 * - opens it on play, closes it on pause, stop, seek and end, which also
 * releases the audio lock and turns the amplifier off - and takes samples from
 * a bounded ring (VIDEO_AUDIO_RING_SAMPLES) the main thread fills. The main
 * thread never calls pocketaudio. A card that cannot be opened (busy, missing)
 * is reported once (`audio busy`, `audio error`) and the picture plays on.
 *
 * SEEK re-positions the backend at the key picture before the target and
 * discards pictures and samples before it; the first picture at or after the
 * target is shown at once (`seeked`), playing or paused. Only the newest
 * target counts. `view` changes the picture size the same way, from the
 * current position.
 *
 * No LVGL, no socket: tested on a host with the fake backend and a scripted
 * sound card (tests/video_player_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VIDEO_PLAYER_H
#define POCKETOS_VIDEO_PLAYER_H

#include "pocketaudio/pocketaudio.h"
#include "video_backend.h"

#include <stdint.h>

/* Four seconds of mono 48 kHz sound decoded ahead at most. */
#define VIDEO_AUDIO_RING_SAMPLES (4 * VIDEO_AUDIO_RATE)
/* play waits this long for the first sound before the clock runs anyway. */
#define VIDEO_AUDIO_START_MS 500
/* A sound clock older than this is not believed (the thread is stuck or the
 * track is over): the monotonic clock carries on from it. */
#define VIDEO_AUDIO_FRESH_MS 150
/* Between two writes the sound clock is extrapolated by at most this. */
#define VIDEO_AUDIO_EXTRAPOLATE_MS 40
/* A late picture is shown anyway when nothing has been for this long. */
#define VIDEO_SHOW_ANYWAY_MS 250
/* Backend calls per step at most, so commands are read between them. */
#define VIDEO_STEP_DECODES 6
/* How long step() lets the caller sleep, at most. */
#define VIDEO_STEP_MAX_WAIT_MS 50

struct video_player_config {
    const struct video_backend *backend;
    /* The sound card: NULL plays no sound at all (reported as muted). */
    const struct pocketaudio_backend *audio_backend;
    const struct pocketaudio_board *audio_board; /* NULL: detect */
    const char *audio_lock_dir;                  /* NULL: the runtime directory */
    int allow_unverified;
    int volume_percent;                          /* 0: muted, no sound opened */
    uint8_t *shm;                                /* the writable mapping */
    /* Emit one event line (no newline). */
    void (*emit)(void *user, const char *line);
    void *user;
    /* Monotonic milliseconds; NULL: CLOCK_MONOTONIC. The audio thread reads
     * it too. */
    int64_t (*now_ms)(void);
};

struct video_player_counters {
    uint64_t shown;
    uint64_t dropped;
    uint64_t late;
    uint64_t decoded;
    unsigned xruns;
};

struct video_player;

int video_player_create(struct video_player **out, const struct video_player_config *cfg);
/* Stops the sound thread (closing the card), closes the file. NULL is fine. */
void video_player_destroy(struct video_player *p);

/* One command line (no newline). Returns 1 for quit, else 0. */
int video_player_command(struct video_player *p, const char *line);

/* Do what is due: announce pictures, decode ahead, notice the end, emit
 * progress. Returns how long the caller may wait before the next step (0 when
 * there is more to do now), at most VIDEO_STEP_MAX_WAIT_MS. */
int video_player_step(struct video_player *p);

void video_player_counters(const struct video_player *p, struct video_player_counters *out);

/* The picture size for a file of src_w x src_h shown in a view_w x view_h box:
 * fitted whole, never enlarged, width a multiple of VIDEO_ALIGN_W and height of
 * VIDEO_ALIGN_H, within the slot limits. 0, or -1 when nothing fits. */
int video_fit(uint32_t src_w, uint32_t src_h, uint32_t view_w, uint32_t view_h, uint32_t *w,
              uint32_t *h);

#endif
