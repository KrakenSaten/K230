# Wave

Short text messages sent and received as sound. SEND turns a typed message
into ggwave's multi-tone FSK audio on the built-in speaker; RECEIVE listens on
the built-in microphone and shows what it decodes. It is also PocketOS's first
end-to-end exercise of audio playback and capture.

**Status:** host-complete on branch `feature/audio-ggwave` (2026-09-13).
Built for riscv64 (make tree and DRM shell, 0 warnings). **RECEIVE validated
on unit A** on 2026-09-13 (build `da3c5e5`, docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md
§15). **SEND not run**: blocked on the hardware gate (§9 there). Both K230
audio paths are still gated in code. Working name; no branding decided.

## What it is

- Two modes, **SEND** and **RECEIVE**, never both at once (half duplex). The
  K230 needs opposite audio routes for them, and a first version needs no
  more.
- SEND: a single-line message of up to **64 bytes** of plain UTF-8 text, a
  speed (**NORMAL**, **FAST**, **FASTEST** - ggwave's three audible
  protocols), and **TRANSMIT**. While sending, the button reads **STOP**.
- RECEIVE: **START LISTENING** / **STOP LISTENING**, and the last five
  messages received, newest first. A message that is not clean text is shown
  as hex.
- A status line in words: ready, starting the speaker, progress in tenths of
  a second, sent, microphone on with its level, heard a signal it could not
  decode, stopped, or the error.

## What it is not

- Not streaming audio, not a voice recorder, not a chat history: nothing is
  stored, and received messages are gone when the app closes.
- Not full duplex, and not a loopback test of one board.
- Not ultrasound, dual-tone or mono-tone ggwave: those protocols are left out
  until the speaker and microphone have been measured.
- No volume control in the app. The level is fixed (§ Limits) and the audio
  layer clamps it.

## Privacy

- The microphone is on only while a listen runs. While it runs, and until
  its helper process has actually exited, the panel shows **MICROPHONE ON**
  in the warning tone above the received list, the status line says
  "Microphone on", and the status bar hint reads **MIC ON**. The indicator is
  on from the moment a listen is started - before the helper confirms it -
  and off only when the process is gone, because the device may be open in
  both of those gaps (tests/wave_view_test.c).
- A listen stops by itself after **120 s**. Leaving the app stops it. If the
  shell dies, the helper gets SIGTERM from the kernel (PR_SET_PDEATHSIG) and
  closes the device.
- The message to send goes to the helper on its stdin, never on its command
  line, so it is not visible in a process list. Decoded messages go to the app
  over a private socket; ggwave's own logging, which prints every decoded
  payload, is compiled out and switched off.

## Keyboard

The message is an ordinary DS §17.1 single-line field in the shell's focus
group, so the touch keyboard (shown when the field is tapped), the physical
keyboard and the simulator's keyboard type into it the same way (§17.4).
**Enter** - the physical key or the touch keyboard's Done, which are the same
key in the stream - transmits when the message is sendable, and otherwise only
puts the touch keyboard away. No button takes focus from the field. The field
is disabled while a send runs.

## Architecture

```
wave_app.c (LVGL, shell)          pos-wave (helper process, one per send or listen)
  wave_view.c   what to show        wave_modem.cpp  ggwave, used safely
  wave_session.c  child process <-> wave_wav.c      WAV files for the bench
  wave_text.c   text rule           core/pocketaudio  ALSA + amplifier + route + gate
        socketpair: text on stdin, one event per line on stdout
```

| File | Role |
| --- | --- |
| `apps/wave/wave_app.c` | The screen. Builds the panels, turns taps and keys into actions, polls the session from a 50 ms LVGL timer. No audio, no decoding, no sleeping. |
| `apps/wave/wave_view.[ch]` | View model: button labels and enablement per phase, status text, the microphone indicator rule, error words, received-text formatting. Pure C. |
| `apps/wave/wave_session.[ch]` | The helper client: fork/exec with PDEATHSIG and closed descriptors, non-blocking socket reads, the event parser, a bounded event queue, SIGTERM then SIGKILL, a bounded abandon for destroy(). Pure C. |
| `apps/wave/wave_text.[ch]` | What a sendable message is: shortest-form UTF-8, no C0/DEL/C1 controls. Shared by app and helper. |
| `apps/wave/wave_protocol.h` | What app and helper agree on: limits, profile names, exit codes, error words. |
| `apps/wave/wave_modem.[ch]pp` | ggwave behind a C interface (§ ggwave). |
| `tools/wave/pos_wave.c` | The helper and bench tool (`info`, `encode`, `decode`, `send`, `listen`, `record`). |
| `tools/wave/wave_wav.[ch]` | PCM S16 WAV reader and writer, bounded, refusing any other format. |
| `core/pocketaudio/` | The audio layer (§ Audio layer). |

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
Icon: `LV_SYMBOL_VOLUME_MAX`, a placeholder until the DS §11 icon set exists.

## pos-wave

