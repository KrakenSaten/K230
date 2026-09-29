# ADR-012: Who owns the video decoder and the sound card for Video

Status: Proposed
Date: 2026-09-29
Deciders: product owner (final), AI engineering partner (author)

## Context

The Video app (docs/apps/VIDEO.md) plays local MP4 files: pictures from the
K230's video processing unit (VPU) and, when the file has one, its sound
track through the speaker. It is the first user of the VPU and the third
audio user after Wave (ADR-004) and the Recorder (ADR-010).

Constraints already binding:

- ADR-002 point 2: services own hardware; apps never touch it.
- ADR-004 and ADR-010 accepted a per-operation helper process on
  `core/pocketaudio` as narrow exceptions for Wave and the Recorder, and
  say a later need is decided against ADR-002, not by extending them. They
  name when to move to an `audiod`: sound outside an app's own screen, or
  mixing. Video needs neither: its sound plays only while its screen is
  open, and never together with another program's.
- The shell is one LVGL loop; nothing on it may block.
- ADR-001: no toolchain, SDK or dependency upgrades without a proposal.

What the image already has (unit B, v0.2.1 image, 2026-09-29, VERIFIED):
FFmpeg 4.4.4 as shared libraries (libavformat, libavcodec, libavutil,
libswscale, libswresample; `--disable-gpl`, so LGPL-2.1+) built by the
vendor with Canaan's patches for the VPU; `/dev/video_vpu` (the Arm
"mvx" V4L2 memory-to-memory driver) decoding H.264 at about 120 pictures
a second at 720p and scaling on the way out; alsa-lib. No gstreamer, no
ffplay, no mpv. The headers are in the pinned SDK's sysroot.

What playing a video needs: a file read and parsed, a hardware decoder
held and fed for minutes, a picture converted every 33 ms, sound written
every 20 ms without a gap, all of it robust to damaged files and to the
app leaving at any moment.

## Options

### A. A `videod` / `mediad` service over pocketipc (ADR-002 as written)

- Pro: the letter of ADR-002; one owner for the VPU system-wide.
- Con: a permanent daemon for an app that is closed almost all the time;
  pictures at 30 per second do not belong in a JSON IPC contract, so the
  shared memory and slot ownership would be needed anyway; the sound card
  would still need the audio lock or an `audiod`; the largest change.

### B. Decode in the shell (a thread in the app)

- Pro: least code, no copy between processes.
- Con: a damaged file that crashes FFmpeg or wedges the vendor driver
  takes the whole shell (and the panel) with it; the shell would link
  FFmpeg, pocketaudio and alsa-lib; contradicts app.h.

### C. A per-playback helper process, `pos-video` (recommended)

- `pos-video session`, started by the app for one played file, over a
  socketpair (commands in, events out) and a sealed memfd of four picture
  slots whose ownership passes by message - Camera's shape (ADR-006).
- The helper owns the file, the VPU (through FFmpeg's `h264_v4l2m2m`) and,
  for the sound, the sound card through the unchanged `core/pocketaudio`
  (its lock, route, amplifier, level ceiling and system volume, and its
  recovery record), on a thread of its own inside the helper.
- Pro: no decoding, no hardware and no blocking on the LVGL thread; a
  crash or a hang ends one helper, which the watchdog kills and the kernel
  cleans up after (`pos-video recover` restores the sound route after a
  kill, as the Recorder's helper does); PR_SET_PDEATHSIG ends it with the
  shell; Video, Wave and the Recorder exclude each other through the one
  `audio.lock`; the bench tool and the app are one binary; host-testable
  end to end with a fake backend.
- Con: still not the letter of ADR-002; a fourth small line protocol
  (`apps/video/video_proto.h`); one copy of each shown picture from the
  shared memory into the app's buffer (about 1 MB at fullscreen, 30 times
  a second: 7-8 % shell CPU measured on unit B, all of the shell's work
  for the picture included).

## Decision (proposed)

**Option C**: the video decoder and, for a video's sound, the sound card
are owned per played file by `pos-video`, as a third narrow exception to
ADR-002 point 2 of the same shape as ADR-004 and ADR-010. Scope: the VPU
(`/dev/video_vpu`) and the sound card's playback path and amplifier line
(through pocketaudio), for Video only, only while its player is on screen.

The decoding library is the FFmpeg the image already ships, linked
dynamically; the Buildroot package gains a build dependency on `ffmpeg`,
not a package (no dependency upgrade, ADR-001). Only H.264 in MP4 is
accepted until other formats are tested; there is no software H.264
fallback.

## Consequences

Needed now (done on `feat/video-player`):

- `tools/video/pos-video`, installed to `/usr/bin`; `POCKETVIDEO_FFMPEG=1`
  in the package build; `ffmpeg` in `POCKETOS_DEPENDENCIES`; the deploy
  script carries the helper; the notices test and docs/LICENSING.md record
  FFmpeg (already in the image, LGPL-2.1+, dynamically linked).
- The shell links no FFmpeg, no pocketaudio and no alsa-lib
  (tests/video_lint.sh).

Useful soon:

- A shared helper-process client for Camera, Vision, Recorder, Wave and
  Video (each has its own, kept separate so no app reaches into another).
- If a fourth audio user appears that needs sound outside its own screen or
  mixing, ADR-004's trigger for an `audiod` is met; pocketaudio would then
  be its backend, and pos-video one of its clients.

Risks:

- The VPU is a vendor driver behind vendor FFmpeg patches. Its behaviour on
  reopen (every seek and resize), on drain at the end of a file, and after
  a helper is ended mid-decode was exercised on unit B (46 files opened,
  a few dozen seeks and resizes, 40 open/play/leave cycles, no leak in CMA,
  no lock-up) but is not proven for every file. A hang is contained by the
  watchdog.
- FFmpeg is large untrusted-input code; it runs as root in the helper, like
  every other Doors helper. Sandboxing helpers is a platform question, not
  Video's.

Migration cost: none; nothing existing changes. Removing Video removes the
helper, the app and one build dependency.

## Evidence

- VERIFIED on unit B (2026-09-29, v0.2.1 image, shell build 87c9f82):
  `ffmpeg -decoders` lists `h264_v4l2m2m`; `/sys/class/video4linux/video0`
  is `mvx`, `/dev/video_vpu` links to it; `v4l2-ctl --list-formats-out`
  lists H.264, HEVC, VP8, VP9, AV1 and more; 720p H.264 decodes at 118 fps
  scaled to 640x360 (`pos-video bench`); odd scaler widths leave a green
  column, widths that are multiples of 8 do not; playback at 30.0 fps with
  0 dropped pictures windowed and fullscreen, helper 13 % / 25 % CPU, shell
  7-8 %; sound running while playing, the PCM closed while paused, after
  BACK and after leaving; 40 open/play/close cycles with the shell's RSS,
  descriptors and CMA unchanged; Camera and Vision working afterwards
  (docs/hardware/VIDEO_GATE.md).
- DOCUMENTED: the vendor FFmpeg patches (SDK overlay `package/ffmpeg`,
  0006-0012: VPU formats, DMA-buf, the decoder scaler); FFmpeg's licence in
  docs/legal/manifest.csv.
- ASSUMED: that the VPU driver copes with every H.264 profile the files a
  user copies will use (Constrained Baseline and High were tested).
