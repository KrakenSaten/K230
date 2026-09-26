# Recorder

Voice notes, field recordings and quick audio checks, kept as WAV files in
the owner's `Recordings` folder, with a list to play and delete them.

Status: **v1 on branch `feat/recorder-app`, not merged.** Host-tested
end to end (the real helper over a file-backed sound card, the app under a
real LVGL pointer in portrait and landscape, the real shell in both
orientations) and cross-built for riscv64. **Not yet run on the K230**: the
Unit A gate is `docs/hardware/RECORDER_GATE.md`. The audio ownership is
ADR-009, **Proposed**.

Screenshots (the real simulator shell with the file-backed sound card):
`docs/design/shots/recorder-portrait.png`, `recorder-landscape.png`,
`recorder-recording.png`, `recorder-recording-landscape.png`.

## What it does

- **Record.** RECORD starts a recording; the same button then reads STOP.
  PAUSE closes the microphone and keeps the file; RESUME opens it again. The
  timer counts the audio written, and a meter shows the measured level.
- **Presets.** VOICE 16 kHz (default) or STANDARD 48 kHz, both mono 16-bit
  PCM WAV. The choice is remembered.
- **The list.** Every `.wav` in the folder, newest first, with its length,
  size and rate; tap one to select it. PLAY plays it (PAUSE/RESUME work
  here too), PLAY again or STOP ends it, DELETE asks once more (CONFIRM) and
  deletes. At most 30 rows are shown; the caption says how many there are.
- **Leaving** the app stops and saves a recording and stops a playback.
  There is no background recording and nothing records until RECORD is
  pressed.

Not in v1: rename (Files renames, and the list shows renamed files),
trimming, notes, gain control (the codec's on-board microphone gain is fixed
at 30 dB and not adjustable through ALSA, AUDIO_HARDWARE_MAP §7), MP3/AAC,
waveform history. No text is typed anywhere, so the touch keyboard never
comes up.

## The audio path (reused, not new)

| | |
| --- | --- |
| Microphone | on-board MIC1 on the codec's right ADC input (`MIC_PR`); VERIFIED on unit A: channel 1 carries it (AUDIO_HARDWARE_MAP §15) |
| Codec / ALSA | card `K230I2SINNO`, PCM `hw:CARD=K230I2SINNO,DEV=0`, one capture and one playback substream, 48 kHz only, S16_LE, 2 channels on the wire (VERIFIED) |
| Route | `External I2S Output Switch` off for capture, on for the speaker; the amplifier enable on IO34 (pocketaudio, unchanged) |
| Start-up | the first 500 ms of every capture is the codec's transient and is discarded inside pocketaudio (VERIFIED, AUDIO_HARDWARE_MAP §15.1) |
| Ownership | the one `audio.lock` flock in `$POCKETOS_RUNTIME_DIR`, shared with Wave; a busy card says "Audio device in use" and nothing is killed |
| Gain | fixed (see above); no mixer control is touched beyond pocketaudio's route |

`core/pocketaudio` is used exactly as Wave uses it and is not modified.
Recorder does not include or run anything of Wave's.

## Architecture

| File | Role |
| --- | --- |
| `core/pocketwav/pocketwav.[ch]` | The WAV container: the canonical 44-byte header, a probe that reports what a file declares and what it holds, and the header repair. No device, no allocation. The shell links it for the list. |
| `tools/recorder/pos_record.c` | `pos-record`: `record`, `play`, `recover`, `info`. The only program that opens audio or writes a recording for the app. |
| `tools/recorder/rec_dsp.[ch]` | DC blocker, 3:1 decimator and interpolator (96-tap windowed sinc), peak/RMS meter, the playback limiter. |
| `tools/recorder/rec_file.[ch]` | A recording on disk: `.part`, checkpoints, finish without replacing, and the repair of interrupted ones. |
| `tools/recorder/rec_engine.[ch]` | One period of recording or playback per step, on a pocketaudio stream. |
| `apps/recorder/rec_protocol.h` | The event and command words, exit codes, the presets and the storage reserve. |
| `apps/recorder/rec_names.[ch]` | Recording names, shared by app and helper. |
| `apps/recorder/rec_state.[ch]` | The state machine (below). Pure. |
| `apps/recorder/rec_session.[ch]` | The helper process client: spawn, events, commands, bounded stop. |
| `apps/recorder/rec_store.[ch]` | The folder, the list, names, delete, the preset. |
| `apps/recorder/rec_ctl.[ch]` | The controller: machine + session + store. |
| `apps/recorder/rec_view.[ch]` | Every word and flag the screen shows. Pure. |
| `apps/recorder/rec_app.c` | The LVGL screen (`app_recorder`, id `recorder`). |

The app polls its helper every 50 ms from an LVGL timer that only makes
non-blocking calls; the screen repaints only while a helper runs or
something changed. The helper streams each 20 ms period from the device
through the DSP to the file: nothing of the recording is kept in memory, and
every buffer is a fixed period.

