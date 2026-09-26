# Wave, one screen with presets and history: unit A gate

**Unit A carries build `f2c22f1`** (branch `feat/wave-next`; only
`/usr/bin/doors-shell` replaced, md5 `05a4d09a…`), rotation mode Automatic
(landscape, keyboard base attached), left at home and locked. Commits after
`f2c22f1` on the branch are docs and test inputs only. Rollback to master's
shell (`d512ba9` == master `11a5174`):
`/root/rollback-wave-next/RESTORE.sh`.

Result: **PASS**, the physical keyboard (§3) checked by the owner. Two
Wave defects were found on the unit and fixed on the branch (§5).
Run 2026-09-26 by Claude; evidence in `out/wave-next-gate/` (not committed).

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

## 1. Rebase

`feat/wave-next` (cloud tip `9042a0f`, from `96f8f14`) rebased onto
origin/master `11a5174` (which contains `140843e`). Two conflicts, both
small: `tests/wave_app_test.c` (master's chrome-derived `STATUS_H` kept, the
branch's landscape frame re-derived from it: the old frame assumed the 56 px
bar Wave never had) and the `wave_app_test` source list in
`ui/shell/CMakeLists.txt` (both sides' files kept). No global chrome touched.

## 2. Host validation (WSL clean clone, real `vendor/ggwave` a38e38b and `vendor/lvgl` 59dc7e4)

| Check | Result |
| --- | --- |
| Wave targets + `pos-wave`, `-O2 -Werror` | rc 0, 0 warnings |
| `wave_view` 171, `wave_model` 59, `wave_store` 30, `wave_layout` 19, `wave_session` 92, `wave_ctl` 62, `wave_modem` 52, `pocketaudio` 151 | 0 failures |
| `wave_sim_test` (real ggwave: TX -> channel -> RX, all three presets, 64 bytes on each, noise/clipping/sample loss, garbage, truncation, repeats folded, preset mismatch, capture decode) | 38 checks, 0 failures |
| `wave_lint.sh` | 71 checks, 0 failures |
| SDL shell (real LVGL), `wave_shell_test.sh` | 0 failures; `wave_app_test` 87 checks, 0 failures |
| ASan + UBSan, all Wave host tests | clean after fixing a test-only over-read in `wave_store_test` (`036c5f9`) |
| Shared shell: `chrome_test` 202, `home_layout_test` 90, `chrome_shell_test.sh`, `doors_shell_test.sh`, `style_lint.sh`, `make all -Werror` | 0 failures, 0 warnings |
| riscv64 `make all` + DRM/sysroot shell | rc 0; 0 first-party warnings (4 known in vendor/ggwave) |
| Real-shell screenshots, portrait and landscape, empty and full history | nothing clipped or under the corners, no page scroll; `chrome: none ... cluster hidden, for wave` |

The two regression checks added with the fixes were proven to fail
without them (the Enter check with `8ddf3f1`'s `wave_app.c`; the four
capture-STOP checks with `036c5f9`'s view and app).

## 3. Unit A

Peer: unit B (`pos-wave` CLI) as sender and receiver over the air. Both
directions were first proven with the CLI alone. A 20 ms sampler of
`/proc/*/comm` counted `pos-wave` processes through every step: **peak 1**
everywhere Wave alone held the audio (thousands of samples).

| # | Check | Result |
| --- | --- | --- |
| 1 | Opens in landscape and portrait; no helper, no keyboard at open | PASS |
| 2 | Physical keyboard in landscape | PASS (owner, on `f2c22f1`, keyboard base attached) |
| 3 | KEYS: touch keyboard, STRIP (composer row + HIDE); HIDE restores WIDE and keeps the text; DONE sends and gives WIDE back | PASS |
| 4 | No overlap with the current chrome: Wave stays `chrome: none`, no cluster over it, header and back button clear of the corners, composer 30 px off the foot | PASS |
| 5 | LISTEN: RX from B, MICROPHONE ON / MIC ON / LISTENING, STOP LISTEN ends the helper | PASS |
| 6 | SEND while listening, 4 cycles: listen stops, send starts 70 ms later, listen resumes ~130 ms after; B decoded all 4; never two helpers | PASS |
| 7 | STANDARD TX (B decoded) and RX | PASS |
| 8 | ROBUST TX: two helper runs, B decoded both; RX: B's two copies -> one entry `x2` | PASS (Waver on a phone not available; B runs the same ggwave) |
| 9 | QUICK TX and RX; mismatch (QUICK sender, ROBUST listener) decoded | PASS |
| 10 | CAPTURE -> decode: record ~11.1 s for 10 s, decode ~0.16 s on the C908, RX entry marked CAPTURE; `/run/pocketos/wave` empty after | PASS |
| 11 | DECODE NOW: record ended at ~5.7 of 10 s, decoded, entry marked CAPTURE | PASS |
| 11a | STOP during a capture: recording dropped, no decode, no entry, run dir empty | PASS on `8ddf3f1` (fix, §5) |
| 12 | History records TX/RX, copies, CAPTURE, FAILED (muted), STOPPED (left mid-send) | PASS |
| 13 | Preset and history survive a shell restart | PASS (QUICK, 16 entries) |
| 14 | Listen on at restart does not come back on | PASS (no helper after restart or reopen) |
| 15 | Mute: "Sound is muted", no helper started; audio busy (CLI holding the device): "Audio is in use by another program", no retry (only the holder for 5 s) | PASS |
| 16 | Close mid-listen and mid-send (back button): helper gone within 1 s; after the killed send the designed `pos-wave recover` ran 0.35 s | PASS |
| 17 | Crash reports | 0 |
| 18 | Supervisor restart loops | none; every shell start is a deploy, the deliberate restart or a rotation restart; 0 shell WARN/ERROR |

Portrait on `f2c22f1`: a finger on the field brings the touch keyboard;
DONE sends (B decoded) and the keyboard stays down; the finger brings it
back. Portrait content box 528 x 1106 (528 x 820 with the keyboard),
landscape 1192 x 442, strip 156 px tall.

## 4. Not done here

- Waver on a phone (unit B stood in: same ggwave, same protocols).
- CPU while listening/decoding under `top`; kill -9 of the shell mid-listen
  (PDEATHSIG is covered by `wave_session_test`); the 10x repeat (4 done).

## 5. Defects found on the unit and fixed

1. **No way to STOP a capture** (`8ddf3f1`). The model discarded a capture
   on STOP and the docs promised it, but the screen had no STOP while
   capturing. The SEND button now reads STOP while a capture runs.
2. **Portrait DONE brought the keyboard back** (`f2c22f1`). Enter reaches
   the focused field as a click after it has sent and hidden the keyboard;
   only a finger now shows it.
