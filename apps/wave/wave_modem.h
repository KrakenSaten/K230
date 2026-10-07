/*
 * Wave's modem: ggwave behind a small C interface that cannot be used the
 * ways ggwave itself can be misused.
 *
 * ggwave (vendor/ggwave, pinned in platforms/k230/vendor_ggwave_commit.txt)
 * turns up to 140 bytes into multi-tone FSK audio and back. Reading its source
 * at the pinned commit found five things a caller has to get right, and this
 * wrapper is where each is got right once (docs/apps/WAVE.md, "ggwave"):
 *
 *   1. Its decoder loses frame sync for good when it is fed a chunk that is
 *      not a whole number of 1024-sample frames (ggwave.cpp:1150-1190, the
 *      non-resampling path never adds the offset). The decoder here buffers
 *      whatever it is given and hands ggwave exact frames only.
 *   2. ggwave_init() returns a usable-looking id even when its parameters
 *      were rejected, and such an instance can crash in decode. The C++
 *      prepare() does report failure, so instances are built with it and a
 *      failure is an error here.
 *   3. It logs to stderr by default, including every decoded payload. It is
 *      compiled with GGWAVE_DISABLE_LOG and its log file is set to none.
 *   4. After a start marker whose end marker is never heard it records for up
 *      to 38.5 s before giving up, and is deaf meanwhile. A watchdog here
 *      stops a reception that has lasted longer than the longest message Wave
 *      accepts (WAVE_MODEM_WATCHDOG_FRAMES) and starts a fresh receiver.
 *   5. One instance that both sends and receives couples the two (an encode
 *      resets a reception). Encoder and decoder are separate instances, which
 *      costs no more memory, and each has only its own half allocated.
 *
 * Also: volume 100 wraps int16 in ggwave's own output, so volume is capped at
 * WAVE_MODEM_MAX_VOLUME, whose peak (about 7980 of 32767 for the audible
 * protocols) stays under pocketaudio's ceiling; and payloads longer than the
 * limit are refused here instead of being truncated silently by ggwave.
 *
 * Fixed numbers at 48 kHz and 1024 samples per frame (46.875 Hz per bin, the
 * ggwave default, so Wave can talk to other ggwave programs): the audible
 * protocols use 1875.0 - 6328.1 Hz. Memory: about 9.2 MB for an encoder and
 * 8.4 MB for a decoder, allocated once at creation, nothing after.
 *
 * Not thread-safe: ggwave's protocol tables are process-global. pos-wave is
 * single-threaded and creates one encoder or one decoder.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_MODEM_H
#define POCKETWAVE_MODEM_H

#include "wave_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAVE_MODEM_RATE 48000
#define WAVE_MODEM_FRAME 1024
#define WAVE_MODEM_MAX_VOLUME 25

/* The longest waveform Wave can produce: WAVE_MAX_MESSAGE_BYTES on the
 * slowest profile (311 frames). */
#define WAVE_MODEM_MAX_FRAMES 311
#define WAVE_MODEM_MAX_SAMPLES (WAVE_MODEM_MAX_FRAMES * WAVE_MODEM_FRAME)
/* A reception still open this many frames after its start marker cannot be
 * a message Wave accepts: the longest one plus one marker's worth of slack. */
#define WAVE_MODEM_WATCHDOG_FRAMES (WAVE_MODEM_MAX_FRAMES + 16)

/* Frames and samples a message of len bytes takes on a profile, from
 * ggwave's own formula (16 marker frames either side, 3 length bytes, Reed-
 * Solomon ECC, 3 bytes per transmission slot). 0 for an invalid profile. */
size_t wave_modem_frames_for(enum wave_profile p, size_t len);
size_t wave_modem_samples_for(enum wave_profile p, size_t len);

/* "audible_fast" -> WAVE_PROFILE_FAST. 0 on success, -1 when unknown. */
int wave_modem_parse_profile(const char *name, enum wave_profile *out);

struct wave_encoder;
struct wave_decoder;

/* NULL with a reason in err on failure. */
struct wave_encoder *wave_encoder_new(char *err, size_t errlen);
void wave_encoder_free(struct wave_encoder *e);

/* Encode 1..WAVE_MAX_MESSAGE_BYTES bytes at volume 1..WAVE_MODEM_MAX_VOLUME.
 * On success returns the number of mono S16 samples at WAVE_MODEM_RATE and
 * points *samples at them (owned by the encoder, valid until the next encode
 * or free). -1 with a reason on failure. */
long wave_encoder_encode(struct wave_encoder *e, const uint8_t *payload, size_t len,
                         enum wave_profile p, int volume, const int16_t **samples, char *err,
                         size_t errlen);

enum wave_decode_kind {
    WAVE_DECODED, /* data/len is a message */
    WAVE_MISSED   /* a transmission was heard and could not be decoded */
};

typedef void (*wave_decode_fn)(void *user, enum wave_decode_kind kind, const uint8_t *data,
                               size_t len);

struct wave_decoder *wave_decoder_new(char *err, size_t errlen);
void wave_decoder_free(struct wave_decoder *d);

/* Any number of mono S16 samples at WAVE_MODEM_RATE, in any chunking. fn is
 * called for each message or miss, from inside this call. 0, or -1 when the
 * receiver could not be rebuilt after a watchdog stop (the decoder is then
 * unusable and says so every call). */
int wave_decoder_feed(struct wave_decoder *d, const int16_t *samples, size_t n, wave_decode_fn fn,
                      void *user);

/* Whether a transmission is being recorded right now, and how many times the
 * watchdog has had to stop one. */
int wave_decoder_receiving(const struct wave_decoder *d);
unsigned wave_decoder_watchdog_stops(const struct wave_decoder *d);

/* The heap ggwave allocated for this instance, in bytes. */
long wave_encoder_heap_bytes(const struct wave_encoder *e);
long wave_decoder_heap_bytes(const struct wave_decoder *d);

#ifdef __cplusplus
}
#endif

#endif
