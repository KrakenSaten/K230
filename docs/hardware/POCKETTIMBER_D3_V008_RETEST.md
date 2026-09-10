# PocketTimber D3: focused retest on the v0.0.8 base

PocketTimber has been rebased from its v0.0.5 branch point onto v0.0.7
master (`6561b50`) as milestone M0 of v0.0.8. This sheet is the hardware
gate that stands between that rebase and M1, the merge to master.

**No transmission at any step; radiod stays on the mock backend.** Commands
run as root on serial.

| | |
| --- | --- |
| Candidate | `pockettimber-v0.0.8-candidate` |
| Source | `aa7137b` on `pockettimber-engine` (clean) |
| Base | v0.0.7 master `6561b5024ff8006d5bdaefda813ca86653409fb4` |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` (pinned, enforced by `apply_to_sdk.sh`) |
| Build UTC | `2026-09-10T17:47:30Z`, defconfig `k230_pocketos_defconfig` |
| Raw image | `out/k230-v0.0.8-timber-candidate/sysimage-sdcard.img`, 763,363,328 bytes, SHA-256 `3b0aae7d4604962fbbb2372c406d466f056817fc787e14e305a7d890d144bb06` |
| Compressed | `sysimage-sdcard.img.gz`, 204,443,358 bytes, SHA-256 `01f5ff70424c5b15b37d8d94adb468894810e9b9a112ce36f073273539077ce4` |
| Identity on the device | `/etc/pocketos-release` → `0.0.8` with `BUILD_ID=aa7137b`; `pos version` → `pos 0.0.8 (build aa7137b)` |

Offline verification, before the card exists: `/etc/pocketos-release` reads
`0.0.8` / `aa7137b`; `S50sysd`, `S60radiod` and `S90pocketos-shell` are all
0755 in the rootfs; `pos`, `pocketos-shell`, `radiod`, `sysd`,
`pos-supervise`, `pos-hwcheck` and `pos-spixfer` are all present; the shell
names five apps (`fleet`, `radar`, `radio`, `system`, `timber`); the shell
was linked from all fifteen Timber engine, app and UI objects and from the
four converted sprites, so the image carries the art rather than the
placeholder blocks; the PocketOS sources contributed no compiler warnings.

## What D3 has already established, and where that evidence lives

D3 ran on candidate 2 (runtime `0.0.5`, build `e3d5be0`) on 2026-09-09 and
got most of the way. On the owner's pause checkpoint the following had
passed, with 0 ERROR and 0 crash, and no blocking PocketTimber defect
known: the input and pull track, the seat and push-back, the viewport with
nothing to scroll, collapse and the result screen, `record.v1` creation and
format, BEST update and persistence, warm reboot, cold power cycle, the
corrupt-record cases (12- and 20-byte), storage path and permissions, and
an `S90` service restart.

Both candidate sheets were blank templates in this repository until
2026-09-10, when their verdict tables were reconstructed from the session
record so the merge carries its own evidence. Each reconstructed table says
so at the top, names the commits that corroborate it, and marks values that
were never separately observed rather than inventing them. **They are
reconstructions, not bench transcripts, and the operator should correct
anything that disagrees with their recollection.**

Three D3 items were still open at the pause: **repeated runs with VmRSS
watched**, the **final regression smoke**, and the **summit attempt**.

## What this sheet asks for

Timber's own sources came through the rebase byte-identical to the
pre-rebase tag `pockettimber-prerebase-v0.0.8`, so the game is not the
thing that changed. The platform under it is: v0.0.6 and v0.0.7 landed
since candidate 2 was built, bringing `sysd` as a fourth process, the
System Status screen, the supervisor state file, and `system.reboot` /
`system.poweroff` driven through init. So this is a short sheet: the three
items D3 never finished, plus the two survival cases whose **mechanism**
has changed underneath a passing result.

**Operator note, from the candidate 1 and 2 sessions: the block is never
dragged. Pulls happen on the dedicated track below the table.** Two bench
sessions were lost to this before it was understood.

### 1. Launch and identity on the new base

- [ ] `pos version` → `pos 0.0.8 (build aa7137b)`;
      `cat /etc/pocketos-release` → `0.0.8` and `BUILD_ID=aa7137b`.
- [ ] Launcher shows **five** tiles. Timber opens: standby, the rendered
      sprites, `BEST 0` on a fresh card, the bottom row inside the panel.
- [ ] One short run to a collapse, to confirm nothing about play changed on
      the new base: `record.v1` appears, 52 bytes, BEST updates.

### 2. Repeated runs, with memory watched (D3 item 12, never done)

- [ ] Five runs back to back without leaving the app. Before the first and
      after the fifth: `grep VmRSS /proc/$(pidof pocketos-shell)/status`.
      Record both. A steadily climbing RSS across five runs is a finding.
- [ ] The runs counter at offset 16 of `record.v1` advances by five:
      `hexdump -C /var/lib/pocketos/timber/record.v1`.
- [ ] No crash, no `/run/pocketos/*.crashloop`, `grep -c ERROR
      /var/lib/pocketos/log/shell.log` → 0.

### 3. Record survival across the v0.0.7 power paths (mechanism changed)

Candidate 2 proved survival across a warm reboot and a cold power cycle,
but it did so on a v0.0.5 image, where neither `system.reboot` nor
`system.poweroff` existed. Both are new code between the game and the
filesystem, so both are re-run here.

- [ ] `md5sum /var/lib/pocketos/timber/record.v1`; note it.
- [ ] **Restart from the System Status screen.** After the boot: Timber in
      the launcher, BEST unchanged, `md5sum` identical.
- [ ] **Power off from the System Status screen** (operator-attended, as in
      `V0.0.7_BLOCK2C_SMOKE.md`). Leave the unit disconnected for about a
      minute before reconnecting — reconnecting within seconds leaves it
      off, which is PMIC behaviour and not a fault. After the boot: BEST
      unchanged, `md5sum` identical.

### 4. Summit attempt (D3 item 6, never done)

The summit is a completed top layer at 36 layers. Two facts bound this:
the spec states it "is not reached in practice" once the D3 shift lean is
in (`docs/apps/POCKETTIMBER.md`), and `$POCKETTIMBER_SCREEN` has no summit
state, so the image carries no way to open the app near one.

- [ ] One careful run aiming at it: pull from alternate sides so the lean
      cancels, place against the lean, let the sway settle before each
      pull. Record the layers reached and the cause.
- [ ] If reached: RESULT title `STILL STANDING`; the summits counter at
      offset 28 of `record.v1` reads `01 00 00 00`.
- [ ] If not reached in two attempts: record **NOT REACHED** with layers
      and cause. This is not a failure of the build. The owner then decides
      between letting `timber_replay_test` and `timber_rules_test` stand
      for the summit — both reach it from a constructed tower, and both
      pass on this commit — and adding a bench hook, which is code and so
      belongs after M0, not inside it.

### 5. Regression smoke (D3 item 13, never done; extended for v0.0.7)

- [ ] Fleet: play one turn, leave, reopen → Resume offers it.
- [ ] Radar: one run to completion; leave, reopen → BEST shown.
- [ ] Radio: opens, the chip row shows the mock backend, rows populate;
      `pos radio info` answers.
- [ ] `ls -l /var/lib/pocketos/fleet/save.v1 /var/lib/pocketos/radar/record.v1 /var/lib/pocketos/timber/record.v1`
      → all three present.
- [ ] `pos system info` and `pos system status` answer; `release_build`
      matches `pos version`.
- [ ] System Status opens: vitals, storage, network, services, radio and
      identity populate.
- [ ] Services healthy from the supervisor state file: `sysd`, `radiod`
      and `pocketos-shell` all `running=true`.
- [ ] `ls /run/pocketos/*.crashloop` → none.

## Flash and boot

Flash with the generic tool, which identifies the card by hardware, not by
what is on it:

```powershell
powershell -ExecutionPolicy Bypass -File C:\K230\tools\flash-devcard.ps1 -Image C:\K230-wt\timber\out\k230-v0.0.8-timber-candidate\sysimage-sdcard.img -Sha256 3b0aae7d4604962fbbb2372c406d466f056817fc787e14e305a7d890d144bb06
```

Only on `RESULT: FLASH PASS`. Boot with the serial console open. On a fresh
card the panel must be handed over once (`ENABLE=0` in
`/etc/default/k230_phone_ui`, `ENABLE=1` in `/etc/default/pocketos-shell`,
`reboot`): `S90pocketos-shell` ships disabled by default and refuses to
start while the vendor launcher owns the panel.

The image has no `stat`, `timeout` or `hd` applets; use `ls -l`, `md5sum`
and `hexdump -C`.

## STOP conditions

Stop at the first of: a button that does not answer when it is drawn
enabled; a block part way out that no drag on the track moves; a block
pushed back that leaves the turn locked; a tap or drag that scrolls the
screen instead of acting; anything clipped at the bottom row; a record file
that does not survive a restart or a power off; a crash
(`/var/lib/pocketos/log/crash-shell-*`, the launcher appearing by itself,
`/run/pocketos/*.crashloop`). Collect `pos logs shell -n 50` before
touching anything else.

## Verdict

| Section | Item | Verdict | Observed |
| --- | --- | --- | --- |
| 1 | Identity 0.0.8 / aa7137b, five tiles, Timber launches | | |
| 1 | One run to a collapse, record written, BEST updates | | |
| 2 | Five repeated runs, VmRSS before and after | | |
| 2 | Runs counter advanced by five | | |
| 3 | Record survives `system.reboot` from System Status | | |
| 3 | Record survives `system.poweroff` from System Status | | |
| 4 | Summit reached, or NOT REACHED with layers and cause | | |
| 5 | Fleet, Radar, Radio smoke | | |
| 5 | v0.0.7 core: `pos system` info/status, System Status, services | | |

**D3 PASS** / **D3 FAIL** (section and STOP condition).

Operator: ______  Date: ______  Unit: ______ (image `aa7137b`).

A PASS here is the gate for M1: freeze PocketTimber v1 and merge
`pockettimber-engine` to master.
