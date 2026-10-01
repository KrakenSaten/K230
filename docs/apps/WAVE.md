# Wave

Short text messages sent and received as sound. SEND turns a typed message
into ggwave's multi-tone FSK audio on the built-in speaker; LISTEN keeps the
built-in microphone decoding; CAPTURE records and decodes afterwards - all on
one screen, with one selected preset for both directions. It is also
PocketOS's first end-to-end exercise of audio playback and capture.

**Status:** host-complete on branch `feature/audio-ggwave` (2026-09-13).
Built for riscv64 (make tree and DRM shell, 0 warnings). **RECEIVE validated
and enabled on unit A** on 2026-09-13 (build `c688309`,
docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md §15): the microphone path is
open in code, no override needed. **SEND validated on unit A** the same day:
the controlled first SEND of `DOORS` was heard, decoded by Waver on a phone
and left the panel steady (§16 there); the speaker path is open in code since
build `e778daf`, with every limit unchanged. Working name; no branding
decided.

**Branch `feat/wave-next` (2026-09-26):** Wave is one screen instead of two
modes, with presets, a persistent history, capture-then-decode, a landscape
layout and a host simulator. Host-tested (see "Tests" below); not yet built
against LVGL or ggwave in the cloud session that wrote it, and not yet on
hardware (docs/hardware/WAVE_NEXT_GATE.md lists what has to be checked).

## What it is

- **One screen, one preset.** The person picks a preset and stays in it, for
  both directions. There is no SEND/RECEIVE switch.
- **LISTEN** is a toggle. While it is on, the microphone listens and decodes
  live, up to the preset's listen length (120 s), and turns itself off after.
- **SEND** plays the typed message with the preset, at any time. While
  listening, a send pauses the listen, plays the preset's copies and resumes
  the listen: talking while listening needs no mode change. Underneath it is
  still half duplex - the K230 needs opposite audio routes for the two, so
  one helper runs at a time and the next is started only once the previous
  has been reaped.
- **CAPTURE** records the preset's capture length without decoding, then
  decodes the recording (`pos-wave record`, then `pos-wave decode`). For a
  sender that started before the button was pressed, a noisy room, or a board
  too busy for live decoding. While capturing the button reads **DECODE NOW**
  and ends the recording early; STOP (the SEND button, while a capture
  runs) throws it away.
- **History:** everything sent and heard, newest first, kept across restarts,
  bounded. A tap on an entry copies its text into the message field (to
  answer or resend). **CLEAR** takes two taps (the first arms it for 4 s and
  reads CONFIRM).
- A message is up to **64 bytes** of plain UTF-8. Enter sends. Received bytes
  that are not clean text are shown as hex.
- The state is always visible: a chip (READY, LISTENING, CAPTURING, SENDING,
  SENDING 1/2, DECODING, STOPPING, ERROR), a status line in words (level,
  progress in tenths of a second, "heard a signal it could not decode",
  errors), and MICROPHONE ON while the microphone may be open.

## Presets

A preset fixes, for both directions: the transmit speed (one of ggwave's
three audible protocols), how many copies a send plays, how long a listen
runs, how long a capture records, and how long a repeat of the same message
folds into the same history entry. `apps/wave/wave_preset.c` is the only
place the set is defined; the id is stored, the label is shown, and a later
(user-defined) preset is one more entry that passes `wave_preset_valid()`.

| Preset | Speed | Copies | Listen | Capture | Folds repeats within | For |
| --- | --- | --- | --- | --- | --- | --- |
| **STANDARD** (default) | FAST (`audible_fast`) | 1 | 120 s | 10 s | 10 s | everyday use |
| **ROBUST** | NORMAL (`audible_normal`) | 2 | 120 s | 20 s | 20 s | noise, distance |
| **QUICK** | FASTEST (`audible_fastest`) | 1 | 120 s | 6 s | 10 s | close range, quiet room |

