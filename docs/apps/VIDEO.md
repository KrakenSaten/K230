# Video

A local video player: the MP4 files in one folder, played with the K230's
hardware decoder. No streaming, no online services, no media library.

Status: **v0.1 on branch `feat/video-player`, not merged.** ADR-012
(PROPOSED) decides who owns the video decoder and the sound card for it;
DS §40 (PROPOSED) is the screen. Hardware gate on unit B: PASS with
recorded limits (docs/hardware/VIDEO_GATE.md).

## What it does (v0.1)

- **The list.** The playable files in the videos folder, sorted by name,
  with their sizes; RESCAN reads the folder again. The folder is
  `$POCKETOS_VIDEOS_DIR` when that is an absolute path, else
  `$HOME/Videos` (`/root/Videos` on the device), made on the first visit
  so there is somewhere to copy videos to. Only `*.mp4` files are listed
  (any case), at most 200; an empty folder says where to copy files.
- **The player.** Choosing a file plays it: the picture fitted into its
  frame (never enlarged), the file name and what the player is doing
  (Opening..., Playing, Paused, Stopped, Ended, or the error), the
  progress bar with elapsed time and length, and BACK, PLAY/PAUSE, STOP
  and FULLSCREEN. A tap on the picture plays or pauses; a tap or a drag
  on the bar seeks (once, on release). STOP goes back to the start,
  paused; PLAY after the end plays again from the start.
- **Fullscreen.** The picture takes the whole body: in landscape the
  whole panel (the app has no shell header there), in portrait the body
  under the shell's header. A tap on the picture leaves fullscreen.
- **Sound** plays at the system volume if the file has a sound track
  FFmpeg can decode. Muted (volume 0), the sound card is never opened. If
  the card is busy (Wave or the Recorder has it) the status says "Sound
  busy" and the picture plays on.
- **Landscape and portrait** both work. Landscape is the primary shape:
  the controls are a column beside the picture. In portrait they are a
  row under it.

## Formats

**Supported and tested:** MP4 (the `mov,mp4` demuxer) with H.264 pictures,
decoded by the video processing unit (VPU), with or without an AAC sound
track. Tested on unit B with:

| File | Pictures | Sound | Result |
| --- | --- | --- | --- |
| `doors-test-720p30.mp4` (made on the unit: FFmpeg `testsrc2` + a 440 Hz tone, VPU-encoded) | 1280x720 H.264 High, 30 fps | AAC LC mono 44.1 kHz | plays, 30 fps, no drops, sound running |
| vendor sample `video01.mp4` (image's `/root/videos`) | 1280x720 H.264 Constrained Baseline, 30 fps | none | plays, 30 fps, "No sound" |
| vendor sample `video02.mp4` | 720x1320 (portrait) H.264 High, 30 fps | AAC LC stereo 44.1 kHz | plays, letterboxed, sound running |

**Refused:** any other container (the list does not show it, and the
helper says "`<name>` files are not supported"), any other picture codec
("`<codec>` pictures are not supported"), and a file with no pictures.
The VPU also lists HEVC, VP8, VP9, AV1, MPEG-2/4 and MJPEG, and the image's
FFmpeg has wrappers for most of them; none of them is claimed until it has
been tested. Other sound codecs FFmpeg decodes (MP3, Opus, AC-3, FLAC,
Vorbis) are used when present but only AAC has been tested.

Nothing falls back to software H.264: measured on unit B, the software
decoder takes about 50 % of the single CPU at 720p before any conversion.

## Architecture

Apps do not touch hardware (ADR-002). A video file is untrusted input to a
large library and a vendor driver, so it is decoded in a child process:

| Part | Where | Role |
| --- | --- | --- |
| `video_app.c` | apps/video | the screen (LVGL); polls the session every 10 ms; copies pictures into its own buffer |
| `video_state.c` | apps/video | the state machine: taps and helper events in, actions out; pure C |
| `video_layout.c` | apps/video | the two shapes and fullscreen; pure arithmetic |
| `video_files.c` | apps/video | the folder, the list, sizes and times; pure C |
| `video_session.c` | apps/video | starts `pos-video`, the shared memory, the line protocol, the watchdog; pure C |
| `video_proto.h` | apps/video | the protocol and the shared-memory layout, shared by both sides |
| `pos_video.c` | tools/video | the helper: `session`, and `probe`, `bench`, `recover` for the bench |
| `video_player.c` | tools/video | the engine: clock, pacing, slots, seeking, the sound thread |
| `video_backend_ffmpeg.c` | tools/video | FFmpeg + the VPU (`POCKETVIDEO_FFMPEG=1`, the device) |
| `video_backend_fake.c` | tools/video | a scripted text "video" (hosts, the simulator, tests) |

**One helper per played file.** Choosing a file starts `pos-video
session`; BACK, leaving the app, a rotation (the shell closes the app
before it restarts itself) or the shell dying ends it. Nothing a damaged
file did outlives its playback.

**Pictures** travel in a sealed memfd of four RGB565 slots (1280 x 720
pixels each, 7.4 MB) that the shell maps read-only. Ownership of a slot
goes by message - `frame <slot> ...` from the helper when the picture is
due, `release <slot>` back - and the app copies the newest picture out and
releases it in the same timer tick, so no LVGL image ever points into
memory the helper writes. The helper never writes a slot the app holds.

**The decode path** (FFmpeg 4.4 as the image ships it, dynamically
linked): libavformat reads the MP4; H.264 goes to `h264_v4l2m2m` on
`/dev/video_vpu`, whose vendor `dsl_width`/`dsl_height` options make the
VPU scale the picture to the frame on the way out, so the CPU never sees
a 720p picture; libswscale converts the scaled YUV 4:2:0 to RGB565
directly into the slot; libavcodec decodes the sound and libswresample
makes it mono 48 kHz for `core/pocketaudio` (unchanged). The helper sizes
pictures to a width that is a multiple of 8 and an even height: at an odd
width the VPU's scaler leaves a green stripe (measured, unit B).

**Time.** The picture follows a clock: the sound's when sound plays (the
position handed to pocketaudio, less the 80 ms the device holds), else the
monotonic clock, re-anchored where the sound stopped. Play waits up to
500 ms for the first sound. A picture decoded more than 100 ms late is
dropped before it is converted; when two are due, the older is dropped.

