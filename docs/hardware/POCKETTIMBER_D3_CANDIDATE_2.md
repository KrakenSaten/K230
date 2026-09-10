# PocketTimber D3: candidate 2, focused input retest

Candidate 1 failed D3 at the first pull (2026-09-09): TEST never answered
after a selection, a block pushed back by hand stayed locked in its seat,
and the screen overflowed the shell's body by 28 px so a tap that rolled
took the body scroll instead of the button. Candidate 2 is candidate 1 plus
the three fixes (`1ea0012`, `eb7e9c9`) and exists only to retest them.
Everything else is the v0.0.5 base. **No transmission at any step; radiod
stays on the mock backend.** Commands run as root on serial.

| | |
| --- | --- |
| Candidate | `pockettimber-d3-candidate-2` |
| Source | `eb7e9c9` on `pockettimber-engine` (clean) |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` (pinned) |
| Build UTC | `2026-09-09T12:34:47Z`, defconfig `k230_pocketos_defconfig` |
| Raw image | `out/pockettimber-d3-candidate-2/sysimage-sdcard.img`, 763,363,328 bytes, SHA-256 `084c479e0b48d64fec01b8641c2ad06adabc533ebc6dd585173cbecf16ae8995` |
| Compressed | `sysimage-sdcard.img.gz`, 204,410,390 bytes, SHA-256 `1688aa95281dcf070ef8f770506fd514e8babb6b0930613963dc03d5cefd0015` |
| Identity on the device | `/etc/pocketos-release` → `0.0.5`; `pos version` → `pos 0.0.5 (build eb7e9c9)` |

Offline verification: the package the image was built from carries the
TEST refresh fix, the seat clamp and `TABLE_HEIGHT 672` (no 700 left); the
four converted sprites are inside the shell; the rootfs file list is
identical to candidate 1 and only `/usr/bin/pocketos-shell` differs beyond
build-time stamps; `pos-hwcheck`, `pos-supervise`, the init scripts,
`pos` (apart from the build id) and the defaults (mock, EU868, 2 dBm) are
byte for byte the v0.0.5 image's.

## Flash and boot

```powershell
powershell -ExecutionPolicy Bypass -File C:\K230\tools\flash-devcard.ps1 -Image C:\K230\out\pockettimber-d3-candidate-2\sysimage-sdcard.img -Sha256 084c479e0b48d64fec01b8641c2ad06adabc533ebc6dd585173cbecf16ae8995
```

Only on `RESULT: FLASH PASS`. Boot with COM9 open; on a fresh card hand
the panel over (`ENABLE=0` in `/etc/default/k230_phone_ui`, `ENABLE=1` in
`/etc/default/pocketos-shell`, `reboot`), then `pos version` →
`pos 0.0.5 (build eb7e9c9)` and the launcher with five tiles.

## STOP conditions

Stop at the first: a button that does not answer when it is drawn enabled;
a block part way out that no drag moves; a block pushed back that leaves
the turn locked (TEST greyed, other block ends refused); a tap or drag that
scrolls the screen instead of acting; anything clipped at the bottom row;
a crash (`/var/lib/pocketos/log/crash-shell-*`, the launcher appearing by
itself, `/run/pocketos/*.crashloop`). Collect `pos logs shell -n 50` before
touching anything else.

## The sequence

Keep every pull short: a block slips free into the hand after about 60 px
of travel, which ends this sequence early (place it, then restart with
another block).

- [ ] **A. Launch.** Tap Timber: standby, `BEST 0`, `LAYERS 18`, the
      rendered sprites, and the whole bottom row (TEST, BEGIN) inside the
      panel with the buttons' bottom edge visible.
- [ ] **B. BEGIN.** `SCORE 0`, status `IN PLAY`.
- [ ] **C. Select a lower block.** Tap an end on a lower layer: the outline,
      PIECE `UNKNOWN` (or `LOOSE?`), `TESTS 2`.
- [ ] **D. TEST must respond.** The class appears, `TESTS 1`.
- [ ] **E. Partial pull.** Drag on the track the way its caption says,
      about 15 px: the block moves out, the chip reads `PULLING`, TEST is
      greyed (correct while a block is out).
- [ ] **F. Release.** The block stays where it is.
- [ ] **G. Drag again, outward.** About 15 px more: it keeps moving out.
- [ ] **H. Push back with uneven movement.** Drag the other way in uneven
      strokes until the block sits in line with its layer.
- [ ] **I. Seated and unlocked.** The block is exactly in line, the chip
      reads `KNOWN`, TEST is drawn enabled, the track caption still names
      the block. Tap another block end: the outline moves (the turn is
      open).
- [ ] **J. Select a different block.** That outline stays; PIECE shows it.
- [ ] **K. TEST must respond.** Its class appears, `TESTS 0`.
- [ ] **L. Leave Timber** (back).
- [ ] **M. Reopen Timber.** Standby, `BEST 0`, nothing selected.
- [ ] **N. Buttons and dragging.** BEGIN, select a lower block, TEST
      (`TESTS 1`), pull about 15 px, push back seated, TEST still enabled,
      another block selectable.

## No scroll-steal regression

- [ ] **O. Wobbly drag.** On the track, pull about 20 px while the finger
      drifts 15 px up and down: the block moves, the screen does not.
- [ ] **P. Rolling tap.** With a block selected, press TEST (or BEGIN in
      standby) with the thumb rolling downward as it lifts: the button
      answers, the screen does not move.
- [ ] Serial: `pos shell info | grep current` → `timber`;
      `grep -c ERROR /var/lib/pocketos/log/shell.log` → 0;
      `ls /var/lib/pocketos/log | grep -c crash` → 0.

## Verdict

**Provenance of this table.** It is reconstructed on 2026-09-10 from the
bench session record of 2026-09-09, not transcribed from a sheet filled in
at the bench — the sheet was left blank on the day. It is recorded at the
granularity the session record supports: group verdicts, plus the few
observations written down verbatim. Where a cell says *not separately
recorded*, that value was never observed individually; it is not a failure
and must not be read as a measurement. The operator should correct anything
here that disagrees with their own recollection.

Corroborating evidence in this repository: `35dd10e` and `7cf6450` (the two
defects candidate 1 found and their fixes, which candidate 2 existed to
retest) and `5b66ef9` (the ruling that the reported dead drags were test
procedure, not software).

| Step | Result | Observed |
| --- | --- | --- |
| A launch, fit | PASS | rendered sprites, screen fits the body; counters not separately recorded |
| B–D begin, select, TEST | PASS | BEGIN, selection and TEST all answer |
| E–G pull, release, pull again | PASS | a drag on the **track** moves the block; see the note below |
| H–I push back seated, turn open | PASS | a push back seats the block and reopens the turn (the `7cf6450` seat clamp) |
| J–K second block, TEST | PASS | second block selectable, TEST answers |
| L–N leave, reopen, all answer | PASS | leave and reopen, then the sequence again |
| O–P wobbly drag, rolling tap | PASS | wobbly drag moves the block, rolling tap does not scroll the screen |
| Serial checks | PASS | 0 ERROR in `shell.log`, 0 crash reports |

**INPUT RETEST PASS**, 2026-09-09, unit A.

**The first attempt on this sheet was a false failure.** It reported that no
drag on the track moved a block. That was investigated in the repository
(`4a6d078`, which fed the GT9895 event grammar through LVGL's real evdev
parser and found no software cause) and finally ruled by the owner to be
test procedure: the block had been dragged **on the table**, where the
design only takes taps. The pull happens on the dedicated track below the
table, and `5b66ef9` records the ruling. Say this to the operator before any
future Timber sheet: **the block is never dragged, only the track.**

**Runtime note.** Steps A–P and the collapse below were exercised on
candidate 2 (`eb7e9c9`). The later items in the same session ran on the
`pockettimber-d3-diag-1` image (`e3d5be0`), which is candidate 2's code plus
a diagnostic trace that is off unless `POCKETTIMBER_TRACE` is set, so
PocketTimber's behaviour is identical between the two. Both report identity
`0.0.5`.

## D3 items reached from this card in the same session

These belong to `POCKETTIMBER_D3_CANDIDATE_1.md`, which is the full D3
sheet; they are recorded here because they were reached from this card and
that sheet is still blank.

| D3 item | Result | Observed |
| --- | --- | --- |
| Collapse and the result screen (1's §4) | PASS | clean collapse, cause JOLT, "Layer 2 gave way after a jolt" |
| Record file created (1's §4) | PASS | `record.v1` 52 bytes, mode 0644, magic `PTR1`, version 1, no stale temp file |
| BEST update and persistence (1's §5) | PASS | best 1650 |
| `S90` service restart (1's §7) | PASS | not separately recorded |
| Warm reboot (1's §8) | PASS | record survived |
| Cold power cycle (1's §9) | PASS | USB removed about a minute; BEST persisted, all three games launch |
| Corrupt record (1's §10) | PASS | 12-byte and 20-byte truncations both refused without stopping play |
| Storage path and permissions (1's §11) | PASS | not separately recorded |

0 ERROR and 0 crash throughout. **No blocking PocketTimber defect was
found.**

Open at the owner's pause, and therefore carried into
`POCKETTIMBER_D3_V008_RETEST.md`: repeated runs with VmRSS watched, the
final regression smoke, and the summit attempt.

**Platform note, not a PocketTimber matter.** After a power off,
reconnecting USB within a few seconds leaves the unit off; about a minute
disconnected and then reconnected boots normally. This looks like PMIC
behaviour and is a candidate for `docs/KNOWN_ISSUES.md`.