### The state machine

`CHECKING` (the repair runs; every open) → `IDLE` ⇄ `STARTING` →
`RECORDING` ⇄ `PAUSED`, or `PLAYING` ⇄ `PLAY_PAUSED`; any of them → `STOPPING`
→ `IDLE`, or `ERROR` (IDLE with the failure shown). One helper at a time, so
capture and playback exclude each other by construction: RECORD during a
playback stops it and starts recording once it has exited; PLAY during a
recording is refused. Repeated STOP, a second RECORD while starting and a
pause sent twice do nothing. A helper killed while recording sends the app to
`CHECKING`, which repairs its file.

### Lifecycle

- **Open:** resolve and create the folder, read the list, start
  `pos-record recover --dir` (restores an audio route a dead helper left,
  repairs interrupted recordings). RECORD waits for it.
- **Stop:** "stop" on stdin and SIGTERM; the helper closes the microphone
  first, then finalizes. SIGKILL only after 5 s.
- **Close (app left, rotation, shell exit):** stop, wait up to 1.5 s
  (`REC_DESTROY_GRACE_MS`), then SIGKILL; a killed recording is a `.part`
  that the next open repairs. On the host the finalize takes about 10 ms.
- **Shell death:** the helper gets SIGTERM (PR_SET_PDEATHSIG) and its stdin
  closes; either one finalizes the file.

## Files

- **Where:** `$HOME/Recordings` - `/root/Recordings` on the unit, where the
  Files app opens, so recordings can be browsed, copied, renamed and deleted
  there too. (`/var/lib/pocketos` would be read-only to Files.)
  `$POCKETOS_RECORDINGS_DIR` overrides it (tests).