**Sound** has a thread of its own inside the helper (the only thread in
Video, and the only first-party thread in a helper), so a picture being
decoded never starves the 80 ms device buffer. It owns the pocketaudio
stream: opened on play, closed on pause, stop, seek, end and quit - which
also turns the amplifier off and releases the audio lock.

**Demuxing** keeps the other stream's packets aside (up to 1024 packets or
8 MB) while it looks for one of this stream, because an MP4 interleaves
sound and pictures only roughly and the sound must not wait behind
pictures nobody wants yet.

**Seeking** re-positions the demuxer at the key picture before the target,
reopens the hardware decoder (a close and open is the sequence the driver
certainly supports), discards pictures and samples before the target, and
shows the first picture at the target at once, playing or paused. A seek
while one is being answered replaces any waiting one (the app sends only
the newest). When a file has no key picture at or before the target (the
start of a file whose first picture is stamped after 0, which is what STOP
and PLAY after the end ask for), the next key picture is taken.

**Changing the frame's size** (fullscreen, rotation) sends `view`, and the
helper reopens the decoder at the new size from where it was.

**Lifetime and watchdog.** The helper gets SIGTERM when the shell dies
(PR_SET_PDEATHSIG). Leaving sends `quit` and SIGTERM and waits 300 ms
before SIGKILL. Every wait has a deadline: 3 s for the first line, 10 s to
open a file and show its first picture, 5 s for an answer to play, pause,
stop or seek, 4 s of silence while playing (a playing helper says `pos`
four times a second). A missed deadline kills the helper; one that ended
by a signal gets `pos-video recover` started for it, detached, so the
sound card's route and amplifier are put back (as the Recorder does).

## The protocol

`apps/video/video_proto.h` is the reference. Commands: `view <w> <h>`,
`open <path>`, `play`, `pause`, `seek <ms>`, `stop`, `release <slot>`,
`quit`. Events: `hello`, `opened <duration> <w> <h> <fps_x100> <audio>
<codec>`, `openfail <reason> <text>`, `frame <slot> <seq> <w> <h> <pts>`,
`state <playing|paused|stopped|ended> <ms>`, `pos <ms>`, `seeked <ms>`,
`audio <word>`, `stats <fps_x10> <shown> <dropped> <late> <cpu%> <rss_kb>
<xruns>`, `error <decode|device|io> <text>`, `bye`. The shell logs the
statistics every 10 s while playing (never a file name).

## The fake backend

