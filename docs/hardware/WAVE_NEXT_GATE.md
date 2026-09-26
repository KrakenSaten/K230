# Wave, one screen with presets and history: unit A gate

Branch `feat/wave-next`, from master `96f8f14` (v0.1.0). **Not yet run.**
This lists what has to be checked on hardware after the branch is rebased
onto the pending top-chrome master and built there. Nothing below has been
done; nothing here is VERIFIED.

Scope: Wave only (apps/wave, its tests, its docs, and the Wave source lines
in ui/shell/CMakeLists.txt). No change to pos-wave, pocketaudio, the shell's
chrome, status area, viewport or launcher, the audio limits (-12 dBFS
ceiling, volume cap 25, default volume 10), or VERSION.

## What changes

One screen instead of SEND/RECEIVE modes: LISTEN is a toggle; SEND works
while listening (the listen pauses for the send and resumes); CAPTURE
records the preset's length and decodes afterwards through the existing
`pos-wave record` and `pos-wave decode`. Three presets (STANDARD, ROBUST,
QUICK). A bounded history (40 entries) kept in
`/var/lib/pocketos/wave/history`, the preset in `wave.conf`; captures only in
`/run/pocketos/wave/`. A landscape layout, and a keyboard rule: portrait
shows the touch keyboard on a tap on the field; landscape never does and
offers KEYS. Details: docs/apps/WAVE.md.

## Host validation still to do (needs the pinned vendor trees)

The cloud session that wrote the branch could not compile `vendor/ggwave` or
`vendor/lvgl`. Before hardware:

| Check | Expect |
| --- | --- |
| `make all` and `make test` (-Werror) | rc 0; `wave_sim_test` and `wave_modem_test` pass (they link ggwave) |
| SDL shell build, `SHELL_BIN=... bash tests/wave_shell_test.sh` | rc 0; `wave_app_test` 82 checks, 0 failures, no LVGL warning |
| riscv64 `make all` and the DRM shell | rc 0, 0 first-party warnings |
| Simulator screenshots, portrait and landscape, keyboard up and down | nothing clipped, nothing under the rounded corners, no body scroll |

## Hardware checks (unit A)

Each with the shell started normally (no audio override).

1. **Open and close.** Open Wave: chip READY, "Ready. Microphone off", no
   MIC ON, no helper running (`pgrep pos-wave` empty), no keyboard. Close:
   still no helper.
2. **SEND (portrait).** Tap the field: touch keyboard up. Type `HELLO`,
   SEND: keyboard away, field empty, chip SENDING, heard, decoded by Waver on
   a phone, "Sent", a TX entry. Amplifier off afterwards (IO34 low), mixer as
   before (`amixer` diff), recovery record gone.
3. **LISTEN.** Phone sends `test`: RX entry, MICROPHONE ON and MIC ON while
   listening. STOP LISTEN: indicator off only when `pos-wave` is gone.
4. **Send while listening.** LISTEN on, SEND `REPLY`: the microphone
   indicator goes off, the speaker plays, Waver decodes it, then LISTENING
   again by itself. Check with `ps` that at no moment two `pos-wave`
   processes exist, and that the route is correct for each (speaker switch
   on only while sending). Repeat ten times; the panel stays steady.
5. **ROBUST.** Two copies, two helper runs, Waver decodes both; on a second
   unit (or a phone playing the WAV twice), one RX entry `x2`.
6. **CAPTURE.** Phone sends during the capture: after it, DECODING, then an
   RX entry marked CAPTURE. `ls /run/pocketos/wave` is empty afterwards and
   nothing appears under `/var/lib/pocketos/wave` but `history` and
   `wave.conf`. DECODE NOW ends a capture early and still decodes; STOP
   discards. Measure how long `pos-wave decode` of a 20 s capture takes on
   the C908 (ASSUMED a few seconds; unmeasured).
7. **Restart.** Pick QUICK, leave some history, restart the shell: QUICK is
   selected, the history is there, the microphone is off. CLEAR twice
   empties it and removes the file.
8. **Landscape (keyboard base attached).** No touch keyboard at open or on a
   tap on the field; the physical keyboard types; Enter sends. KEYS brings
   the touch keyboard up and the screen becomes the composer row; HIDE gives
   the screen back. Without the base in forced landscape, KEYS is the way to
   type. Nothing overlaps the (new) top chrome after the rebase.
9. **Errors.** Mute in Controls: SEND says it is muted and plays nothing.
   Another program holding the audio lock: "Audio is in use", no retry loop
   (`pgrep` stays empty).
10. **Kill.** `kill -9` the shell mid-listen and mid-send: `pos-wave` ends
    (PDEATHSIG), the next open reconciles the route; no capture left in
    `/run/pocketos/wave`.
11. **CPU.** `top` while listening and while decoding a capture: note the
    numbers (listening was 0.6 % on build `c688309`).

## Result

Not run.
