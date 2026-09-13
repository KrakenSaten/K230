# ADR-004: Who owns the audio hardware

Status: Proposed (awaiting the product owner)
Date: 2026-09-13
Deciders: product owner (final), AI engineering partner (author)

## Context

The first real audio application is Wave (docs/apps/WAVE.md): short text
messages sent and received as sound with ggwave. It needs the speaker for
about one to seven seconds per message and the microphone for up to two
minutes at a time, never both at once, and only while its screen is open.

Constraints already binding:

- ADR-002 point 2: "Services own hardware exclusively." Apps run in the shell
  and "never touch hardware; they talk to services over pocketipc"
  (ui/shell/app.h).
- First-party code has no threads: the shell is one LVGL loop, and the
  keyboard design records that as the convention
  (KEYBOARD_DRIVER_DESIGN_2026-09-12.md §3).
- The owner asked for no blocking audio work on the LVGL thread, stop and
  cleanup that also hold after a crash, no audio after the app is left, no
  background capture, and preferred "a small reusable audio HAL/library" to a
  large `audiod` unless the architecture clearly requires one.

Facts that shape it (docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md): the K230
has one sound card with one playback and one capture substream; speaker and
microphone need opposite settings of one mixer switch; the speaker amplifier
has an enable GPIO; ggwave decoding costs real CPU (an analysis step of up to
tens of milliseconds on a desktop core, unmeasured on the C908).

## Options

### A. `audiod`, a supervised service over pocketipc (ADR-002 as written)

- Pro: the letter of ADR-002; one owner for the card for the whole system;
  a place for later mixing, alerts and a system volume.
- Con: a permanent daemon, supervisor entry, init script and versioned
  `audio.*` IPC contract for one app that uses sound for seconds; audio
  sample streams or ggwave itself over pocketipc (JSON) are a poor fit, so
  the modem would live in the daemon, making it app-specific; the largest
  change of the three.

### B. In the shell: a worker thread owns ALSA and ggwave

- Pro: least code; lowest latency.
- Con: the first thread in first-party code; a driver that hangs a read
  blocks the join and the panel; a crash in ggwave takes the shell down;
  contradicts app.h (the app process would touch hardware).

### C. A helper process per operation, on a small audio library (recommended)

- `core/pocketaudio`: the audio library (ownership lock, route, amplifier,
  level ceiling, bounded waits, hardware gate) behind a backend seam.
- `pos-wave`: a short-lived process that links it and ggwave, started by the
  app for one send or one listen, talking to the app over a socketpair (text
  in on stdin, one event per line out), and gone when the operation ends.
- Pro: no audio or decoding in the shell; a hung helper is killed and the
  kernel closes its PCM; PR_SET_PDEATHSIG ends it with the shell; nothing owns
  the microphone when no listen runs; exclusion between any two users of the
  card is a kernel flock; the bench tool and the app are one binary; host
  testable end to end with a fake helper and ALSA's null device.
- Con: not a pocketipc service, so not the letter of ADR-002; a second small
  protocol (the event lines) outside `docs/api/`; a process start per
  operation (tens of milliseconds, and about 9 MB of ggwave buffers each
  time); a future second audio user must also take the lock or go through
  pos-wave.

## Decision

Proposed: **Option C** for Wave, recorded as a deliberate, scoped exception
to ADR-002 point 2 in the shape of the keyboard driver's (owner-approved
2026-09-12): the hardware still has exactly one owner at a time and the app
never opens it; the owner is a per-operation helper rather than a daemon.

Scope of the exception: the sound card and the speaker amplifier line, for
Wave. It does not cover any other hardware.

Revisit and move to Option A when a second audio user appears that needs
sound outside an app's own screen (an alarm tone from PocketClock, system
sounds) or needs mixing: pocketaudio then becomes the daemon's backend
unchanged, and pos-wave becomes one of its clients.

## Consequences

Needed now: pocketaudio, pos-wave, the Wave app and their tests (all on
`feature/audio-ggwave`); alsa-lib as a build dependency of the pocketos
package (already in the image); ggwave pinned like RadioLib.

Useful soon: the controlled hardware tests; a C908 measurement of ggwave's
analysis step; once validated, flipping the K230 board entry's gates.

Risks: after a SIGKILL the mixer route and the amplifier line keep their last
values (the kernel closes the PCM, so nothing plays); the next pos-wave run
sets both. Another program that opens `hw:0,0` without the lock (aplay,
the vendor launcher) is only kept out by the card's single substream
returning EBUSY, which pocketaudio reports as busy.

Migration cost to Option A later: an IPC front for the operations pos-wave
already exposes; pocketaudio and the modem move unchanged.

## Evidence

- ADR-002 wording, app.h comment, no-threads convention: DOCUMENTED (repository).
- One playback and one capture substream on `hw:0,0`: VERIFIED on unit A
  (AUDIO_FEASIBILITY_2026-09-12 §6).
- Opposite route for speaker and microphone, amplifier enable on IO34:
  DOCUMENTED (vendor launcher, device tree, driver); not VERIFIED.
- ggwave CPU and memory figures: measured on a desktop host; C908 ASSUMED
  slower by an unknown factor.
- Helper behaviour (stop bounds, crash, PDEATHSIG, lock): VERIFIED on the
  host by tests/wave_session_test.c, tests/wave_tool_test.sh and
  tests/wave_app_test.c; not on hardware.