The receiver needs no choice to hear a sender: ggwave's decoder listens for
all three audible protocols at once (`wave_modem.cpp`), so a STANDARD
listener decodes a ROBUST or QUICK sender too (tests/wave_sim_test.c,
"mismatch"). A received message is filed under the listener's preset; which
protocol it actually arrived on is not reported by the helper today
(recording that needs `wave_modem` to expose ggwave's received protocol id -
left for a change built against ggwave). Each copy of a ROBUST send is its own
helper run, so the amplifier and route go through the validated open/close
sequence each time. The preset button cycles STANDARD -> ROBUST -> QUICK; it
is locked while a send, capture or decode is on its way.

## History

`apps/wave/wave_history.[ch]`: a ring of **40** entries (`WAVE_HISTORY_MAX`),
the oldest dropped when full. Each entry: direction (RX/TX), wall-clock time
(0, shown as `--:--`, while the board's clock is not set - the shell's rule,
`pocketos_shell_system_day()`), preset, result (sent / stopped / failed for
TX; decoded / nothing decoded for RX), a count (copies played, or times
heard), whether it came from a capture, and the payload bytes. A repeat of
the same bytes within the preset's window of the last hearing folds into the
newest entry (x2, x3) - so a ROBUST exchange reads as one message; the window
is monotonic time of this run and is not stored. A send that was stopped or
failed is kept too, so its text is not lost.

On screen: `RX 14:05  STANDARD  x2` over the text; another day's entries show
the date; the newest 20 are on screen and the list scrolls inside its own
card.

## Persistence

`apps/wave/wave_store.[ch]`, the only file in the app that touches files:

| What | Where | Written |
| --- | --- | --- |
| Selected preset | `$POCKETOS_STATE_DIR/wave/wave.conf` (`wave-prefs 1`, `preset=<id>`) | when it changes |
| History | `$POCKETOS_STATE_DIR/wave/history` (`wave-history 1`, one line per entry, payload as hex; at most ~8 KB) | after each change; removed on CLEAR |
| A capture | `$POCKETOS_RUNTIME_DIR/wave/capture.wav` (tmpfs) | between a capture and its decode only |

Temporary file, fsync, rename (as Notes and Clock); directories 0700, files
0600. A damaged line costs that line only; a foreign or oversized file is
refused. **Not stored:** the listen toggle (the microphone never turns itself
on when the app opens), the typed message, and audio. A capture is removed
after its decode, when it is stopped, when the app starts and when it closes;
at the longest (30 s) it is 2.9 MB of RAM on a tmpfs for a few seconds.

## Privacy

- The microphone is on only while a listen or a capture runs. While it runs,
  and until its helper process has actually exited, the panel shows
  **MICROPHONE ON** in the warning tone, the chip reads LISTENING or
  CAPTURING, the status line says "Microphone on", and the header hint reads
  **MIC ON** (Wave is fullscreen and has no status bar, DS §30.8). The
  indicator is on from the moment a helper is started - before it confirms -
  and off only when the process is gone (tests/wave_view_test.c).
- A listen stops by itself after the preset's listen length (no preset may
  exceed 120 s). Leaving the app stops it. Opening the app never starts one.
  If the shell dies, the helper gets SIGTERM from the kernel
  (PR_SET_PDEATHSIG) and closes the device.
- **What changed from v0.1.0:** received and sent messages are now kept on
  flash (above) so the history survives a restart; CLEAR removes them.
  Audio is still never stored.
- The message to send goes to the helper on its stdin, never on its command
  line. Decoded messages go to the app over a private socket; ggwave's own
  logging is compiled out and switched off.

## Screen, orientation and keyboard

`apps/wave/wave_layout.[ch]` decides from numbers alone
(tests/wave_layout_test.c); `wave_app.c` applies it. Nothing in the app
touches the shell's chrome, status area or viewport: it lays out inside the
body it is given (`POCKETOS_CHROME_NONE`, unchanged).

- **Portrait (TALL, 528 x 1106):** chip and status; MICROPHONE ON when on;
  the history, which takes the rest and scrolls inside itself; the preset
  button with its one-line summary; LISTEN and CAPTURE; CLEAR and the byte
  counter; and the composer row at the foot - message field and **SEND** side
  by side, right above the keyboard. With the touch keyboard up (528 x 820)
  the history shrinks and everything else stays.
