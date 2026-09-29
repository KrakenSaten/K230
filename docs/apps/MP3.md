# MP3

Local music on the device: MP3 files (and the other formats below) already
on its storage, played through the built-in speaker. Not a streaming app:
nothing is fetched, nothing leaves the device, and there is no media
library - one folder at a time.

Status: **v0.1 on branch `feat/audio-player`, not merged.** The design
amendment is DS §40 (PROPOSED). Hardware gate on unit B
(`docs/hardware/MP3_GATE.md`): **PASS on dd2e343** except audibility, which
is the owner's to hear.

## What it does

- **Browse.** The list starts at the places - Music (`$HOME/Music`, made the
  first time), Home, Recordings (what Recorder saved plays here too), and
  removable storage under `/mnt` and `/media` - and opens one folder at a
  time: folders first, then audio files, each by name. UP climbs to the
  place and then to the places. The folder last shown comes back next time.
- **Play.** A file tapped plays, and the rest of its folder follows it: the
  queue is that folder's audio files as the list showed them.
- **Control.** PLAY/PAUSE, STOP, PREV, NEXT; a tap or a drag on the
  progress bar seeks; VOL - and VOL + step the system volume. PREV more than
  3 s into a track starts it again, otherwise it plays the one before. NEXT
  and PREV while stopped only choose a track.
- **Show.** The title and artist from the file's own tags when it has them,
  else the file name and its folder; elapsed and length; the place in the
  queue (`3 / 9`); the current track marked in the list (`NOW`).
- **End.** A track's end starts the next. The queue's end stops and says
  so. Leaving the app stops the music.

Nice-to-haves left out on purpose (the brief said not to widen v0.1):
repeat, shuffle, album art, background playback.

## Backend: what the image already had

Nothing was added to the image. The K230 image has carried **FFmpeg 4.4.4**
since v0.0.1, because Buildroot selects it for OpenCV
(`BR2_PACKAGE_OPENCV4_WITH_FFMPEG=y` in
`platforms/k230/configs/k230_pocketos_defconfig`), with Buildroot's default
of every decoder, demuxer and parser, `--disable-gpl --disable-nonfree`.

| Fact | Class | Evidence |
| --- | --- | --- |
| `libavformat.so.58`, `libavcodec.so.58`, `libswresample.so.3`, `libavutil.so.56` in `/usr/lib` | VERIFIED | unit B on the v0.2.1 image (build 9dc66c2), 2026-09-29 |
| Headers and `.pc` files in the SDK sysroot | VERIFIED | `~/work/t-display-k230/.../sysroot/usr/include/libav*`, 2026-09-29 |
| `CONFIG_MP3_DECODER`, `CONFIG_MP3FLOAT_DECODER`, `CONFIG_MP3_DEMUXER`, `CONFIG_SWRESAMPLE` | VERIFIED | `ffbuild/config.mak` in the SDK build tree |
| The image's own samples (`/root/music/music01..09.mp3`) probe as `mp3float`, 44.1/48 kHz stereo, in about 35 ms each | VERIFIED | cross-built `pos-mp3 probe` on unit B, 2026-09-29 |
| LGPL-2.1-or-later, dynamic linking | DOCUMENTED | `docs/legal/manifest.csv`, `docs/LICENSING.md` |

So no decoder was written and no library vendored. `pos-mp3` links FFmpeg
dynamically, as `pos-browser` links libcurl: a build dependency
(`POCKETOS_DEPENDENCIES += ffmpeg`, `MP3_FFMPEG=1`), not a package.

The output side is unchanged `core/pocketaudio`: 48 kHz mono S16 to the
speaker through the one audio lock that Wave and Recorder already share.

### Formats

Listed as audio (by extension): `mp3`, `wav`, `flac`, `ogg`, `oga`,
`opus`, `m4a`, `aac`. What really plays is FFmpeg's decision on the device:

- **MP3** - the v0.1 target: decoded and played on unit B (VERIFIED,
  2026-09-29; heard: owner).
- **WAV, FLAC, Ogg Vorbis, Opus, AAC/M4A** - each decoded and played on
  unit B from a 12 s file made by the unit's own `ffmpeg` (VERIFIED,
  2026-09-29; heard: owner).