A fake video is a text file whose first line is `DOORS-FAKE-VIDEO` and
`key=value` words: `w=`, `h=`, `fps=`, `ms=`, `gop=`, `audio=0|1|bad`,
`codec=`, `fail=missing|unsupported|corrupt|device|io`, `error_at=N`,
`hang_at=N`, `crash_at=N`, `slow_ms=N`, `output_fail=1`. Anything else is
a damaged file. Pictures are a pattern that moves one step a frame, with
the picture number in the first pixel. The simulator's shell defaults to
it (`VIDEO_BACKEND_DEFAULT="fake"`, SDL builds only) and says SIMULATED.

## Bench tools

```
pos-video probe FILE                 # what open says, time to the first picture
pos-video bench [--size WxH] FILE    # every picture as fast as possible, CPU
```

On unit B, `bench` of the 20 s 720p30 test clip (all 600 pictures and the
whole sound track): 118 pictures/s and 12 % of real time in CPU at
640x360, 110 and 18 % at 816x460, 93 and 25 % at 1008x568.

## Tests

- `tests/video_files_test.c` (30 checks): names, the folder from the
  environment, sorting, sizes, links, the bounds, time and size texts.
- `tests/video_layout_test.c` (44): both shapes and fullscreen on the
  reference panel, the corners, touch targets, overlaps, too-small bodies.
- `tests/video_state_test.c` (71): choosing, opening, play/pause/stop,
  rapid taps, coalesced seeks, dragging, the end, every open failure, a
  helper that errs, hangs, crashes or misbehaves, sound changes,
  fullscreen, BACK, the abandoned helper's last words, reopening.
- `tests/video_player_test.c` (81): the engine in process with the fake
  backend and the file-backed sound card: open (valid, damaged, missing,
  unsupported, no decoder), the first picture paused, 25 fps, pause,
  resume, seek (paused, playing, past the end), stop, resize while
  playing, the end and playing again, a decode error mid-file (what was
  decoded is still shown), a slow decoder (dropped and late pictures,
  real-time position), 300 random commands, a session that holds every
  slot (never overwritten), the card opened on play and closed on pause,
  end and destroy, a busy card, muted, an undecodable sound track, closing
  while playing.
- `tests/video_session_test.c` (39): the client against the real helper:
  hello, open, the first picture copied out, play/pause/seek/stop
  answered, an invalid file then a good one, a buffer too small, a quiet
  paused helper left alone, a hung decoder killed by the watchdog and
  recovered, a crash, no hello, another protocol version, impossible
  pictures, an exec failure, leaving while playing, twenty reopens
  without a descriptor left.
- `tests/video_app_test.c` (45, SDL builds, run by
  `tests/video_shell_test.sh`): the screen with the real helper in both
  orientations, tapped: the list, play, pause, stop, a tap on the bar,
  every control a usable safe target, fullscreen and back, the end, BACK,
  a damaged file, the landscape back slab, closing while playing, twenty
  opens and closes with no helper or object left.
- `tests/video_lint.sh`: the boundaries (no FFmpeg, audio or devices in
  the app; FFmpeg only in its backend; the hardware decoder only; the one
  thread; deadlines; sealed memory; hooks in the test build only;
  registration).
- `make video-test`, `make video-san-test` (address and undefined-behaviour
  sanitizers, all suites clean) and `make video-tsan-test` (the engine's
  two threads; on this WSL kernel run it with `setarch -R`).

## Limitations (v0.1)

- One format family tested: MP4 + H.264 (+ AAC). See Formats.
- No upscaling: a video smaller than its frame is shown at its own size.
- Seeks land on the first picture the decoder gives back at or after the
  target; on the VPU-encoded test clip that was up to 200 ms after it (the
  vendor samples landed exactly), and a seek costs 0.7 to 1.2 s (the
  decoder is reopened and up to one key interval decoded).
- The volume is taken when a file is opened; a change during playback
  applies to the next file.
- Pausing closes the sound card; the up to 80 ms the device held is
  dropped, so the picture moves on by that much on resume.
- A rotation restarts the shell (shell policy): playback ends and the app
  starts on its list.
- The controls do not hide by themselves; fullscreen hides them.
- No keyboard keys (Space for play/pause is the obvious first).
- The folder is read on the LVGL thread (one bounded readdir per visit).
- Portrait playback was tested on the host only; unit B was gated in
  landscape.

## Deferred

Other codecs and containers (after each is tested), subtitles, a resume
position, playlists, background playback, streaming, HDMI output (the
shell drives one display), a hardware colour conversion path (the
`canaan-non-ai-2d` block; the CPU cost measured does not need it yet),
auto-hiding controls, and keyboard keys.