- **Landscape (WIDE, 1192 x 442):** the history column on the left, a 400 px
  rail of controls on the right, the composer row across the full width
  underneath with a **KEYS** button.
- **Landscape with the touch keyboard up (STRIP, 1192 x 156):** only the
  composer row - field, SEND, HIDE.
- The body never scrolls; every control is at least 64 px.

Keyboard:

- **Portrait:** a tap on the field brings up the touch keyboard. Nothing
  brings it up by itself.
- **Landscape:** a tap on the field only focuses it - landscape is where the
  physical keyboard is, and the touch keyboard would take three quarters of
  the body. **KEYS** brings the touch keyboard up (and, as HIDE, puts it
  away), so Wave stays usable in landscape without a physical keyboard.
- **Everywhere:** Enter (the physical key or the touch keyboard's Done)
  sends; a send clears the field and puts the touch keyboard away so the
  answer can be seen. The field stays editable while a send or listen runs:
  the text is copied when the send is asked. No button takes focus from the
  field.

## Architecture

```
wave_app.c (LVGL, the screen)           pos-wave (helper process, one per send,
  wave_layout.c  shape + keyboard rule   listen, record or decode)
  wave_ctl.c     the loop  ------------>   wave_modem.cpp  ggwave, used safely
    wave_view.c    model + words           wave_wav.c      WAV files
    wave_preset.c  the presets             core/pocketaudio ALSA + amp + route + gate
    wave_history.c bounded history
    wave_store.c   the only file I/O
    wave_session.c child process  <--  socketpair: text on stdin, events on stdout
  wave_text.c    text rule (shared with the helper)
```

| File | Role |
| --- | --- |
| `apps/wave/wave_app.c` | The screen. Builds the objects, applies the layout, turns taps and keys into controller calls, polls the controller from a 50 ms LVGL timer, paints the model. No audio, no files, no decoding, no sleeping. |
| `apps/wave/wave_ctl.[ch]` | The controller: stops a running helper when a request needs it, starts whatever the model says is due once no helper runs, reads the system volume before a send, saves the history and the preset, removes a decoded capture, and ends everything on close (the one bounded wait). Pure C, tested against real helper processes. |
| `apps/wave/wave_view.[ch]` | The model: the intents (listen toggle, queued send and its copies, capture, pending decode), the running helper's phase, what to start next, the history it keeps, and everything shown - labels, enablement, chip, status words, the microphone rule, error words, entry formatting. Pure C. |
| `apps/wave/wave_preset.[ch]` | The preset table and its bounds. |
| `apps/wave/wave_history.[ch]` | The bounded history ring, repeat folding, and its text form. |
| `apps/wave/wave_store.[ch]` | Preferences, history and capture paths on disk. |
| `apps/wave/wave_layout.[ch]` | Portrait/landscape shapes and the keyboard rule. |
| `apps/wave/wave_session.[ch]` | The helper client: fork/exec with PDEATHSIG and closed descriptors, non-blocking socket reads, the event parser, a bounded event queue, SIGTERM then SIGKILL, a bounded abandon, `recover` after a signal death. Starts `send`, `listen`, `record` and `decode`. Pure C. |
| `apps/wave/wave_text.[ch]` | What a sendable message is: shortest-form UTF-8, no C0/DEL/C1 controls. Shared by app and helper. |
| `apps/wave/wave_protocol.h` | What app and helper agree on: limits, profile names, exit codes, error words. |
| `apps/wave/wave_modem.[ch]pp` | ggwave behind a C interface (§ ggwave). Unchanged on this branch. |
| `tools/wave/pos_wave.c` | The helper and bench tool (`info`, `encode`, `decode`, `send`, `listen`, `record`). Unchanged on this branch: the app uses `record` and `decode` as they were. |
| `tools/wave/wave_wav.[ch]` | PCM S16 WAV reader and writer, bounded, refusing any other format. |
| `core/pocketaudio/` | The audio layer (§ Audio layer). Unchanged. |

**Audio lifecycle.** At most one helper exists at any time: the model asks
for the next start only after the session has reaped the previous helper
(its EXITED event), and every start goes through `wave_session.c`. A send
while listening is: SIGTERM to the listener, wait for it to be reaped
(never on the UI thread - the 50 ms poll sees it), start the send, and once
that is reaped, start a new listen. A failed helper turns the listen toggle
off and drops whatever was queued, so a broken microphone or speaker is
never reopened in a loop. Closing the app abandons the running helper within
300 + 200 ms, removes any capture and saves the history.

**Why a helper process.** First-party PocketOS code has no threads
(KEYBOARD_DRIVER_DESIGN_2026-09-12 §3), apps do not touch hardware
(ui/shell/app.h), and ADR-002 has services own hardware. Decoding sound and
waiting on a sound card cannot happen on the LVGL thread. A small process that
exists only for the length of one send or listen gives: no blocking audio work
in the shell; a helper hung in a driver is killed instead of freezing the
panel; the kernel closes the PCM when the helper dies; no background
microphone; and the first hardware test runs the same binary the app runs. A
supervised `audiod` would own the card permanently for one app that needs it
briefly. The choice is **ADR-004, accepted by the owner for this milestone as
a narrow exception for Wave** - not a replacement for ADR-002's "services own
hardware", which stays the rule everywhere else.

**When a helper is killed.** A SIGKILLed helper runs no cleanup; the kernel
stops the sound but leaves the route switched and the amplifier line as they
were. pocketaudio therefore writes a recovery record before it changes
either, whoever takes the audio lock next restores from it first, and the
Wave session - the process that sees a helper die by a signal - starts a
detached `pos-wave recover` at once (pocketaudio.h, "Recovery";
tests/audio_recovery_test.sh kills real helpers to prove it).

Launcher: the eleventh tile, after Settings; the grid gained a sixth row.
Since DS §47 Wave is in the Apps folder.
Icon: `LV_SYMBOL_VOLUME_MAX`, a placeholder until the DS §11 icon set exists.

## Left for later

- **Wall-clock time for apps (shared API, not implemented here).** app.h
  gives an app the day (`pocketos_shell_system_day()`) but not the time of
  day, so Wave reads `time()` itself and dates an entry only when the shell
  says the clock is set. A shared `pocketos_shell_wall_time()` from the
  shell's one clock reader would remove that second reader; it belongs to
  the shell, so it is recorded here rather than added on this branch.
- **The protocol a message arrived on.** ggwave knows which of the three
  audible protocols it decoded; `wave_modem` does not pass it on and
  `pos-wave` does not print it. Adding it needs a build against ggwave; a
  separate event line before `received` (say `protocol audible_fast`) keeps
  older apps working, since the session ignores lines it does not know,
  whereas a third word on `received` would make them drop the message.
- **User-defined presets.** The table and `wave_preset_valid()` are ready for
  more entries; there is no editor.
- **A history on/off setting.** CLEAR exists; a switch to keep no history at
  all is a small addition if the owner wants one.

## pos-wave

```
pos-wave info
pos-wave encode [--protocol P] [--volume V] [--text T] OUT.wav
pos-wave decode [--channel C] IN.wav
pos-wave send   [--protocol P] [--volume V] [--volume-percent L] [--text T] [--events] [--allow-unverified]
pos-wave listen [--seconds N] [--channel C] [--events] [--allow-unverified]
pos-wave record [--seconds N] [--channel C] [--allow-unverified] OUT.wav
pos-wave recover
```

`P` = `audible_normal`, `audible_fast` (default), `audible_fastest`; `V` = 1..25
(default 10); listen `N` = 1..3600 (default 120); record `N` = 1..30 (default 5).
Text comes from `--text` or stdin (one trailing newline is dropped).
`--allow-unverified` opens the command's own direction on a path whose board
entry is not yet validated; `POCKETOS_AUDIO_ALLOW_UNVERIFIED` does the same for
the directions it names (`capture`, `playback`, or both with a comma - `1`
opens nothing), so an environment set for a microphone test cannot open the
speaker. `recover` undoes what a killed pos-wave left switched and opens no
stream. `POCKETOS_AUDIO_BOARD=generic|k230` and `POCKETOS_AUDIO_PCM=<name>`
override detection for bench and tests.

Events (stdout, one per line): `ready <board>`, `sending <ms>`, `listening`,
`level <0-100>`, `received <hex>`, `missed`, `sent`, `stopped`, `recovered`,
`error <code> <text>` with code `usage`, `too_long`, `invalid_text`, `encode`,
`decode`, `audio_disabled`, `audio_busy`, `audio_nodev` or `audio`.
Exit codes: 0 done, 1 ran and failed (decode found nothing), 2 usage or input,
3 audio unavailable or failed. SIGTERM/SIGINT stop within one 200 ms audio
wait with full cleanup; a closed stdout is treated as a stop.

## ggwave

- **Upstream:** https://github.com/ggerganov/ggwave, tag `ggwave-v0.4.3`,
  commit `a38e38b7373f9adf45baf737a104206664b225a1`
  (platforms/k230/vendor_ggwave_commit.txt). `include/` and `src/` are
  byte-identical to master `060aec7` at the time of the study. MIT; its
  Reed-Solomon code carries its own MIT licence; `fft.h` is Ooura's FFT with a
  copyright line and no licence text (docs/LICENSING.md).
- **Convention:** like RadioLib - an ignored checkout at `vendor/ggwave`,
  pinned, refused by `apply_to_sdk.sh` when drifted or dirty, and copied into
  the package as `third_party/ggwave` (the header, `ggwave.cpp`, `fft.h`, the
  three Reed-Solomon headers and both licences; about 160 KB). Compiled into
  `tools/wave/ggwave.o`, never beside its sources.
- **Build:** C++11, standard library only; `-DNDEBUG -DGGWAVE_DISABLE_LOG
  -fno-exceptions -fno-rtti`. 0 warnings on the host with `-Wall -Wextra
  -Werror`; object 42 KB text for riscv64 with the image's C908 flags.

What reading its source at the pin found, and what wave_modem does about it:

| Finding (ggwave.cpp at the pin) | wave_modem |
| --- | --- |
| The decoder loses frame sync for good after a chunk that is not a whole number of 1024-sample frames (1150-1190) | buffers any input and feeds exact frames; tested with 1, 333, 512, 960, 1000, 4096 and 60000-sample chunks and a misaligned start |
| `ggwave_init()` returns a valid-looking id when its parameters were rejected; such an instance can crash in decode (57-79, 460-462) | builds instances with `prepare()`, which reports failure |
| Logs to stderr by default, including every decoded payload (1735) | compiled out and log file set to none; tested: 0 bytes on stderr |
| After a missed end marker it records for up to 38.5 s and is deaf meanwhile (1818-1822) | a watchdog stops a reception older than the longest accepted message (327 frames, 7 s) and starts a fresh receiver; tested |
| One RX+TX instance: an encode resets a reception (746-762) | separate TX-only and RX-only instances |
| Volume 100 wraps int16 | volume capped at 25 |
| Longer payloads are truncated silently (691-696) | refused above 64 bytes |

**Numbers** (48 kHz, 1024 samples per frame, measured on the host unless
marked DER, derived from the source's formula):

| | NORMAL | FAST | FASTEST |
| --- | --- | --- | --- |
| Frames per slot | 9 | 6 | 3 |
| `DOORS` (5 bytes) | 1.451 s | 1.195 s | 0.939 s (DER) |
| 64 bytes | 6.635 s | 4.651 s (DER) | 2.667 s (DER) |
| Tones | 1875.0 - 6328.1 Hz | same | same |

- Frames = 32 + ceil((3 + N + ECC(N)) / 3) x framesPerTx, ECC(N) = 2 for
  N < 4, else max(4, 2 x floor(N / 5)).
- Memory: TX-only instance 9,193,984 bytes, RX-only 8,436,224 bytes, one
  allocation at creation and none after (study: counted with a malloc wrap).
- CPU on the host: listening idle 0.02 % of a core. The worst single decode
  call (a start marker never followed by an end) was 6.2 ms with one protocol
  enabled and 17.9 ms with all twelve; Wave enables the three audible ones,
  which share one start frequency and are all analysed, so expect between
  the two (not measured separately). **Unmeasured on the C908.**
- Receive latency: a message is reported about 0.24 s before the end of its
  waveform.

## Audio layer: core/pocketaudio

One mono S16 48 kHz stream at a time through a backend seam (ALSA and the GPIO
uAPI v2 in `pocketaudio_alsa.c`, a fake in the test). Guarantees: one owner
across both directions (flock on `$POCKETOS_RUNTIME_DIR/audio.lock`); every
read or write moves at most one 20 ms period and waits at most 200 ms; one
period of scratch, allocated at open; every played sample clamped to a
**-12 dBFS ceiling**; on close, amplifier off first, then the PCM, then the
route restored, then the lock, on every path including a failed open; a
**hardware gate** refusing a board path not yet validated; and a **capture
startup discard** for boards that declare one. The K230 board entry (card by
id, stereo wire, mic on the right slot, `External I2S Output Switch` on for
the speaker and off for the mic, amplifier on gpiochip1 line 2 active high)
has both paths validated on unit A.

**K230 codec/capture startup transient.** Every K230 capture starts with both
slots at negative full scale for 140-210 ms. The board entry declares 500 ms
(`capture_settle_frames`), and `pocketaudio_read()` reads and drops that much
after every open, inside its usual one-wait budget per call: the helper says
`listening` at once (the microphone is on), STOP works throughout, and the
first sample the decoder, the level meter or a recording receives comes after
the window. It is a property of the codec, not a ggwave delay.

## Limits

| Limit | Value | Where |
| --- | --- | --- |
| Message | 64 bytes of UTF-8, no controls | `WAVE_MAX_MESSAGE_BYTES` |
| Longest transmission | 6.635 s audio, 318,464 samples | `WAVE_MODEM_MAX_SAMPLES` |
| Volume | app 10 (peak 3192, -20 dBFS); helper 1..25 | `WAVE_DEFAULT_VOLUME`, `WAVE_MODEM_MAX_VOLUME` |
| Playback ceiling | 8192 (-12 dBFS) | `POCKETAUDIO_PEAK_CEILING` |
| Listen | 120 s per start, at most (every preset) | `WAVE_LISTEN_SECONDS`, `wave_preset_valid()` |
| Capture | 6 / 10 / 20 s by preset, 30 s at most; RAM (tmpfs) only, removed after decoding | `WAVE_CAPTURE_MAX_SECONDS` |
| Copies per send | 1..3 (ROBUST: 2), one helper each | `WAVE_PRESET_MAX_COPIES` |
| History | 40 entries, ~8 KB on disk; newest 20 on screen | `WAVE_HISTORY_MAX`, `WAVE_ROWS` |
| Stop | SIGTERM, SIGKILL after 1 s; destroy waits at most 300 + 200 ms | `WAVE_STOP_GRACE_MS`, `WAVE_DESTROY_GRACE_MS`, `WAVE_KILL_REAP_MS` |
| Helper output | 16 queued events, 512-byte lines | `WAVE_EVENT_QUEUE`, `WAVE_LINE_MAX` |
| Bench recording | 30 s | `RECORD_MAX_SECONDS` |

## Tests

| Test | What it covers | Checks |
| --- | --- | --- |
| `tests/pocketaudio_test.c` | board detection; the gate touching nothing on an unvalidated board, both validated K230 paths opening without an override and the clamp still applied; route/PCM/amplifier order on open and close; restore; busy; every open failure leaving nothing behind; clamp; period and wait bounds; xrun recovery; channel mapping; drain bound; the capture startup discard (exact boundary, a read crossing it, the per-call wait on a slow device, timeouts, overruns and failures inside it, close mid-discard, a fresh discard per open, bounded length); the recovery record written before each change, a child process that dies holding a stream, recovery order, next-open reconciliation, a live owner, failed restores kept for retry, corrupt records | 137 |
| `tests/audio_recovery_test.sh` + `tests/pos-wave-testhooks` | real pos-wave processes SIGKILLed mid-send and mid-listen against file-backed hardware: the amplifier and route left behind, `recover`, the next send and listen reconciling first, no recovery over a live owner, SIGTERM needing none, a foreign record, the direction-scoped override, no hook in the shipped binary | 31 |
| `tests/capture_settle_test.sh` + `tests/pos-wave-testhooks` | a real pos-wave fed a full-scale transient and DOORS: the transient never reaches the decoder, the level meter or a recording; the first level is the 10 ms after the window; a message inside the window is never decoded; SIGTERM, SIGKILL (and recovery) and a device failure during the discard leave the audio clean | 22 |
| `tests/wave_modem_test.c` | the host acceptance round trip text -> PCM -> text for every profile; empty, maximum, one over, volume; 64 arbitrary bytes; UTF-8; seven chunkings; misaligned start; truncated PCM and the watchdog; noise on a message; noise alone; full-scale garbage; two messages; peak against the ceiling; heap | 52 |
| `tests/wave_session_test.c` + `tests/fake_pos_wave.sh` | event parser; argv (send, listen, record, decode) and their refusals; text on stdin; crash; SIGTERM ignored then SIGKILL; bounded abandon; flood; garbage; missing helper; no SIGPIPE; no descriptor leak; the helper ended by PR_SET_PDEATHSIG when its parent dies; `recover` started after every signal death and never after a normal exit; the real helper stopped inside the capture startup discard, and SIGKILLed with the route switched and the session switching it back | 88 |
| `tests/wave_view_test.c` | message rules; the one-screen workflow: LISTEN toggle, a send that pauses and resumes the listen (and 20 round trips of it), the preset's copies, STOP between copies and while pausing, capture then decode, finish early, stop discards, nothing decoded, a crashed decode; the microphone indicator through start, stop and pause; failures that never loop (listen, send, missing helper), a microphone failure that keeps a queued send; history kept by the model (fold, stopped, failed, undecoded); CLEAR's two taps; preset lock; every error word and exit; entry and payload formatting | 171 |
| `tests/wave_model_test.c` | the presets (bounds, lookup, cycling, a later preset); the history ring (bound, newest first, folding within the window and not after it, a clock going back, truncation, clear keeps the sequence); the text form (round trip with binary bytes, twelve kinds of damaged line, foreign and later formats, the bound, the longest possible history) | 59 |
| `tests/wave_store_test.c` | against a temporary state and runtime directory: nothing stored yet; the preset and the history survive a restart; 500 sends keep the file at 40 entries; clear removes the file; damaged, foreign and oversized files; 0600 files in a 0700 directory, no temporary left; the capture only in the runtime directory and removed; an unwritable store says so | 30 |
| `tests/wave_layout_test.c` | the shapes and the keyboard rule: portrait with and without the keyboard, landscape, landscape with the keyboard up (strip), a landscape body too narrow for two columns, an unsized body, the rule following the orientation | 19 |
| `tests/wave_ctl_test.c` + `tests/fake_pos_wave.sh` (auto) | the audio lifecycle against real processes: one helper at a time; listen -> send -> listen in that order; five send/listen cycles; copies as separate helpers and STOP ending the rest; record -> decode -> file gone; a noisy capture; finish early; stop discards; a failing listen not retried; a failing send; mute sends nothing; the system volume passed on; a missing helper; the preset and history across a restart; the file bounded; CLEAR; a stale capture removed at open; close mid-send keeps the message as stopped; close mid-listen within the grace; no child, zombie or descriptor left | 62 |
| `tests/wave_sim_test.c` + `tests/wave_channel.c` | the host simulator (below): two Waves and a simulated air path over the real modem, the real event parser and the real model | 38 |
| `tests/wave_tool_test.sh` | pos-wave on files and on ALSA's null device: encode/decode, limits, unsupported and broken WAVs, stereo slot choice, event order, the K230 gates (both validated paths pass the gate and stop at the missing card), missing device, held lock, SIGTERM, a vanished reader, bench recording | 59 |
| `tests/wave_lint.sh` | boundaries: LVGL only in the screen, no hardware in apps/wave, ALSA only in pocketaudio, no threads or shell-outs, no sleeping in the app or the controller, the app never calls the session's blocking functions, the one bounded wait only in close() from destroy(), files only in wave_store.c, captures only in the runtime directory, the listen toggle never stored, the history bounded, no preset past the privacy bound, text never in argv, PDEATHSIG, logging off, both K230 paths validated with the ceiling, volume cap and default volume unchanged, the test board still unvalidated, the 500 ms discard, nothing shipped setting the override, test registration | 71 |
| `tests/wave_app_test.c` (LVGL, host) | the screen against the fake helper, portrait and landscape: one screen, typing, counter, SEND and Enter (field cleared, keyboard away), chip and SENDING hint, Sent, LISTEN with MICROPHONE ON and MIC ON, a received message, a send while listening that resumes the listen, STOP LISTEN, CAPTURE and its decode, STOP throwing a capture away, a tap on an entry into the field, preset cycling, ROBUST's two copies, errors in words, mute, the preset and history across a restart, CLEAR twice, leaving mid-listen (and a helper that ignores SIGTERM); portrait: the field brings the keyboard and Enter's own click does not bring it back after a send; landscape: it does not, KEYS does, the strip above the keyboard, HIDE; 64 px targets and no body scroll in every state | 87 |
| `tests/wave_shell_test.sh` | runs the app test; launcher and CMake wiring (every Wave source); the real shell opens and closes Wave with no lock taken and nothing stored | 21 |

```sh
make CC=gcc CFLAGS="-O2 -Wall -Wextra -Werror" test          # all but the last two
SHELL_BIN=~/work/.../pocketos-shell bash tests/wave_shell_test.sh
```

### The host simulator

`tests/wave_sim_test.c` runs two Waves and the air between them in memory:
the sender's model asks for a send; the real modem encodes the preset's
copies with pos-wave's lead-in and tail silence; `tests/wave_channel.c`
applies gain, white noise, a 50 Hz hum, a DC offset, clipping, lost samples
or a late start, deterministically; the real modem decodes in 20 ms chunks,
live or from a capture buffer (as `pos-wave decode` does, with half a second
of silence after); each result becomes the helper's own event line
(`received <hex>`, `missed`), parsed by the real `wave_session_parse_line()`;
and the receiver's real model files it. Nothing predicts what the decoder
"should" return - every received message came out of ggwave.

Scenarios: a normal message on every preset (each copy decoded, one entry);
empty, control-character, broken-UTF-8 and 65-byte input refused by the
sender and the modem; noise + hum + DC + half level still decoding; a
misaligned start; heavy noise, hard clipping and lost samples, which may lose
a message but must never decode a wrong one; full-scale garbage; a
transmission cut off halfway (missed, then the next message decodes);
repeats folded and not folded after the window; a capture that decodes and
two that decode nothing; 64 bytes on every speed and 64 bytes of UTF-8; and
preset mismatch both ways. What it does not model: echo, the speaker's and
microphone's frequency response, clock drift between two boards - those stay
hardware checks (docs/hardware/WAVE_NEXT_GATE.md).

It links ggwave (like `wave_modem_test`), so it needs `vendor/ggwave` at the
pinned commit; `make tests/wave_sim_test && ./tests/wave_sim_test`.

## Hardware

RECEIVE ran on unit A on 2026-09-13 with the shell started under
`POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture`: the owner's phone sent `test` and
Wave decoded it. Channel 1, helper lifecycle, CPU (0.6 % while listening),
mixer restore, the capture-only override refusing playback, and SIGKILL
recovery were then checked over SSH
(docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md §15). With build `c688309`
the K230 entry has `capture_verified` 1: RECEIVE works with no override, the
startup transient is discarded, and both were checked on unit A (§15.1).
The controlled first SEND (§12, §16 there: `DOORS`, audible_fast, volume 5)
passed on unit A - heard, decoded by Waver, panel steady, no residual sound,
IO34 low, mixer identical - and build `e778daf` sets `playback_verified` 1. A
SEND with no override was then verified on unit A: exit 0, amplifier on for
1.45 s and off after, PCM closed, recovery record removed. The -12 dBFS
ceiling, the volume cap of 25 and Wave's default of 10 are unchanged and
held by tests/wave_lint.sh. Finally the owner used the app itself: SEND,
`HELLO`, TRANSMIT - heard, decoded by Waver, panel steady, back to idle, no
sound afterwards (§16.3 there). The Wave UI -> helper -> pocketaudio ->
speaker path is validated.
