/*
 * What the Wave app and its helper (pos-wave) agree on: the message limit,
 * the transmit profiles, the listening bound, and the words and exit codes
 * the helper uses to say what went wrong. Both sides include this file and
 * nothing else of each other.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_PROTOCOL_H
#define POCKETWAVE_PROTOCOL_H

/* The longest message, in UTF-8 bytes. ggwave itself accepts up to 140
 * (variable-length mode); 64 keeps the longest transmission on the slowest
 * profile to a few seconds (docs/apps/WAVE.md, "Limits"). */
#define WAVE_MAX_MESSAGE_BYTES 64

/* ggwave's volume argument for the app's transmissions. Its peak sample
 * value is measured, not assumed (docs/apps/WAVE.md): pocketaudio clamps
 * whatever this produces to its own ceiling anyway. */
#define WAVE_DEFAULT_VOLUME 10

/* A listening session ends by itself after this long, so a microphone left
 * on by a forgotten screen is bounded even while the app stays open. */
#define WAVE_LISTEN_SECONDS 120

/* A capture (`pos-wave record`, decoded afterwards with `pos-wave decode`)
 * is at most this long: the helper's own bound (RECORD_MAX_SECONDS), whose
 * buffer is 2.9 MB at that length. */
#define WAVE_CAPTURE_MAX_SECONDS 30

/* The transmit profiles the app offers: ggwave's three audible protocols.
 * Their names are the helper's --protocol words. Ultrasound, dual-tone and
 * mono-tone protocols are left out of the app until the speaker and the
 * microphone have been measured (docs/apps/WAVE.md). A receiver needs no
 * choice: it listens for all of them. */
enum wave_profile {
    WAVE_PROFILE_NORMAL = 0,
    WAVE_PROFILE_FAST,
    WAVE_PROFILE_FASTEST,
    WAVE_PROFILE_COUNT
};

#define WAVE_PROFILE_NAMES { "audible_normal", "audible_fast", "audible_fastest" }
#define WAVE_PROFILE_LABELS { "NORMAL", "FAST", "FASTEST" }
#define WAVE_DEFAULT_PROFILE WAVE_PROFILE_FAST

/* pos-wave exit codes. */
#define WAVE_EXIT_OK 0
#define WAVE_EXIT_FAILED 1      /* the operation ran and failed (see its error event) */
#define WAVE_EXIT_USAGE 2       /* bad arguments or input */
#define WAVE_EXIT_AUDIO 3       /* the audio device could not be opened or used */

/* The first word of an "error" event. */
#define WAVE_ERR_USAGE "usage"
#define WAVE_ERR_TOO_LONG "too_long"
#define WAVE_ERR_INVALID_TEXT "invalid_text"
#define WAVE_ERR_ENCODE "encode"
#define WAVE_ERR_DECODE "decode"
#define WAVE_ERR_AUDIO_DISABLED "audio_disabled"
#define WAVE_ERR_AUDIO_BUSY "audio_busy"
#define WAVE_ERR_AUDIO_NODEV "audio_nodev"
#define WAVE_ERR_AUDIO "audio"

#endif
