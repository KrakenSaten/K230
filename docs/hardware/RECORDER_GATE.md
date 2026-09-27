# Recorder - Unit A hardware gate

Branch `feat/recorder-app`. **Run on unit A on 2026-09-27 with build
`608f972`: PASS** (results at the end, "Result on unit A"). Every step is
on unit A with the shell started normally (no
`POCKETOS_AUDIO_ALLOW_UNVERIFIED`), and a second machine on SSH for the
reads. Record PASS/FAIL, the build id and the evidence class (LIVE-READ,
OWNER) per step.

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

## Result on unit A (2026-09-27)

**Unit A carries this branch's build `608f972`** (rebased onto master
`bd1c4f9`, built with the pinned Xuantie gcc 14.1.1 / Buildroot sysroot).
It was deployed as a reversible userspace install, not `deploy.sh`, and not
by reflashing:

| File | Change | md5 (stripped) |
| --- | --- | --- |
| `/usr/bin/doors-shell` | replaced | `d80c3675…` |
| `/usr/bin/pos-record` | added | `21e51029…` |
| `/usr/share/doors/ui/icon-recorder.bin` | added | `68e29a6d…` |

Rollback: `/root/rollback-recorder/RESTORE.sh` puts back shell `301fadf`
(the Browser gate build, master `bd1c4f9` device code, md5 `107a3190…`). It
also removes `pos-record`, the icon and `/var/lib/pocketos/recorder`, and
**moves** (does not delete) `/root/Recordings` to
`/root/rollback-recorder/Recordings-gate`. Nothing else was touched:
services, CLI, `pos-wave`, init scripts, other art and settings. The
rotation mode was changed during the gate and set back to Automatic.
Commits after `608f972` are tests and docs only.

**Sound source.** Every step except the owner's voice used a real acoustic
signal: unit B (K230-B) played a known 10 s test file (silence, a 1 kHz
tone, a 300-3000 Hz sweep, 1.5 kHz bursts) through its own speaker, using a
temporary `pos-record` in B's `/tmp`. Unit B also recorded the room while A
played back. Each file was fetched and analysed per 250 ms, for level and
for tone energy (Goertzel).

| # | Step | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Open | PASS: READY, no MIC ON, no helper after the check; `/root/Recordings` created 0700; "Room for about 17 min" agrees with `df` (81.5 MB free, less 48 MiB) | LIVE-READ |
| 2 | Microphone | PASS: `pos-record info` reports capture and playback validated; while recording, `hw_params` shows S16_LE, 2 ch, 48000, period 960; chip RECORDING, header MIC ON | LIVE-READ |
| 3 | 10 s take (Voice) | PASS: 10.34 s, 330,924 B, 16 kHz mono; B's tone is at 2-4 s (-24 dBFS, clear 1 kHz peak), the sweep at 5-8 s and the bursts at 8-9 s, all time-aligned | LIVE-READ |
| 3b | Owner's voice | PASS: 13.36 s; speech 1.0-9.5 s at -28..-40 dBFS RMS, then room | OWNER + LIVE-READ |
| 4 | STOP | PASS: helper gone about 0.35 s after the tap; "Saved REC-... (0:27)" (first take); `-rw-------`, no `.part` | LIVE-READ |
| 5 | PLAY | PASS: plays and ends by itself; B's recording of the room shows the 1 kHz tone at -28 dBFS, the sweep and the bursts, for both the Standard and the Voice (16 → 48 kHz) take | LIVE-READ |
| 6 | Intelligible | PASS: the owner understood their clip played back on A's speaker | OWNER |
| 7 | Level responds | PASS: the meter read -18 dB with the tone and -37 dB after it, with the peak-hold mark; its playback meter -20 dB | LIVE-READ (screen) |
| 8 | Silence | Noise floor about -47 dBFS RMS in the files (quiet room); the meter showed "-" when idle | LIVE-READ |
| 9 | Ten cycles (open / RECORD 3 s / STOP / close) | PASS 10 of 10: ten 3.3 s files, no `.part`, no helper, lock free; shell RSS 14848 → 14976 kB (flat after cycle 3), 11 fds throughout | LIVE-READ |
| 10 | 5-minute take | PASS (Voice; storage allowed Standard only about 5 min): 302.30 s, 9,673,644 B (1.92 MB/min); see "Measurements" | LIVE-READ |
| 11 | Rotate while idle | PASS: portrait and landscape layouts drawn whole, list intact, PLAY/DELETE clear of the corners | LIVE-READ (screen) |
| 12 | Rotate while recording | PASS both ways (landscape → portrait, portrait → Automatic/landscape): the shell restarts in place, and the take is saved whole (5.30 s, 5.40 s), no `.part`, lock free. The shell comes back at home, not in Recorder: that is the shell's rotation restart, not the app | LIVE-READ |
| 13 | Leave while recording | PASS: Back, helper gone 0.88 s after the tap (injection included); Home, 0.06 s; both files saved, no `.part`, lock free | LIVE-READ |
| 14 | No network | NOT RUN (the app has no network code, `recorder_lint.sh`) | - |
| 15 | No crashes | PASS: no crash files; 0 ERROR and 0 WARN in `shell.log` since deploy; the only shell starts are the four deliberate rotation restarts (same pid); no supervisor restart | LIVE-READ |
| 16 | No leftovers | PASS: no `pos-record` or `pos-wave`, nothing holds `/dev/snd`, `audio.lock` free, no `audio.recovery`, `External I2S Output Switch` on, 0 `.part`. IO34 not read | LIVE-READ |
| 17 | Files elsewhere | PASS (partial): the Voice and Standard files open in Python's `wave` reader with the right rate, width, channels and duration. No desktop player was tried | LIVE-READ |
| - | Audio in use | PASS: while `pos-wave listen` holds the card, RECORD shows "Audio device in use"; no file, no helper, Wave not killed; after Wave stops, RECORD works | LIVE-READ (screen) |
| - | Interruption | PASS: `kill -9` of the helper mid-take: "The recorder stopped unexpectedly; repairing the recording", then READY, and `...-recovered.wav` (5.10 s, valid) is listed as recovered; the route switch is back on | LIVE-READ (screen) |
| - | Portrait launcher | 17 apps: `shell.info` reports `scrolls: true`; a swipe reaches Lock and Controls; the Recorder icon is drawn from the art | LIVE-READ (screen) |

### Measurements

Over the whole take, from `/proc/<pid>/stat` ticks (one C908 core):

| | pos-record CPU | doors-shell CPU | pos-record RSS |
| --- | --- | --- | --- |
| Recording, Voice, 301 s | 1.81 % | 4.05 % | 2176 kB, flat (VmHWM 2176 kB) |
| Recording, Standard, 60 s | 1.04 % | 4.15 % | 2176 kB |
| Playing (one `top` sample each) | 0 % Standard, 10 % Voice | - | 3424 kB VSZ |

- Shell RSS 14688 kB when first opened; 14976 kB during the long take; 14592 kB at the end.
- STOP to helper gone for the 5-minute take: 0.87 s, tap injection included.
- Sizes: Voice 32,000 B/s, Standard 96,000 B/s, as specified.

### Notes (not defects of this gate)

- After a repair the status line still says "repairing the recording", in the
  present tense, although the repair is done and the list shows the file.
- The first 250 ms of a take can carry a small start-up click (peaks of 1600
  to 6700); the DC blocker and the 500 ms discard are in place.
- Unit A's root filesystem has about 60 MB free after the gate (18.8 MB of
  gate recordings). Standard allows only about 5 minutes above the 48 MiB
  reserve.
