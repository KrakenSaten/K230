# ADR-010: Who owns the audio hardware for the Recorder

Status: Proposed (awaiting the product owner). Written on branch
`feat/recorder-app`.
Date: 2026-09-26
Deciders: product owner (final), AI engineering partner (author)

## Context

The Recorder app (docs/apps/RECORDER.md) records the built-in microphone to
WAV files and plays them back. It is the second audio user after Wave.

Constraints already binding:

- ADR-002 point 2: services own hardware; apps never touch it.
- ADR-004 accepted a per-operation helper process on `core/pocketaudio` as a
  narrow exception **for Wave**, and says: "ADR-002 stays the rule for every
  other hardware owner, and a later need is decided against ADR-002, not by
  extending this." It names when to move to an `audiod` (Option A): "when a
  second audio user appears that needs sound outside an app's own screen
  (an alarm tone from PocketClock, system sounds) or needs mixing".
- ADR-004 also records the consequence that "a future second audio user must
  also take the lock or go through pos-wave".
- The shell is one LVGL loop; nothing on it may block.

What the Recorder needs: the microphone for as long as a recording runs
(minutes to hours, not seconds), the speaker for a playback, never both at
once, both only while its screen is open, and a file written for the whole
time. It needs no mixing and no sound outside its screen: leaving the app
stops and saves the recording (v1 has no background recording).

## Options

### A. `audiod`, a supervised service (ADR-002 as written)

- Pro: one owner for the card system-wide; the place for mixing and system
  sounds later.
- Con: long recordings would stream audio or file writes through a daemon
  and a pocketipc (JSON) contract designed for small messages; a permanent
  process for an app that is closed most of the time; the largest change,
  and it would drag Wave's settled design along with it. Neither of ADR-004's
  triggers for it (sound outside an app's screen, mixing) is met.

### B. Reuse `pos-wave` for recording and playback

- Pro: no second helper.
- Con: puts WAV writing, long files and a recorder's lifecycle into Wave's
  modem tool; couples two apps' release and test cycles; `pos-wave record` is
  deliberately a 30 s in-RAM bench tool.

### C. A second per-operation helper, `pos-record`, on the same `pocketaudio` (recommended)

- `pos-record` links the unchanged `core/pocketaudio` (lock, route, start-up
  discard, bounded waits, recovery record) and a new `core/pocketwav` (the
  WAV container). One process per recording, playback or repair, started by
  the app over a socketpair, gone when the operation ends.
- Pro: exactly ADR-004's proven shape - no audio on the LVGL thread, a hung
  helper killed and its PCM closed by the kernel, PR_SET_PDEATHSIG and a
  closed socket both finalize the file, recovery after SIGKILL - with no new
  mechanism; Wave and Recorder exclude each other through the one
  `audio.lock` that already exists, so the "second user must take the lock"
  consequence is met by construction; nothing in Wave changes.
- Con: still not the letter of ADR-002; a third small line protocol
  (apps/recorder/rec_protocol.h); the helper-client code is similar to
  Wave's (a shared client is a follow-up, not done here to leave Wave
  untouched).

## Decision (proposed)

**Option C**: the Recorder's audio is owned, per operation, by `pos-record`
on `core/pocketaudio`, as a second narrow exception to ADR-002 point 2 of
the same shape as ADR-004. Scope: the sound card's capture and playback
paths and the amplifier line (through pocketaudio), for the Recorder, only
while its screen is open. The audio lock is shared with Wave; whoever holds
it first owns the card, and the other says "Audio device in use".

Revisit exactly when ADR-004 says to: background recording, system sounds,
or mixing. Then `pocketaudio` becomes `audiod`'s backend and both helpers
its clients.

## Consequences

Needed now: `tools/recorder` (pos-record), `core/pocketwav`,
`apps/recorder`, their tests, and the launcher entry - all on
`feat/recorder-app`. No change to `core/pocketaudio`, Wave or `pos-wave`.

Useful soon: the Unit A gate (docs/hardware/RECORDER_GATE.md); a measurement
of the C908's cost for the 3:1 filter and for fsync at stop; a shared
helper-process client for Wave, Camera and Recorder.

Risks: a recording holds the audio lock for its whole length, so Wave cannot
listen or send while one runs (and vice versa) - intended, and said on
screen. A shell and its helper SIGKILLed together leave the route switched
until the next audio open (ADR-004's known gap) and the recording as a
`.part` until the Recorder next opens, which repairs it.

Migration cost to an `audiod` later: an IPC front for pos-record's three
commands; pocketaudio, pocketwav and the file logic move unchanged.

## Evidence

- ADR-002 and ADR-004 wording: DOCUMENTED (repository).
- One capture and one playback substream on `hw:0,0`, 48 kHz S16_LE stereo on
  the wire, microphone on the right slot, 500 ms start-up transient: VERIFIED
  on unit A (AUDIO_HARDWARE_MAP §11, §15), reused unchanged.
- Helper lifecycle (stop, pause, SIGKILL and repair, space limits, busy lock,
  abandon on close): VERIFIED on the host by tests/rec_session_test.c,
  tests/rec_ctl_test.c, tests/rec_tool_test.sh and tests/rec_app_test.c; not
  on hardware.
- Recording and playback on the K230 itself: not yet tested (RECORDER_GATE).