```
pos-wave info
pos-wave encode [--protocol P] [--volume V] [--text T] OUT.wav
pos-wave decode [--channel C] IN.wav
pos-wave send   [--protocol P] [--volume V] [--text T] [--events] [--allow-unverified]
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
**hardware gate** refusing a board path not yet validated. The K230 board
entry (card by id, stereo wire, mic on the right slot, `External I2S Output
Switch` on for the speaker and off for the mic, amplifier on gpiochip1 line 2
active high) is DOCUMENTED from vendor sources and gated.

## Limits

| Limit | Value | Where |
| --- | --- | --- |
| Message | 64 bytes of UTF-8, no controls | `WAVE_MAX_MESSAGE_BYTES` |
| Longest transmission | 6.635 s audio, 318,464 samples | `WAVE_MODEM_MAX_SAMPLES` |
| Volume | app 10 (peak 3192, -20 dBFS); helper 1..25 | `WAVE_DEFAULT_VOLUME`, `WAVE_MODEM_MAX_VOLUME` |
| Playback ceiling | 8192 (-12 dBFS) | `POCKETAUDIO_PEAK_CEILING` |
| Listen | 120 s per start | `WAVE_LISTEN_SECONDS` |
| Stop | SIGTERM, SIGKILL after 1 s; destroy waits at most 300 + 200 ms | `WAVE_STOP_GRACE_MS`, `WAVE_DESTROY_GRACE_MS`, `WAVE_KILL_REAP_MS` |
| Helper output | 16 queued events, 512-byte lines | `WAVE_EVENT_QUEUE`, `WAVE_LINE_MAX` |
| Bench recording | 30 s | `RECORD_MAX_SECONDS` |

## Tests

| Test | What it covers | Checks |
| --- | --- | --- |
| `tests/pocketaudio_test.c` | board detection; the gate touching nothing; route/PCM/amplifier order on open and close; restore; busy; every open failure leaving nothing behind; clamp; period and wait bounds; xrun recovery; channel mapping; drain bound; the recovery record written before each change, a child process that dies holding a stream, recovery order, next-open reconciliation, a live owner, failed restores kept for retry, corrupt records | 113 |
| `tests/audio_recovery_test.sh` + `tests/pos-wave-testhooks` | real pos-wave processes SIGKILLed mid-send and mid-listen against file-backed hardware: the amplifier and route left behind, `recover`, the next send and listen reconciling first, no recovery over a live owner, SIGTERM needing none, a foreign record, the direction-scoped override, no hook in the shipped binary | 31 |
| `tests/wave_modem_test.c` | the host acceptance round trip text -> PCM -> text for every profile; empty, maximum, one over, volume; 64 arbitrary bytes; UTF-8; seven chunkings; misaligned start; truncated PCM and the watchdog; noise on a message; noise alone; full-scale garbage; two messages; peak against the ceiling; heap | 52 |
| `tests/wave_session_test.c` + `tests/fake_pos_wave.sh` | event parser; argv; text on stdin; crash; SIGTERM ignored then SIGKILL; bounded abandon; flood; garbage; missing helper; no SIGPIPE; no descriptor leak; the helper ended by PR_SET_PDEATHSIG when its parent dies; `recover` started after every signal death and never after a normal exit; the real helper SIGKILLed with the route switched and the session switching it back | 76 |
| `tests/wave_view_test.c` | message rules; the action per phase; the microphone indicator through start and stop; every error word and exit; received formatting; misses | 91 |
| `tests/wave_tool_test.sh` | pos-wave on files and on ALSA's null device: encode/decode, limits, unsupported and broken WAVs, stereo slot choice, event order, the K230 gate, missing device, held lock, SIGTERM, a vanished reader, bench recording | 59 |
| `tests/wave_lint.sh` | boundaries: LVGL only in the screen, no hardware in apps/wave, ALSA only in pocketaudio, no threads or shell-outs, no sleeping in the app, text never in argv, PDEATHSIG, logging off, gates still closed, test registration | 35 |
| `tests/wave_app_test.c` (LVGL, host) | the screen against the fake helper: panels, typing, counter, TRANSMIT, Enter, SENDING hint, Sent, RECEIVE, MICROPHONE ON and MIC ON, received list, locked mode, stop, errors in words, missing helper, leaving the app ends a listen (and a helper that ignores SIGTERM), 64 px targets, nothing stored | 48 |
| `tests/wave_shell_test.sh` | runs the app test; launcher and CMake wiring; the real shell opens and closes Wave with no lock taken and nothing stored | 16 |

```sh
make CC=gcc CFLAGS="-O2 -Wall -Wextra -Werror" test          # all but the last two
SHELL_BIN=~/work/.../pocketos-shell bash tests/wave_shell_test.sh
```

## Hardware

RECEIVE ran on unit A on 2026-09-13 with the shell started under
`POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture`: the owner's phone sent `test` and
Wave decoded it. Channel 1, helper lifecycle, CPU (0.6 % while listening),
mixer restore, the capture-only override refusing playback, and SIGKILL
recovery were then checked over SSH
(docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md §15). SEND has not run; it
stays blocked on the hardware gate (§9 there). `pocketaudio.c` still keeps
the K230 entry's `playback_verified` and `capture_verified` at 0 and
tests/wave_lint.sh keeps them there, so without the override the app reports
"Audio is not enabled on this device yet". Raising `capture_verified` is an
owner decision.
