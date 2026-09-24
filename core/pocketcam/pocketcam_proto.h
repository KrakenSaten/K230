/*
 * The link between the Camera app's session (apps/camera/camera_session.c)
 * and the pos-camera helper (tools/camera/pos_camera.c): the shared memory the
 * pictures travel in, and the lines the two exchange.
 *
 * TRANSPORT. The session starts `pos-camera session ...` with a socketpair on
 * stdin and stdout and a sealed memfd on descriptor 3. Commands go in one per
 * line, events come out one per line; pixels never go through the socket.
 *
 * SHARED MEMORY. A header page, then POCKETCAM_SLOTS slots of
 * POCKETCAM_SLOT_BYTES, each able to hold one RGB565 picture of up to
 * POCKETCAM_VIEW_MAX_W x POCKETCAM_VIEW_MAX_H. The session creates it, writes
 * the header, seals its size (so the helper cannot shrink it under the shell's
 * mapping, which would crash the shell on its next read) and maps it
 * read-only. Slots 0..2 carry preview frames, slot 3 the review picture of the
 * last capture.
 *
 * OWNERSHIP OF A SLOT is passed by messages, never shared: the helper writes a
 * slot only while it owns it, announces it with `frame`, and does not touch it
 * again until the session answers `release`. The session copies a frame out
 * in the same timer tick it reads the event and releases it at once, so the
 * app never holds a pointer into this memory at all.
 *
 * COMMANDS (session to helper):
 *   view <w> <h> <portrait|landscape>   the size and orientation of the preview
 *   start                               stream the preview
 *   stop                                stop it; answered by `stopped`
 *   release <slot>                      the session is done with a slot
 *   capture <portrait|landscape>        take, save and review one still
 *   delete <name>                       delete a photo from the store
 *   quit                                close the camera and leave; `bye`
 *
 * EVENTS (helper to session):
 *   hello <version> <backend>           first line, before the camera is opened
 *   ready <name> <still_w> <still_h> <simulated 0|1> <photos> <last|->
 *   nodevice <text>                     there is no camera; the helper leaves
 *   error <what> <text>                 it cannot go on; the helper leaves
 *   frame <slot> <seq> <w> <h>          a preview picture is in slot
 *   stall <ms>                          streaming, but no frame for ms
 *   malformed <seq>                     the driver delivered a damaged frame
 *   stopped
 *   saving                              the still is taken, it is being written
 *   captured <slot> <w> <h> <bytes> <name> <photos>
 *   capfail <nospace|quota|device|io> <text>
 *   deleted <name> <photos>
 *   delfail <text>
 *   lost <text>                         the camera went away; the helper leaves
 *   bye
 *
 * Unknown lines are ignored by both sides, so either can learn a word the
 * other does not know yet. A line longer than POCKETCAM_LINE_MAX is a
 * protocol error.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_PROTO_H
#define POCKETOS_POCKETCAM_PROTO_H

#include <stdint.h>

#define POCKETCAM_PROTO_VERSION 1
#define POCKETCAM_LINE_MAX 256

#define POCKETCAM_SHM_MAGIC 0x4d414350u /* "PCAM" */
#define POCKETCAM_SHM_FD 3
#define POCKETCAM_SLOTS 4
#define POCKETCAM_PREVIEW_SLOTS 3
#define POCKETCAM_REVIEW_SLOT 3
/* The largest picture a slot holds: more than the preview needs in either
 * orientation on the reference panel (at most 528 x 939 portrait and 802 x 452
 * landscape, camera_layout.c). */
#define POCKETCAM_VIEW_MAX_W 1024
#define POCKETCAM_VIEW_MAX_H 1024
#define POCKETCAM_SLOT_BYTES (POCKETCAM_VIEW_MAX_W * POCKETCAM_VIEW_MAX_H * 2)
#define POCKETCAM_SHM_HEADER 4096
#define POCKETCAM_SHM_BYTES (POCKETCAM_SHM_HEADER + POCKETCAM_SLOTS * POCKETCAM_SLOT_BYTES)

/* The preview is sent at most this often. A frame that arrives sooner is
 * dropped before it is converted, so the cap also caps the CPU the helper
 * spends. Correctness first: raise it only after unit A has been measured. */
#define POCKETCAM_PREVIEW_MIN_INTERVAL_MS 100
/* Streaming with no frame for this long is reported as a stall, once per
 * interval, so the screen can say it is waiting. */
#define POCKETCAM_STALL_MS 1000
/* How long the helper waits for the backend's still. */
#define POCKETCAM_STILL_TIMEOUT_MS 5000
/* This many damaged frames in a row mean the camera is not working. */
#define POCKETCAM_MALFORMED_LIMIT 10

struct pocketcam_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t slots;
    uint32_t slot_bytes;
    uint32_t max_w;
    uint32_t max_h;
};

static inline uint32_t pocketcam_slot_offset(uint32_t slot)
{
    return POCKETCAM_SHM_HEADER + slot * (uint32_t)POCKETCAM_SLOT_BYTES;
}

#endif
