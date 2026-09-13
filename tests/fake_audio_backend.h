/*
 * A pocketaudio backend whose "hardware" is files, for tests that kill a
 * real pos-wave process and look at what it left behind.
 *
 * In <dir>:  route  the mixer switch, "0" or "1" (created by the test)
 *            amp    the amplifier enable line, "0" or "1"
 *            pcm    "closed", "playback" or "capture"
 *            log    one line per switch, line or stream operation
 *            capture.raw          optional: mono S16 (host order) the
 *                                 microphone delivers from the start of each
 *                                 capture, silence after it
 *            capture_fail_after   optional: a frame count after which a
 *                                 capture read fails with EIO
 *
 * The PCM paces itself like a real device (a read or write of n frames takes
 * n / 48 kHz of wall time) and captures silence unless fed. Releasing the
 * amplifier line leaves the file as it is, which is what the kernel does with
 * a GPIO line whose owner dies. Compiled only into tests/pos-wave-testhooks.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_FAKE_AUDIO_BACKEND_H
#define POCKETOS_FAKE_AUDIO_BACKEND_H

#include "pocketaudio/pocketaudio.h"

const struct pocketaudio_backend *fake_audio_backend(const char *dir);

/* Shaped like the K230 entry - a route switch, an amplifier line, stereo
 * wire, microphone on the right, a 500 ms capture startup discard - but not
 * validated in either direction, so the gate and the override's direction
 * words stay under test. */
const struct pocketaudio_board *fake_audio_board(void);

#endif
