# Recorder - Unit A hardware gate

Branch `feat/recorder-app`. Nothing below has been run yet: the Recorder is
host-tested only (docs/apps/RECORDER.md, "Tests"). Every step is on unit A
with the build deployed by `platforms/k230/scripts/deploy.sh`, the shell
started normally (no `POCKETOS_AUDIO_ALLOW_UNVERIFIED`), and a second machine
on SSH for the reads. Record PASS/FAIL, the build id and the evidence class
(LIVE-READ, OWNER) per step.

## Before

```sh
cat /etc/doors-release                         # the build under test
pos-record info                                # board k230-t-display, capture/playback validated
cat /proc/asound/cards; amixer -c 0 contents   # card present, route switch on
ls -l /proc/[0-9]*/fd 2>/dev/null | grep /dev/snd   # nothing holds the card
df -h /root; ls -la /root/Recordings 2>/dev/null
```

## The gate

| # | Step | Pass when |
| --- | --- | --- |
| 1 | Open Recorder from the launcher (DEVICE) | READY within a second; no MIC ON; `pgrep pos-record` empty after the repair; `/root/Recordings` exists, 0700 |
| 2 | Microphone detected | RECORD starts: chip RECORDING, header MIC ON; `pos-record` running; `cat /proc/asound/card0/pcm0c/sub0/hw_params` shows 48000 S16_LE 2 channels |
| 3 | Record a 10-second voice clip (Voice preset), speaking at arm's length | timer reaches 0:10; meter follows the voice |
| 4 | STOP | "Saved REC-..." within a second; the file is in the list, 0:10, ~320 KB, 16 kHz; `ls -l` shows `-rw-------`, no `.part` left |
| 5 | PLAY it | heard on the speaker, 0:10, ends by itself; no click at start or end worth noting |
| 6 | Intelligible | the words are understood by the owner |
| 7 | Level responds | talking, clapping and tapping move the meter; a shout reads near 0 dB or "Too loud" |
| 8 | Silence | in a quiet room the meter reads low (-45 dB or below, or "Silence"); note the reading |
| 9 | Repeated cycles | ten record (3 s) / stop cycles: ten files, each plays; no ERROR |
| 10 | 5-minute recording | Standard preset, 5:00; ~28.8 MB; plays; `top` during it: note `pos-record` %CPU and the shell's; note RSS (`grep VmRSS /proc/$(pgrep pos-record)/status`) |
| 11 | Rotate while idle | portrait to landscape (keyboard base, or Settings) and back: list intact, both layouts usable, nothing in a rounded corner |
| 12 | Rotate while recording | the shell restarts; the recording is saved (not `.part`), the reopened app is READY |
| 13 | Leave the app while recording | Back or Home during a recording: saved within 1.5 s; no `pos-record` left; MIC ON gone |
| 14 | No network | unplug Ethernet / Wi-Fi off: record, stop, play all work |
| 15 | No crashes | `pos system crashes` shows nothing new; `pos logs shell` has no ERROR from the session |
| 16 | No leftovers | after all of it: no `pos-record` process, nothing under `/proc/*/fd` holds `/dev/snd`, `External I2S Output Switch` on, IO34 low, no `/run/pocketos/audio.recovery` |
| 17 | The files elsewhere | copy two recordings (Voice and Standard) to another computer: both open and play in an ordinary player; `soxi`/`ffprobe` report PCM s16le mono at 16000 / 48000 Hz with the right duration |

## Also worth running (short)

- Audio in use: start a Wave LISTEN, leave Wave (it stops), open Recorder:
  records. Then from SSH hold the lock
  (`flock /run/pocketos/audio.lock sleep 20 &`) and press RECORD: "Audio device
  in use", nothing created.
- Interruption: during a recording `kill -9 $(pgrep pos-record)`: the app says
  it stopped unexpectedly and repairs it (`...-recovered.wav`, plays); the
  route switch is back on.
- Storage: `df /root` before a long recording; the "Room for about N min"
  figure should agree with it (free minus 48 MiB, at the preset's rate).
- Power loss (optional, destructive to the session only): pull power during a
  recording, boot, open Recorder: the recording comes back as
  `-recovered`, playable, missing at most the last seconds.

## Measurements to record

`pos-record` CPU % and RSS while recording (Voice, Standard) and playing;
the shell's RSS before opening Recorder and with it open; the time from STOP
to "Saved" for a 5-minute Standard file; the silence and speech meter
readings from steps 7 and 8.
