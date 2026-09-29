# Video hardware gate - unit B, 2026-09-29

**Unit B (K230-B, Wi-Fi 192.168.10.187) carried during this gate: shell
build `87c9f82` (`/usr/bin/doors-shell` md5 `824e78b0…`), `pos-video` md5
`b3042b61…` (87c9f82 plus the seek fix of the next commit; the first
install was `6b54294a…` from 87c9f82), `icon-video.bin` md5 `af73e32f…`,
hand-installed over the v0.2.1 image. Rotation: landscape (the unit's
mode, unchanged). After the gate the unit was put back on the
combined-master shell `82084ab7` (build a680c56) by
`/root/rollback-video/RESTORE.sh`; Video is not on the unit now.**

Branch `feat/video-player`. Payload kept on the bench PC:
`out/video-gate/payload-87c9f82/` (MD5SUMS beside it). Gate scripts:
`out/video-gate/*.sh`, `drive.py` (the helper's protocol on the device,
without the shell), captures `out/video-gate/caps/`.

## Verdict

**PASS for v0.1 in landscape**, with one defect found and fixed during
the gate (seek to 0 on a file stamped after 0) and the limits below. Not
checked: the sound by ear (nobody at the bench; the PCM state and the
absence of xruns were checked instead), portrait on the unit, the
keyboard base.

## Feasibility (before any code)

| Question | Answer (VERIFIED on unit B unless marked) |
| --- | --- |
| Decoders in the image | FFmpeg 4.4.4 shared libs + `ffmpeg` CLI (no ffprobe/ffplay); software h264/hevc/vp8/vp9/mpeg4/aac/mp3/opus/…; V4L2 m2m wrappers for h264/hevc/vp8/vp9/mpeg2/mpeg4/vc1/mjpeg |
| Media frameworks | no gstreamer, mpv, mplayer |
| Hardware decoder | `/dev/video_vpu` -> `/dev/video0`, driver `mvx` ("Linlon Video device"), V4L2 M2M multiplanar; vendor FFmpeg option `dsl_width/dsl_height` scales on output |
| Realistic without a rebuild | MP4 + H.264 on the VPU, AAC sound (both in the image already) |
| Presenting frames | RGB565 into shared memory, copied into an LVGL image by the app (Camera's model); no change to the display stack |
| Audio | `core/pocketaudio` (mono 48 kHz, lock, amplifier), as Wave and Recorder |
| Speed | VPU 720p: ~120 fps; software H.264 720p: 9.2 s CPU for 18.2 s of video (~50 %); VPU + scaler + swscale to 640x360 RGB565: ~11 % |
| Linux CPUs | 1 (`nproc`) |

## Measurements

Clip: `doors-test-720p30.mp4`, made on the unit (`ffmpeg -f lavfi -i
testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 -t 20
-c:v h264_v4l2m2m -b:v 2500k -c:a aac`): 1280x720 H.264 High, 30 fps,
AAC LC mono, 20 s, 6.6 MB.

| Case | Picture | fps shown | Dropped / late | Helper CPU | Shell CPU | System busy | Helper RSS | xruns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| windowed, with sound | 648x368 | 30.0 | 0 / 0 | 13 % | 7 % | 27 % | 35.6 MB | 0 |
| fullscreen, with sound | 1008x568 | 30.0 | 0 / 0 | 25 % | 8 % | 42 % | 47.9 MB | 0 |
| vendor 720p30, no sound, windowed | 648x368 | 30.0 | 0 / 0 | 10 % | 7 % | 22 % | 31.9 MB | - |
| vendor 720p30, no sound, fullscreen | 1008x568 | 29.5 | 0 / 0 | 22 % | 7 % | 38 % | 44.3 MB | - |
| vendor 720x1320 High + AAC, windowed | 200x368 | 29.7 | 0 / 0 | 6 % | - | - | 29.7 MB | 0 |

Across a session with pause, resume, seeks and fullscreen changes: 2
pictures dropped in total (around the seek and the resize), none late.
`pos-video bench` (decode only, all pictures and sound, no clock): 118 fps /
12 % of real time at 640x360, 110 / 18 % at 816x460, 93 / 25 % at 1008x568.
First picture: 0.6-1.0 s after open. A seek: 0.7-1.2 s to the new picture.
Memory: `MemAvailable` 852-904 MB throughout; the shell's RSS 16-17 MB.

## Checks

| # | Check | Result |
| --- | --- | --- |
| 1 | Video on the launcher (DEVICE, after Vision), opens, list of `~/Videos` (4 files) | PASS (`g1-list`) |
| 2 | A file plays: picture upright, not mirrored, colours right (testsrc2's bars in order, timecode readable) | PASS (`g1-playing`) |
| 3 | Smooth enough to be useful: 30 fps shown, 0 dropped | PASS |
| 4 | Controls respond: PAUSE, PLAY, STOP, a tap on the bar, FULLSCREEN, a tap on the picture, BACK | PASS (`g3-*`) |
| 5 | Sound: PCM `RUNNING` with hw_ptr moving while playing, `closed` while paused, `RUNNING` after resume, `closed` after BACK and after leaving mid-playback; 0 xruns | PASS (by the device's state; **not heard**) |
| 6 | Seek: to 3/4 of the bar; the elapsed label and the burned-in timecode agree (0:17 / 17.3 s) | PASS |
| 7 | STOP, and PLAY after the end | **FAIL, fixed**: "seek: Operation not permitted" - the clip's first picture is stamped 1/15360 s, so a backward seek to 0 found no key picture. Fixed by taking the next key picture (`video_backend_ffmpeg.c`); re-tested with `drive.py`: end -> play, stop, play, seek 0 all answered |
| 8 | Fullscreen: the whole panel, 1008x568 picture, tap returns | PASS (`g3-fullscreen`) |
| 9 | A portrait video in landscape: letterboxed, upright | PASS (`g4-portrait-clip`) |
| 10 | No sound track: plays, "No sound" | PASS (`g4-nosound`) |
| 11 | Damaged file: "This file is damaged", BACK works | PASS (`g4-damaged`) |
| 12 | Leaving the app while playing: helper gone, PCM closed | PASS |
| 13 | Reopen and play again | PASS (`g4-reopened`) |
| 14 | 40 open -> play -> leave cycles: shell pid unchanged, RSS 16444 kB before and after, 11 descriptors, CMA free within 0.3 MB, no helper, no zombie | PASS |
| 15 | Camera and Vision work afterwards; RIFT opens | PASS (`g6-*`; Vision 25.9 fps) |
| 16 | No service restarted, nothing crashed: radiod, sysd, netd, meshcored kept their pids, the shell pid unchanged through the whole gate, 0 crash reports, supervise logs without restarts | PASS |

## Limits recorded

- On the VPU-encoded test clip a seek lands up to 200 ms after the target
  (the first picture the decoder returns); on the vendor samples exactly.
- Sound heard by nobody: the owner should listen once (any clip with sound).
- Portrait not run on the unit (host-tested in both orientations).
