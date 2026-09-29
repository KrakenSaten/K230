# MP3 hardware gate (unit B)

The MP3 app (docs/apps/MP3.md, DS §40) on a real K230: the image's own
FFmpeg decoding MP3 and the other listed formats, the helper on the real
sound card, and the app under touch, without a reflash.

**Result: PASS, 2026-09-29, on unit B, except audibility**, which a gate run
over SSH cannot check: the unit cannot hear itself (the owner's ears are the
test, as for Wave and Recorder). Every step below was driven and measured
remotely.

## Build under test

- Branch `feat/audio-player`, commit **dd2e343** (device code identical to
  8f41b58; the commit after it changed tests only).
- Cross-built with the pinned SDK toolchain: `pos-mp3` with `MP3_FFMPEG=1`,
  the DRM shell (`POCKETOS_DISPLAY=drm`, LVGL from the sysroot), both with
  0 warnings. `pos-mp3` needs `libavformat.so.58`, `libavcodec.so.58`,
  `libswresample.so.3`, `libavutil.so.56` and `libasound.so.2`, all already
  in the image.
- Installed by hand, userspace only (no flash, no services touched):
  `/usr/bin/doors-shell` (md5 e1a695a3), `/usr/bin/pos-mp3` (584471f2),
  `/usr/share/doors/ui/icon-mp3.bin` (07474126). The shell reported
  `build=dd2e343`.
- Unit B before and after: image v0.2.1 (9dc66c2) carrying shell a680c56
  (md5 82084ab7), landscape 1232 x 568. Rollback `/root/rollback-mp3/RESTORE.sh`,
  run at the end: the unit is back on a680c56 with no MP3 files; the gate's
  test files are kept in `/root/rollback-mp3/Music-gate` (13.7 MB).

Tools: `C:\K230\out\mp3-gate` (env.sh, g0-g9, captures in `caps/`).

## Test files

Made on the unit with its own `ffmpeg` from the image's samples
(`/root/music`, untouched), in `/root/Music`: a 20 s MP3 with ID3 tags
(title "Gate Song", artist "Doors Gate"), two untagged 2-minute MP3s, 200 KB
of random bytes named `.mp3`, an empty `.mp3`, and 12 s of FLAC, Ogg
Vorbis, Opus, AAC (`.m4a`) and 22.05 kHz mono WAV.

## Steps and results

| # | Step | Result |
| --- | --- | --- |
| 1 | `pos-mp3 info` / `probe` on the unit | K230 board, playback validated, decoder ffmpeg. MP3 `mp3float` 44.1/48 kHz stereo, tags read; FLAC, Vorbis, Opus, AAC, WAV all probe; random bytes "the file holds no audio", empty "the file is empty", missing "not there". Probe ~35 ms. PASS |
| 2 | App launches (`doors app start mp3`) | MP3 opens fullscreen under the shell header, places Music, Home, /mnt, /media, READY, no helper started. PASS |
| 3 | Local MP3 plays | Tapped: PLAYING, "Gate Song" / "Doors Gate" from the tags, 1 / 5, progress and 0:20 length, header hint PLAYING, row NOW. `pos-mp3` owns `/dev/snd/pcmC0D0p`, substream RUNNING, pocketaudio's recovery record armed. PASS (audibility: owner) |
| 4 | End of track | The 20 s track ended and 02 started by itself. PASS |
| 5 | Pause / resume | PAUSE: the same helper stays, the PCM is closed; RESUME: RUNNING again. Four quick PLAY/PAUSE taps end playing. PASS |
| 6 | Next / previous | NEXT: a new helper on the next file. PREV more than 3 s in: the track restarts (0:03). While stopped, PREV and NEXT move the choice only. PASS |
| 7 | Seek | A tap at 80 % of the bar: 1:37 of 1:58 in a real MP3. PASS |
| 8 | Volume | VOL - / VOL +: `audio_volume=90` / `100` in settings.conf, applied to the running helper (same pid). Heard: owner |
| 9 | Bad files | Tapped: "04 broken.mp3 cannot be played: the file holds no audio." and nothing plays. Reached by the queue: the broken file is skipped and the empty last one ends it with "the file is empty". PASS |
| 10 | Other formats | FLAC, Ogg Vorbis, Opus, AAC/M4A, WAV each play (new helper, PCM RUNNING). PASS |
| 11 | Responsive while playing | `doors shell info` round trip 22 ms mean, 30 ms max, idle and playing alike. CPU over 10 s while playing: `pos-mp3` 3.5 %, `doors-shell` 1.9 %. RSS: `pos-mp3` 9.5 MB, shell 15 MB. PASS |
| 12 | Close while playing | Back slab while playing: 0.3 s later no helper, PCM closed, recovery record removed (a clean close). PASS |
| 13 | Reopen | Comes back on the Music folder, READY. PASS |
| 14 | Another app afterwards | Recorder (READY, gets the audio), Wave, Calculator open. PASS |
| 15 | No crash or restart | Shell pid unchanged for the whole gate; sysd, netd, radiod, meshcored pids as before; no crash reports. PASS |

## Not covered here

- **Audibility and loudness** (steps 3 and 8): for the owner, by ear.
- **Portrait on the unit**: unit B stayed in landscape; portrait is covered on
  the host (tests/mp3_app_test.c, both orientations).
- **Removable storage**: nothing was mounted under /mnt or /media; a folder
  or file disappearing is covered on the host (tests/mp3_ctl_test.c).
- **Audio in use by Wave or Recorder while MP3 starts**: covered on the host
  against the real helper holding the real lock (tests/mp3_session_test.c,
  tests/mp3_ctl_test.c), not on the unit.

## Redeploying for a listening check

From WSL, with the build in `~/work/mp3-rv/src`:

```
bash /mnt/c/K230/out/mp3-gate/g1_deploy.sh /home/dolby/work/mp3-rv/src dd2e343
```

and `sh /root/rollback-mp3/RESTORE.sh` on the unit to put it back.