- Anything else is refused with "not an audio file" or "no decoder for
  this audio" before the device is touched.

A host build of `pos-mp3` (no FFmpeg development files on the build host)
decodes 16-bit PCM WAV only; that is what the tests run.

## Architecture

```
apps/mp3/                                 (in the shell, the LVGL thread)
  mp3_app.c      the screen: paints mp3_view, forwards taps, 50 ms timer
  mp3_view.c     the view model: every text and every enabled state
  mp3_ctl.c      the controller: folder, queue, volume, end-of-track rules
  mp3_player.c   one track at a time: start, pause, seek, stop, how it ended
  mp3_session.c  the helper client: socketpair, non-blocking, bounded
  mp3_library.c  places, one folder read, last folder; the scanner thread
  mp3_protocol.h the line protocol, shared with the helper
        |  one child process per track, stdin/stdout on a socketpair
        v
tools/mp3/pos_mp3.c                       (pos-mp3, its own process)
  mp3_decoder_ffmpeg.c   the image's decoder (FFmpeg)
  mp3_decoder_wav.c      the host decoder (WAV, for tests)
  core/pocketaudio       lock, route, amplifier, bounded writes, recovery
```

- **Nothing blocks the LVGL thread.** Decoding and the device are in the
  helper process. A folder is read on the scanner's thread (one per scan,
  joined by nobody: it frees itself; at most four may wait on hung storage,
  after which scans are refused). The timer makes only non-blocking calls.
  The one wait is `destroy()` giving a playing helper 1 s to close the
  device (it takes one audio wait, 200 ms; measured under 20 ms on the host)
  before it is killed.
- **One helper at a time.** The audio lock admits one stream, so a new track
  never starts while the old helper lives: NEXT while playing stops the old
  helper and starts the new track when it has gone. A burst of NEXTs moves
  the choice at once and starts one helper, for the last choice.
- **Pause closes the device** (amplifier off, the lock free for Wave or
  Recorder) and keeps the decoder and the position. A resume the device
  refuses stays paused and says why.
- **Level.** Decoded music is full scale and pocketaudio clamps at
  -12 dBFS, the validated ceiling (AUDIO_HARDWARE_MAP §11). `pos-mp3`
  therefore attenuates by those 12 dB before the clamp instead of letting it
  clip, then applies the system volume on pocketaudio's own curve
  (`pocketaudio_volume_gain_q15`). The stream is opened at 100 % and the
  volume applied in the helper, so a change is heard at once without
  reopening the device.
- **Volume** is the one system volume (`pocketos_shell_volume_*`, the same
  as Controls), in 10 % steps. Muting pauses what plays; nothing starts
  while muted; VOL + unmutes.
- **The file first.** The helper opens and probes the file before the
  device, so a missing, empty or damaged file never switches the route or
  enables the amplifier.
- **Only local files.** FFmpeg is given `file:<path>` with a protocol
  whitelist of `file`; no path can make it open a network or a device.
- **Closing and dying.** A closed socket, SIGTERM or "stop" ends the helper
  within one audio wait. A helper killed outright is followed by a detached
  `pos-mp3 recover`, which restores the audio route (pocketaudio's recovery
  record), as Wave and Recorder do. No callback can reach a destroyed app:
  destroy() deletes the timer and removes every event callback that carries
  the app before freeing it.

### Errors, and what the screen says

