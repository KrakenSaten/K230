/*
 * What the Recorder app and its helper, pos-record, agree on: the words of
 * the event lines, the commands, the exit codes and the two recording
 * formats. Shared, so the two cannot drift; the helper is the only side that
 * opens audio or writes a recording (docs/apps/RECORDER.md).
 *
 * EVENTS, helper to app, one line each on stdout:
 *
 *   ready <board>               the audio device is open
 *   recovered                   a dead owner's audio route was restored first
 *   recording <rate> <name>     capture running, writing <name>.part
 *   level <peak> <rms>          0..32767 each, measured on the samples being
 *                               written (recording) or played; every 100 ms
 *   progress <ms> <bytes>       recording: audio written and file size;
 *                               playback: position; every 250 ms
 *   paused / resumed            the device was closed / opened again
 *   limit space|length|time     recording stopped itself: storage reached
 *                               its reserve, the WAV size limit, or the
 *                               --seconds given
 *   saved <ms> <bytes> <gaps> <name>
 *                               finalized under <name>; <gaps> counts capture
 *                               overruns (audio lost inside the recording)
 *   empty                       stopped before any audio arrived; no file
 *   kept <name>                 finalizing failed; the audio is in
 *                               <name>.part and will be repaired next time
 *   playing <total_ms> <rate> <channels>
 *   played                      playback reached the end of the file
 *   stopped                     ended on request, device closed
 *   repaired <name>             recover: an interrupted recording was saved
 *   damaged <name>              recover: a .part that could not be repaired
 *   error <code> <text>         <code> is one of REC_ERR_*
 *
 * COMMANDS, app to helper, one line each on stdin: pause, resume, stop.
 * SIGTERM and a closed stdin are stops as well, so a helper whose app is
 * gone finalizes and exits.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_PROTOCOL_H
#define POCKETREC_PROTOCOL_H

/* The two presets. Voice is the default: the codec runs at 48 kHz only
 * (AUDIO_HARDWARE_MAP §11), and a 3:1 decimation to 16 kHz makes a minute
 * a third of the size, which matters on a root filesystem with about 120 MB
 * free. Standard is the native rate, no resampling. Both are mono: the
 * second wire channel is the empty headset input. */
#define REC_RATE_VOICE 16000
#define REC_RATE_STANDARD 48000

/* The filesystem keeps at least this much free: a recording never starts
 * below it plus REC_START_MARGIN_S of audio, and stops itself on reaching
 * it. The same reserve as the Camera's photo store, for the same reason: the
 * root filesystem also holds the settings, the logs and every app's state. */
#define REC_RESERVE_BYTES (48ull << 20)
#define REC_START_MARGIN_S 10

#define REC_LEVEL_EVERY_MS 100
#define REC_PROGRESS_EVERY_MS 250

/* Exit codes. */
#define REC_EXIT_OK 0
#define REC_EXIT_USAGE 2
#define REC_EXIT_AUDIO 3
#define REC_EXIT_STORAGE 4
#define REC_EXIT_FAILED 5

/* Error codes on the error line. */
#define REC_ERR_USAGE "usage"
#define REC_ERR_AUDIO "audio"
#define REC_ERR_AUDIO_BUSY "audio_busy"
#define REC_ERR_AUDIO_NODEV "audio_nodev"
#define REC_ERR_AUDIO_DISABLED "audio_disabled"
#define REC_ERR_STORAGE_FULL "storage_full"
#define REC_ERR_STORAGE "storage"
#define REC_ERR_FORMAT "format"

#endif
