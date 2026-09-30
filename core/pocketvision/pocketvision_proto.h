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
 *   mode detect|track|traffic|color|edge|trace
 *                                 what to look for: everything the model
 *                                 knows (detect and track run the same
 *                                 pipeline; track is the screen's ids and
 *                                 line); traffic only (car, truck, bus,
 *                                 motorcycle, bicycle, person), counted per
 *                                 class and timed between the speed lines;
 *                                 or one of the pixel modes, in which the
 *                                 detector idles and the preview picture
 *                                 is worked on before it is shown
 *   color <r> <g> <b> | color off the COLOR target (8-bit), or none
 *   sample <x> <y>                COLOR: take the target from the next
 *                                 preview at this view point
 *   tol <n>                       COLOR: the tolerance, 0..765
 *   edge <threshold>              EDGE: 0 for grey magnitudes, 1..255 for
 *                                 black and white at the threshold
 *   trace dark|light              TRACE: the line to look for
 *   line <x0> <y0> <x1> <y1>      the counting line, in per-mille of the
 *                                 view (0..1000 each), or `line off`
 *   speed <ax0> <ay0> <ax1> <ay1> <bx0> <by0> <bx1> <by1>
 *                                 the two speed lines, A then B, per-mille
 *                                 of the view, or `speed off`
 *   distance <cm>                 the ground distance between the speed
 *                                 lines
 *   range near|normal|far         TRAFFIC's detection range, a pipeline
 *                                 preset (vision_range.h); other modes
 *                                 always detect as normal
 *   reset                         counts and speeds to zero, tracks
 *                                 forgotten
 *   quit                          close the camera and leave; `bye`
 *
 * EVENTS (helper to session):
 *   hello <version> <backend> <kpu>
 *                                 first line: the camera backend and the
 *                                 detector backend ("nncase" or "fake")
 *   ready <name> <pw> <ph> <simulated 0|1> <model> <in_w> <in_h> <classes>
 *                                 the camera (its preview size) and the
 *                                 model are open
 *   caps <mode>...                right after ready: the modes this helper
 *                                 can run, by their `mode` words (a mode
 *                                 whose model is not on the unit is not
 *                                 listed, and the screen never offers it)
 *   nodevice <text>               there is no camera; the helper leaves
 *   nomodel <text>                the model could not be opened; the
 *                                 helper leaves
 *   error <what> <text>           it cannot go on; the helper leaves
 *   frame <slot> <seq> <w> <h>    a preview picture is in slot
 *   det <seq> <n> [<id>:<cls>:<conf>:<x>:<y>:<w>:<h>:<dir>:<kmh10>]...
 *                                 what is tracked after frame seq: id 0 for
 *                                 a track not yet confirmed; conf per-mille;
 *                                 the box in view pixels; dir the way it
 *                                 has gone on the picture (0 unknown, 1
 *                                 left, 2 right, 3 up, 4 down); kmh10 the
 *                                 speed measured on it, x10, 0 for none; n
 *                                 of them, at most VISION_MAX_SHOWN
 *   count <ab> <ba>               the line's crossing counts, when they
 *                                 change and on reset
 *   traffic <ab> <ba> <cur> <last> <max> <mean> <n> <rejected> <c0ab>:<c0ba> ... <c5ab>:<c5ba>
 *                                 in traffic mode, when it changes: the
 *                                 count line's totals (ab is IN), the
 *                                 current, last, highest and mean speed in
 *                                 km/h x10 with the measurements in the
 *                                 mean and the refusals, and the counts per
 *                                 traffic class (car, truck, bus,
 *                                 motorcycle, bicycle, person)
 *   recent <window_s> <crossed> <ab> <ba> <c0> .. <c5> <speeds> <mean_kmh10> <saturated>
 *                                 in traffic mode, after an event and once
 *                                 a second: the last window's crossings
 *                                 each way and per traffic class, the
 *                                 speeds measured and their mean, and 1
 *                                 when more happened than the window
 *                                 holds (vision_window.h)
 *   color <r> <g> <b> <matched_pm> <cx> <cy>
 *                                 COLOR, with every preview: the target,
 *                                 the share of the picture that matched
 *                                 (per-mille) and the matches' centroid in
 *                                 view pixels (-1 -1 with none)
 *   edge <strong_pm>              EDGE, with every preview: the share of
 *                                 strong edges
 *   trace <found> <offset_pm> <slope_pm> <rows>
 *                                 TRACE, with every preview: whether a
 *                                 line was found, the bottom band's offset
 *                                 from the centre (-1000..1000), the lean
 *                                 (dx per 1000 rows going down), rows with
 *                                 a plausible run
 *   stats <fps_x10> <infer_ms> <pre_ms> <post_ms> <cpu_pct> <rss_kb> <bad> <dropped>
 *                                 once a second while streaming: frames
 *                                 inferred per second (previews worked on,
 *                                 in a pixel mode), the last run's
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

#define VISION_PROTO_VERSION 2
#define VISION_LINE_MAX 2048
/* Boxes on one det line: 24 of at most 52 characters fit VISION_LINE_MAX
 * with the head to spare. */
#define VISION_MAX_SHOWN 24
#define VISION_STATS_INTERVAL_MS 1000

/* The traffic classes a `traffic` line counts, in order: car, truck, bus,
 * motorcycle, bicycle, person (vision_traffic.h agrees). */
#define VISION_PROTO_TRAFFIC_CLASSES 6

/* The direction a track has gone on the picture (det lines). */
#define VISION_DIR_NONE 0
#define VISION_DIR_LEFT 1
#define VISION_DIR_RIGHT 2
#define VISION_DIR_UP 3
#define VISION_DIR_DOWN 4

#endif
