/*
 * The link between the Vision app's session (apps/vision/vision_session.c)
 * and the pos-vision helper (tools/vision/pos_vision.c).
 *
 * TRANSPORT and SHARED MEMORY are Camera's, unchanged
 * (core/pocketcam/pocketcam_proto.h): a socketpair on stdin and stdout, one
 * line per message each way, and a sealed memfd on descriptor 3 laid out as
 * POCKETCAM_SLOTS slots of POCKETCAM_SLOT_BYTES with the same header. The
 * helper writes a preview picture into a slot it owns, announces it with
 * `frame`, and touches it again only after `release`. Slot 3 is unused.
 * Lines here may be longer than Camera's: a detection line carries up to
 * VISION_MAX_SHOWN boxes.
 *
 * COMMANDS (session to helper):
 *   view <w> <h> <rotation>       the preview's size and the display's
 *                                 rotation; boxes come in these pixels
 *   start                         stream, detect, track, count
 *   stop                          stop; answered by `stopped`
 *   release <slot>                the session is done with a slot
 *   line <x0> <y0> <x1> <y1>      the counting line, in per-mille of the
 *                                 view (0..1000 each), or `line off`
 *   reset                         counts to zero, tracks forgotten
 *   quit                          close the camera and leave; `bye`
 *
 * EVENTS (helper to session):
 *   hello <version> <backend> <kpu>
 *                                 first line: the camera backend and the
 *                                 detector backend ("nncase" or "fake")
 *   ready <name> <pw> <ph> <simulated 0|1> <model> <in_w> <in_h> <classes>
 *                                 the camera (its preview size) and the
 *                                 model are open
 *   nodevice <text>               there is no camera; the helper leaves
 *   nomodel <text>                the model could not be opened; the
 *                                 helper leaves
 *   error <what> <text>           it cannot go on; the helper leaves
 *   frame <slot> <seq> <w> <h>    a preview picture is in slot
 *   det <seq> <n> [<id>:<cls>:<conf>:<x>:<y>:<w>:<h>]...
 *                                 what is tracked after frame seq: id 0 for
 *                                 a track not yet confirmed; conf per-mille;
 *                                 the box in view pixels; n of them, at most
 *                                 VISION_MAX_SHOWN
 *   count <ab> <ba>               the line's crossing counts, when they
 *                                 change and on reset
 *   stats <fps_x10> <infer_ms> <pre_ms> <post_ms> <cpu_pct> <rss_kb> <bad> <dropped>
 *                                 once a second while streaming: frames
 *                                 inferred per second, the last run's
 *                                 timings, the helper's own CPU share and
 *                                 resident set, tensors refused as
 *                                 malformed so far, and detections dropped
 *                                 for lack of a track slot
 *   malformed <n>                 the driver delivered a damaged frame (n in
 *                                 a row); or the model output was not a
 *                                 tensor of the declared shape (see `bad`)
 *   stall <ms>                    streaming, but no frame for ms
 *   stopped
 *   lost <text>                   the camera went away; the helper leaves
 *   bye
 *
 * Unknown lines are ignored by both sides. A line longer than
 * VISION_LINE_MAX is a protocol error.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETVISION_PROTO_H
#define POCKETOS_POCKETVISION_PROTO_H

#include "pocketcam/pocketcam_proto.h"

#define VISION_PROTO_VERSION 1
#define VISION_LINE_MAX 1024
/* Boxes on one det line: 24 of at most 40 characters fit VISION_LINE_MAX
 * with the head to spare. */
#define VISION_MAX_SHOWN 24
#define VISION_STATS_INTERVAL_MS 1000

#endif
