# PocketTimber D3: hardware candidate 1

The first PocketTimber image for the K230. It is PocketOS v0.0.5 as validated
on unit A (master `d128a37`) plus the PocketTimber branch and nothing else:
the shell gains the Timber app with the real sprites, every other runtime
file is byte for byte the v0.0.5 one. The candidate is named
`pockettimber-d3-candidate-1`; the image still identifies itself as PocketOS
0.0.5, build `233455a`. It is not v0.0.6, which is reserved for the
`pos-hwcheck` probe fix and is not in here. This sheet tests the D3
acceptance list from the D2 status update and a short regression smoke
test. **No transmission at any step; radiod stays on the mock backend.**
Every command runs as root on the device over serial (COM9, 115200 8N1).

| | |
| --- | --- |
| Candidate | `pockettimber-d3-candidate-1` (not flashed when this sheet was written) |
| Source | `233455a` on `pockettimber-engine` (clean) = master `d128a37` (v0.0.5) + the PocketTimber commits |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` (pinned) |
| Defconfig | `k230_pocketos_defconfig` |
| Build UTC | `2026-09-09T08:39:34Z` |
| Raw image | `out/pockettimber-d3-candidate-1/sysimage-sdcard.img`, 763,363,328 bytes, SHA-256 `f0f2de38c262364c5b29f5a44c9ffd304180e31b6945c004834313c82ccec710` |
| Compressed | `sysimage-sdcard.img.gz`, 204,402,929 bytes, SHA-256 `da3d8a6c0118984fa5554990707eb3bb28590dba7abb56ca52b029a29b9c5313` (inflates to the raw image above) |
| Layout | identical to v0.0.5 (MBR, p1 80 MiB at 30 MiB, p2 600 MiB at 128 MiB) |
| Identity on the device | `/etc/pocketos-release` → `0.0.5`; `pos version` → `pos 0.0.5 (build 233455a)`; first line of `/var/lib/pocketos/log/shell.log` for the boot → `start version=0.0.5 build=233455a` |

Offline verification (clean ext4 clone, `git archive` apply with the pin
check, `build_image.sh`; whole rootfs of the image compared with the v0.0.5
image): the file list is identical, the only file that differs in content
is `/usr/bin/pocketos-shell` (+274,432 bytes); the four converted sprites
(`block_x_t0_p0`, `block_y_t0_p0`, `felt`, `shadow`) are found byte for byte
inside that shell and in no other file, `POCKETTIMBER_ART=1` in the package
build, no PNG or converter installed on the rootfs; `pos-hwcheck` is byte
for byte the v0.0.5 one; `pos`, `radiod`, the init scripts and the defaults
(mock, EU868, 2 dBm, `PermitEmptyPasswords no`, telnet on loopback) are
unchanged apart from build-time stamps; kernel and device trees identical;
package build 0 warnings.

## Flashing (not done yet)

`Get-FileHash` of the raw image must match the SHA-256 above before
anything is written. Use the v0.0.5 writer pattern
(`out/k230-v0.0.5/flash-v0.0.5.ps1` with the image path and hash changed):
lock and dismount the volume, throttled and resumable write, read-back
compare that tolerates only the four MBR bytes at 440–443. Do not write
over the golden, v0.0.3, v0.0.4 or v0.0.5 cards unless the owner says so.

## STOP conditions

Stop the session at the first of these, note the step, and collect
`pos logs shell -n 50`, `ls -l /var/lib/pocketos/log /var/lib/pocketos/timber`
and a copy of `record.v1` before touching anything else:

- **Crash.** The shell pid in `/run/pocketos/pocketos-shell.pid` changes
  without a restart of yours, a `crash-shell-*.txt` appears in
  `/var/lib/pocketos/log`, the panel goes to the launcher by itself, or
  `/run/pocketos/*.crashloop` exists.
- **Touch or input failure.** A tap on a block end or a button is ignored
  for more than 2 s, the wrong block is selected, the track does not follow
  the finger, or the block moves without a finger on the track.
- **Visual corruption.** Torn or garbled frames, blocks or text drawn
  outside the table panel, missing sprites, wrong colours, overlapping or
  clipped text, a frozen picture while the clock in the status bar advances.
- **Save-state corruption.** After a finished run `record.v1` is not exactly
  52 bytes, a `record.v1.tmp` is left behind, BEST is lower than a score
  this session reached, the log says `rejected` about a record this build
  wrote, or the file's `md5sum` changes across a reboot or power cycle
  without a run having finished.
- **Regression in the shell, Fleet or Radar.** A launcher tile missing,
  Fleet's saved turn or Radar's BEST gone, `pos radio info` failing, radiod
  not running, the shell stop reporting `(forced)`.
- **Asset fallback.** Blocks drawn as three flat single-colour faces with a
  hairline (the P7 placeholder), no felt texture under the tower, no
  contact shadow. The art is in the binary; a fallback on the device is a
  runtime failure to report, not a look to accept.

## 0. Boot, identity, panel handover

- [ ] Boot with COM9 open. Expect `Starting radiod (mock, EU868, 2 dBm): OK`.
- [ ] `cat /etc/pocketos-release` → `0.0.5`; `pos version` →
      `pos 0.0.5 (build 233455a)`.
- [ ] Fresh card: hand the panel over as in the v0.0.3 sheet (`ENABLE=0` in
      `/etc/default/k230_phone_ui`, `ENABLE=1` in
      `/etc/default/pocketos-shell`, `reboot`). Expect
      `Starting pocketos-shell: OK` and the launcher with **five** tiles:
      Radio, System, Fleet, Radar, Timber.
- [ ] `head -1 /var/lib/pocketos/log/shell.log` (or the last `start` line)
      → `start version=0.0.5 build=233455a`; `pidof radiod pocketos-shell`
      → two pids.
- [ ] `ls /var/lib/pocketos` → no `timber` directory yet (first launch).

## 1. Launch (D3: app launches)

- [ ] Tap **Timber**. The TABLE screen: HUD with `BEST 0`, `LAYERS 18` and a
      full STABILITY meter; the 18-layer tower on the felt with its contact
      shadow; the PIECE card; the controls with **BEGIN**.
- [ ] `pos shell info | grep current` → `timber`.
- [ ] `grep timber /var/lib/pocketos/log/shell.log` → no line yet (no record
      is not an error), and no `ERROR` line in the log.
- [ ] The blocks are the rendered sprites: wood shading on the top and both
      visible faces, the felt is a texture, the shadow sits under the base.
      If not → STOP (asset fallback).

## 2. Fit on the 528 × 700 table (D3: no clipping, scaling or font problems)

- [ ] The table panel's hairline is visible on all four sides with a margin
      to the panel edges; nothing is drawn outside it.
- [ ] HUD captions and values, the PIECE card text and the button labels are
      complete: no `…`, no overlap, no character cut at an edge, no
      blurred or double-size text.
- [ ] The pull track is a clear 64 px band the thumb can reach; the three
      side buttons (during placing, step 3) are fully visible.
- [ ] The whole tower is inside the table at 18 layers and stays inside as
      it grows (check again at the tallest tower of the session).

## 3. Normal play (D3: normal play works)

- [ ] **BEGIN.** `SCORE 0`, the meter full.
- [ ] Tap a block end on a lower layer: the end gets the selection outline;
      PIECE shows `UNKNOWN` (or `LOOSE?` for a tell), its layer and side,
      WORTH and TESTS.
- [ ] **TEST**: PIECE shows a class (LOOSE … STUCK). A second TEST spends
      the second test; a third is refused.
- [ ] Drag on the track in the direction the track's caption gives: the
      block slides out of the tower in the viewport, never under the finger;
      stop and it stays; drag back and it pushes in.
- [ ] Pull it free: PIECE shows `IN HAND`, the ghost appears on the new
      layer, LEFT / CENTRE / RIGHT move the ghost, **PLACE** commits. SCORE
      and LAYERS update; the tower sways a little and settles; the meter
      reflects the thinned layer.
- [ ] Play about five turns this way. Note any pull that felt late or
      jumpy (drag latency is a gate to measure, not a STOP).

## 4. Collapse and the first record (D3: collapse works, record file created)

- [ ] Fell the tower: yank a bottom-layer block fast, or keep pulling from
      the same side. The collapse plays block by block, the meter empties,
      then RESULT: title `TIMBER`, SCORE, `NEW BEST`, the cause sentence
      (e.g. `Layer N gave way …`), layers, pulls, clean, streak, and AGAIN.
- [ ] `ls -l /var/lib/pocketos/timber/` → `record.v1`, **52** bytes,
      `-rw-r--r--`, and no `record.v1.tmp`.
- [ ] `hexdump -C /var/lib/pocketos/timber/record.v1`. Line `00000000`
      starts `50 54 52 31 01 00` (`PTR1`, version 1); the next four bytes
      are the best score, little-endian (read them right to left as hex).
      Line `00000010` holds runs, lifetime pulls, collapses, summits as four
      little-endian 32-bit counters: runs `01 00 00 00`. Note the score.

## 5. Completed run and BEST (D3: completed run works, BEST updates)

- [ ] **AGAIN**, play a better run than the first (test the loose blocks,
      alternate sides, place against the lean). On RESULT: `NEW BEST` if
      the score is higher, else `BEST <first score>`.
- [ ] Leave the app (home) and reopen Timber: standby shows `BEST <best>`.
- [ ] `hexdump -C` again: best score equals the higher score, runs
      `02 00 00 00`, collapses `02 00 00 00`.
- [ ] `grep 'timber: best' /var/lib/pocketos/log/shell.log | tail -1` →
      `timber: best score <best> over 2 run(s) from /var/lib/pocketos/timber/record.v1`.

## 6. Summit (D3: summit works)

The summit is a completed top layer at 36 layers. With the current tuning
the modelled players end by collapse at 28–29 layers
(`docs/apps/POCKETTIMBER.md`, Pacing), and the image has no bench hook
that opens the app near the summit (`POCKETTIMBER_SCREEN` has no summit
state).

- [ ] One careful run aiming at the summit: pull from alternate sides so
      the lean cancels, place against the lean, wait for the sway to settle
      before each pull. Note the layers reached and the cause.
- [ ] If reached: RESULT title `STILL STANDING`, SCORE, BEST; standby
      afterwards; `hexdump -C` line `00000010` last four bytes (summits)
      `01 00 00 00`.
- [ ] If not reached in two attempts: record **NOT REACHED**, layers and
      cause. This is not a FAIL of the build; the owner decides whether the
      host tests (`timber_replay_test`, `timber_rules_test`) stand for the
      summit on hardware or a bench hook goes into candidate 2.

## 7. App restart (D3: record survives app restart)

- [ ] `md5sum /var/lib/pocketos/timber/record.v1`; note it.
- [ ] Leave Timber, open Fleet, come back to Timber: `BEST <best>`.
- [ ] `/etc/init.d/S90pocketos-shell restart` → `Stopping pocketos-shell: OK`
      (plain, not `(forced)`), `Starting pocketos-shell: OK`; the launcher is
      back on the panel; open Timber: `BEST <best>`; `md5sum` unchanged.

## 8. Reboot (D3: record survives OS reboot)

- [ ] `reboot`. After boot: `pidof radiod pocketos-shell` → two pids;
      `ls /run/pocketos/*.crashloop` → none.
- [ ] `md5sum /var/lib/pocketos/timber/record.v1` unchanged; `ls -l` shows
      the same size and time; open Timber: `BEST <best>`.

## 9. Full power cycle (D3: record survives full power cycle)

- [ ] `sync`, then remove power completely (USB and, if fitted, the
      battery); wait 10 s; power on with COM9 open.
- [ ] Same checks as step 8: pids, no crash-loop marker, `md5sum`
      unchanged, `BEST <best>` in Timber.

## 10. Corrupt record (D3: corrupt record does not stop the game)

With the shell running and Timber **not** open (home screen):

- [ ] `printf 'PTR1 damaged' > /var/lib/pocketos/timber/record.v1`
      (12 bytes: wrong length). Open Timber: standby with `BEST 0`, the game
      playable; `grep rejected /var/lib/pocketos/log/shell.log | tail -1` →
      `timber: stored record at /var/lib/pocketos/timber/record.v1 rejected, starting from nothing`;
      no `ERROR` line; `ls -l` shows the 12-byte file still in place.
- [ ] Play one run to its end: RESULT `NEW BEST`; `ls -l` → `record.v1` is
      52 bytes again, no `.tmp`; reopen Timber → `BEST <that score>`.
- [ ] Home, then `head -c 20 /var/lib/pocketos/timber/record.v1 > /tmp/t; cat /tmp/t > /var/lib/pocketos/timber/record.v1`
      (truncated to 20 bytes). Open Timber: `BEST 0`, `rejected` logged
      again, no `ERROR`; play one run to its end → 52 bytes again.

## 11. Storage path and permissions (D3: storage permissions/path correct)

- [ ] `ls -ld /var/lib/pocketos /var/lib/pocketos/timber` → both
      `drwxr-xr-x root root`.
- [ ] `ls -l /var/lib/pocketos/timber` → only `record.v1`,
      `-rw-r--r-- root root`, 52 bytes.
- [ ] `df /var/lib/pocketos | tail -1` → the root filesystem (p2), not
      tmpfs; `mount | grep ' / '` → `rw`.

## 12. Repeated runs (D3: repeated runs remain stable)

- [ ] `P=$(cat /run/pocketos/pocketos-shell.pid); grep VmRSS /proc/$P/status`;
      note it and the runs counter from `hexdump -C`.
- [ ] Play **five** runs back to back with AGAIN (quick reckless runs are
      fine). Between runs the collapse plays fully and RESULT appears every
      time.
- [ ] `grep VmRSS /proc/$P/status` (same pid): a growth of more than a few
      hundred kB per run is a finding to record; the pid must be the same.
- [ ] `hexdump -C`: runs increased by exactly 5; `grep -c ERROR
      /var/lib/pocketos/log/shell.log` → 0; `ls /var/lib/pocketos/log` → no
      `crash-shell-*`.

## 13. Regression smoke (Fleet, Radar, Radio, radiod)

- [ ] Fleet: the saved turn from the v0.0.5 card is not on this card; play
      one turn, leave, reopen → Resume offers it.
- [ ] Radar: one run to completion; leave, reopen → BEST shown.
- [ ] Radio: opens, the chip row shows the mock backend, rows populated;
      `pos radio info` answers.
- [ ] `ls -l /var/lib/pocketos/fleet/save.v1 /var/lib/pocketos/radar/record.v1 /var/lib/pocketos/timber/record.v1`
      → all three present; `ls /run/pocketos/*.crashloop` → none.

## Summary

**This run ended at a STOP condition.** The table below is reconstructed on
2026-09-10 from the bench session record of 2026-09-09; the sheet was left
blank on the day. The two defects it found are corroborated by their fixes
in this repository, `35dd10e` and `7cf6450`, whose commit messages carry the
root causes.

| D3 item | Verdict | Observed |
| --- | --- | --- |
| App launches on K230 (1) | PASS | identity `0.0.5` build `233455a`; handover clean; five tiles |
| No clipping / scaling / font problems on 528 × 700 (2) | **FAIL** | the table screen overflowed the body by 28 px, so the body scrolled and LVGL stole any tap or drag that rolled more than 10 px. Fixed by `TABLE_HEIGHT` 700 → 672 (`7cf6450`) |
| Normal play works (3) | **FAIL — STOP** | TEST never answered after a selection, and a block pushed back by hand stayed locked in its seat. Two separate defects, fixed by `35dd10e` and `7cf6450` |
| Collapse works (4) | not reached | |
| Record file is created (4) | not reached | |
| Completed run works (5) | not reached | |
| BEST updates (5) | not reached | |
| Summit works (6) | not reached | |
| Record survives app restart (7) | not reached | |
| Record survives OS reboot (8) | not reached | |
| Record survives full power cycle (9) | not reached | |
| Corrupt record does not stop the game (10) | not reached | |
| Storage permissions / path correct (11) | not reached | |
| Repeated runs remain stable (12) | not reached | |
| Regression smoke: Fleet, Radar, Radio (13) | not reached | |

Verdict: **D3 FAIL** at section 3, first pull. 2026-09-09, unit A, image
`233455a`.

The three fixes went onto the branch as `35dd10e` (the TEST refresh) and
`7cf6450` (the seat clamp and the 672 px table), and candidate 2 was built
to retest exactly those. See `POCKETTIMBER_D3_CANDIDATE_2.md`, which carries
the retest result and the D3 items later reached from that card.