| Case | Helper | Screen |
| --- | --- | --- |
| File missing (or storage removed) | `error missing`, exit 4 | "x.mp3 is not there any more." |
| Read error part way | `error storage`, exit 4 | "x.mp3 could not be read. Was the storage removed?" |
| Empty, not audio, unsupported | `error format`, exit 5 | "x.mp3 cannot be played: the file is empty." (FFmpeg's reason) |
| Damaged part way (32 bad packets in a row) | `error decode`, exit 5 | "x.mp3 stopped playing: the file is damaged." |
| Device in use (Wave, Recorder) | `error audio_busy`, exit 3 | "The audio device is in use (Wave or Recorder)." |
| No device / not validated | `audio_nodev` / `audio_disabled` | said, nothing plays |
| Helper missing or crashed | exit 127 / signal | "Playback failed: ..." (and `recover` after a signal) |
| Folder gone | (scanner) | "x cannot be opened. Was the storage removed?" |

A file that fails when the user chose it is said, and playback stops. A
file reached by the previous track's end is skipped with the same words
("Skipped: ...") and the next one tried, at most once round the queue.

### Bounds

- 200 entries kept from a folder (and so the longest queue), 4096
  directory entries looked at; the list says when there were more.
- 16 events queued between polls; progress readings are dropped first.
- Title and artist: 128 bytes of valid UTF-8, control characters dropped.
- No recursive scan anywhere.

### Resource cost (unit B, 2026-09-29, a 44.1 kHz stereo MP3)

`pos-mp3` 3.5 % CPU and 9.5 MB RSS while playing; the shell 1.9 % CPU; the
shell's IPC round trip 22 ms mean both idle and playing; probing a file
about 35 ms; closing while playing 0.3 s from tap to device closed.

## Protocol

`apps/mp3/mp3_protocol.h` is the whole contract. In short: `pos-mp3 play
--events [--start-ms MS] --volume-percent V FILE`; events `ready`,
`recovered`, `meta title|artist <text>`, `playing <total_ms> <seekable>
<rate> <channels> <codec>`, `progress <ms>` (every 250 ms and after a
seek), `paused`, `resumed`, `played`, `stopped`, `error <code> <text>`;
commands `pause`, `resume`, `stop`, `seek <ms>`, `volume <percent>`.
`pos-mp3 info` and `pos-mp3 probe FILE` open no audio.

## Tests

All on the host, none on audio hardware (`make mp3-test`, also part of
`make test`; `make mp3-san-test` runs them under ASan/UBSan with leak
checking):

- `tests/mp3_decoder_test.c` - the WAV decoder at six rates, the stereo mix,
  seeking, and every bad file; the tag cleaner (UTF-8, Latin-1, surrogates,
  cuts).
- `tests/mp3_library_test.c` - places and their environment, one folder
  (links, hidden names, order, sizes), the 200 and 4096 boundaries, the path
  rules, the remembered folder, and the scanner with storage that hangs.
- `tests/mp3_session_test.c` - the parser; the scripted fake helper
  (`tests/fake_pos_mp3.sh`: floods, garbage, over-long lines, a helper deaf
  to stop, one that crashes); the real helper over the file-backed sound
  card: to the end, pause (device closed), resume, seek, start part way,
  volume, stop, abandon; every bad file with the device untouched; the
  device in use.
- `tests/mp3_ctl_test.c` - the first screen, places and folders,
  play/pause/stop, next/prev playing and stopped, end-of-track and
  end-of-queue, bad files said or skipped, decode and storage failure, the
  helper missing, a refused resume, bursts of NEXT and PLAY/PAUSE with one
  helper at a time, volume and mute, a track and a folder disappearing, the
  200-entry boundary, the remembered folder, and ten closes while
  starting, playing or paused (no helper, thread or descriptor left).
- `tests/mp3_app_test.c` + `tests/mp3_shell_test.sh` - the screen under a
  real pointer in both orientations (see DS §40.5) and the real shell.
- `tests/mp3_lint.sh` - the boundaries above as source rules.

## Follow-ups

1. **Gaps between tracks.** Each track is a new helper, so the device is
   closed and opened between tracks (a fraction of a second of silence, the
   amplifier off and on). Gapless playback would need one helper for the
   queue.
2. **Tags in the list.** Titles and artists are read by the helper for the
   track that plays; the list shows file names.
3. **Audibility.** The gate proved the device path (the helper owns the
   PCM, the substream runs, the position advances in real time); the owner
   has still to hear it, and the volume steps.
4. **Repeat, shuffle, album art, background playback** - out of v0.1 scope.
   Background playback would need ADR-010's revisit (an `audiod`).
5. **A shared helper-process client** for Wave, Recorder, Camera and MP3
   (the same follow-up Recorder recorded).
6. **Launcher place and icon** are for the owner to confirm (DS §40.4).