- **Names:** `REC-YYYYMMDD-HHMMSS.wav` when the wall clock is set (the
  shell's rule), `REC-0001.wav`, `REC-0002.wav`... when it is not (no RTC).
  A taken name gets `-2`, `-3`...; nothing is ever overwritten (O_EXCL, and
  RENAME_NOREPLACE when finishing).
- **Format:** RIFF/WAVE, PCM, 16-bit signed little-endian, mono, 16000 Hz
  (Voice) or 48000 Hz (Standard); the canonical 44-byte header.
- **Permissions:** the folder 0700 (group/other write removed if found), each
  recording 0600 whatever the umask (the helper sets `umask(077)`).
- **Size:** Voice 32 000 B/s = **1.92 MB per minute**, 115 MB per hour;
  Standard 96 000 B/s = **5.76 MB per minute**, 346 MB per hour.
- **Longest recording:** the data is capped just under 2 GiB so neither
  32-bit size field of the WAV can wrap or read as negative:
  **18 h 38 min** at Voice, **6 h 12 min** at Standard ("Longest possible
  recording reached", saved). In practice storage ends it first: unit A's
  root filesystem has about 120 MB free (docs/apps/CAMERA.md), about
  37 minutes of Voice above the reserve.
- **Storage reserve:** a recording does not start with less than 48 MiB plus
  10 s of audio free, and stops itself, saved, when the filesystem reaches
  48 MiB - the Camera's reserve, for the same reason. A disk that fills
  anyway (ENOSPC) stops and saves what fit. The free space at the current
  preset is shown ("Room for about 37 min").

### Integrity

A recording is written as `<name>.part`: born under a hidden name, flocked,
given a complete header (sizes 0) and synced with its folder before the first
sample, then renamed to `.part`. Every 2 s the header is rewritten with the
sizes so far and writeback of the new audio is started (`sync_file_range`,
which does not wait). Finishing writes the final header, fsyncs, renames to
the final name without replacing anything, and fsyncs the folder.

So a name without `.part` is always a complete file, and a `.part` is always
a readable WAV whose header is at most one checkpoint behind. The repair (at
every open, and after a helper crash) takes an unlocked `.part` - a locked
one has a live writer - and makes its header match the whole frames it holds,
cuts a half frame, syncs, and renames it `<name>-recovered.wav`. A `.part`
without audio is removed. One whose header cannot be read at all is left
exactly as it is and listed as unfinished; it is never renamed into
something that looks valid. What a power cut can lose is what the kernel had
not yet written back (a few seconds; `sync_file_range` starts it every 2 s).

## Signal processing and playback

- **DC blocker** (one pole, about 20 Hz) before everything: the codec
  settles with an offset of about -1200 after the discard (AUDIO_HARDWARE_MAP
  §14 item 12) that would otherwise be a thump in every file and a false
  level in silence. Measured: a settling offset is below 20 of 32767 after a
  second; a 1 kHz tone passes within 0.1 dB.
- **Voice** decimates 48 → 16 kHz with a 96-tap Blackman-windowed sinc,
  cut-off 7 kHz: flat within 0.5 dB to 4 kHz, 9.5 kHz down 69 dB, 12 kHz
  (which would alias to 4 kHz) down 78 dB (tests/rec_dsp_test.c). About
  1.5 M multiply-adds per second.
- **Standard** writes the device's samples (after the DC blocker): no
  resampling.
- **Playback** of a Voice file interpolates 16 → 48 kHz with the same filter
  (15 kHz image down 73 dB). Every played sample is clamped by pocketaudio to
  its -12 dBFS hardware ceiling; so that loud recordings are turned down
  rather than clipped, a limiter lowers the gain - never raising it again -
  exactly as far as the loudest sample so far needs. A quiet recording plays
  at its own level. The system volume (Controls) applies; muted, nothing
  plays and the screen says so. The file itself is never altered.
- **The meter** is the peak (and RMS) of the samples actually written or
  played in each 100 ms, on a -60..0 dBFS scale, with the peak of the last
  1.5 s marked; "Too loud" from -0.5 dBFS, "Silence" below -60, "No reading"
  when no fresh measurement arrived (paused, stalled). It is computed in the
  helper, so the UI only draws one number ten times a second.

## Resource cost

Measured on the development host (x86-64), **not on the K230**:

| | |
| --- | --- |
| Helper while recording (Voice / Standard) | ~0.5 % / ~0.4 % of one core; peak RSS 2.7 MB |
| Helper while playing (Voice / Standard) | ~0.3 % / ~0.2 % of one core; peak RSS 2.6 MB |
| App open, idle (heap incl. its LVGL objects, 12 recordings listed) | ~88 KB |
| 20 more opens and closes after warm-up | +0 bytes heap; shell fds unchanged |
| Finalizing on close | ~10 ms |

Estimated for the K230's C908: the 3:1 filter is ~1.5 M multiply-adds per
second, the rest is copying; Wave's helper, which runs ggwave's FFTs, was
measured at 0.6 % of the core while listening (AUDIO_HARDWARE_MAP §15), so a
few percent at most is expected. To be measured by the gate.

## Privacy

Recording starts only on RECORD. The header says MIC ON while the microphone
may be open (from the tap until the helper has closed it); the chip says
RECORDING. Nothing is uploaded, shared or sent anywhere; the app has no
network code (tests/recorder_lint.sh). Nothing is logged: no names, no audio;
the helper prints only its event lines to the app and its usage to stderr.
Files are 0600 in a 0700 folder.

## Tests

`make test` runs them all; `make recorder-test` just these;
`make recorder-san-test` the C suites and the helper under ASan, UBSan and
leak checking.

- `tests/rec_wav_test.c`: the header byte for byte, probes of complete, cut,
  over-long, placeholder, stereo and extensible files, the repair, the size
  limit, 2000 random headers.
- `tests/rec_dsp_test.c`: filter gain, passband and rejection both ways, DC
  blocker, meter, limiter.
- `tests/rec_names_test.c`: names made, parsed and refused.
- `tests/rec_file_test.c`: a real folder: complete, colliding, empty,
  abandoned, disk-full and at-the-limit recordings, and the repair of every
  way a writer can disappear.
- `tests/rec_engine_test.c`: recording and playback steps against the real
  pocketaudio over a scripted device: short reads, timeouts, overruns,
  failures, slow reads, a device returning more than asked; the start-up
  transient; disk full; the WAV limit; partial accepts on playback.
- `tests/rec_state_test.c`: every transition, rapid start/stop, repeated
  stop, close in every state, exhaustive state x input.
- `tests/rec_view_test.c`: every word on the screen.
- `tests/rec_store_test.c`: folder, permissions, list, cap, names, delete
  rules, preset persistence.
- `tests/rec_session_test.c`: a misbehaving fake helper (flood, garbage,
  over-long lines, ignoring stop, crashing) and the real helper (record,
  pause, resume, stop, play, SIGKILL and repair, space before and during,
  disk full, busy device, abandon, ten cycles).
- `tests/rec_ctl_test.c`: the controller end to end with the real helper.
- `tests/rec_tool_test.sh`: the CLI, stops by signal, command and closed
  stdin, permissions, no test hooks in the shipped helper.
- `tests/recorder_lint.sh`: the boundaries above.
- `tests/rec_app_test.c` via `tests/recorder_shell_test.sh`: the app under a
  real pointer in both orientations (targets, safe area, no scrolling but the
  list), a whole record/pause/resume/stop/play/delete journey, rotation mid
  recording, 20 open/close cycles (no helper, no .part, no objects, no heap
  growth), and the real shell opening Recorder in both orientations.

## Follow-ups

1. A shared helper-process client (spawn, event queue, bounded stop) for
   Wave, Camera and Recorder, in `core/`; today each app has its own.
2. Rename in the app (Files covers it today).
3. The Unit A gate, including the C908's measured costs.
