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

| Step | Result |
| --- | --- |
| A launch, fit | |
| B–D begin, select, TEST | |
| E–G pull, release, pull again | |
| H–I push back seated, turn open | |
| J–K second block, TEST | |
| L–N leave, reopen, all answer | |
| O–P wobbly drag, rolling tap | |

**INPUT RETEST PASS** / **INPUT RETEST FAIL** (step and STOP condition).
Operator: ______  Date: ______  Unit: ______ (image `eb7e9c9`).
