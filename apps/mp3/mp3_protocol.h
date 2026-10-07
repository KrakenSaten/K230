/*
 * What the MP3 app and its helper, pos-mp3, agree on: the words of the event
 * lines, the commands, the exit codes and the error codes. Shared, so the two
 * cannot drift; the helper is the only side that decodes or opens audio
 * (docs/apps/MP3.md).
 *
 * One helper process plays one file, from `pos-mp3 play` to the file's end,
 * a stop, or an error.
 *
 * EVENTS, helper to app, one line each on stdout:
 *
 *   ready <board>               the audio device is open
 *   recovered                   a dead owner's audio route was restored first
 *   meta title <text>           the file's own title tag, if it has one
 *   meta artist <text>          the file's own artist tag, if it has one
 *   playing <total_ms> <seekable> <rate> <channels> <codec>
 *                               decoding started; <total_ms> 0 when the
 *                               length is not known, <seekable> 0 or 1,
 *                               <rate> and <channels> are the file's own,
 *                               <codec> a lower-case word ("mp3")
 *   progress <ms>               the position heard, every 250 ms and after
 *                               every seek
 *   paused / resumed            the device was closed / opened again
 *   played                      the file played to its end
 *   stopped                     ended on request, device closed
 *   error <code> <text>         <code> is one of MP3_ERR_*
 *
 * Every event comes before the helper exits, so an app that has read up to
 * the exit knows how it ended: "played", "stopped", or "error".
 *
 * COMMANDS, app to helper, one line each on stdin:
 *
 *   pause, resume, stop
 *   seek <ms>                   to that position (clamped to the length)
 *   volume <percent>            1..100, the system volume, applied at once
 *
 * SIGTERM and a closed stdin are stops as well, so a helper whose app is gone
 * closes the device and exits.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETMP3_PROTOCOL_H
#define POCKETMP3_PROTOCOL_H

#define MP3_PROGRESS_EVERY_MS 250

/* The longest title or artist the helper reports, in bytes of UTF-8 (cut on
 * a character boundary). */
#define MP3_META_MAX 128

/* Exit codes. */
#define MP3_EXIT_OK 0
#define MP3_EXIT_USAGE 2
#define MP3_EXIT_AUDIO 3
#define MP3_EXIT_FILE 4   /* missing, unreadable, or the storage went away */
#define MP3_EXIT_FORMAT 5 /* not audio, not supported, damaged, or empty */

/* Error codes on the error line. */
#define MP3_ERR_USAGE "usage"
#define MP3_ERR_AUDIO "audio"
#define MP3_ERR_AUDIO_BUSY "audio_busy"
#define MP3_ERR_AUDIO_NODEV "audio_nodev"
#define MP3_ERR_AUDIO_DISABLED "audio_disabled"
#define MP3_ERR_MISSING "missing"   /* the file is not there */
#define MP3_ERR_STORAGE "storage"   /* it could not be read (I/O error, storage removed) */
#define MP3_ERR_FORMAT "format"     /* not a playable audio file, or an empty one */
#define MP3_ERR_DECODE "decode"     /* it stopped decoding part way */

#endif
