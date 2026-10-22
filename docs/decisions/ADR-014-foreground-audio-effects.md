<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# ADR-014: Foreground mixed audio effects

Status: Accepted scope by owner instruction, 2026-10-22: implement the Relay Nine
direction in `0a6e482`, including actual sound effects and its foreground service.
This records that authorized implementation; existing app-specific exceptions
remain unchanged.

## Decision

A foreground `audio.effects` session is owned by `pos-audio-effects`, a small
service process on the existing `core/pocketaudio` policy and ALSA backend.
ADR-002's separation of UI and hardware remains. ADR-004's mixing trigger is met
by four effect voices mixed into one mono PCM stream. This is not permission
for game code to open hardware or for apps to bypass audio.lock.

The service is demand-started, one session/client per process, with no permanent
background daemon, microphone, cross-app mixer or new dependency. The app sends
bounded effect IDs and monotonic timestamps; it does not stream PCM or create a
process per shot. Its lifetime is bounded by its client connection and parent.
Pause/end/close releases PCM, route, amplifier and lock. System volume/mute,
peak limits, audio.lock and abnormal-death recovery use pocketaudio. Audio busy
or unavailable is visible; it never blocks or stops game logic.

The existing Wave/Recorder/RIFT/Video paths are unchanged. `pocketaudio` defaults
(20 ms periods, 80 ms playback buffer, bounded 200 ms waits) are not retuned by
this change. Source-level/host evidence does not establish physical sound onset
or underrun performance on K230; the local gate must measure those.

## Evidence

- `core/audio_effects/effects.c`: deterministic first-party recipes, fixed bank,
  four voices and mixed-sample clipping; no allocation after initialization.
- `tools/audio-effects/pos_audio_effects.c`: sole hardware owner, parent-death,
  EOF/termination cleanup, bounded reads and stale-command rejection.
- `apps/neon-outpost/relay_audio.c`: nonblocking client and bounded stop/reap,
  recovery after signaled service death, explicit unavailable/muted state.
- `tests/relay_audio_test.py`: real service with ALSA file/null output, sample
  limits, ownership exclusion, EOF/SIGTERM/SIGKILL recovery and expired events.
- No physical hardware access or device validation is claimed here.
