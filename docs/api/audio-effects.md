<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# audio.effects foreground session, version 1

Hardware owner: `pos-audio-effects <volume-percent>` (0..100), authorized by
ADR-014. Its inherited stdin/stdout are one close-on-exec Unix socketpair. The
UI client uses nonblocking SOCK_SEQPACKET; stream stdin also supports the same
framing for host tests. No public board/device names are in the protocol.

The owner responds once with `READY\n`, `MUTED\n` (volume zero, no PCM opened) or
`UNAVAILABLE <pocketaudio-error>\n`. The app gives startup 1500 ms; silence is a
failed session. No audio operation executes on the LVGL thread.

Each play event is eight bytes: kind (0..9), three zero reserved bytes, uint32
little-endian CLOCK_MONOTONIC milliseconds. Kinds: shot, impact, empty, reload,
wind-up, attack, door, pickup, shotgun shot, shotgun reload. IDs 0..7 retain
their original meaning; 8..9 are an appended compatible extension. At most 32 commands are read per iteration; the
UI coalesces counters to at most ten events per frame and never retries an old
shot. Commands older than 200 ms or over 20 ms in the future, malformed kinds
or nonzero reserved bytes are discarded. Partial stream frames use an eight-byte
carry buffer; no unbounded queue. Four voices, with carbine/shotgun/empty priority when
full, mix at 48 kHz mono S16 and clamp to ±8192 before the existing audio policy's
volume/ceiling. Samples are generated once from original recipes.

EOF or SIGTERM stops within existing bounded audio waits and closes all hardware
state. The app escalates after a 300 ms stop grace, reaps, and invokes `recover`
after signaled death; recovery uses the same pocketaudio write-ahead record and
exclusive lock. The client closes its socket on pause, terminal states and app
close. Resume opens a new session and drops sounds accumulated while paused.
No campaign or persistent sound queue survives close.
